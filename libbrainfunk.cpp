#include "libbrainfunk.hpp"
#include "ctre.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <iostream>
#include <numeric>
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
    if (!std::has_single_bit(memsize)) {
        throw BrainfunkException("Memory size must be a power of two");
    }

    base_ptr_ = 0;
    memory_.resize(memsize, memory_t{0});
}

void Brainfunk::clear() {
    reset_state();
    bitcode_.clear();
}

void Brainfunk::reset_state() {
    std::ranges::fill(memory_, memory_t{0});
    base_ptr_ = 0;
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
            bitcode_.push_back(Bitcode::none(Opcode::X));   // placeholder
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
            bitcode_.push_back(Bitcode::branch(Opcode::JN, 0, last_pc - pc));
            bitcode_[static_cast<std::size_t>(last_pc)] =
                Bitcode::branch(Opcode::JE, 0, pc - last_pc);

            code.remove_prefix(1);
            continue;
        }

        /* ---- simple instructions ---- */
        if (code.front() == '+' || code.front() == '-') {
            auto [net, len] = leading_run(code, "+-");
            bitcode_.push_back(Bitcode::data_op(Opcode::A, 0, static_cast<memory_t>(net)));
            code.remove_prefix(len);
            continue;
        }

        if (code.front() == '>' || code.front() == '<') {
            auto [net, len] = leading_run(code, "><");
            bitcode_.push_back(Bitcode::move(static_cast<offset_t>(net)));
            code.remove_prefix(len);
            continue;
        }

        if (code.front() == '.') {
            bitcode_.push_back(Bitcode::data_op(Opcode::IO, 0, IO_OUT));
            code.remove_prefix(1);
            continue;
        }

        if (code.front() == ',') {
            bitcode_.push_back(Bitcode::data_op(Opcode::IO, 0, IO_IN));
            code.remove_prefix(1);
            continue;
        }

        /* Skip any unrecognised character */
        code.remove_prefix(1);
    }

    bitcode_.push_back(Bitcode::none(Opcode::H));

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

    /* A pair that writes the induction cell itself changes how many times the
     * loop runs, so it cannot be a multiply.  An offset that does not fit the
     * instruction's 12-bit field must stay an ordinary loop as well. */
    for (std::size_t i = 0; i < pairs; ++i) {
        if (offset[i] == 0 || !fits_signed(offset[i], 12)) return false;
    }

    for (std::size_t i = 0; i < pairs; ++i) {
        bitcode_.push_back(Bitcode::mul(offset[i], 0, mul[i]));
    }

    /* Insert S(0) to clear the cell */
    bitcode_.push_back(Bitcode::data_op(Opcode::S, 0, memory_t{0}));

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
    bitcode_.push_back(Bitcode::scan(0, static_cast<offset_t>(net)));
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

    bitcode_.push_back(Bitcode::data_op(Opcode::S, 0, memory_t{0}));
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
                  "static size_t base_ptr;\n";
        }
        os << '\n';
        if (needs_io) {
            os << "static void brainfunk_io(size_t address, uint8_t arg)\n"
              "{\n"
              "\tif (arg == 0) {\n"
              "\t\tconst int input = getchar();\n"
              "\t\tmemory[address] = input == EOF ? 0 : (uint8_t)input;\n"
              "\t} else {\n"
              "\t\tputchar(memory[address]);\n"
              "\t\tfflush(stdout);\n"
              "\t}\n"
              "}\n\n";
        }
        os << "#define EA(offset) ((base_ptr + (size_t)(int64_t)(offset)) & (MEMSIZE - 1u))\n\n"
              "#define X()                                ((void)0)\n"
              "#define A(addr_offset, data)               (memory[EA(addr_offset)] += (uint8_t)(data))\n"
              "#define S(addr_offset, data)               (memory[EA(addr_offset)] = (uint8_t)(data))\n"
              "#define MUL(addr_offset, src_offset, factor) \\\n"
              "\t(memory[EA(addr_offset)] += (uint8_t)(memory[EA(src_offset)] * (factor)))\n"
              "#define F(test_offset, step)               while (memory[EA(test_offset)] != 0) base_ptr = EA(step)\n"
              "#define M(move_offset)                     (base_ptr = EA(move_offset))\n"
              "#define JE(test_offset, jump_offset)       while (memory[EA(test_offset)] != 0) {\n"
              "#define JN(test_offset, jump_offset)       }\n"
              "#define IO(addr_offset, operation)         brainfunk_io(EA(addr_offset), (operation))\n"
              "#define H()                                return 0\n\n"
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
//  run_counted  -  run() with instruction and memory access counts
// ------------------------------------------------------------------

