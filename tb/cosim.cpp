// Lock-step cosimulation: the RTL core against libbrainfunk, compared at every retire.
//
//   cosim [--stall] [--limit N] image.hex [input-file]   run one program image
//   cosim --random N [--seed S] [--stall]                run N random programs at -O0..-O2
//   cosim --evict N [--seed S] [--stall]                 run N random images built to force evictions
// --chaos makes the branch predictor guess at random, which must not change what a program does.
//
// Exit status is non-zero on the first divergence. --json prints the results as one JSON object.
#include "Vbrainfunk_soc.h"
#include "Vbrainfunk_soc__Dpi.h"
#include "libbrainfunk.hpp"
#include "program_generator.hpp"
#include "svdpi.h"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t TAPE_SIZE = std::size_t{1} << 16;
constexpr std::size_t ROM_SIZE  = std::size_t{1} << 20;
constexpr std::uint32_t SDRAM_TAPE = std::uint32_t{1} << 20; // word address of the tape in SDRAM

struct Divergence {
    std::string what;
};

class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed * 2u + 1u) {}
    bool chance(unsigned one_in) {
        state_ = state_ * 1664525u + 1013904223u;
        return (state_ >> 8) % one_in == 0;
    }

private:
    std::uint32_t state_;
};

struct Result {
    std::uint64_t cycles  = 0;
    std::uint64_t retired = 0;
    bool          halted  = false;
    bool          trapped = false;
    bool          truncated = false;
    std::array<std::uint64_t, 16> cycles_per_op{};
    std::array<std::uint64_t, 16> count_per_op{};
};

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

std::string describe(const std::string& what, std::uint64_t retired, std::uint32_t pc,
                     std::uint32_t instr) {
    return what + " at retire #" + std::to_string(retired) + ", pc " + std::to_string(pc) +
           ", instr " + hex(instr);
}

class Cosim {
public:
    Cosim() : top_(&context_) {
        top_.eval(); // so that scopes exist
        sdram_ = scope("phy");
        tag_   = scope("dcache.tag");
        for (int cell = 0; cell < 32; ++cell) cell_rams_[cell] = scope("dcache.g_byte[" + std::to_string(cell) + "].ram");
        svSetScope(sdram_);
        for (std::size_t word = 0; word < TAPE_SIZE / 4; ++word) sdram_poke(static_cast<int>(SDRAM_TAPE + word), 0);

        top_.por = 1;
        top_.predict_chaos = chaos;
        top_.rst = 1;
        for (int i = 0; i < 4; ++i) tick();
        top_.por = 0;
    }

    bool chaos = false;

