/* bfconsole -- loads a program onto the YADAN board, and is a console for it over the serial port.
 *
 * The board frames the program's output for it: FF FF is an FF byte, and FF with another byte says
 * what is not output, flow control and the program's end. Input goes as it is, a little at a time,
 * and the end of input is a short Break. The protocol is in doc/rtl.md, the image format in
 * doc/bincode.txt. */
#include "brainfunk/libbrainfunk.hpp"

#include <getopt.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr int PROGRAM_BAUD = 115200;          // the program's own I/O, in rtl/board/yadan_top.sv
constexpr int DEFAULT_LOAD_BAUD = 2000000;    // LOAD_BAUD there
constexpr std::size_t CODE_RAM_WORDS = 1 << 20;
constexpr std::string_view BOARD_USB_VENDOR = "1a86"; // the board's CH340

/* Input goes out this much at a time, each drained before the next, so that when the board says
 * stop, what is already on its way is about this plus 64 bytes: measured on the board, against
 * half of its 1 KiB receive FIFO free. Writing more at once leaves up to 4 KiB in the driver. */
constexpr std::size_t CHUNK = 64;

/* Breaks are told apart by length (rtl/brainfunk_system.sv): from 1 ms the end of input, from
 * 10 ms framing on, from 100 ms a command. The CH340 makes a Break about 1 ms longer than asked,
 * and 2 ms at the least. */
constexpr int EOF_BREAK_MS = 1;
constexpr int FRAME_BREAK_MS = 30;
constexpr int COMMAND_BREAK_MS = 250;

constexpr std::uint8_t ESCAPE = 0xFF, XOFF = 0x13, XON = 0x11;
constexpr std::uint8_t KEY_EXIT = 0x18, KEY_EOF = 0x04; // ^X, ^D

constexpr int EXIT_DONE = 0, EXIT_ERROR = 1, EXIT_USAGE = 2, EXIT_TRAPPED = 3, EXIT_TIMEOUT = 4, EXIT_SIGNAL = 130;

enum class Mode { Run, Flash, Id };

volatile std::sig_atomic_t stop_signal = 0;
extern "C" void on_signal(int number) { stop_signal = number; }

void usage(std::ostream& os, const char* name) {
    os << "Usage: " << name << " [options] [image | -f file | -s code]\n"
       << "       " << name << " -m flash [options] (image | -f file | -s code)\n"
       << "       " << name << " -m id [-p port] [-b baud]\n"
       << "       " << name << " -h\n"
       << "\n"
       << "Talks to the YADAN board over its serial port. Given a program, loads it into Code RAM,\n"
       << "or with -m flash writes it to the user flash and boots it, and then is a console for it;\n"
       << "given none, is a console for what the board runs. The program is an image in any form\n"
       << "brainfunk -b reads, '-' for standard input, or Brainfuck source, which is compiled first.\n"
       << "\n"
       << "  -m mode   run (default): load into Code RAM and run; flash: write the user flash and\n"
       << "            boot from it; id: print the user flash's JEDEC ID\n"
       << "  -f file   Brainfuck source instead of an image\n"
       << "  -s code   Brainfuck source on the command line\n"
       << "  -O level  optimization level for source, 0 to " << OPT_MAX << " (default " << OPT_FOLD << ", the highest the\n"
       << "            core runs: the fused instructions of " << OPT_FUSE << " trap)\n"
       << "  -e mode   what input stores at the end of input, 0, 255 or keep, recorded in the image\n"
       << "            sent. The default is what an image file says, else nothing: no end of input\n"
       << "  -n        load, and do not stay as a console\n"
       << "  -i file   send the file as input first, then standard input\n"
       << "  -o file   write the program's output to the file too, byte for byte\n"
       << "  -t ms     exit when nothing has come or gone for that many milliseconds\n"
       << "  -X        ^X and ^D are bytes like any other\n"
       << "  -L        no local echo\n"
       << "  -p port   the board's serial port (default $BOARD_PORT, or else the one port under\n"
       << "            /dev/serial/by-id from USB vendor " << BOARD_USB_VENDOR << ", the board's CH340)\n"
       << "  -b baud   the loader's speed, LOAD_BAUD in rtl/board/yadan_top.sv (default " << DEFAULT_LOAD_BAUD << ")\n"
       << "  -h        show this help\n"
       << "\n"
       << "At a terminal, ^X exits, and ^D ends the input if the image says what that stores;\n"
       << "otherwise ^D goes as it is. What is typed is echoed, Enter sends LF, and the output's LF\n"
       << "shows as CR LF. Input from a file or a pipe ends with it, the same way.\n"
       << "\n"
       << "Exit status: 0 when the program halts or at ^X, 3 when it traps, 4 at the -t time,\n"
       << "1 on an error, 2 on a usage error.\n"
       << "\n"
       << "Examples:\n"
       << "  " << name << " -f program.b                   run it, and talk to it\n"
       << "  " << name << " -e 0 -i input.txt -o out.txt -f program.b < /dev/null\n"
       << "  " << name << " -m flash -O2 -f program.b      boot it from now on\n"
       << "  " << name << "                                 a console for what the board runs\n";
}

