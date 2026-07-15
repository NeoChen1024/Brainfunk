#include "libbrainfunk.hpp"
#include "ctre.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

/* ================================================================
 *             Brainfunk implementation
 * ================================================================ */

// ------------------------------------------------------------------
// Construction / destruction
// ------------------------------------------------------------------

Brainfunk::Brainfunk(std::size_t memsize) {
    if (memsize == 0) {
        throw BrainfunkException("Memory size must be greater than zero");
    }

    ptr_ = 0;
    memory_.resize(memsize, memory_t{0});
}

void Brainfunk::clear() {
    reset_state();
    bitcode_.clear();
}

void Brainfunk::reset_state() {
    std::ranges::fill(memory_, memory_t{0});
    ptr_ = 0;
    pc_  = 0;
}

// ------------------------------------------------------------------
//  count_mul_offset  –  accumulates one mul/offset pair
// ------------------------------------------------------------------

namespace {

void validate_brackets(std::string_view code) {
    std::vector<std::size_t> stack;

    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '[') {
            stack.push_back(i);
        } else if (code[i] == ']') {
            if (stack.empty()) {
                throw BrainfunkException("Unmatched closing bracket");
            }
            stack.pop_back();
        }
    }

    if (!stack.empty()) {
        throw BrainfunkException("Unmatched opening bracket");
    }
}

void count_mul_offset(std::string_view text,
                      std::vector<memory_t>& mul,
                      std::vector<offset_t>& offset,
                      offset_t last_offset) {
    auto add = count_net(text, "+-");
    auto mov = count_net(text, "><");
    mul.emplace_back(static_cast<memory_t>(add % 256));
    offset.emplace_back(mov + last_offset);
}

} // anonymous namespace

// ------------------------------------------------------------------
//  translate  –  Brainfuck source → bitcode
// ------------------------------------------------------------------

void Brainfunk::translate(std::string_view code) {
    bitcode_.clear();
    bitcode_.reserve(code.size() + 1);

    /* Validate bracket matching */
    validate_brackets(code);

    std::vector<addr_t> stack;   // jump-address stack
    offset_t last_pc = 0;

    while (!code.empty()) {

        /* ---------- Optimisation patterns ---------- */
        if (code.front() == '[') {

            if (try_parse_mul_offset_loop_(code)) continue;
            if (try_parse_scan_loop_(code))        continue;
            if (try_parse_set_zero_loop_(code))    continue;

            /* Fall through – ordinary loop */
            bitcode_.emplace_back(Opcode::X);               // placeholder
            stack.push_back(bitcode_.size() - 1);
            code.remove_prefix(1);
            continue;
        }

        if (code.front() == ']') {
            if (stack.empty()) {
                throw BrainfunkException("Unmatched closing bracket");
            }
            last_pc = static_cast<offset_t>(stack.back());
            stack.pop_back();

            auto pc = static_cast<offset_t>(bitcode_.size());
            bitcode_.emplace_back(Opcode::JN, last_pc - pc);
            bitcode_[static_cast<std::size_t>(last_pc)] =
                Bitcode(Opcode::JE, pc - last_pc);

            code.remove_prefix(1);
            continue;
        }

        /* ---- simple instructions ---- */
        if (code.front() == '+' || code.front() == '-') {
            auto [net, len] = leading_run(code, "+-");
            bitcode_.emplace_back(Opcode::A,
                                  static_cast<memory_t>(net));
            code.remove_prefix(len);
            continue;
        }

        if (code.front() == '>' || code.front() == '<') {
            auto [net, len] = leading_run(code, "><");
            bitcode_.emplace_back(Opcode::M,
                                  static_cast<offset_t>(net));
            code.remove_prefix(len);
            continue;
        }

        if (code.front() == '.') {
            bitcode_.emplace_back(Opcode::IO, IO_OUT);
            code.remove_prefix(1);
            continue;
        }

        if (code.front() == ',') {
            bitcode_.emplace_back(Opcode::IO, IO_IN);
            code.remove_prefix(1);
            continue;
        }

        /* Skip any unrecognised character */
        code.remove_prefix(1);
    }

    bitcode_.emplace_back(Opcode::H);

    if (!stack.empty()) {
        throw BrainfunkException("Unmatched opening bracket");
    }
}

// ------------------------------------------------------------------
//  try_parse_mul_offset_loop_
//
//  Pattern: [ -? ( [><]+ [+-]+ )+ [><]+ ]   (mode 1: starts with '-')
//           [ ( [><]+ [+-]+ )+ [><]+ - ]     (mode 2: ends   with '-')
//
//  When a match is found we emit one MUL instruction per
//  (offset, mul) pair, followed by an S(0) to clear the cell.
// ------------------------------------------------------------------