    Result run(const std::vector<std::uint32_t>& image, const std::string& input, bool stall,
               std::uint64_t retire_limit, std::uint32_t stall_seed) {
        if (image.size() > ROM_SIZE) throw Divergence{"program image larger than the RTL ROM"};


        Brainfunk iss(TAPE_SIZE);
        iss.load_bincode(image);

        std::istringstream iss_in(input);
        std::ostringstream iss_out;
        std::string rtl_out;
        std::size_t input_pos = 0;
        Rng rng(stall_seed);

        top_.predict_chaos = chaos;
        top_.rst = 1;
        top_.io_in_valid = 0;
        top_.io_in_data = 0;
        top_.io_out_ready = 0;
        for (int i = 0; i < 4; ++i) tick();
        top_.rst = 0;

        // The caches invalidate themselves, and the controller finishes what it was doing.
        while (!top_.core_ready) tick();

        // A previous program leaves its tape behind, and the rest of its image past the end
        // of this one, where speculative fetch may go.
        svSetScope(sdram_);
        for (std::size_t i = 0; i < image.size(); ++i) sdram_poke(static_cast<int>(i), static_cast<int>(image[i]));
        for (const auto cell : written_) sdram_poke(static_cast<int>(SDRAM_TAPE + cell / 4), 0);
        written_.clear();

        Result result;
        std::uint64_t cycles_since_retire = 0;
        const std::uint64_t scan_cycle_limit = 8 * TAPE_SIZE + 64; // cache misses all the way

        for (;;) {
            /* Inputs for this cycle.  End of input reads as a stream of zero bytes. */
            const bool in_ok = !stall || rng.chance(3);
            top_.io_in_valid = in_ok;
            top_.io_in_data = input_pos < input.size()
                                  ? static_cast<std::uint8_t>(input[input_pos]) : std::uint8_t{0};
            top_.io_out_ready = !stall || rng.chance(3);
            top_.clk = 0;
            top_.eval();

            if (top_.trapped) {
                const auto pc = iss.pc();
                bool iss_trapped = false;
                try {
                    (void)iss.step(iss_in, iss_out);
                } catch (const BrainfunkException&) {
                    iss_trapped = true;
                }
                if (!iss_trapped) throw Divergence{"RTL trapped, ISS did not, at pc " + std::to_string(pc)};
                if (iss.pc() != pc) throw Divergence{"ISS moved on a trap"};
                result.trapped = true;
                break;
            }
            if (top_.halted) throw Divergence{"RTL halted without retiring H"};

            if (top_.io_in_ready && top_.io_in_valid && input_pos < input.size()) ++input_pos;
            if (top_.io_out_valid && top_.io_out_ready) {
                rtl_out += static_cast<char>(top_.io_out_data);
            }

            const bool retire = top_.retire_valid;
            const bool wrote = top_.dmem_we_o;
            const std::uint32_t write_addr = top_.dmem_addr_o;
            const std::uint32_t write_data = top_.dmem_wdata_o;
            const std::uint32_t retire_pc = top_.retire_pc;
            const std::uint32_t retire_instr = top_.retire_instr;
            const std::uint32_t retire_next_pc = top_.retire_next_pc;
            const std::uint32_t retire_base_ptr = top_.retire_base_ptr;

            if (wrote && !retire) throw Divergence{"write outside a retiring cycle"};
            if (wrote) written_.push_back(write_addr);

            tick_high();
            ++result.cycles;
            ++cycles_since_retire;

            if (retire) {
                check_retire(iss, iss_in, iss_out, image, result, retire_pc, retire_instr,
                             retire_next_pc, retire_base_ptr, wrote, write_addr, write_data);
                const auto op = retire_instr >> 28;
                result.cycles_per_op[op] += cycles_since_retire;
                ++result.count_per_op[op];
                cycles_since_retire = 0;
                ++result.retired;

                /* str() copies, so only look when output can have changed. */
                if (op == static_cast<unsigned>(Opcode::IO) && iss_out.str() != rtl_out) {
                    throw Divergence{describe("output differs", result.retired, retire_pc, retire_instr)};
                }
                if (op == static_cast<unsigned>(Opcode::H)) {
                    result.halted = true;
                    break;
                }
                if ((retire_limit != 0 && result.retired >= retire_limit) || scan_never_ends(iss)) {
                    result.truncated = true;
                    break;
                }
            } else if (cycles_since_retire > scan_cycle_limit && !top_.io_in_ready && !top_.io_out_valid) {
                throw Divergence{"no retire for " + std::to_string(cycles_since_retire) + " cycles, pc " +
                                 std::to_string(iss.pc())};
            }
        }

        /* The whole architectural state, once more. */
        const auto memory = iss.memory();
        for (std::size_t i = 0; i < TAPE_SIZE; ++i) {
            if (peek_tape(i) != memory[i]) {
                throw Divergence{"final tape differs at cell " + std::to_string(i)};
            }
        }
        return result;
    }

private:
    VerilatedContext context_;
    Vbrainfunk_soc   top_;
    svScope sdram_ = nullptr, tag_ = nullptr;
    std::array<svScope, 32> cell_rams_{};
    std::vector<std::uint32_t> written_;

    static svScope scope(const std::string& path) {
        const auto name = "TOP.brainfunk_soc." + path;
        const svScope found = svGetScopeFromName(name.c_str());
        if (found == nullptr) throw Divergence{"no such scope: " + name};
        return found;
    }

    /* A cell as the program sees it: the data cache first, then SDRAM. */
    std::uint8_t peek_tape(std::size_t cell) {
        const auto index = static_cast<int>((cell >> 5) & 511);
        const auto tag = static_cast<long long>(cell >> 14);
        svSetScope(tag_);
        if (ram_peek(index) == (4 | tag)) { // valid, tag
            svSetScope(cell_rams_[cell & 31]);
            return static_cast<std::uint8_t>(ram_peek(index));
        }
        svSetScope(sdram_);
        const auto word = static_cast<std::uint32_t>(sdram_peek(static_cast<int>(SDRAM_TAPE + cell / 4)));
        return static_cast<std::uint8_t>(word >> (8 * (cell & 3)));
    }