[[noreturn]] void usage_error(const char* name, const std::string& message) {
    std::cerr << name << ": " << message << "\n\n";
    usage(std::cerr, name);
    std::exit(EXIT_USAGE);
}

std::optional<Mode> parse_mode(std::string_view text) {
    if (text == "run") return Mode::Run;
    if (text == "flash") return Mode::Flash;
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
}

/* The serial port, raw, and owned for as long as this runs: no other tool's settings get in. */
class Port {
public:
    explicit Port(const std::string& name) {
        // Without O_NONBLOCK this waits for a carrier that never comes if CLOCAL is off, as minicom leaves it.
        fd_ = open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) throw std::runtime_error(name + ": " + std::strerror(errno));
        speed(PROGRAM_BAUD);
        fcntl(fd_, F_SETFL, 0);
    }
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    ~Port() { close(fd_); }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    void speed(int baud) {
        termios settings{};
        if (tcgetattr(fd_, &settings) != 0) throw std::runtime_error(std::strerror(errno));
        cfmakeraw(&settings);
        // HUPCL drops RTS on close, so that the next open pulses the board's reset. minicom clears it,
        // and the setting outlives the port being closed.
        settings.c_cflag |= CLOCAL | CREAD | HUPCL;
        settings.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS | CSTOPB | PARENB);
        settings.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        cfsetispeed(&settings, speed_constant(baud));
        cfsetospeed(&settings, speed_constant(baud));
        if (tcsetattr(fd_, TCSADRAIN, &settings) != 0) throw std::runtime_error(std::strerror(errno));
    }

    /* After everything written so far. tcdrain() returns once the driver has handed the bytes to
     * the CH340, which still holds some to send: a Break then cut the last 50 or so short. */
    void hold_break(int milliseconds) {
        tcdrain(fd_);
        usleep(20000);
        if (ioctl(fd_, TIOCSBRK) != 0) throw std::runtime_error(std::string("cannot send a Break: ") + std::strerror(errno));
        usleep(static_cast<useconds_t>(milliseconds) * 1000);
        ioctl(fd_, TIOCCBRK);
    }

private:
    int fd_ = -1;
};

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

/* A command Break, then the loader's speed. Framing, if on, stays on through a command. */
void start_command(Port& port, int baud) {
    usleep(50000);
    port.hold_break(COMMAND_BREAK_MS);
    usleep(20000);
    port.speed(baud);
    tcflush(port.fd(), TCIOFLUSH);
}

void load_into_ram(Port& port, int baud, std::span<const std::uint8_t> packed) {
    start_command(port, baud);
    const std::uint8_t command[] = {'R'};
    write_all(port.fd(), command);
    write_all(port.fd(), packed);
    tcdrain(port.fd());
    const auto answer = wait_byte(port.fd(), 3000);
    port.speed(PROGRAM_BAUD);
    if (!answer) throw std::runtime_error("the board did not answer the image");
    if (*answer == 'E') throw std::runtime_error("the board found the image corrupt");
    if (*answer != 'K') throw std::runtime_error("the board answered the image with '" + std::string(1, static_cast<char>(*answer)) + "'");
    std::fprintf(stderr, "loaded into Code RAM\n");
}

void write_to_flash(Port& port, int baud, std::span<const std::uint8_t> packed) {
    start_command(port, baud);
    const auto length = static_cast<std::uint32_t>(packed.size());
    const std::uint8_t command[] = {'F', static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                                    static_cast<std::uint8_t>(length >> 16)};
    write_all(port.fd(), command);
    tcdrain(port.fd());
    expect(port.fd(), 'R', 30000, "the erase");
    std::vector<std::uint8_t> padded(packed.begin(), packed.end());
    padded.resize((padded.size() + 255) / 256 * 256, 0xFF);
    for (std::size_t page = 0; page < padded.size(); page += 256) {
        write_all(port.fd(), std::span(padded).subspan(page, 256));
        tcdrain(port.fd());
        expect(port.fd(), '.', 2000, "page " + std::to_string(page / 256));
    }
    port.speed(PROGRAM_BAUD);
    std::fprintf(stderr, "flash written; the board checks it as it boots from it\n");
}

