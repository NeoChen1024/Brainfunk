/* ==================================== *
 * Brainfunk Visual TUI (ncurses)        *
 * Modern C++20 Refactored               *
 * Neo_Chen                              *
 * ==================================== */

#include "libbrainfunk.hpp"

#include <ncurses.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <locale>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

/* ================================================================
 *  ncurses window layout (same spirit as legacy visualbrainfunk.c)
 *
 *   0             59        79
 *  +--------------+---------+  0
 *  |     MEM      |         |    (WHITE on BLACK)
 *  +--------------+---------+  4
 *  |              |         |
 *  |    CODE      |   REG   |    (YELLOW BG) : (GREEN BG)
 *  |              |         |
 *  +--------------+---------+ 10
 *  |              |         |
 *  |     IO       |  HELP   |    (BLUE BG)  : (CYAN BG)
 *  |              |         |
 *  +--------------+---------+ 23
 * ================================================================ */

/* ---------- colour pair IDs ---------- */
enum {
    CP_MSG   = 1,
    CP_MEM   = 2,
    CP_CODE  = 3,
    CP_REG   = 4,
    CP_IO    = 5,
    CP_HELP  = 6,
    CP_HIGHLIGHT = 7,   // current cell / current instruction highlight
};

/* ---------- UI strings ---------- */
static constexpr const char* HELP_TEXT[] = {
    " SPACE / p  Pause",
    " s          Step",
    " q / ESC    Quit",
    " UP   / +   Faster",
    " DOWN / -   Slower",
    "",
    " Delay:",
    nullptr,
};

/* ================================================================
 *  ncurses_streambuf  –  redirects I/O operations to ncurses
 *                         windows instead of stdin/stdout.
 * ================================================================ */

class NcursesStreambuf : public std::streambuf {
public:
    explicit NcursesStreambuf(WINDOW* io_win, WINDOW* msg_win = nullptr)
        : io_win_(io_win), msg_win_(msg_win) {}

protected:
    /* ---- output (program prints '.' etc. to IO window) ---- */
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::eof();
        waddch(io_win_, static_cast<char>(ch));
        wrefresh(io_win_);
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) {
            waddch(io_win_, s[i]);
        }
        wrefresh(io_win_);
        return n;
    }

    /* ---- input (program reads ',' to IO window) ---- */
    int_type underflow() override {
        if (has_pending_input_) {
            return pending_char_;
        }
        return do_input_();
    }

    int_type uflow() override {
        if (has_pending_input_) {
            has_pending_input_ = false;
            return pending_char_;
        }
        return do_input_();
    }

private:
    WINDOW* io_win_;
    WINDOW* msg_win_;
    bool    has_pending_input_ = false;
    int     pending_char_      = 0;

    int_type do_input_() {
        if (msg_win_) {
            wattron(msg_win_, COLOR_PAIR(CP_MSG) | A_BOLD);
            mvwprintw(msg_win_, 0, 0, " Input (press a key)... ");
            wattroff(msg_win_, COLOR_PAIR(CP_MSG) | A_BOLD);
            wrefresh(msg_win_);
        }

        /* Block until a character is received */
        int ch = wgetch(io_win_);

        if (msg_win_) {
            werase(msg_win_);
            wrefresh(msg_win_);
        }

        if (ch == ERR) {
            return traits_type::eof();
        }

        return static_cast<char>(ch);
    }
};

/* ================================================================
 *  TUI class
 * ================================================================ */

class VisualBrainfunk {
public:
    VisualBrainfunk(int delay_ms = 100)
        : delay_us_(delay_ms * 1000), bf_(MEMSIZE) {}

    ~VisualBrainfunk() {
        if (ncurses_inited_) endwin();
    }

    /* Load Brainfuck source from a file or string */
    void load_file(const std::string& path) {
        std::ifstream input(path);
        if (!input.is_open()) {
            std::perror(path.c_str());
            std::exit(1);
        }
        std::string code;
        char c;
        while (input.get(c)) {
            switch (c) {
            case '+': case '-': case '>': case '<':
            case '[': case ']': case '.': case ',':
                code += c;
                break;
            default: break;
            }
        }
        bf_.translate(code);
    }

