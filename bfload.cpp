/* bfload -- packs a program image for the YADAN board's loader, and sends it over the serial port.
 * The packed format is in doc/bincode.txt, the board's side in doc/rtl.md. */
#include "libbrainfunk.hpp"

#include <getopt.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr int PROGRAM_BAUD = 115200;          // the program's own I/O, in rtl/board/yadan_top.sv
constexpr int DEFAULT_LOAD_BAUD = 2000000;    // LOAD_BAUD there
constexpr std::size_t CODE_RAM_WORDS = 1 << 20;
constexpr std::string_view BOARD_USB_VENDOR = "1a86"; // the board's CH340

enum class Mode { Run, Flash, Pack, Id };

void usage(std::ostream& os, const char* name) {
    os << "Usage: " << name << " [-m run|flash] [-t] [-p port] [-b baud] image\n"
       << "       " << name << " -m pack -o file image\n"
       << "       " << name << " -m id [-p port] [-b baud]\n"
       << "       " << name << " -h\n"
       << "\n"
       << "Packs a program image for the YADAN board's loader, and sends it over the serial port.\n"
       << "The image is what bit2bin prints, one hexadecimal word per line, or an image already\n"
       << "packed; '-' is standard input.\n"
       << "\n"
       << "  -m mode   what to do (default run):\n"
       << "              run    load it into Code RAM and run it\n"
       << "              flash  write it to the user flash, then boot from there\n"
       << "              pack   only pack it, into -o\n"
       << "              id     print the user flash's JEDEC ID\n"
       << "  -t        after run or flash, stay as a terminal for the program, at " << PROGRAM_BAUD << " baud\n"
       << "  -o file   where pack writes, '-' for standard output\n"
       << "  -p port   the board's serial port (default $BOARD_PORT, or else the one port under\n"
       << "            /dev/serial/by-id from USB vendor " << BOARD_USB_VENDOR << ", the board's CH340)\n"
       << "  -b baud   the loader's speed, LOAD_BAUD in rtl/board/yadan_top.sv (default " << DEFAULT_LOAD_BAUD << ")\n"
       << "  -h        show this help\n"
       << "\n"
       << "Examples:\n"
       << "  brainfunk -m bit -f program.b | bit2bin > program.hex\n"
       << "  " << name << " -t program.hex                    run it, and talk to it\n"
       << "  " << name << " -m flash program.hex              boot it from now on\n"
       << "  " << name << " -m pack -o program.bfi program.hex\n";
}

[[noreturn]] void usage_error(const char* name, const std::string& message) {
    std::cerr << name << ": " << message << "\n\n";
    usage(std::cerr, name);
    std::exit(2);
}

std::optional<Mode> parse_mode(std::string_view text) {
    if (text == "run") return Mode::Run;
    if (text == "flash") return Mode::Flash;
    if (text == "pack") return Mode::Pack;
    if (text == "id") return Mode::Id;
    return std::nullopt;
}

/* -p, else $BOARD_PORT, else the only serial port the board's USB bridge could be. */
std::string find_port(const std::string& given) {
    if (!given.empty()) return given;
    if (const char* from_env = std::getenv("BOARD_PORT"); from_env && *from_env) return from_env;

    const std::filesystem::path by_id = "/dev/serial/by-id";
    std::vector<std::string> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(by_id, error)) {
        if (entry.path().filename().string().find(BOARD_USB_VENDOR) != std::string::npos) found.push_back(entry.path().string());
    }
    if (found.size() == 1) return found.front();
    std::string message = found.empty() ? "no serial port under " + by_id.string() + " looks like the board's"
                                        : "more than one serial port looks like the board's:";
    for (const auto& port : found) message += "\n  " + port;
    throw std::runtime_error(message + "\ngive one with -p, or set BOARD_PORT");
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

/* The next byte from the board, or nothing within the time. */
std::optional<std::uint8_t> wait_byte(int fd, int milliseconds) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    if (select(fd + 1, &read_set, nullptr, nullptr, &timeout) <= 0) return std::nullopt;
    std::uint8_t byte = 0;
    if (read(fd, &byte, 1) != 1) return std::nullopt;
    return byte;
}

void expect(int fd, std::uint8_t answer, int milliseconds, const std::string& what) {
    const auto got = wait_byte(fd, milliseconds);
    if (!got) throw std::runtime_error("the board did not answer " + what);
    if (*got != answer) throw std::runtime_error("the board answered " + what + " with '" + std::string(1, static_cast<char>(*got)) + "'");
}

/* The serial port, with the board's loader listening: a Break, then the loader's speed. */
class Board {
public:
    Board(const std::string& name, int baud) {
        // Without O_NONBLOCK this waits for a carrier that never comes if CLOCAL is off, as minicom leaves it.
        fd_ = open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) throw std::runtime_error(name + ": " + std::strerror(errno));
        set_speed(fd_, PROGRAM_BAUD);
        fcntl(fd_, F_SETFL, 0);
        usleep(50000);
        tcsendbreak(fd_, 0);
        usleep(20000);
        set_speed(fd_, baud);
        tcflush(fd_, TCIOFLUSH);
    }
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;
    ~Board() { close(fd_); }

    /* Back to the program's speed, for whatever comes after the loader. */
    void to_program() { set_speed(fd_, PROGRAM_BAUD); }
    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

