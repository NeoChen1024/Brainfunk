// Packs a program image for the board's loader, and sends it.
//
//   bfload image.hex                        load into Code RAM over the serial port and run it
//   bfload -r image.hex                     program the user flash and boot from it
//   bfload -c image.hex                     either of those, then stay as a terminal for the program
//   bfload -o image.bfi image.hex           write the packed image to a file instead
//   bfload -i                               print the user flash's JEDEC ID
//
//   -p port       serial port (default /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0)
//   -b baud       loader speed; must match LOAD_BAUD in rtl/board/yadan_top.sv (default 2000000)
//
// The format is in doc/bincode.txt.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr std::size_t WINDOW_WORDS = 65536; // as far back as a 16-bit distance reaches
constexpr std::size_t MAX_LITERAL = 128, MIN_COPY = 2, MAX_COPY = 129, MAX_CHAIN = 64;

using Bytes = std::vector<std::uint8_t>;

std::vector<std::uint32_t> read_image(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) throw std::runtime_error("cannot open " + filename);
    std::vector<std::uint32_t> words;
    std::string line;
    while (std::getline(file, line)) {
        std::size_t used = 0;
        const unsigned long word = std::stoul(line, &used, 16);
        if (used != 8) throw std::runtime_error("malformed image line: " + line);
        words.push_back(static_cast<std::uint32_t>(word));
    }
    if (words.empty()) throw std::runtime_error("empty image");
    return words;
}

void put_little(Bytes& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

/* Tokens: 0lllllll, then l+1 literal words; 1lllllll, then a 16-bit distance, for l+2 copied words. */
Bytes compress(std::span<const std::uint32_t> words) {
    Bytes out;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> seen;
    std::size_t literal_start = 0;

    const auto flush_literals = [&](std::size_t end) {
        while (literal_start < end) {
            const std::size_t run = std::min(end - literal_start, MAX_LITERAL);
            out.push_back(static_cast<std::uint8_t>(run - 1));
            for (std::size_t k = 0; k < run; ++k) put_little(out, words[literal_start + k], 4);
            literal_start += run;
        }
    };
    const auto remember = [&](std::size_t at) {
        if (at + 1 < words.size()) seen[{words[at], words[at + 1]}].push_back(at);
    };

    std::size_t at = 0;
    while (at < words.size()) {
        std::size_t best = 0, best_distance = 0;
        if (at + 1 < words.size()) {
            const auto found = seen.find({words[at], words[at + 1]});
            if (found != seen.end()) {
                const auto& places = found->second;
                std::size_t tried = 0;
                for (auto place = places.rbegin(); place != places.rend() && tried < MAX_CHAIN; ++place, ++tried) {
                    if (at - *place > WINDOW_WORDS - 1) break;
                    std::size_t length = 0;
                    while (at + length < words.size() && length < MAX_COPY && words[*place + length] == words[at + length]) ++length;
                    if (length > best) {
                        best = length;
                        best_distance = at - *place;
                    }
                }
            }
        }
        if (best >= MIN_COPY) {
            flush_literals(at);
            out.push_back(static_cast<std::uint8_t>(0x80 | (best - MIN_COPY)));
            put_little(out, static_cast<std::uint32_t>(best_distance), 2);
            for (std::size_t k = 0; k < best; ++k) remember(at + k);
            at += best;
            literal_start = at;
        } else {
            remember(at);
            ++at;
        }
    }
    flush_literals(at);
    return out;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

Bytes pack(std::span<const std::uint32_t> words) {
    if (words.size() >= (std::size_t{1} << 20)) throw std::runtime_error("image larger than Code RAM");
    const Bytes payload = compress(words);
    if (payload.size() >= (std::size_t{1} << 24)) throw std::runtime_error("packed image too large");
    Bytes out{'B', 'F', 'I', 'M'};
    put_little(out, static_cast<std::uint32_t>(payload.size()), 3);
    put_little(out, static_cast<std::uint32_t>(words.size()), 3);
    put_little(out, crc32(payload), 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

speed_t speed_constant(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    case 576000: return B576000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    default: throw std::runtime_error("unsupported baud rate " + std::to_string(baud));
    }
}

void set_speed(int fd, int baud) {
    termios settings{};
    if (tcgetattr(fd, &settings) != 0) throw std::runtime_error(std::strerror(errno));
    cfmakeraw(&settings);
    // HUPCL drops RTS on close, so that the next open pulses the board's reset. minicom clears it,
    // and the setting outlives the port being closed.
    settings.c_cflag |= CLOCAL | CREAD | HUPCL;
    settings.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS | CSTOPB | PARENB);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    cfsetispeed(&settings, speed_constant(baud));
    cfsetospeed(&settings, speed_constant(baud));
    if (tcsetattr(fd, TCSADRAIN, &settings) != 0) throw std::runtime_error(std::strerror(errno));
}

void write_all(int fd, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto wrote = write(fd, bytes.data() + sent, bytes.size() - sent);
        if (wrote < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            throw std::runtime_error(std::strerror(errno));
        }
        sent += static_cast<std::size_t>(wrote);
    }
    tcdrain(fd);
}

/* The byte the loader answers with, or 0 on timeout. */
std::uint8_t wait_byte(int fd, int milliseconds) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    if (select(fd + 1, &read_set, nullptr, nullptr, &timeout) <= 0) return 0;
    std::uint8_t byte = 0;
    return read(fd, &byte, 1) == 1 ? byte : std::uint8_t{0};
}