    void load_code(const std::string& code_str) {
        bf_.translate(code_str);
    }

    /* ---- main event loop ---- */
    void run() {
        init_ncurses_();
        bf_.reset_state();

        bool halted = false;
        bool paused = false;
        bool step_requested = false;

        /* Create custom I/O streams */
        NcursesStreambuf  ncurses_buf(io_win_, msg_win_);
        NcursesStreambuf  ncurses_out(io_win_);  // output only, no msg window
        std::ostream      ncurses_os(&ncurses_out);
        std::istream      ncurses_is(&ncurses_buf);

        while (!halted && !quit_requested_) {
            /* ---------- handle keyboard ---------- */
            handle_input_(paused, step_requested);

            /* ---------- execute if not paused ---------- */
            if (!halted && (!paused || step_requested)) {
                halted = !bf_.step(ncurses_is, ncurses_os);
                step_requested = false;

                if (!halted && !paused) {
                    /* Sleep for the configured delay */
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(delay_us_));
                }
            }

            /* ---------- refresh display ---------- */
            print_mem_();
            print_code_();
            print_reg_(paused);
            print_help_();
            refresh_all_();
        }

        /* Halt message */
        if (halted) {
            print_mem_();
            print_code_();
            print_reg_(paused);
            print_help_();
            refresh_all_();

            wattron(msg_win_, COLOR_PAIR(CP_MSG) | A_BOLD);
            mvwprintw(msg_win_, 0, 0, " HALTED — press any key to quit ");
            wattroff(msg_win_, COLOR_PAIR(CP_MSG) | A_BOLD);
            wrefresh(msg_win_);
            wgetch(msg_win_);
        }
    }

    int delay_ms() const { return static_cast<int>(delay_us_ / 1000); }

