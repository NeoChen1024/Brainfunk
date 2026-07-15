/* ==================================== *
 * Brainfunk Visual TUI (ncurses)        *
 * Modern C++20 Refactored               *
 * Neo_Chen                              *
 * ==================================== */

#include "libbrainfunk.hpp"

#include <ncurses.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <locale>
#include <limits>
#include <streambuf>
#include <string>
#include <string_view>
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
enum ColourPair : std::uint8_t {
    CP_MSG   = 1,
    CP_MEM   = 2,
    CP_CODE  = 3,
    CP_REG   = 4,
    CP_IO    = 5,
    CP_HELP  = 6,
    CP_HIGHLIGHT = 7,   // current cell / current instruction highlight
};

/* ---------- UI strings ---------- */
static constexpr std::array HELP_TEXT = {
    " SPACE / p  Pause",
    " s          Step",
    " q / ESC    Quit",
    " UP   / +   Faster",
    " DOWN / -   Slower",
    "",
    " Delay:",
};

/* ================================================================
 *  ncurses_streambuf  –  redirects I/O operations to ncurses
 *                         windows instead of stdin/stdout.
 * ================================================================ */

class NcursesStreambuf : public std::streambuf {
public:
    NcursesStreambuf(WINDOW*& io_win, WINDOW*& msg_win,
                     std::deque<unsigned char>& pending_input, bool& resize_requested)
        : io_win_(&io_win), msg_win_(&msg_win), pending_input_(&pending_input),
          resize_requested_(&resize_requested) {}

    NcursesStreambuf(WINDOW*& io_win, std::deque<unsigned char>& output_history)
        : io_win_(&io_win), output_history_(&output_history) {}

protected:
    /* ---- output (program prints '.' etc. to IO window) ---- */
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::eof();
        append_output_(static_cast<unsigned char>(ch));
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) {
            append_output_(static_cast<unsigned char>(s[i]));
        }
        return n;
    }

    /* ---- input (program reads ',' to IO window) ---- */
    int_type underflow() override {
        if (gptr() != nullptr && gptr() < egptr())
            return traits_type::to_int_type(*gptr());

        const auto input = do_input_();
        if (traits_type::eq_int_type(input, traits_type::eof()))
            return traits_type::eof();

        input_char_ = traits_type::to_char_type(input);
        setg(&input_char_, &input_char_, &input_char_ + 1);
        return traits_type::to_int_type(input_char_);
    }

private:
    WINDOW** io_win_;
    WINDOW** msg_win_ = nullptr;
    std::deque<unsigned char>* pending_input_ = nullptr;
    std::deque<unsigned char>* output_history_ = nullptr;
    bool* resize_requested_ = nullptr;
    char input_char_ = 0;

    void append_output_(unsigned char ch) {
        waddch(*io_win_, ch);
        if (output_history_ != nullptr) {
            output_history_->push_back(ch);
            constexpr std::size_t max_history = std::size_t{64} * 1024;
            if (output_history_->size() > max_history)
                output_history_->pop_front();
        }
    }

    int_type do_input_() {
        if (pending_input_ != nullptr && !pending_input_->empty()) {
            const auto ch = pending_input_->front();
            pending_input_->pop_front();
            return traits_type::to_int_type(static_cast<char>(ch));
        }

        if (msg_win_ != nullptr && *msg_win_ != nullptr) {
            wattron(*msg_win_, A_BOLD);
            mvwprintw(*msg_win_, 0, 0, " PROGRAM INPUT — press a key ");
            wattroff(*msg_win_, A_BOLD);
            wnoutrefresh(*msg_win_);
            doupdate();
        }

        /* Program input is intentionally blocking; controls are suspended. */
        wtimeout(*io_win_, -1);
        int ch = wgetch(*io_win_);
        while (ch > 0xff) {
            if (ch == KEY_RESIZE && resize_requested_ != nullptr)
                *resize_requested_ = true;
            ch = wgetch(*io_win_);
        }
        wtimeout(*io_win_, 0);

        if (msg_win_ != nullptr && *msg_win_ != nullptr) {
            werase(*msg_win_);
        }

        if (ch == ERR) {
            return traits_type::eof();
        }

        if (ch < 0 || ch > 0xff)
            return traits_type::eof();
        return traits_type::to_int_type(static_cast<char>(
            static_cast<unsigned char>(ch)));
    }
};

/* ================================================================
 *  TUI class
 * ================================================================ */