std::uint64_t ExecutionStats::static_total() const noexcept {
    return std::accumulate(static_count.begin(), static_count.end(), std::uint64_t{0});
}

std::uint64_t ExecutionStats::dynamic_total() const noexcept {
    return std::accumulate(dynamic_count.begin(), dynamic_count.end(), std::uint64_t{0});
}

ExecutionStats Brainfunk::run_counted(std::istream& is, std::ostream& os) {
    reset_state();

    ExecutionStats stats;
    for (const auto& instruction : bitcode_) {
        ++stats.static_count[static_cast<std::size_t>(instruction.opcode())];
    }

    while (pc_ < bitcode_.size()) {
        const auto& instruction = bitcode_[pc_];
        const auto opcode = instruction.opcode();
        ++stats.dynamic_count[static_cast<std::size_t>(opcode)];

        switch (opcode) {
        case Opcode::A:
            ++stats.memory_reads;
            ++stats.memory_writes;
            break;
        case Opcode::S:
            ++stats.memory_writes;
            break;
        case Opcode::MUL:
            stats.memory_reads += 2;
            ++stats.memory_writes;
            break;
        case Opcode::F: {
            /* One test per iteration, plus the one that ends the scan. */
            auto base_ptr = base_ptr_;
            const auto size = memory_.size();
            ++stats.memory_reads;
            while (memory_[effective_address(base_ptr, instruction.test_offset(), size)] != 0) {
                base_ptr = effective_address(base_ptr, instruction.step(), size);
                ++stats.scan_iterations;
                ++stats.memory_reads;
            }
            break;
        }
        case Opcode::JE:
        case Opcode::JN:
            ++stats.memory_reads;
            break;
        case Opcode::IO:
            if (instruction.data() == IO_IN) {
                ++stats.memory_writes;
            } else {
                ++stats.memory_reads;
            }
            break;
        case Opcode::X:
        case Opcode::M:
        case Opcode::H:
        case Opcode::Count:
            break;
        }

        if (!step(is, os)) break;
    }

    return stats;
}

// ------------------------------------------------------------------
//  step  –  execute exactly one bitcode instruction
// ------------------------------------------------------------------

bool Brainfunk::step(std::istream& is, std::ostream& os) {
    if (bitcode_.empty() || pc_ >= bitcode_.size()) return false;

    auto mem_span = std::span(memory_);
    return bitcode_[pc_].execute(mem_span, pc_, base_ptr_, is, os);
}

/* ================================================================
 *             Bitcode implementation
 * ================================================================ */

// ------------------------------------------------------------------
//  to_string
// ------------------------------------------------------------------