    void tick() {
        top_.clk = 0;
        top_.eval();
        if (top_.dmem_we_o) written_.push_back(top_.dmem_addr_o);
        tick_high();
    }

    void tick_high() {
        top_.clk = 1;
        top_.eval();
    }

    /* The RTL just retired one instruction: the ISS runs it and they must agree. */
    void check_retire(Brainfunk& iss, std::istream& iss_in, std::ostream& iss_out,
                      const std::vector<std::uint32_t>& image, const Result& result,
                      std::uint32_t pc, std::uint32_t instr, std::uint32_t next_pc,
                      std::uint32_t base_ptr, bool wrote, std::uint32_t write_addr,
                      std::uint32_t write_data) {
        const auto fail = [&](const std::string& what) {
            throw Divergence{describe(what, result.retired, pc, instr)};
        };

        if (pc != iss.pc()) fail("pc is " + std::to_string(pc) + ", ISS has " + std::to_string(iss.pc()));
        if (instr != image[pc]) fail("retired word is not the one in the image");

        const auto& instruction = iss.bitcode()[iss.pc()];
        const auto base_before = iss.base_ptr();
        const auto opcode = instruction.opcode();

        bool expect_write = opcode == Opcode::A || opcode == Opcode::S || opcode == Opcode::MUL ||
                            (opcode == Opcode::IO && instruction.data() == IO_IN);
        const auto expected_addr =
            expect_write ? effective_address(base_before, instruction.addr_offset(), TAPE_SIZE) : 0;

        const bool running = iss.step(iss_in, iss_out);
        if (!running && opcode != Opcode::H) fail("ISS stopped");

        if (next_pc != iss.pc()) fail("next pc is " + std::to_string(next_pc) + ", ISS has " + std::to_string(iss.pc()));
        if (base_ptr != iss.base_ptr()) fail("base_ptr is " + std::to_string(base_ptr) + ", ISS has " + std::to_string(iss.base_ptr()));

        if (wrote != expect_write) fail(wrote ? "unexpected write" : "missing write");
        if (expect_write) {
            if (write_addr != expected_addr) fail("write address " + std::to_string(write_addr) + ", ISS has " + std::to_string(expected_addr));
            if (write_data != iss.memory()[expected_addr]) fail("write data " + std::to_string(write_data) + ", ISS has " + std::to_string(iss.memory()[expected_addr]));
        }
    }
};

/* Words for a program that moves between cells a whole number of data cache sizes apart, so that
   lines keep evicting each other, including dirty ones, and reads what it wrote back again. */
std::vector<std::uint32_t> eviction_image(std::uint32_t seed) {
    std::uint32_t state = seed * 2654435761u + 12345u;
    const auto next = [&state](std::uint32_t range) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % range;
    };
    const auto field = [](int value, unsigned bits) { return static_cast<std::uint32_t>(value) & ((std::uint32_t{1} << bits) - 1); };
    const auto cell = [&next] { return static_cast<int>(next(129)) - 64; };

    std::vector<std::uint32_t> words;
    const auto plain = [&](unsigned choice) {
        const auto data = next(256);
        switch (choice) {
        case 0: words.push_back(0x10000000u | field(cell(), 12) << 8 | data); break;
        case 1: words.push_back(0x20000000u | field(cell(), 12) << 8 | data); break;
        case 2: words.push_back(0x30000000u | field(cell(), 8) << 20 | field(cell(), 12) << 8 | data); break;
        case 3: words.push_back(0x80000001u | field(cell(), 12) << 8); break;
        default: words.push_back(0x80000000u | field(cell(), 12) << 8); break;
        }
    };

    const auto length = 200 + next(800);
    for (std::uint32_t i = 0; i < length; ++i) {
        const auto choice = next(16);
        if (choice < 5) {
            // Same index, another tag, and a little way along.
            const int lines = static_cast<int>(next(7)) - 3;
            words.push_back(0x50000000u | field(lines * 16384 + static_cast<int>(next(65)) - 32, 20));
        } else if (choice == 5) {
            // A counted loop, with an eviction inside it.
            const int counter = cell();
            words.push_back(0x20000000u | field(counter, 12) << 8 | (2 + next(3)));
            const auto start = words.size();
            const auto body = 1 + next(6);
            for (std::uint32_t k = 0; k < body; ++k) plain(next(4));
            words.push_back(0x50000000u | 16384u);
            words.push_back(0x50000000u | field(-16384, 20));
            words.push_back(0x10000000u | field(counter, 12) << 8 | 0xFFu);
            const int back = static_cast<int>(start) - static_cast<int>(words.size()) - 1;
            words.push_back(0x70000000u | field(counter, 8) << 20 | field(back, 20));
        } else if (choice == 6) {
            // A scan that crosses far-apart cells.
            words.push_back(0x40000000u | field(cell(), 8) << 20 | field((next(2) ? 16384 : -16384) + static_cast<int>(next(9)) - 4, 20));
        } else {
            plain(next(5));
        }
    }
    words.push_back(0x90000000u);
    return words;
}