class VisualBrainfunk {
public:
    explicit VisualBrainfunk(std::chrono::milliseconds delay = std::chrono::milliseconds{100})
        : delay_(delay), bf_(MEMSIZE) {}

    ~VisualBrainfunk() {
        destroy_windows_();
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
            if (is_brainfuck_instruction(c))
                code += c;
        }
        bf_.translate(code);
        cache_code_();
    }

    void load_code(const std::string& code_str) {
        bf_.translate(code_str);
        cache_code_();
    }

    /* ---- main event loop ---- */
    void run() {
        init_ncurses_();
        bf_.reset_state();

        bool halted = false;
        bool paused = false;
        bool step_requested = false;

        /* Create custom I/O streams */
        NcursesStreambuf  ncurses_buf(io_win_, msg_win_, pending_program_input_,
                                      resize_requested_);
        NcursesStreambuf  ncurses_out(io_win_, output_history_);
        std::ostream      ncurses_os(&ncurses_out);
        std::istream      ncurses_is(&ncurses_buf);

        print_mem_();
        print_code_();
        print_reg_(paused);
        print_help_();
        refresh_all_();

        auto next_step = std::chrono::steady_clock::now();

        while (!halted && !quit_requested_) {
            const auto now = std::chrono::steady_clock::now();
            int timeout_ms = -1;
            if (!paused) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                    next_step - now);
                const auto bounded = std::clamp<std::int64_t>(
                    remaining.count(), 0, std::numeric_limits<int>::max());
                timeout_ms = static_cast<int>(bounded);
            }

            handle_input_(paused, step_requested, timeout_ms);
            if (quit_requested_) break;

            /* ---------- execute if not paused ---------- */
            const auto after_input = std::chrono::steady_clock::now();
            if (!halted && (step_requested || (!paused && after_input >= next_step))) {
                halted = !bf_.step(ncurses_is, ncurses_os);
                step_requested = false;
                next_step = std::chrono::steady_clock::now() + delay_;
                if (resize_requested_) {
                    resize_term(0, 0);
                    rebuild_windows_();
                    resize_requested_ = false;
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

            wattron(msg_win_, pair_attr_(CP_MSG) | A_BOLD);
            mvwprintw(msg_win_, 0, 0, " HALTED — press any key to quit ");
            wattroff(msg_win_, pair_attr_(CP_MSG) | A_BOLD);
            wrefresh(msg_win_);
            wgetch(msg_win_);
        }
    }

    [[nodiscard]] int delay_ms() const { return static_cast<int>(delay_.count()); }