std::string Bitcode::to_string(BitcodeFormat format) const {
    std::ostringstream operand;

    switch (opcode_layout(opcode_)) {
    case OperandLayout::None:
        break;
    case OperandLayout::Data:
        operand << addr_offset() << ", " << static_cast<unsigned>(data());
        break;
    case OperandLayout::Mul:
        operand << addr_offset() << ", " << src_offset() << ", "
                << static_cast<unsigned>(data());
        break;
    case OperandLayout::Branch:
        operand << test_offset() << ", " << jump_offset();
        break;
    case OperandLayout::Scan:
        operand << test_offset() << ", " << step();
        break;
    case OperandLayout::Move:
        operand << move_offset();
        break;
    }

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
                      addr_t& base_ptr,
                      std::istream& is,
                      std::ostream& os) const {

    const auto size = memory.size();
    const auto ea = [&](offset_t offset) {
        return effective_address(base_ptr, offset, size);
    };

    switch (opcode_) {

    case Opcode::X:
        throw BrainfunkException("Empty instruction");

    case Opcode::A:
        memory[ea(addr_offset())] += data();
        break;

    case Opcode::S:
        memory[ea(addr_offset())] = data();
        break;

    case Opcode::MUL:
        memory[ea(addr_offset())] +=
            static_cast<memory_t>(memory[ea(src_offset())] * data());
        break;

    case Opcode::F:
        while (memory[ea(test_offset())] != 0) {
            base_ptr = ea(step());
        }
        break;

    case Opcode::M:
        base_ptr = ea(move_offset());
        break;

    case Opcode::JE:
        if (memory[ea(test_offset())] == 0) {
            pc += static_cast<addr_t>(jump_offset());
        }
        break;

    case Opcode::JN:
        if (memory[ea(test_offset())] != 0) {
            pc += static_cast<addr_t>(jump_offset());
        }
        break;

    case Opcode::IO:
        if (data() == IO_IN) {
            memory_t io_input = 0;
            is >> std::noskipws >> io_input;
            if (is.eof()) io_input = 0;
            memory[ea(addr_offset())] = io_input;
        } else {
            os << static_cast<char>(memory[ea(addr_offset())]) << std::flush;
        }
        break;

    case Opcode::H:
        return false;   // halt

    default:
        throw BrainfunkException("Invalid opcode");
    }

    ++pc;
    return true;
}

// ------------------------------------------------------------------
//  factories  -  every field is range-checked against what it encodes to
// ------------------------------------------------------------------

namespace {

std::int8_t narrow_field(offset_t value, unsigned bits, const char* name) {
    if (!fits_signed(value, bits)) {
        throw BrainfunkException(std::string(name) + " out of range");
    }
    return static_cast<std::int8_t>(value);
}

std::int32_t wide_field(offset_t value, unsigned bits, const char* name) {
    if (!fits_signed(value, bits)) {
        throw BrainfunkException(std::string(name) + " out of range");
    }
    return static_cast<std::int32_t>(value);
}

/* Moves and jumps may exceed their 20-bit binary field in a program that is only
 * interpreted; bit2bin is what rejects those. */
constexpr unsigned IN_MEMORY_WIDE_BITS = 32;

} // anonymous namespace

Bitcode Bitcode::none(Opcode opcode) {
    Bitcode bitcode;
    bitcode.opcode_ = opcode;
    return bitcode;
}

Bitcode Bitcode::data_op(Opcode opcode, offset_t addr_offset, memory_t data) {
    auto bitcode = none(opcode);
    bitcode.data_ = data;
    bitcode.wide_ = wide_field(addr_offset, ADDR_OFFSET_BITS, "addr_offset");
    return bitcode;
}

Bitcode Bitcode::mul(offset_t addr_offset, offset_t src_offset, memory_t factor) {
    auto bitcode = data_op(Opcode::MUL, addr_offset, factor);
    bitcode.narrow_ = narrow_field(src_offset, SRC_OFFSET_BITS, "src_offset");
    return bitcode;
}

Bitcode Bitcode::branch(Opcode opcode, offset_t test_offset, offset_t jump_offset) {
    auto bitcode = none(opcode);
    bitcode.narrow_ = narrow_field(test_offset, TEST_OFFSET_BITS, "test_offset");
    bitcode.wide_ = wide_field(jump_offset, IN_MEMORY_WIDE_BITS, "jump_offset");
    return bitcode;
}

Bitcode Bitcode::scan(offset_t test_offset, offset_t step) {
    auto bitcode = none(Opcode::F);
    bitcode.narrow_ = narrow_field(test_offset, TEST_OFFSET_BITS, "test_offset");
    bitcode.wide_ = wide_field(step, IN_MEMORY_WIDE_BITS, "step");
    return bitcode;
}

Bitcode Bitcode::move(offset_t move_offset) {
    auto bitcode = none(Opcode::M);
    bitcode.wide_ = wide_field(move_offset, IN_MEMORY_WIDE_BITS, "move_offset");
    return bitcode;
}