void print_flash_id(Port& port, int baud) {
    start_command(port, baud);
    const std::uint8_t command[] = {'I'};
    write_all(port.fd(), command);
    tcdrain(port.fd());
    std::uint32_t id = 0;
    for (int i = 0; i < 3; ++i) {
        const auto byte = wait_byte(port.fd(), 1000);
        if (!byte) throw std::runtime_error("the board did not send the flash's ID");
        id = id << 8 | *byte;
    }
    port.speed(PROGRAM_BAUD);
    std::printf("%06x\n", id);
}

/* The image's end-of-input field, as the board keeps it: 0 none, else EofMode plus one. */
std::uint8_t eof_field(std::optional<EofMode> eof) {
    return eof ? static_cast<std::uint8_t>(static_cast<std::uint8_t>(*eof) + 1u) : 0;
}

/* A terminal on standard input goes raw for as long as this lives, so that each key goes as it
 * is typed; standard output, if a terminal, shows LF as CR LF. Both come back as they were. */
class Terminal {
public:
    Terminal() {
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_in_) == 0) {
            input_ = true;
            termios raw = saved_in_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
            raw.c_iflag &= ~static_cast<tcflag_t>(IXON | INLCR | IGNCR | ISTRIP);
            raw.c_iflag |= ICRNL;
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        if (isatty(STDOUT_FILENO) && tcgetattr(STDOUT_FILENO, &saved_out_) == 0) {
            output_ = true;
            termios cooked = saved_out_;
            cooked.c_oflag |= OPOST | ONLCR;
            tcsetattr(STDOUT_FILENO, TCSANOW, &cooked);
        }
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    ~Terminal() {
        if (output_) tcsetattr(STDOUT_FILENO, TCSANOW, &saved_out_);
        if (input_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_in_);
    }

    [[nodiscard]] bool input() const noexcept { return input_; }

private:
    termios saved_in_{}, saved_out_{};
    bool input_ = false, output_ = false;
};

struct ConsoleOptions {
    bool keys = true;
    bool echo = true;
    std::string input_file, output_file;
    std::optional<int> timeout_ms;
};

/* The program's side of the line, from the board's framed bytes. */
class Decoder {
public:
    enum class End { None, Halted, Trapped };

    /* Takes a byte, and gives the program's output byte it is, if any. */
    std::optional<std::uint8_t> take(std::uint8_t byte) {
        if (flags_left_) {
            flags_left_ = false;
            flags_ = byte;
            framed_ = true;
            stopped_ = false;
            return std::nullopt;
        }
        if (!escape_) {
            if (byte == ESCAPE) {
                escape_ = true;
                return std::nullopt;
            }
            return byte;
        }
        escape_ = false;
        switch (byte) {
        case ESCAPE: return byte;
        case XOFF: stopped_ = true; break;
        case XON: stopped_ = false; break;
        case 'F': flags_left_ = true; break;
        case 'H': end_ = End::Halted; break;
        case 'T': end_ = End::Trapped; break;
        default: break; // nothing this knows: a newer board
        }
        return std::nullopt;
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] End end() const noexcept { return end_; }
    [[nodiscard]] bool framed() const noexcept { return framed_; }
    [[nodiscard]] std::uint8_t flags() const noexcept { return flags_; }

private:
    bool escape_ = false, flags_left_ = false, stopped_ = false, framed_ = false;
    std::uint8_t flags_ = 0;
    End end_ = End::None;
};

/* Turns framing on, and waits for the board to say so, with its image's flags. Output that comes
 * first is unframed, the program's own. */
std::optional<std::uint8_t> turn_framing_on(Port& port, Decoder& decoder, std::vector<std::uint8_t>& early) {
    usleep(50000); // opening the port may have restarted the program, which turns framing off
    port.hold_break(FRAME_BREAK_MS);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < until) {
        const auto byte = wait_byte(port.fd(), 50);
        if (!byte) continue;
        if (const auto out = decoder.take(*byte)) early.push_back(*out);
        if (decoder.framed()) return decoder.flags();
    }
    return std::nullopt;
}