std::vector<std::uint32_t> read_image(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) throw Divergence{"cannot open " + filename};
    std::vector<std::uint32_t> words;
    std::string line;
    while (std::getline(file, line)) {
        std::uint32_t word = 0;
        const auto result = std::from_chars(line.data(), line.data() + line.size(), word, 16);
        if (result.ec != std::errc{} || line.size() != 8) throw Divergence{"malformed image line: " + line};
        words.push_back(word);
    }
    return words;
}

/* Same encoding as bit2bin, for programs generated in-process. */
std::vector<std::uint32_t> assemble(const Brainfunk& program) {
    std::vector<std::uint32_t> words;
    for (const auto& instruction : program.bitcode()) {
        std::uint32_t word = static_cast<std::uint32_t>(instruction.opcode()) << 28;
        const auto field = [](offset_t value, unsigned bits) {
            return static_cast<std::uint32_t>(value) & ((std::uint32_t{1} << bits) - 1);
        };
        switch (opcode_layout(instruction.opcode())) {
        case OperandLayout::Data:
            word |= field(instruction.addr_offset(), 12) << 8 | instruction.data();
            break;
        case OperandLayout::Mul:
            word |= field(instruction.src_offset(), 8) << 20 | field(instruction.addr_offset(), 12) << 8 | instruction.data();
            break;
        case OperandLayout::Branch:
            word |= field(instruction.test_offset(), 8) << 20 | field(instruction.jump_offset(), 20);
            break;
        case OperandLayout::Scan:
            word |= field(instruction.test_offset(), 8) << 20 | field(instruction.step(), 20);
            break;
        case OperandLayout::Move:
            word |= field(instruction.move_offset(), 20);
            break;
        default:
            break;
        }
        words.push_back(word);
    }
    return words;
}

void print_json(const Result& result) {
    std::printf("{\"retired\":%llu,\"cycles\":%llu,\"halted\":%s,\"trapped\":%s,\"truncated\":%s,\"ops\":{",
                static_cast<unsigned long long>(result.retired), static_cast<unsigned long long>(result.cycles),
                result.halted ? "true" : "false", result.trapped ? "true" : "false", result.truncated ? "true" : "false");
    const char* separator = "";
    for (std::size_t op = 0; op < result.count_per_op.size(); ++op) {
        if (result.count_per_op[op] == 0) continue;
        const auto name = opcode_name(static_cast<Opcode>(op));
        std::printf("%s\"%.*s\":{\"count\":%llu,\"cycles\":%llu}", separator, static_cast<int>(name.size()), name.data(),
                    static_cast<unsigned long long>(result.count_per_op[op]), static_cast<unsigned long long>(result.cycles_per_op[op]));
        separator = ",";
    }
    std::printf("}}\n");
}

void print_batch(bool json, const char* what, std::uint32_t count, std::uint64_t halted, std::uint64_t truncated,
                 std::uint64_t trapped, std::uint64_t retired) {
    const auto ull = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    if (json) {
        std::printf("{\"programs\":%u,\"halted\":%llu,\"truncated\":%llu,\"trapped\":%llu,\"retired\":%llu}\n", count,
                    ull(halted), ull(truncated), ull(trapped), ull(retired));
    } else {
        std::printf("%u %s: %llu halted, %llu truncated, %llu trapped, %llu instructions compared\n", count, what, ull(halted),
                    ull(truncated), ull(trapped), ull(retired));
    }
}