bool Brainfunk::try_parse_mul_offset_loop_(std::string_view& code) {

    // clang-format off
    auto m = ctre::starts_with<
        "^\\[(\\-([\\<\\>]+[\\+\\-]+)+[\\<\\>]+|([\\<\\>]+[\\+\\-]+)+[\\<\\>]+\\-)\\]"
    >(code);
    // clang-format on

    if (!m) return false;

    std::string_view body = m.to_view();

    /* Must return to the original cell */
    if (count_net(body, "><") != 0) return false;

    /* Strip brackets */
    int mode;
    if (body[1] == '-') {          // mode 1:  [- ...]
        body.remove_prefix(2);
        mode = 1;
    } else {                       // mode 2:  [ ... -]
        body.remove_prefix(1);
        mode = 2;
    }

    std::vector<memory_t> mul;
    std::vector<offset_t> offset;

    while (auto p = ctre::starts_with<"^[\\>\\<]+[\\+\\-]+">(body)) {
        count_mul_offset(p.to_view(), mul, offset,
                         offset.empty() ? 0 : offset.back());
        body.remove_prefix(p.size());
    }

    /* In mode 2 the last pair is a fake – we omit it */
    auto pairs = (mode == 2) ? offset.size() - 1 : offset.size();

    assert(pairs > 0);
    for (std::size_t i = 0; i < pairs; ++i) {
        bitcode_.emplace_back(Opcode::MUL, mul[i], offset[i]);
    }

    /* Insert S(0) to clear the cell */
    bitcode_.emplace_back(Opcode::S, memory_t{0});

    code.remove_prefix(m.size());
    return true;
}

// ------------------------------------------------------------------
//  try_parse_scan_loop_
//
//  Pattern: [ [><]+ ]   →  F(offset)
// ------------------------------------------------------------------

bool Brainfunk::try_parse_scan_loop_(std::string_view& code) {
    auto m = ctre::starts_with<"^\\[[\\>\\<]+\\]">(code);
    if (!m) return false;

    auto net = count_net(m.to_view(), "><");
    bitcode_.emplace_back(Opcode::F, static_cast<offset_t>(net));
    code.remove_prefix(m.size());
    return true;
}

// ------------------------------------------------------------------
//  try_parse_set_zero_loop_
//
//  Pattern: [ [+-]+ ]   →  S(0)   (but only when the length is odd)
// ------------------------------------------------------------------

bool Brainfunk::try_parse_set_zero_loop_(std::string_view& code) {
    auto m = ctre::starts_with<"^\\[[\\+\\-]+\\]">(code);
    if (!m) return false;

    auto net = count_net(m.to_view(), "+-");
    /* The operation inside is e.g. "+-", net == 0 but length == 2.
     * We need the parity of the *length* of the inner run of +- to be odd.
     * The original code checked net just to be sure, but really it's
     * the length parity that matters.  Keep the original semantics:
     * net must be odd for the net effect to be set-to-zero. */
    if (net % 2 != 1 && net % 2 != -1) return false;

    bitcode_.emplace_back(Opcode::S, memory_t{0});
    code.remove_prefix(m.size());
    return true;
}

// ------------------------------------------------------------------
//  dump  –  prints bitcode as plain text or C source
// ------------------------------------------------------------------

void Brainfunk::dump(std::ostream& os, Format format) const {
    if (format == Format::LLVM_IR) {
        dump_llvm_ir(os);
        return;
    }

    if (format == Format::C) {
        const auto has_opcode = [this](Opcode opcode) {
            return std::ranges::any_of(bitcode_, [opcode](const Bitcode& instruction) {
                return instruction.opcode() == opcode;
            });
        };
        const bool needs_wrap = has_opcode(Opcode::M) || has_opcode(Opcode::MUL) ||
                                has_opcode(Opcode::F);
        const bool needs_io = has_opcode(Opcode::IO);
        const bool needs_memory = std::ranges::any_of(bitcode_, [](const Bitcode& instruction) {
            return instruction.opcode() != Opcode::X && instruction.opcode() != Opcode::H;
        });

        // clang-format off
        os << "#include <stddef.h>\n"
              "#include <stdint.h>\n"
              "#include <stdio.h>\n\n"
              "#define MEMSIZE " << memory_.size() << "u\n";
        if (needs_memory) {
            os << "static uint8_t memory[MEMSIZE];\n"
                  "static size_t ptr;\n";
        }
        os << '\n';
        if (needs_wrap) {
            os << "static size_t wrap_offset(size_t address, int64_t offset)\n"
              "{\n"
              "\tif (offset >= 0)\n"
              "\t\treturn (address + (uint64_t)offset % MEMSIZE) % MEMSIZE;\n"
              "\tconst uint64_t backward = (uint64_t)(-(offset + 1)) + 1;\n"
              "\treturn (address + MEMSIZE - backward % MEMSIZE) % MEMSIZE;\n"
              "}\n\n";
        }
        if (needs_io) {
            os << "static void brainfunk_io(uint8_t arg)\n"
              "{\n"
              "\tif (arg == 0) {\n"
              "\t\tconst int input = getchar();\n"
              "\t\tmemory[ptr] = input == EOF ? 0 : (uint8_t)input;\n"
              "\t} else {\n"
              "\t\tputchar(memory[ptr]);\n"
              "\t\tfflush(stdout);\n"
              "\t}\n"
              "}\n\n";
        }
        os << "#define X()                 ((void)0)\n"
              "#define A(value)             (memory[ptr] += (uint8_t)(value))\n"
              "#define S(value)             (memory[ptr] = (uint8_t)(value))\n"
              "#define MUL(offset, factor)  (memory[wrap_offset(ptr, (offset))] += (uint8_t)(memory[ptr] * (factor)))\n"
              "#define F(offset)            while (memory[ptr] != 0) ptr = wrap_offset(ptr, (offset))\n"
              "#define M(offset)            (ptr = wrap_offset(ptr, (offset)))\n"
              "#define JE(offset)           while (memory[ptr] != 0) {\n"
              "#define JN(offset)           }\n"
              "#define IO(operation)        brainfunk_io(operation)\n"
              "#define H()                  return 0\n\n"
              "int main(void)\n"
              "{\n";
        // clang-format on
    }

    for (addr_t pc = 0; pc < bitcode_.size(); ++pc) {
        if (format == Format::BIT)
            os << pc << ":\t" << bitcode_[pc].to_string(BitcodeFormat::Plain)
               << '\n';
        else
            os << '\t' << bitcode_[pc].to_string(BitcodeFormat::C) << '\n';
    }

    if (format == Format::C) {
        os << "}\n";
    }
}

