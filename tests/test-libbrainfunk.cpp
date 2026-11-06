#include "libbrainfunk.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string run(std::string_view code, std::string_view input = {},
                std::size_t memory_size = MEMSIZE) {
    Brainfunk brainfunk(memory_size);
    brainfunk.translate(code);
    std::istringstream in{std::string(input)};
    std::ostringstream out;
    brainfunk.run(in, out);
    return out.str();
}

std::string dump(std::string_view code, Format format = Format::BIT, int level = OPT_NONE) {
    Brainfunk brainfunk;
    brainfunk.translate(code);
    brainfunk.optimize(level);
    std::ostringstream out;
    brainfunk.dump(out, format);
    return out.str();
}

/* Everything an optimization level must preserve. */
struct Outcome {
    bool halted = false;
    std::string output;
    std::vector<memory_t> memory;
    addr_t base_ptr = 0;
    addr_t steps = 0;

    bool operator==(const Outcome& other) const {
        return halted == other.halted && output == other.output && memory == other.memory &&
               base_ptr == other.base_ptr;
    }
};

bool scan_never_ends(const Brainfunk& brainfunk) {
    const auto& instruction = brainfunk.bitcode()[brainfunk.pc()];
    if (instruction.opcode() != Opcode::F) return false;

    const auto memory = brainfunk.memory();
    auto base_ptr = brainfunk.base_ptr();
    for (std::size_t i = 0; i <= memory.size(); ++i) {
        if (memory[effective_address(base_ptr, instruction.test_offset(), memory.size())] == 0) {
            return false;
        }
        base_ptr = effective_address(base_ptr, instruction.step(), memory.size());
    }
    return true;
}

Outcome execute(std::string_view code, int level, std::size_t memory_size, std::string_view input,
                addr_t step_limit) {
    Brainfunk brainfunk(memory_size);
    brainfunk.translate(code);
    brainfunk.optimize(level);

    std::istringstream in{std::string(input)};
    std::ostringstream out;
    Outcome outcome;
    while (outcome.steps < step_limit) {
        /* F runs to completion inside one step, so the limit alone cannot stop it. */
        if (scan_never_ends(brainfunk)) break;
        ++outcome.steps;
        if (!brainfunk.step(in, out)) {
            outcome.halted = true;
            break;
        }
    }
    outcome.output = out.str();
    const auto memory = brainfunk.memory();
    outcome.memory.assign(memory.begin(), memory.end());
    outcome.base_ptr = brainfunk.base_ptr();
    return outcome;
}

ExecutionStats count(std::string_view code, int level, std::size_t memory_size,
                     std::string_view input) {
    Brainfunk brainfunk(memory_size);
    brainfunk.translate(code);
    brainfunk.optimize(level);
    std::istringstream in{std::string(input)};
    std::ostringstream out;
    return brainfunk.run_counted(in, out);
}

/* Deterministic, so that a failure reproduces. */
class ProgramGenerator {
public:
    explicit ProgramGenerator(std::uint32_t seed) : state_(seed) {}

    std::string program() {
        std::string code;
        block(code, 0);
        return code;
    }

private:
    std::uint32_t state_;

    std::uint32_t next(std::uint32_t bound) {
        state_ = state_ * 1664525u + 1013904223u;
        return (state_ >> 8) % bound;
    }

    void block(std::string& code, int depth) {
        const auto pieces = 1 + next(8);
        for (std::uint32_t piece = 0; piece < pieces; ++piece) {
            switch (next(depth < 3 ? 11 : 9)) {
            case 0: case 1: code.append(1 + next(4), '+'); break;
            case 2:         code.append(1 + next(3), '-'); break;
            case 3: case 4: code.append(1 + next(4), '>'); break;
            case 5:         code.append(1 + next(4), '<'); break;
            case 6:         code += '.'; break;
            case 7:         code += ','; break;
            case 8: {
                /* Shapes the translator lowers to S, F and MUL. */
                constexpr std::array shapes = {"[-]", "[>]", "[<]", "[>>]", "[->+<]", "[>++<-]",
                                               "[-<+>>++<]", "[->+<+>-<]", "[->>+<<]", "[+]",
                                               "[-]++", ">[-]+++<", "[.]", ",[-]"};
                code += shapes[next(shapes.size())];
                break;
            }
            default:
                code += '[';
                block(code, depth + 1);
                code += ']';
                break;
            }
        }
    }
};

} // namespace