void terminal(int port) {
    termios saved{};
    const bool is_tty = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (is_tty) {
        termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    std::array<char, 4096> buffer{};
    bool input_open = true;
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(port, &read_set);
        if (input_open) FD_SET(STDIN_FILENO, &read_set);
        if (select(port + 1, &read_set, nullptr, nullptr, nullptr) < 0) break;
        if (FD_ISSET(port, &read_set)) {
            const auto got = read(port, buffer.data(), buffer.size());
            if (got > 0 && write(STDOUT_FILENO, buffer.data(), static_cast<std::size_t>(got)) < 0) break;
        }
        if (input_open && FD_ISSET(STDIN_FILENO, &read_set)) {
            const auto got = read(STDIN_FILENO, buffer.data(), buffer.size());
            if (got <= 0) input_open = false;
            else write_all(port, std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(got)));
        }
    }
    if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
}

} // namespace

int main(int argc, char** argv) {
    std::string port_name = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0", output, input;
    int baud = 2000000;
    bool stay = false, to_rom = false, ask_id = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (++i >= argc) throw std::runtime_error(arg + " needs a value");
                return argv[i];
            };
            if (arg == "-p") port_name = value();
            else if (arg == "-b") baud = std::stoi(value());
            else if (arg == "-o") output = value();
            else if (arg == "-c") stay = true;
            else if (arg == "-r") to_rom = true;
            else if (arg == "-i") ask_id = true;
            else input = arg;
        }
        if (input.empty() && !ask_id) {
            std::cerr << "usage: bfload [-r] [-c] [-p port] [-b baud] [-o packed] image.hex\n"
                         "       bfload -i [-p port] [-b baud]\n";
            return 2;
        }

        Bytes packed;
        if (!ask_id) {
            const auto words = read_image(input);
            packed = pack(words);
            std::fprintf(stderr, "%zu words, %zu bytes packed from %zu\n", words.size(), packed.size(), words.size() * 4);
        }

        if (!output.empty()) {
            std::ofstream file(output, std::ios::binary);
            file.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
            if (!file) throw std::runtime_error("cannot write " + output);
            return 0;
        }

        // Without O_NONBLOCK this waits for a carrier that never comes if CLOCAL is off, as minicom leaves it.
        const int port = open(port_name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (port < 0) throw std::runtime_error(port_name + ": " + std::strerror(errno));
        set_speed(port, 115200);
        fcntl(port, F_SETFL, 0);
        usleep(50000);
        tcsendbreak(port, 0);
        usleep(20000);
        set_speed(port, baud);
        tcflush(port, TCIOFLUSH);

        if (ask_id) {
            const std::uint8_t command[] = {'I'};
            write_all(port, command);
            int id = 0;
            for (int i = 0; i < 3; ++i) id = id << 8 | wait_byte(port, 1000);
            set_speed(port, 115200);
            std::printf("%06x\n", id);
            close(port);
            return 0;
        }

        if (to_rom) {
            const std::uint32_t length = static_cast<std::uint32_t>(packed.size());
            const std::uint8_t command[] = {'F', static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                                            static_cast<std::uint8_t>(length >> 16)};
            write_all(port, command);
            if (wait_byte(port, 30000) != 'R') throw std::runtime_error("the board did not erase the flash");
            Bytes padded = packed;
            padded.resize((padded.size() + 255) / 256 * 256, 0xFF);
            for (std::size_t page = 0; page < padded.size(); page += 256) {
                write_all(port, std::span(padded).subspan(page, 256));
                if (wait_byte(port, 2000) != '.') throw std::runtime_error("the board did not write page " + std::to_string(page / 256));
            }
            set_speed(port, 115200);
            std::fprintf(stderr, "flash programmed; the board verifies it by booting from it\n");
        } else {
            const std::uint8_t command[] = {'R'};
            write_all(port, command);
            write_all(port, packed);
            const auto answer = wait_byte(port, 3000);
            set_speed(port, 115200);
            if (answer != 'K') {
                std::fprintf(stderr, "the board %s\n", answer == 'E' ? "rejected the image" : "did not answer");
                return 1;
            }
            std::fprintf(stderr, "loaded\n");
        }
        if (stay) terminal(port);
        close(port);
    } catch (const std::exception& error) {
        std::cerr << "bfload: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