// ------------------------------------------------------------------
//  run  –  execute the bitcode
// ------------------------------------------------------------------

void Brainfunk::run(std::istream& is, std::ostream& os) {
    reset_state();
    while (step(is, os)) {
        /* nothing — step() handles everything */
    }
}

// ------------------------------------------------------------------
//  step  –  execute exactly one bitcode instruction
// ------------------------------------------------------------------

bool Brainfunk::step(std::istream& is, std::ostream& os) {
    if (bitcode_.empty() || pc_ >= bitcode_.size()) return false;

    auto mem_span = std::span(memory_);
    return bitcode_[pc_].execute(mem_span, pc_, ptr_, is, os);
}

/* ================================================================
 *             Bitcode implementation
 * ================================================================ */

// ------------------------------------------------------------------
//  to_string
// ------------------------------------------------------------------

std::string Bitcode::to_string(BitcodeFormat format) const {
    std::ostringstream operand;

    /* Build the operand string */
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, memory_t>) {
            operand << static_cast<unsigned>(arg);
        } else if constexpr (std::is_same_v<T, offset_t>) {
            operand << arg;
        } else if constexpr (std::is_same_v<T, DualOperand>) {
            operand << arg.offset << ", " << static_cast<unsigned>(arg.mul);
        } else {
            operand << "";   // std::monostate – no operand
        }
    }, operand_);

    /* Format output */
    if (format == BitcodeFormat::C) {
        return std::string(opcode_name(opcode_)) + "(" + operand.str() + ");";
    }
    return std::string(opcode_name(opcode_)) + "\t" + operand.str();
}

// ------------------------------------------------------------------
//  execute  –  runs one instruction, returns false on halt
// ------------------------------------------------------------------

bool Bitcode::execute(std::span<memory_t> memory,
                      addr_t& pc,
                      addr_t& ptr,
                      std::istream& is,
                      std::ostream& os) const {

    auto size = memory.size();

    switch (opcode_) {

    case Opcode::X:
        throw BrainfunkException("Empty instruction");

    case Opcode::A:
        memory[ptr] += std::get<memory_t>(operand_);
        break;

    case Opcode::S:
        memory[ptr] = std::get<memory_t>(operand_);
        break;

    case Opcode::MUL: {
        auto d = std::get<DualOperand>(operand_);
        memory[wrap_offset(ptr, d.offset, size)] +=
            static_cast<memory_t>(memory[ptr] * d.mul);
        break;
    }

    case Opcode::F: {
        auto off = std::get<offset_t>(operand_);
        while (memory[ptr] != 0) {
            ptr = wrap_offset(ptr, off, size);
        }
        break;
    }

    case Opcode::M:
        ptr = wrap_offset(ptr, std::get<offset_t>(operand_), size);
        break;

    case Opcode::JE:
        if (memory[ptr] == 0) {
            pc += static_cast<addr_t>(std::get<offset_t>(operand_));
        }
        break;

    case Opcode::JN:
        if (memory[ptr] != 0) {
            pc += static_cast<addr_t>(std::get<offset_t>(operand_));
        }
        break;

    case Opcode::IO: {
        auto which = std::get<memory_t>(operand_);
        if (which == IO_IN) {
            memory_t io_input = 0;
            is >> std::noskipws >> io_input;
            if (is.eof()) io_input = 0;
            memory[ptr] = io_input;
        } else {
            os << static_cast<char>(memory[ptr]) << std::flush;
        }
        break;
    }

    case Opcode::H:
        return false;   // halt

    default:
        throw BrainfunkException("Invalid opcode");
    }

    ++pc;
    return true;
}