void print_result(const Result& result) {
    std::printf("retired %llu cycles %llu cpi %.3f%s%s%s\n",
                static_cast<unsigned long long>(result.retired),
                static_cast<unsigned long long>(result.cycles),
                result.retired ? static_cast<double>(result.cycles) / static_cast<double>(result.retired) : 0.0,
                result.halted ? " halted" : "", result.trapped ? " trapped" : "",
                result.truncated ? " truncated" : "");
    for (std::size_t op = 0; op < result.count_per_op.size(); ++op) {
        if (result.count_per_op[op] == 0) continue;
        const auto name = opcode_name(static_cast<Opcode>(op));
        std::printf("  %-4.*s %12llu insts %14llu cycles %8.3f cpi\n", static_cast<int>(name.size()), name.data(),
                    static_cast<unsigned long long>(result.count_per_op[op]),
                    static_cast<unsigned long long>(result.cycles_per_op[op]),
                    static_cast<double>(result.cycles_per_op[op]) / static_cast<double>(result.count_per_op[op]));
    }
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    bool stall = false, json = false, chaos = false;
    std::uint64_t limit = 0;
    std::uint32_t random_count = 0, evict_count = 0, seed = 1;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stall") stall = true;
        else if (arg == "--chaos") chaos = true;
        else if (arg == "--json") json = true;
        else if (arg == "--limit") limit = std::stoull(argv[++i]);
        else if (arg == "--random") random_count = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--evict") evict_count = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--seed") seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        else files.push_back(arg);
    }

    try {
        Cosim cosim;
        cosim.chaos = chaos;
        if (evict_count != 0) {
            std::uint64_t halted = 0, truncated = 0, trapped = 0, retired = 0;
            for (std::uint32_t n = 0; n < evict_count; ++n) {
                try {
                    const auto result = cosim.run(eviction_image(seed + n), "Az\xff\x01?", stall, 60000, seed + n);
                    halted += result.halted;
                    truncated += result.truncated;
                    trapped += result.trapped;
                    retired += result.retired;
                } catch (const Divergence& divergence) {
                    std::cerr << "DIVERGED, eviction image seed " << seed + n << ": " << divergence.what << '\n';
                    return 1;
                }
            }
            print_batch(json, "eviction images", evict_count, halted, truncated, trapped, retired);
            return 0;
        }
        if (random_count != 0) {
            std::uint64_t halted = 0, truncated = 0, trapped = 0, retired = 0;
            constexpr std::string_view input = "Az\xff\x01?";
            for (std::uint32_t n = 0; n < random_count; ++n) {
                const auto program_seed = seed + n;
                ProgramGenerator generator(program_seed);
                const auto code = generator.program();
                for (int level = OPT_NONE; level <= OPT_FOLD; ++level) {
                    Brainfunk program;
                    program.translate(code);
                    program.optimize(level);
                    try {
                        const auto image = assemble(program);

                        /* assemble() is a second encoder; it must agree with the decoder. */
                        Brainfunk decoded;
                        decoded.load_bincode(image);
                        if (!std::equal(program.bitcode().begin(), program.bitcode().end(),
                                        decoded.bitcode().begin(), decoded.bitcode().end(),
                                        [](const Bitcode& a, const Bitcode& b) { return a.to_string() == b.to_string(); })) {
                            throw Divergence{"image does not decode back to the program"};
                        }

                        const auto result = cosim.run(image, std::string(input), stall, 20000, program_seed);
                        halted += result.halted;
                        truncated += result.truncated;
                        trapped += result.trapped;
                        retired += result.retired;
                    } catch (const Divergence& divergence) {
                        std::cerr << "DIVERGED, seed " << program_seed << " -O" << level << ": " << code << '\n'
                                  << divergence.what << '\n';
                        return 1;
                    }
                }
            }
            print_batch(json, "programs at 3 levels", random_count, halted, truncated, trapped, retired);
            return 0;
        }

        if (files.empty()) {
            std::cerr << "usage: cosim [--json] [--stall] [--chaos] [--limit N] image.hex [input-file]\n"
                         "       cosim --random N [--seed S] [--stall]\n"
                         "       cosim --evict N [--seed S] [--stall]\n";
            return 2;
        }
        std::string input;
        if (files.size() > 1) {
            std::ifstream file(files[1], std::ios::binary);
            input.assign(std::istreambuf_iterator<char>(file), {});
        }
        const auto result = cosim.run(read_image(files[0]), input, stall, limit, seed);
        if (json) print_json(result);
        else      print_result(result);
    } catch (const Divergence& divergence) {
        std::cerr << "DIVERGED: " << divergence.what << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