int main() {
    check(run("++++++++[>++++++++<-]>+.+.+.") == "ABC",
          "optimized execution produces expected output");
    check(run("<+.", {}, 16) == std::string(1, '\x01'),
          "data pointer wraps backwards");
    check(run(",.") == std::string(1, '\0'),
          "EOF input stores zero");

    check(dump("[-]").find("S\t0") != std::string::npos,
          "odd decrement loop becomes set-zero");
    check(dump("[>]").find("F\t0, 1") != std::string::npos,
          "scan loop becomes find-zero");
    check(dump("[->++<]").find("MUL\t1, 0, 2") != std::string::npos,
          "multiply-offset loop is optimized");

    check(dump("[->+<+>-<]").find("MUL") == std::string::npos,
          "loop that writes its own induction cell is not a multiply");
    check(run("++++[->++<->+++<]>.") == std::string(1, '\x0a'),
          "loop that writes its own induction cell keeps its iteration count");
    {
        std::string far_loop = "[-";
        far_loop.append(3000, '>');
        far_loop += '+';
        far_loop.append(3000, '<');
        far_loop += ']';
        check(dump(far_loop).find("MUL") == std::string::npos,
              "multiply offset beyond 12 bits stays an ordinary loop");
    }

    const auto llvm_ir = dump(",.[>]", Format::LLVM_IR);
    check(llvm_ir.find("define i32 @brainfunk_program") != std::string::npos,
          "LLVM mode emits the portable runtime entry point");
    check(llvm_ir.find("\ntarget triple =") == std::string::npos &&
              llvm_ir.find("\ntarget datalayout =") == std::string::npos,
          "portable LLVM IR omits target metadata");
    check(llvm_ir.find("call i32 %read_byte") != std::string::npos &&
              llvm_ir.find("call void %write_byte") != std::string::npos,
          "LLVM IR uses runtime I/O callbacks");

    {
        bool rejected_size = false;
        try {
            Brainfunk brainfunk(10);
        } catch (const BrainfunkException&) {
            rejected_size = true;
        }
        check(rejected_size, "memory size that is not a power of two is rejected");
    }

    bool rejected = false;
    try {
        Brainfunk brainfunk;
        brainfunk.translate("[+");
    } catch (const BrainfunkException&) {
        rejected = true;
    }
    check(rejected, "unmatched bracket is rejected");

    /* ---- statistics ---- */
    {
        const auto stats = count("++[->+<]>.", OPT_NONE, MEMSIZE, {});
        const auto dynamic = [&](Opcode opcode) {
            return stats.dynamic_count[static_cast<std::size_t>(opcode)];
        };
        check(dynamic(Opcode::A) == 1, "stats count A");
        check(dynamic(Opcode::MUL) == 1, "stats count MUL");
        check(dynamic(Opcode::IO) == 1, "stats count IO");
        check(stats.dynamic_total() == stats.static_total(), "straight-line code runs once");
        /* A: 1R1W, MUL: 2R1W, S: 1W, IO out: 1R */
        check(stats.memory_reads == 4, "stats count memory reads");
        check(stats.memory_writes == 3, "stats count memory writes");

        const auto scan = count("+>+>+<<[>]", OPT_NONE, MEMSIZE, {});
        check(scan.scan_iterations == 3, "stats count scan iterations");
    }

    /* ---- lazy move ---- */
    check(dump("+.", Format::BIT, OPT_LAZY_MOVE).find("\tM\t") == std::string::npos,
          "a program without pointer motion emits no M");
    {
        const auto listing = dump(">>+<<.", Format::BIT, OPT_LAZY_MOVE);
        check(listing.find("A\t2, 1") != std::string::npos,
              "pending motion becomes addr_offset");
        check(listing.find("\tM\t") == std::string::npos, "balanced motion emits no M");
    }
    check(dump(">>[->+<]", Format::BIT, OPT_LAZY_MOVE).find("MUL\t3, 2, 1") != std::string::npos,
          "pending motion reaches both MUL operands");
    {
        const auto listing = dump(">>[>+<-]<<", Format::BIT, OPT_LAZY_MOVE);
        check(listing.find("\tM\t") == std::string::npos, "multiply loop needs no flush");
    }
    {
        const auto listing = dump(">>[.>+<-.]<<", Format::BIT, OPT_LAZY_MOVE);
        check(listing.find("JE\t2,") != std::string::npos &&
                  listing.find("JN\t2,") != std::string::npos,
              "branches test through test_offset");
        check(listing.find("\tM\t") == std::string::npos, "balanced loop needs no M");
    }
    {
        const auto listing = dump(">>[.>]", Format::BIT, OPT_LAZY_MOVE);
        check(listing.find("M\t1") != std::string::npos,
              "loop with net motion keeps M on its back edge");
    }
    check(dump(">>[>]", Format::BIT, OPT_LAZY_MOVE).find("F\t2, 1") != std::string::npos,
          "scan tests through test_offset");

    for (const auto* code : {"+[>+]", ">>[.>]<<.", "+>+[[-]>]", ">,[>,]<[.<]"}) {
        Brainfunk once;
        once.translate(code);
        once.optimize(OPT_LAZY_MOVE);
        std::ostringstream first;
        once.dump(first);
        once.optimize(OPT_LAZY_MOVE);
        std::ostringstream second;
        once.dump(second);
        check(first.str() == second.str(), "lazy move is idempotent");
    }

    /* ---- fold ---- */
    const auto folded = [](std::string_view code) { return dump(code, Format::BIT, OPT_FOLD); };
    const auto contains = [](const std::string& text, std::string_view part) {
        return text.find(part) != std::string::npos;
    };
    {
        const auto listing = folded(",+>+<+.");
        check(contains(listing, "A\t0, 2") && contains(listing, "A\t1, 1"),
              "adds to one cell merge across other cells");
    }
    check(contains(folded(",[-]+++."), "S\t0, 3"), "set then add becomes one set");
    check(!contains(folded(",[-]++[-]."), "S\t0, 2"), "overwritten set is dropped");
    check(!contains(folded(",[-]+,."), "S\t0, 1"), "write overwritten by input is dropped");
    {
        const auto listing = folded(",>[-]++[->+++<]");
        check(!contains(listing, "MUL"), "multiply from a known cell folds away");
        check(contains(listing, "A\t2, 6"), "folded multiply adds the product");
    }
    check(!contains(folded(",[.][.]").substr(folded(",[.][.]").find("JN")), "JE"),
          "loop right after an identical test never runs");
    check(!contains(folded(",[>][-]"), "S\t"), "cell is known zero after a scan");
    check(!contains(folded(",[>][>]").substr(folded(",[>][>]").find("F\t")+1), "F\t"),
          "scan from a known-zero cell is dropped");
    {
        const auto listing = folded(",[.][-]+.");
        check(contains(listing, "S\t0, 1"), "cell is known zero after a loop");
        check(!contains(listing, "S\t0, 0"), "known-zero cell needs no clear");
    }
    {
        /* Offsets 0 and 16 name one cell on a 16-cell tape. */
        std::string aliased = "[-]+";
        aliased.append(16, '>');
        aliased += "+.";
        aliased.append(16, '<');
        aliased += '.';
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            check(execute(aliased, level, 16, {}, 100).output == std::string(2, '\x02'),
                  "offsets that alias on a small tape are one cell");
        }
        std::string aliased_set = "+";
        aliased_set.append(16, '>');
        aliased_set += "[-]";
        aliased_set.append(16, '<');
        aliased_set += "++.";
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            check(execute(aliased_set, level, 16, {}, 100).output == std::string(1, '\x02'),
                  "set through an alias overrides the other offset");
        }
        std::string aliased_mul = ",[-]+";
        aliased_mul.append(16, '>');
        aliased_mul += "++";
        aliased_mul.append(16, '<');
        aliased_mul += "[->+<]>.";
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            check(execute(aliased_mul, level, 16, "x", 100).output == std::string(1, '\x03'),
                  "known value is tracked through an alias");
        }
    }

    {
        const auto agrees = [](std::string_view code, std::string_view in, std::string_view want) {
            for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
                const auto outcome = execute(code, level, MEMSIZE, in, 100000);
                if (!outcome.halted || outcome.output != want) return false;
            }
            return true;
        };
        check(agrees(",>[-]+++<[->++<]>.", "\x05", "\x0d"),
              "multiply adds to a pending set of its destination");
        check(agrees(",>[-]<[->+<]>[-].", "\x05", std::string_view("\0", 1)),
              "multiply invalidates what is known about its destination");

        /* Offset overflow is the only way a known value survives an M. */
        std::string far = std::string(3000, '>') + "+" + std::string(3000, '<') + "[-]" +
                          std::string(3000, '>') + "[.-]";
        check(agrees(far, {}, "\x01"), "known values follow base_ptr across M");
    }

    /* ---- differential: every level against the unoptimized program ---- */
    constexpr addr_t step_limit = 20000;
    constexpr std::string_view input = "Az\xff\x01?";
    int halted_programs = 0;
    for (std::uint32_t seed = 1; seed <= 8000; ++seed) {
        ProgramGenerator generator(seed);
        const auto code = generator.program();
        const std::size_t memory_size = seed % 2 == 0 ? std::size_t{16} : MEMSIZE;
        const auto reference = execute(code, OPT_NONE, memory_size, input, step_limit);
        if (!reference.halted) continue;   // cannot compare a truncated run
        ++halted_programs;

        /* Lazy moves only remove M: everything else runs exactly as often. */
        const auto before = count(code, OPT_NONE, memory_size, input);
        const auto after = count(code, OPT_LAZY_MOVE, memory_size, input);
        for (std::size_t i = 0; i < before.dynamic_count.size(); ++i) {
            if (static_cast<Opcode>(i) == Opcode::M) {
                check(after.dynamic_count[i] <= before.dynamic_count[i], "lazy move adds no M");
            } else {
                check(after.dynamic_count[i] == before.dynamic_count[i],
                      "lazy move only changes how often M runs");
            }
        }
        check(after.memory_reads == before.memory_reads &&
                  after.memory_writes == before.memory_writes &&
                  after.scan_iterations == before.scan_iterations,
              "lazy move leaves memory traffic unchanged");

        for (int level = OPT_NONE + 1; level <= OPT_MAX; ++level) {
            /* Optimized code never takes more steps than the original. */
            const auto optimized = execute(code, level, memory_size, input, step_limit);
            if (!(optimized == reference)) {
                std::cerr << "FAIL: level " << level << " differs on seed " << seed << ": " << code
                          << '\n';
                ++failures;
            }
            check(optimized.steps <= reference.steps, "optimized code takes no extra steps");
        }
    }
    check(halted_programs > 1000, "differential test exercised enough programs");

    return failures == 0 ? 0 : 1;
}