private:
    int        delay_us_;           // microseconds per step
    Brainfunk  bf_;
    bool       ncurses_inited_ = false;
    bool       quit_requested_ = false;

    /* ncurses windows */
    WINDOW* mem_win_  = nullptr;
    WINDOW* code_win_ = nullptr;
    WINDOW* reg_win_  = nullptr;
    WINDOW* io_win_   = nullptr;
    WINDOW* help_win_ = nullptr;
    WINDOW* msg_win_  = nullptr;   // 1-line message bar at bottom

    /* ---------- ncurses initialisation ---------- */
    void init_ncurses_() {
        setlocale(LC_ALL, "");
        initscr();
        ncurses_inited_ = true;

        if (!has_colors()) {
            endwin();
            ncurses_inited_ = false;
            std::cerr << "Terminal does not support colours.\n";
            std::exit(1);
        }

        start_color();
        cbreak();
        noecho();
        curs_set(0);   // hide cursor
        keypad(stdscr, TRUE);

        /* Colour pairs */
        init_pair(CP_MSG,   COLOR_BLACK,  COLOR_WHITE);
        init_pair(CP_MEM,   COLOR_WHITE,  COLOR_BLACK);
        init_pair(CP_CODE,  COLOR_BLACK,  COLOR_YELLOW);
        init_pair(CP_REG,   COLOR_BLACK,  COLOR_GREEN);
        init_pair(CP_IO,    COLOR_WHITE,  COLOR_BLUE);
        init_pair(CP_HELP,  COLOR_BLACK,  COLOR_CYAN);
        init_pair(CP_HIGHLIGHT, COLOR_YELLOW, COLOR_RED);

        /* Window layout — fixed 24 rows × 80 columns */
        mem_win_  = newwin(4,  80, 0,  0);
        code_win_ = newwin(7,  60, 4,  0);
        reg_win_  = newwin(7,  20, 4,  60);
        io_win_   = newwin(12, 60, 11, 0);
        help_win_ = newwin(12, 20, 11, 60);
        msg_win_  = newwin(1,  80, 23, 0);

        /* Background colours */
        wbkgd(mem_win_,  COLOR_PAIR(CP_MEM));
        wbkgd(code_win_, COLOR_PAIR(CP_CODE));
        wbkgd(reg_win_,  COLOR_PAIR(CP_REG));
        wbkgd(io_win_,   COLOR_PAIR(CP_IO));
        wbkgd(help_win_, COLOR_PAIR(CP_HELP));
        wbkgd(msg_win_,  COLOR_PAIR(CP_MSG));

        scrollok(io_win_, TRUE);
        keypad(io_win_, TRUE);

        /* Non-blocking input: poll every 50ms */
        wtimeout(io_win_, 50);

        refresh_all_();

        /* Start message */
        wattron(msg_win_, COLOR_PAIR(CP_MSG) | A_BOLD);
        mvwprintw(msg_win_, 0, 0, " READY — press any key to start ");
        wattroff(msg_win_, COLOR_PAIR(CP_MSG) | A_BOLD);
        wrefresh(msg_win_);
        wgetch(msg_win_);
        werase(msg_win_);
        wrefresh(msg_win_);
    }

    void refresh_all_() {
        for (auto* w : {mem_win_, code_win_, reg_win_, io_win_, help_win_, msg_win_}) {
            if (w) wrefresh(w);
        }
    }

    /* ---------- keyboard handler ---------- */
    void handle_input_(bool& paused, bool& step_requested) {
        int ch = wgetch(io_win_);
        while (ch != ERR) {
            switch (ch) {
            case ' ':
            case 'p':
            case 'P':
                paused = !paused;
                break;

            case 's':
            case 'S':
                step_requested = true;
                break;

            case KEY_UP:
            case '+':
            case '=':
                delay_us_ = std::max(0, delay_us_ - 10000);  // -10ms
                break;

            case KEY_DOWN:
            case '-':
            case '_':
                delay_us_ += 10000;  // +10ms
                break;

            case 'q':
            case 'Q':
            case 27:   // ESC
                quit_requested_ = true;
                return;

            default:
                break;
            }
            ch = wgetch(io_win_);
        }
    }

    /* ---------- MEM window ---------- */
    void print_mem_() {
        werase(mem_win_);
        const auto& mem = bf_.memory();
        auto memsize = mem.size();
        auto ptr = bf_.ptr();

        /* Show 6 cells before and 5 after ptr (fits 80-col window) */
        for (int offset = -6; offset <= 5; ++offset) {
            auto addr = static_cast<long>(ptr) + offset;
            if (addr < 0 || static_cast<std::size_t>(addr) >= memsize) {
                wprintw(mem_win_, "    |");
            } else {
                if (offset == 0) {
                    wattron(mem_win_, COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
                    wprintw(mem_win_, " %02x |",
                            static_cast<unsigned>(mem[static_cast<std::size_t>(addr)]));
                    wattroff(mem_win_, COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
                } else {
                    wprintw(mem_win_, " %02x |",
                            static_cast<unsigned>(mem[static_cast<std::size_t>(addr)]));
                }
            }
        }
        mvwprintw(mem_win_, 3, 0, " Memory  (ptr = %zu)", ptr);
    }

    /* ---------- CODE window ---------- */
    void print_code_() {
        werase(code_win_);
        const auto& bc = bf_.bitcode();
        auto pc = bf_.pc();

        if (bc.empty()) {
            mvwprintw(code_win_, 0, 0, " (empty)");
            return;
        }

        /* Show a window of ~7 instructions around pc */
        int lines = 7;
        int half  = lines / 2;
        long start = static_cast<long>(pc) - half;
        if (start < 0) start = 0;
        if (start + lines > static_cast<long>(bc.size()))
            start = static_cast<long>(bc.size()) - lines;
        if (start < 0) start = 0;

        for (int i = 0; i < lines; ++i) {
            long idx = start + i;
            if (idx < 0 || static_cast<std::size_t>(idx) >= bc.size()) {
                wprintw(code_win_, "\n");
                continue;
            }
            auto str = bc[static_cast<std::size_t>(idx)].to_string(BITCODE_FORMAT_PLAIN);
            if (static_cast<addr_t>(idx) == pc) {
                wattron(code_win_, COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
                wprintw(code_win_, "%zu:\t%s\n", static_cast<std::size_t>(idx), str.c_str());
                wattroff(code_win_, COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
            } else {
                wprintw(code_win_, "%zu:\t%s\n", static_cast<std::size_t>(idx), str.c_str());
            }
        }
    }

    /* ---------- REG window ---------- */
    void print_reg_(bool paused) {
        werase(reg_win_);
        mvwprintw(reg_win_, 0, 0, " Registers");
        mvwprintw(reg_win_, 2, 0, " PC  = %zu", bf_.pc());
        mvwprintw(reg_win_, 3, 0, " PTR = %zu", bf_.ptr());

        const auto& mem = bf_.memory();
        auto ptr = bf_.ptr();
        if (ptr < mem.size()) {
            mvwprintw(reg_win_, 4, 0, " *PTR= 0x%02x (%d)",
                      static_cast<unsigned>(mem[ptr]),
                      static_cast<int>(mem[ptr]));
        }
        mvwprintw(reg_win_, 6, 0, " [%s]",
                  paused ? "PAUSED" : "RUNNING");
    }

    /* ---------- HELP window ---------- */
    void print_help_() {
        werase(help_win_);
        mvwprintw(help_win_, 0, 0, " Controls");

        int row = 2;
        for (int i = 0; HELP_TEXT[i] != nullptr; ++i, ++row) {
            if (HELP_TEXT[i][0] == '\0') {
                /* Blank line for separator */
                continue;
            }
            if (std::strncmp(HELP_TEXT[i], " Delay:", 7) == 0) {
                mvwprintw(help_win_, row, 0, "%s %d ms",
                          HELP_TEXT[i], delay_ms());
            } else {
                mvwprintw(help_win_, row, 0, "%s", HELP_TEXT[i]);
            }
        }
    }
};

/* ================================================================
 *  main
 * ================================================================ */

[[noreturn]] static void helpmsg(int argc, char** argv) {
    std::cerr
        << "Usage: " << argv[0]
        << " [-h] [-f file] [-s code] [-t msec]\n"
        << "  -f file    Read Brainfuck source from file\n"
        << "  -s code    Inline Brainfuck code\n"
        << "  -t msec    Initial delay per step in milliseconds (default 100)\n"
        << "  -h         Show this help\n"
        << "\n"
        << "TUI Controls:\n"
        << "  SPACE / p  Pause / Resume execution\n"
        << "  s          Single-step (execute one instruction)\n"
        << "  q / ESC    Quit\n"
        << "  UP / +     Decrease delay (faster)\n"
        << "  DOWN / -   Increase delay (slower)\n";
    std::exit(0);
}

int main(int argc, char** argv) {
    std::string filename;
    std::string code_str;
    int delay_ms = 100;
    bool valid = false;

    int opt;
    while ((opt = ::getopt(argc, argv, "hf:s:t:")) != -1) {
        switch (opt) {
        case 'f':
            filename = optarg;
            valid = true;
            break;
        case 's':
            code_str = optarg;
            valid = true;
            break;
        case 't':
            delay_ms = std::atoi(optarg);
            if (delay_ms < 0) delay_ms = 0;
            break;
        case 'h':
            helpmsg(argc, argv);
            break;
        default:
            std::exit(1);
        }
    }

    if (!valid) {
        helpmsg(argc, argv);
    }

    try {
        VisualBrainfunk vbf(delay_ms);

        if (!filename.empty()) {
            vbf.load_file(filename);
        } else {
            vbf.load_code(code_str);
        }

        vbf.run();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}