private:
    std::chrono::milliseconds delay_;
    Brainfunk  bf_;
    bool       ncurses_inited_ = false;
    bool       quit_requested_ = false;
    bool       colours_enabled_ = false;
    bool       resize_requested_ = false;
    std::deque<unsigned char> pending_program_input_;
    std::deque<unsigned char> output_history_;
    std::vector<std::string> code_cache_;

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
        cbreak();
        noecho();
        curs_set(0);   // hide cursor
        keypad(stdscr, TRUE);

        colours_enabled_ = has_colors();
        if (colours_enabled_) {
            start_color();
            init_pair(CP_MSG,   COLOR_BLACK,  COLOR_WHITE);
            init_pair(CP_MEM,   COLOR_WHITE,  COLOR_BLACK);
            init_pair(CP_CODE,  COLOR_BLACK,  COLOR_YELLOW);
            init_pair(CP_REG,   COLOR_BLACK,  COLOR_GREEN);
            init_pair(CP_IO,    COLOR_WHITE,  COLOR_BLUE);
            init_pair(CP_HELP,  COLOR_BLACK,  COLOR_CYAN);
            init_pair(CP_HIGHLIGHT, COLOR_YELLOW, COLOR_RED);
        }

        rebuild_windows_();

        refresh_all_();

        /* Start message */
        wattron(msg_win_, pair_attr_(CP_MSG) | A_BOLD);
        mvwprintw(msg_win_, 0, 0, " READY — press any key to start ");
        wattroff(msg_win_, pair_attr_(CP_MSG) | A_BOLD);
        wrefresh(msg_win_);
        wgetch(msg_win_);
        werase(msg_win_);
        wrefresh(msg_win_);
    }

    [[nodiscard]] chtype pair_attr_(ColourPair pair) const noexcept {
        return colours_enabled_ ? COLOR_PAIR(pair) : A_NORMAL;
    }

    [[nodiscard]] chtype highlight_attr_() const noexcept {
        return colours_enabled_ ? COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD
                                : A_REVERSE | A_BOLD;
    }

    void destroy_windows_() noexcept {
        for (auto** window : {&mem_win_, &code_win_, &reg_win_,
                              &io_win_, &help_win_, &msg_win_}) {
            if (*window != nullptr) {
                delwin(*window);
                *window = nullptr;
            }
        }
    }

    void rebuild_windows_() {
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);

        while (rows < 18 || columns < 60) {
            erase();
            mvprintw(0, 0, "Terminal too small: need at least 60x18 (current %dx%d)",
                     columns, rows);
            mvprintw(1, 0, "Resize the terminal, or press q to quit.");
            refresh();
            const int ch = getch();
            if (ch == 'q' || ch == 'Q' || ch == 27)
                throw BrainfunkException("Terminal is too small for the visualizer");
            resize_term(0, 0);
            getmaxyx(stdscr, rows, columns);
        }

        destroy_windows_();
        const int mem_height = 4;
        const int message_height = 1;
        const int available_height = rows - mem_height - message_height;
        const int code_height = std::max(7, available_height / 3);
        const int io_height = available_height - code_height;
        const int right_width = std::max(20, columns / 4);
        const int left_width = columns - right_width;

        mem_win_  = newwin(mem_height, columns, 0, 0);
        code_win_ = newwin(code_height, left_width, mem_height, 0);
        reg_win_  = newwin(code_height, right_width, mem_height, left_width);
        io_win_   = newwin(io_height, left_width, mem_height + code_height, 0);
        help_win_ = newwin(io_height, right_width, mem_height + code_height, left_width);
        msg_win_  = newwin(message_height, columns, rows - message_height, 0);

        if (mem_win_ == nullptr || code_win_ == nullptr || reg_win_ == nullptr ||
            io_win_ == nullptr || help_win_ == nullptr || msg_win_ == nullptr)
            throw BrainfunkException("Unable to create ncurses windows");

        wbkgd(mem_win_,  pair_attr_(CP_MEM));
        wbkgd(code_win_, pair_attr_(CP_CODE));
        wbkgd(reg_win_,  pair_attr_(CP_REG));
        wbkgd(io_win_,   pair_attr_(CP_IO));
        wbkgd(help_win_, pair_attr_(CP_HELP));
        wbkgd(msg_win_,  pair_attr_(CP_MSG));
        scrollok(io_win_, TRUE);
        keypad(io_win_, TRUE);
        wtimeout(io_win_, 0);
        for (const auto ch : output_history_)
            waddch(io_win_, ch);
    }

    void cache_code_() {
        code_cache_.clear();
        code_cache_.reserve(bf_.bitcode().size());
        for (const auto& instruction : bf_.bitcode())
            code_cache_.push_back(instruction.to_string(BitcodeFormat::Plain));
    }

    void refresh_all_() {
        for (auto* w : {mem_win_, code_win_, reg_win_, io_win_, help_win_, msg_win_}) {
            if (w) wnoutrefresh(w);
        }
        doupdate();
    }

    /* ---------- keyboard handler ---------- */
    void handle_input_(bool& paused, bool& step_requested, int timeout_ms) {
        wtimeout(io_win_, timeout_ms);
        int ch = wgetch(io_win_);
        wtimeout(io_win_, 0);
        while (ch != ERR) {
            switch (ch) {
            case ' ':
            case 'p':
            case 'P':
                paused = !paused;
                break;

            case 's':
            case 'S':
                paused = true;
                step_requested = true;
                break;

            case KEY_UP:
            case '+':
            case '=':
                delay_ = std::max(std::chrono::milliseconds{0},
                                  delay_ - std::chrono::milliseconds{10});
                break;

            case KEY_DOWN:
            case '-':
            case '_':
                delay_ += std::chrono::milliseconds{10};
                break;

            case KEY_RESIZE:
                resize_term(0, 0);
                rebuild_windows_();
                break;

            case 'q':
            case 'Q':
            case 27:   // ESC
                quit_requested_ = true;
                return;

            default:
                if (ch >= 0 && ch <= 0xff)
                    pending_program_input_.push_back(static_cast<unsigned char>(ch));
                break;
            }
            ch = wgetch(io_win_);
        }
    }

    /* ---------- MEM window ---------- */
    void print_mem_() {
        werase(mem_win_);
        const auto mem = bf_.memory();
        const auto memsize = mem.size();
        const auto ptr = bf_.ptr();
        int cells = std::max(1, getmaxx(mem_win_) / 6);
        if (cells % 2 == 0) --cells;
        const int half = cells / 2;

        for (int offset = -half; offset <= half; ++offset) {
            const auto addr = wrap_offset(ptr, offset, memsize);
            if (offset == 0) {
                wattron(mem_win_, highlight_attr_());
                wprintw(mem_win_, " %02x |",
                        static_cast<unsigned>(mem[addr]));
                wattroff(mem_win_, highlight_attr_());
            } else {
                wprintw(mem_win_, " %02x |",
                        static_cast<unsigned>(mem[addr]));
            }
        }
        mvwprintw(mem_win_, getmaxy(mem_win_) - 1, 0, " Memory  (ptr = %zu)", ptr);
    }

    /* ---------- CODE window ---------- */
    void print_code_() {
        werase(code_win_);
        const auto bc = bf_.bitcode();
        const auto pc = bf_.pc();

        if (bc.empty()) {
            mvwprintw(code_win_, 0, 0, " (empty)");
            return;
        }

        const auto lines = static_cast<std::size_t>(getmaxy(code_win_));
        const auto half = lines / 2;
        auto start = pc > half ? pc - half : addr_t{0};
        if (bc.size() > lines && start + lines > bc.size())
            start = bc.size() - lines;

        for (std::size_t i = 0; i < lines; ++i) {
            const auto idx = start + i;
            if (idx >= bc.size()) {
                wprintw(code_win_, "\n");
                continue;
            }
            const auto& str = code_cache_[idx];
            if (idx == pc) {
                wattron(code_win_, highlight_attr_());
                wprintw(code_win_, "%zu:\t%s\n", idx, str.c_str());
                wattroff(code_win_, highlight_attr_());
            } else {
                wprintw(code_win_, "%zu:\t%s\n", idx, str.c_str());
            }
        }
    }

    /* ---------- REG window ---------- */
    void print_reg_(bool paused) {
        werase(reg_win_);
        mvwprintw(reg_win_, 0, 0, " Registers");
        mvwprintw(reg_win_, 2, 0, " PC  = %zu", bf_.pc());
        mvwprintw(reg_win_, 3, 0, " PTR = %zu", bf_.ptr());

        const auto mem = bf_.memory();
        auto ptr = bf_.ptr();
        if (ptr < mem.size()) {
            mvwprintw(reg_win_, 4, 0, " *PTR= 0x%02x (%d)",
                      static_cast<unsigned>(mem[ptr]),
                      static_cast<int>(mem[ptr]));
        }
        mvwprintw(reg_win_, std::min(6, getmaxy(reg_win_) - 1), 0, " [%s]",
                  paused ? "PAUSED" : "RUNNING");
    }

    /* ---------- HELP window ---------- */
    void print_help_() {
        werase(help_win_);
        mvwprintw(help_win_, 0, 0, " Controls");

        int row = 2;
        for (std::string_view text : HELP_TEXT) {
            if (row >= getmaxy(help_win_)) break;
            if (text.empty()) {
                /* Blank line for separator */
                ++row;
                continue;
            }
            if (text == " Delay:") {
                mvwprintw(help_win_, row, 0, "%.*s %d ms",
                          static_cast<int>(text.size()), text.data(), delay_ms());
            } else {
                mvwprintw(help_win_, row, 0, "%.*s",
                          static_cast<int>(text.size()), text.data());
            }
            ++row;
        }
    }
};

/* ================================================================
 *  main
 * ================================================================ */

[[noreturn]] static void helpmsg(char** argv, int status = 0) {
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
    std::exit(status);
}

int main(int argc, char** argv) {
    std::string filename;
    std::string code_str;
    auto delay = std::chrono::milliseconds{100};
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
        {
            std::int64_t milliseconds = 0;
            const std::string_view argument{optarg};
            const auto result = std::from_chars(argument.data(),
                                                argument.data() + argument.size(),
                                                milliseconds);
            if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() ||
                milliseconds < 0 || milliseconds > std::numeric_limits<int>::max()) {
                std::cerr << "Invalid delay: " << argument << '\n';
                return 1;
            }
            delay = std::chrono::milliseconds{milliseconds};
            break;
        }
        case 'h':
            helpmsg(argv);
            break;
        default:
            std::exit(1);
        }
    }

    if (!valid) {
        helpmsg(argv, 1);
    }

    try {
        VisualBrainfunk vbf(delay);

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