int console(Port& port, Decoder& decoder, std::uint8_t eof, const ConsoleOptions& options, std::vector<std::uint8_t> early) {
    const bool ends = eof != 0;
    std::ofstream copy;
    if (!options.output_file.empty()) {
        copy.open(options.output_file, std::ios::binary);
        if (!copy.is_open()) throw std::runtime_error("cannot write " + options.output_file);
    }
    const auto show = [&](std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) return;
        write_all(STDOUT_FILENO, bytes);
        if (copy.is_open()) copy.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    show(early);

    std::deque<std::uint8_t> pending;
    if (!options.input_file.empty()) {
        std::ifstream in(options.input_file, std::ios::binary);
        if (!in.is_open()) throw std::runtime_error("cannot open " + options.input_file);
        pending.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    Terminal terminal;
    const bool keys = terminal.input() && options.keys;
    const bool echo = terminal.input() && options.echo;
    bool input_open = true;
    int eofs_wanted = 0; // each after what is pending
    auto last_activity = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> buffer(4096), out;

    const auto finish = [&](int status) {
        if (copy.is_open()) {
            copy.flush();
            if (!copy) throw std::runtime_error("cannot write " + options.output_file);
        }
        return status;
    };

    for (;;) {
        if (stop_signal) return finish(EXIT_SIGNAL);

        if (!decoder.stopped() && !pending.empty()) {
            const auto count = std::min(CHUNK, pending.size());
            const std::vector<std::uint8_t> chunk(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(count));
            write_all(port.fd(), chunk);
            tcdrain(port.fd());
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(count));
            last_activity = std::chrono::steady_clock::now();
        } else if (pending.empty() && eofs_wanted > 0) {
            port.hold_break(EOF_BREAK_MS);
            --eofs_wanted;
            last_activity = std::chrono::steady_clock::now();
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(port.fd(), &read_set);
        if (input_open) FD_SET(STDIN_FILENO, &read_set);
        const bool more_to_send = (!pending.empty() && !decoder.stopped()) || (pending.empty() && eofs_wanted > 0);
        timeval wait{0, more_to_send ? 0 : 100000};
        const int ready = select(std::max(port.fd(), STDIN_FILENO) + 1, &read_set, nullptr, nullptr, &wait);
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("select: ") + std::strerror(errno));
        }

        if (ready > 0 && FD_ISSET(port.fd(), &read_set)) {
            const auto got = read(port.fd(), buffer.data(), buffer.size());
            if (got < 0 && errno != EAGAIN && errno != EINTR) throw std::runtime_error(std::string("the serial port: ") + std::strerror(errno));
            out.clear();
            for (ssize_t i = 0; i < got; ++i) {
                if (const auto byte = decoder.take(buffer[static_cast<std::size_t>(i)])) out.push_back(*byte);
            }
            show(out);
            if (got > 0) last_activity = std::chrono::steady_clock::now();
            if (decoder.end() == Decoder::End::Halted) return finish(EXIT_DONE);
            if (decoder.end() == Decoder::End::Trapped) {
                std::fprintf(stderr, "\nthe program trapped\n");
                return finish(EXIT_TRAPPED);
            }
        }

        if (ready > 0 && input_open && FD_ISSET(STDIN_FILENO, &read_set)) {
            const auto got = read(STDIN_FILENO, buffer.data(), buffer.size());
            if (got < 0 && errno != EAGAIN && errno != EINTR) throw std::runtime_error(std::string("standard input: ") + std::strerror(errno));
            if (got == 0) {
                input_open = false;
                if (ends) ++eofs_wanted;
            }
            out.clear();
            for (ssize_t i = 0; i < got; ++i) {
                const auto byte = buffer[static_cast<std::size_t>(i)];
                if (keys && byte == KEY_EXIT) return finish(EXIT_DONE);
                if (keys && byte == KEY_EOF && ends) {
                    ++eofs_wanted;
                    continue;
                }
                pending.push_back(byte);
                if (echo) out.push_back(byte);
            }
            if (echo) write_all(STDOUT_FILENO, out);
            if (got > 0) last_activity = std::chrono::steady_clock::now();
        }

        if (options.timeout_ms && std::chrono::steady_clock::now() - last_activity > std::chrono::milliseconds(*options.timeout_ms))
            return finish(EXIT_TIMEOUT);
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* name = argv[0];
    std::optional<Mode> mode;
    std::string port_name, source_file, source_code;
    bool have_source = false, stay = true;
    int baud = DEFAULT_LOAD_BAUD;
    int opt_level = OPT_FOLD; // the core does not run OPT_FUSE's fused instructions yet
    std::optional<EofMode> eof;
    ConsoleOptions options;

    int opt;
    while ((opt = ::getopt(argc, argv, "hm:p:b:f:s:O:e:ni:o:t:XL")) != -1) {
        switch (opt) {
        case 'f':
            source_file = optarg;
            have_source = true;
            break;
        case 's':
            source_code = optarg;
            have_source = true;
            break;
        case 'O':
            if (const auto level = parse_opt_level(optarg)) opt_level = *level;
            else usage_error(name, std::string("not an optimization level: ") + optarg);
            break;
        case 'e':
            if (const auto parsed = parse_eof_mode(optarg)) eof = *parsed;
            else usage_error(name, std::string("not an EOF mode (0, 255 or keep): ") + optarg);
            break;
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
        case 'n':
            stay = false;
            break;
        case 'i':
            options.input_file = optarg;
            break;
        case 'o':
            options.output_file = optarg;
            break;
        case 't':
            try {
                options.timeout_ms = std::stoi(optarg);
            } catch (const std::exception&) {
                usage_error(name, std::string("not a time in milliseconds: ") + optarg);
            }
            break;
        case 'X':
            options.keys = false;
            break;
        case 'L':
            options.echo = false;
            break;
        default:
            usage(std::cerr, name);
            return EXIT_USAGE;
        }
    }

    const int programs = argc - optind + (have_source ? 1 : 0);
    if (programs > 1) usage_error(name, "more than one program given");
    if (mode == Mode::Id && programs != 0) usage_error(name, "id takes no program");
    if (mode == Mode::Flash && programs == 0) usage_error(name, "flash needs a program");
    if (programs == 0 && !stay) usage_error(name, "-n is for loading a program");
    if (programs == 0 && eof) usage_error(name, "-e is for a program to load");
    const bool loading = programs == 1;
    if (!mode) mode = Mode::Run;

    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    for (const int number : {SIGINT, SIGTERM, SIGHUP}) sigaction(number, &action, nullptr);

    try {
        std::vector<std::uint8_t> packed;
        if (loading) {
            std::vector<std::uint32_t> words;
            if (have_source) {
                std::string code = source_code;
                if (!source_file.empty()) {
                    std::ifstream in(source_file);
                    if (!in.is_open()) throw std::runtime_error("cannot open " + source_file);
                    code.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                Brainfunk program;
                program.translate(code);
                program.optimize(opt_level);
                words = encode_bincode(program.bitcode());
            } else {
                auto read = read_program_file(argv[optind]);
                words = std::move(read.words);
                if (!eof) eof = read.eof;
            }
            if (words.size() > CODE_RAM_WORDS)
                throw std::runtime_error(std::to_string(words.size()) + " words is more than Code RAM's " + std::to_string(CODE_RAM_WORDS));
            packed = pack_image(words, Compression::LZ, eof);
            std::fprintf(stderr, "%zu words, %zu bytes packed from %zu\n", words.size(), packed.size(), words.size() * 4);
        }

        Port port(find_port(port_name));
        if (*mode == Mode::Id) {
            print_flash_id(port, baud);
            return 0;
        }

        Decoder decoder;
        std::vector<std::uint8_t> early;
        std::uint8_t image_eof = eof_field(eof);
        if (stay) {
            const auto flags = turn_framing_on(port, decoder, early);
            if (!flags) std::fprintf(stderr, "the board did not turn framing on: its bitstream is older than bfconsole, "
                                             "so there is no flow control, and no end of the program to see\n");
            else if (!loading) image_eof = *flags & 3u;
            if (!loading && flags && !(*flags & 0x80u)) std::fprintf(stderr, "the board has no program to run\n");
        }
        if (loading) {
            early.clear();
            if (*mode == Mode::Flash) write_to_flash(port, baud, packed);
            else load_into_ram(port, baud, packed);
            decoder = Decoder{}; // the command emptied the receive FIFO, so the board starts with no stop
        }
        if (!stay) return 0;
        return console(port, decoder, image_eof, options, std::move(early));
    } catch (const std::exception& error) {
        std::cerr << name << ": " << error.what() << '\n';
        return EXIT_ERROR;
    }
}
