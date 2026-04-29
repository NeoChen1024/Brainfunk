#include "libbrainfunk.hpp"
#include "ctre.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <optional>
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
    ptr_ = 0;
    try {
        memory_.resize(memsize, memory_t{0});
        bitcode_.reserve(memsize);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
}

Brainfunk::~Brainfunk() = default;   // RAII handles everything

void Brainfunk::clear() {
    std::fill(memory_.begin(), memory_.end(), memory_t{0});
    ptr_ = 0;
    bitcode_.clear();
}

// ------------------------------------------------------------------
//  count_mul_offset  –  accumulates one mul/offset pair
// ------------------------------------------------------------------

namespace {

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

    /* Validate bracket matching */
    if (count_net(code, "[]") != 0) {
        throw BrainfunkException("Unmatched brackets");
    }

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
            assert(!stack.empty());
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
    if (format == Format::C) {
        // clang-format off
        os << "#include <stdio.h>\n"
              "#include <stdlib.h>\n"
              "#include <stdint.h>\n"
              "uint8_t *mem;\n"
              "#define MEMSIZE\t\t(1<<21)\n"
              "#define\tX(x)\t/* NOP */\n"
              "#define\tA(x)\t*mem += x\n"
              "#define\tS(x)\t*mem = x\n"
              "#define\tMUL(offset, mul)\tmem[offset] += *mem * mul\n"
              "#define\tF(x)\twhile(*mem != 0) mem += x\n"
              "#define\tM(x)\tmem += x\n"
              "#define\tJE(x)\twhile(*mem) {\n"
              "#define\tJN(x)\t}\n"
              "#define\tH()\texit(0);\n"
              "static inline void IO(uint8_t arg)\n"
              "{\n"
              "\tint c = 0;\n"
              "\tswitch(arg)\n"
              "\t{\n"
              "\t\tcase 0: /* IN */ c = getchar(); *mem = c == EOF ? 0 : c; break;\n"
              "\t\tcase 1: /* OUT */ putchar(*mem); break;\n"
              "\t}\n"
              "}\n"
              "int main(void)\n"
              "{\n"
              "\tsetvbuf(stdin, NULL, _IONBF, 0);\n"
              "\tsetvbuf(stdout, NULL, _IONBF, 0);\n"
              "\tmem = (uint8_t *)calloc(sizeof(uint8_t), MEMSIZE) + MEMSIZE/2;\n"
              "\tif(!mem) { puts(\"Out of memory\"); exit(1); }\n\n";
        // clang-format on
    }

    for (addr_t pc = 0; pc < bitcode_.size(); ++pc) {
        if (format == Format::BIT)
            os << pc << ":\t" << bitcode_[pc].to_string(BITCODE_FORMAT_PLAIN)
               << '\n';
        else
            os << bitcode_[pc].to_string(BITCODE_FORMAT_C) << '\n';
    }

    if (format == Format::C) {
        os << "}" << std::endl;
    }
}

// ------------------------------------------------------------------
//  run  –  execute the bitcode
// ------------------------------------------------------------------

void Brainfunk::run(std::istream& is, std::ostream& os) {
    if (bitcode_.empty()) return;

    /* Reset memory */
    std::fill(memory_.begin(), memory_.end(), memory_t{0});

    auto mem_span = std::span(memory_);
    addr_t pc = 0;      // program counter

    while (pc < bitcode_.size()) {
        if (!bitcode_[pc].execute(mem_span, pc, ptr_, is, os)) {
            break;   // halt
        }
    }
}

/* ================================================================
 *             Bitcode implementation
 * ================================================================ */

// ------------------------------------------------------------------
//  to_string
// ------------------------------------------------------------------

std::string Bitcode::to_string(int format) const {
    auto idx = static_cast<std::size_t>(opcode_);
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
    if (format == BITCODE_FORMAT_C) {
        return opcode_name(idx) + std::string("(") + operand.str() + std::string(");");
    }
    return opcode_name(idx) + std::string("\t") + operand.str();
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
        memory[wrap_addr(ptr + static_cast<addr_t>(d.offset), size)] +=
            static_cast<memory_t>(memory[ptr] * d.mul);
        break;
    }

    case Opcode::F: {
        auto off = std::get<offset_t>(operand_);
        while (memory[ptr] != 0) {
            ptr = wrap_addr(ptr + static_cast<addr_t>(off), size);
        }
        break;
    }

    case Opcode::M:
        ptr = wrap_addr(ptr + static_cast<addr_t>(std::get<offset_t>(operand_)),
                        size);
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
