/* Random Brainfuck programs for differential testing. */
#pragma once

#include "libbrainfunk.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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

/* True if the instruction at pc is an F that never finds a zero.  F runs to
 * completion in one step, so a step limit alone cannot stop it.  A scan repeats
 * after at most one trip around the tape. */
[[nodiscard]] inline bool scan_never_ends(const Brainfunk& brainfunk) {
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

/* The next instruction is a JE or JN to itself that is taken: it runs for ever. */
[[nodiscard]] inline bool branches_to_itself(const Brainfunk& brainfunk) {
    const auto& instruction = brainfunk.bitcode()[brainfunk.pc()];
    if ((instruction.opcode() != Opcode::JE && instruction.opcode() != Opcode::JN) || instruction.jump_offset() != -1) {
        return false;
    }
    const auto memory = brainfunk.memory();
    const bool zero = memory[effective_address(brainfunk.base_ptr(), instruction.test_offset(), memory.size())] == 0;
    return zero == (instruction.opcode() == Opcode::JE);
}