void load_into_ram(Board& board, std::span<const std::uint8_t> packed) {
    const std::uint8_t command[] = {'R'};
    write_all(board.fd(), command);
    write_all(board.fd(), packed);
    const auto answer = wait_byte(board.fd(), 3000);
    board.to_program();
    if (!answer) throw std::runtime_error("the board did not answer the image");
    if (*answer == 'E') throw std::runtime_error("the board found the image corrupt");
    if (*answer != 'K') throw std::runtime_error("the board answered the image with '" + std::string(1, static_cast<char>(*answer)) + "'");
    std::fprintf(stderr, "loaded into Code RAM\n");
}

void write_to_flash(Board& board, std::span<const std::uint8_t> packed) {
    const auto length = static_cast<std::uint32_t>(packed.size());
    const std::uint8_t command[] = {'F', static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                                    static_cast<std::uint8_t>(length >> 16)};
    write_all(board.fd(), command);
    expect(board.fd(), 'R', 30000, "the erase");
    std::vector<std::uint8_t> padded(packed.begin(), packed.end());
    padded.resize((padded.size() + 255) / 256 * 256, 0xFF);
    for (std::size_t page = 0; page < padded.size(); page += 256) {
        write_all(board.fd(), std::span(padded).subspan(page, 256));
        expect(board.fd(), '.', 2000, "page " + std::to_string(page / 256));
    }
    board.to_program();
    std::fprintf(stderr, "flash written; the board checks it as it boots from it\n");
}

void print_flash_id(Board& board) {
    const std::uint8_t command[] = {'I'};
    write_all(board.fd(), command);
    std::uint32_t id = 0;
    for (int i = 0; i < 3; ++i) {
        const auto byte = wait_byte(board.fd(), 1000);
        if (!byte) throw std::runtime_error("the board did not send the flash's ID");
        id = id << 8 | *byte;
    }
    board.to_program();
    std::printf("%06x\n", id);
}

/* Passes the program's output to stdout and stdin to the program, until either ends. */
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

void write_packed(const std::string& output, std::span<const std::uint8_t> packed) {
    if (output == "-") {
        std::cout.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("cannot write standard output");
        return;
    }
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
    if (!file) throw std::runtime_error("cannot write " + output);
}

} // namespace

int main(int argc, char** argv) {
    const char* name = argv[0];
    Mode mode = Mode::Run;
    std::string port_name, output;
    int baud = DEFAULT_LOAD_BAUD;
    bool stay = false;

    int opt;
    while ((opt = ::getopt(argc, argv, "hm:p:b:to:")) != -1) {
        switch (opt) {
        case 'h':
            usage(std::cout, name);
            return 0;
        case 'm':
            if (const auto parsed = parse_mode(optarg)) mode = *parsed;
            else usage_error(name, std::string("unknown mode: ") + optarg);
            break;
        case 'p':
            port_name = optarg;
            break;
        case 'b':
            try {
                baud = std::stoi(optarg);
            } catch (const std::exception&) {
                usage_error(name, std::string("not a baud rate: ") + optarg);
            }
            break;
        case 't':
            stay = true;
            break;
        case 'o':
            output = optarg;
            break;
        default:
            usage(std::cerr, name);
            return 2;
        }
    }

    const int images = argc - optind;
    if (mode == Mode::Id ? images != 0 : images != 1)
        usage_error(name, mode == Mode::Id ? "id takes no image" : images == 0 ? "no image given" : "more than one image given");
    if (!output.empty() && mode != Mode::Pack) usage_error(name, "-o is for -m pack; the other modes send the image");
    if (mode == Mode::Pack && output.empty()) usage_error(name, "-m pack needs -o");
    if (stay && (mode == Mode::Pack || mode == Mode::Id)) usage_error(name, "-t is for -m run and -m flash");

    try {
        std::vector<std::uint8_t> packed;
        if (mode != Mode::Id) {
            const auto words = read_image_file(argv[optind]);
            if (words.size() > CODE_RAM_WORDS)
                throw std::runtime_error(std::to_string(words.size()) + " words is more than Code RAM's " + std::to_string(CODE_RAM_WORDS));
            packed = pack_image(words);
            std::fprintf(stderr, "%zu words, %zu bytes packed from %zu\n", words.size(), packed.size(), words.size() * 4);
        }
        if (mode == Mode::Pack) {
            write_packed(output, packed);
            return 0;
        }

        Board board(find_port(port_name), baud);
        switch (mode) {
        case Mode::Run: load_into_ram(board, packed); break;
        case Mode::Flash: write_to_flash(board, packed); break;
        case Mode::Id: print_flash_id(board); break;
        case Mode::Pack: break;
        }
        if (stay) terminal(board.fd());
    } catch (const std::exception& error) {
        std::cerr << name << ": " << error.what() << '\n';
        return 1;
    }
    return 0;
}
