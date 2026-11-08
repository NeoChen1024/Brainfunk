#include "libbrainfunk.hpp"
#include "ctre.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
//  Pattern: [ - ( [><]+ [+-]+ )+ [><]+ ]     (mode 1: starts with '-')
//           [ ( [><]+ [+-]+ )+ [><]+ - ]     (mode 2: ends   with '-')
//
//  When a match is found we emit one MUL instruction per
//  (offset, mul) pair, followed by an S(0) to clear the cell.
//
//  The pairs are matched one at a time.  A single regex for the whole loop would
//  recurse once per pair inside CTRE and overflow the stack on a long enough loop.
//  The character classes never overlap, so each token can be possessive.
// ------------------------------------------------------------------

bool Brainfunk::try_parse_mul_offset_loop_(std::string_view& code) {
    auto rest = code.substr(1);    // after '['

    const bool decrements_first = rest.starts_with('-');
    if (decrements_first) rest.remove_prefix(1);

    std::vector<memory_t> mul;
    std::vector<offset_t> offset;
    std::string_view last_pair;

    while (auto pair = ctre::starts_with<"[\\>\\<]++[\\+\\-]++">(rest)) {
        last_pair = pair.to_view();
        count_mul_offset(last_pair, mul, offset, offset.empty() ? 0 : offset.back());
        rest.remove_prefix(last_pair.size());
    }
    if (offset.empty()) return false;

    std::size_t pairs = offset.size();
    if (decrements_first) {
        /* The moves back to the induction cell are not a pair: no +- follows. */
        const auto tail = ctre::starts_with<"[\\>\\<]++\\]">(rest);
        if (!tail) return false;
        if (offset.back() + count_net(tail.to_view(), "><") != 0) return false;
        rest.remove_prefix(tail.size());
    } else {
        /* The last pair is really the moves back, then the decrement. */
        if (pairs < 2 || !rest.starts_with(']')) return false;
        if (!ctre::match<"[\\>\\<]++\\-">(last_pair)) return false;
        if (offset.back() != 0) return false;
        rest.remove_prefix(1);
        --pairs;
    }

    /* A pair that writes the induction cell itself changes how many times the
     * loop runs, so it cannot be a multiply.  An offset that does not fit the
     * instruction's 12-bit field must stay an ordinary loop as well. */
    for (std::size_t i = 0; i < pairs; ++i) {
        if (offset[i] == 0 || !fits_signed(offset[i], ADDR_OFFSET_BITS)) return false;
    }

    for (std::size_t i = 0; i < pairs; ++i) {
        bitcode_.push_back(Bitcode::mul(Opcode::MUL, offset[i], 0, mul[i]));
    }

    /* Insert S(0) to clear the cell */
    bitcode_.push_back(Bitcode::data_op(Opcode::S, 0, memory_t{0}));

    code.remove_prefix(code.size() - rest.size());
    return true;
}

// ------------------------------------------------------------------
//  try_parse_scan_loop_
//
//  Pattern: [ [><]+ ]   →  F(offset)
// ------------------------------------------------------------------

bool Brainfunk::try_parse_scan_loop_(std::string_view& code) {
    auto m = ctre::starts_with<"^\\[[\\>\\<]++\\]">(code);
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
    auto m = ctre::starts_with<"^\\[[\\+\\-]++\\]">(code);
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
              "#define H()                                return 0\n"
              "#define MULC(addr_offset, src_offset, factor) do { \\\n"
              "\tMUL(addr_offset, src_offset, factor); \\\n"
              "\tmemory[EA(src_offset)] = 0; \\\n"
              "} while (0)\n"
              "#define DJN(test_offset, jump_offset)      --memory[EA(test_offset)]; }\n"
              "#define MJN(test_offset, move_offset, jump_offset) M(move_offset); }\n"
              "#define SRUN(addr_offset, data, count) \\\n"
              "\tfor (int64_t i_ = 0; i_ < (count); ++i_) memory[EA((addr_offset) + i_)] = (uint8_t)(data)\n\n"
              "int main(void)\n"
              "{\n";
        // clang-format on
    }

    if (format == Format::BIT) {
        write_listing(os, bitcode_);
        return;
    }
    for (const auto& instruction : bitcode_) os << '\t' << instruction.to_string(BitcodeFormat::C) << '\n';

    if (format == Format::C) {
        os << "}\n";
    }
}

// ------------------------------------------------------------------
//  run  –  execute the bitcode
// ------------------------------------------------------------------

void Brainfunk::run(std::istream& is, std::ostream& os) {
    reset_state();
    (void)interpret_<false>(is, os);
}

// ------------------------------------------------------------------
//  ExecutionStats, which StatSink in trace.hpp fills
// ------------------------------------------------------------------

std::uint64_t ExecutionStats::static_total() const noexcept {
    return std::accumulate(static_count.begin(), static_count.end(), std::uint64_t{0});
}

std::uint64_t ExecutionStats::dynamic_total() const noexcept {
    return std::accumulate(dynamic_count.begin(), dynamic_count.end(), std::uint64_t{0});
}

// ------------------------------------------------------------------
//  step  –  execute exactly one bitcode instruction
// ------------------------------------------------------------------

bool Brainfunk::step(std::istream& is, std::ostream& os) {
    return interpret_<true>(is, os);
}

// ------------------------------------------------------------------
//  interpret_  -  the interpreter loop
//
//  One body serves run() and step(), so that they cannot disagree.  Machine
//  state lives in locals while running, and operands are read straight from the
//  flat instruction record.
//
//  GCC and Clang get computed goto: each instruction has its own indirect jump,
//  which the branch predictor can learn separately.  On mandel.b under GCC this
//  takes half the time of a switch.
// ------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define BRAINFUNK_COMPUTED_GOTO 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#else
#define BRAINFUNK_COMPUTED_GOTO 0
#endif

template <bool SingleStep>
bool Brainfunk::interpret_(std::istream& is, std::ostream& os) {
    if (pc_ >= bitcode_.size()) return false;

    const Bitcode* const code = bitcode_.data();
    const Bitcode* next = code + pc_;
    const Bitcode* instruction = nullptr;
    addr_t base_ptr = base_ptr_;
    memory_t* const memory = memory_.data();
    const addr_t mask = memory_.size() - 1;

    const auto ea = [&](std::int32_t offset) {
        return (base_ptr + static_cast<addr_t>(static_cast<offset_t>(offset))) & mask;
    };
    const auto save_state = [&](const Bitcode* pc) {
        pc_ = static_cast<addr_t>(pc - code);
        base_ptr_ = base_ptr;
    };

#if BRAINFUNK_COMPUTED_GOTO
    /* Same order as Opcode */
    static const void* const handlers[] = {
        &&op_X, &&op_A, &&op_S, &&op_MUL, &&op_F, &&op_M, &&op_JE, &&op_JN,
        &&op_IO, &&op_H, &&op_MULC, &&op_DJN, &&op_MJN, &&op_SRUN,
    };
    static_assert(std::size(handlers) == static_cast<std::size_t>(Opcode::Count));
#define VM_CASE(name) op_##name
#define VM_DISPATCH()                                                          \
    do {                                                                       \
        instruction = next++;                                                  \
        goto* handlers[static_cast<std::size_t>(instruction->opcode_)];        \
    } while (0)
#else
#define VM_CASE(name) case Opcode::name
#define VM_DISPATCH() goto dispatch
#endif

#define VM_NEXT()                                                              \
    do {                                                                       \
        if constexpr (SingleStep) {                                            \
            save_state(next);                                                  \
            return true;                                                       \
        } else {                                                               \
            VM_DISPATCH();                                                     \
        }                                                                      \
    } while (0)

#if BRAINFUNK_COMPUTED_GOTO
    VM_DISPATCH();
#else
[[maybe_unused]] dispatch:
    instruction = next++;
    switch (instruction->opcode_) {
#endif

    VM_CASE(A):
        memory[ea(instruction->wide_)] += instruction->data_;
        VM_NEXT();

    VM_CASE(S):
        memory[ea(instruction->wide_)] = instruction->data_;
        VM_NEXT();

    VM_CASE(MUL):
        memory[ea(instruction->wide_)] +=
            static_cast<memory_t>(memory[ea(instruction->narrow_)] * instruction->data_);
        VM_NEXT();

    VM_CASE(MULC): {
        const memory_t source = memory[ea(instruction->narrow_)];
        memory[ea(instruction->wide_)] += static_cast<memory_t>(source * instruction->data_);
        memory[ea(instruction->narrow_)] = 0;
        VM_NEXT();
    }

    VM_CASE(F):
        while (memory[ea(instruction->narrow_)] != 0) {
            base_ptr = ea(instruction->wide_);
        }
        VM_NEXT();

    VM_CASE(M):
        base_ptr = ea(instruction->wide_);
        VM_NEXT();

    VM_CASE(JE):
        if (memory[ea(instruction->narrow_)] == 0) next += instruction->wide_;
        VM_NEXT();

    VM_CASE(JN):
        if (memory[ea(instruction->narrow_)] != 0) next += instruction->wide_;
        VM_NEXT();

    VM_CASE(DJN):
        if (--memory[ea(instruction->narrow_)] != 0) next += instruction->wide_;
        VM_NEXT();

    VM_CASE(MJN):
        base_ptr = ea(instruction->short_);
        if (memory[ea(instruction->narrow_)] != 0) next += instruction->wide_;
        VM_NEXT();

    VM_CASE(SRUN): {
        const auto count = static_cast<std::uint8_t>(instruction->narrow_);
        for (unsigned i = 0; i < count; ++i) {
            memory[ea(instruction->wide_ + static_cast<std::int32_t>(i))] = instruction->data_;
        }
        VM_NEXT();
    }

    VM_CASE(IO):
        if (instruction->data_ == IO_IN) {
            memory_t input = 0;
            is >> std::noskipws >> input;
            if (is.eof()) input = 0;
            memory[ea(instruction->wide_)] = input;
        } else {
            os << static_cast<char>(memory[ea(instruction->wide_)]) << std::flush;
        }
        VM_NEXT();

    VM_CASE(H):
        save_state(instruction);   // pc stays on H
        return false;

    VM_CASE(X):
        save_state(instruction);
        throw BrainfunkException("Empty instruction");

#if !BRAINFUNK_COMPUTED_GOTO
    case Opcode::Count:
        break;
    }
    throw BrainfunkException("Invalid opcode");
#endif
}

#undef VM_CASE
#undef VM_DISPATCH
#undef VM_NEXT

#if BRAINFUNK_COMPUTED_GOTO
#pragma GCC diagnostic pop
#endif
#undef BRAINFUNK_COMPUTED_GOTO

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
    case OperandLayout::MoveBranch:
        operand << test_offset() << ", " << move_offset() << ", " << jump_offset();
        break;
    case OperandLayout::Fill:
        operand << addr_offset() << ", " << static_cast<unsigned>(data()) << ", " << count();
        break;
    }

    /* Format output */
    if (format == BitcodeFormat::C) {
        return std::string(opcode_name(opcode_)) + "(" + operand.str() + ");";
    }
    if (opcode_ == Opcode::X && wide_ != 0) {
        /* The illegal word it was decoded from. */
        char word[16];
        std::snprintf(word, sizeof word, "0x%08x", static_cast<std::uint32_t>(wide_));
        operand << word;
    }
    const auto text = operand.str();
    return std::string(opcode_name(opcode_)) + (text.empty() ? "" : "\t" + text);
}

// ------------------------------------------------------------------
//  factories  -  every field is range-checked against what it encodes to
// ------------------------------------------------------------------

namespace {

std::int8_t narrow_field(offset_t value, unsigned bits, const char* name) {
    if (!fits_signed(value, bits)) {
        throw BrainfunkException(std::string(name) + " " + std::to_string(value) + " does not fit in " +
                                 std::to_string(bits) + " bits");
    }
    return static_cast<std::int8_t>(value);
}

std::int32_t wide_field(offset_t value, unsigned bits, const char* name) {
    if (!fits_signed(value, bits)) {
        throw BrainfunkException(std::string(name) + " " + std::to_string(value) + " does not fit in " +
                                 std::to_string(bits) + " bits");
    }
    return static_cast<std::int32_t>(value);
}

/* Moves and jumps may exceed their 20-bit binary field in a program that is only
 * interpreted; Bitcode::encode() is what rejects those. */
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

Bitcode Bitcode::mul(Opcode opcode, offset_t addr_offset, offset_t src_offset, memory_t factor) {
    auto bitcode = data_op(opcode, addr_offset, factor);
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

Bitcode Bitcode::move_branch(offset_t test_offset, offset_t move_offset, offset_t jump_offset) {
    auto bitcode = none(Opcode::MJN);
    bitcode.narrow_ = narrow_field(test_offset, TEST_OFFSET_BITS, "test_offset");
    bitcode.short_ = narrow_field(move_offset, MJN_MOVE_BITS, "move_offset");
    bitcode.wide_ = wide_field(jump_offset, MJN_JUMP_BITS, "jump_offset");
    return bitcode;
}

Bitcode Bitcode::fill(offset_t addr_offset, memory_t data, unsigned count) {
    if (count == 0 || count > 255) {
        throw BrainfunkException("count " + std::to_string(count) + " is not from 1 to 255");
    }
    auto bitcode = data_op(Opcode::SRUN, addr_offset, data);
    bitcode.narrow_ = static_cast<std::int8_t>(static_cast<std::uint8_t>(count));
    return bitcode;
}

Bitcode Bitcode::with_jump_offset(offset_t jump_offset) const {
    auto bitcode = *this;
    bitcode.wide_ = wide_field(jump_offset,
                               opcode_ == Opcode::MJN ? MJN_JUMP_BITS : IN_MEMORY_WIDE_BITS,
                               "jump_offset");
    return bitcode;
}

// ------------------------------------------------------------------
//  decode  -  one bincode word
// ------------------------------------------------------------------

namespace {

[[nodiscard]] constexpr offset_t sign_extend(std::uint32_t value, unsigned bits) noexcept {
    const std::uint32_t sign = std::uint32_t{1} << (bits - 1);
    const std::uint32_t field = value & ((sign << 1) - 1);
    return static_cast<offset_t>(static_cast<std::int64_t>(field ^ sign) -
                                 static_cast<std::int64_t>(sign));
}

} // anonymous namespace

Bitcode Bitcode::decode(std::uint32_t word) {
    auto illegal = none(Opcode::X);
    illegal.wide_ = static_cast<std::int32_t>(word);

    const auto op = word >> 28;
    if (op >= static_cast<std::uint32_t>(Opcode::Count)) return illegal;
    const auto opcode = static_cast<Opcode>(op);

    const auto top_byte = (word >> 20) & 0xFFu;
    const auto low_byte = static_cast<memory_t>(word & 0xFFu);
    const auto narrow = sign_extend(word >> 20, 8);
    const auto addr = sign_extend(word >> 8, ADDR_OFFSET_BITS);
    const auto wide = sign_extend(word, LONG_OFFSET_BITS);

    switch (opcode_layout(opcode)) {
    case OperandLayout::None:
        return (word & 0x0FFFFFFFu) == 0 ? none(opcode) : illegal;
    case OperandLayout::Data:
        if (top_byte != 0) return illegal;
        if (opcode == Opcode::IO && low_byte != IO_IN && low_byte != IO_OUT) return illegal;
        return data_op(opcode, addr, low_byte);
    case OperandLayout::Mul:
        return mul(opcode, addr, narrow, low_byte);
    case OperandLayout::Branch:
        return branch(opcode, narrow, wide);
    case OperandLayout::Scan:
        return scan(narrow, wide);
    case OperandLayout::Move:
        return top_byte == 0 ? move(wide) : illegal;
    case OperandLayout::MoveBranch:
        return move_branch(narrow, sign_extend(word >> 12, MJN_MOVE_BITS),
                           sign_extend(word, MJN_JUMP_BITS));
    case OperandLayout::Fill:
        return top_byte == 0 ? illegal : fill(addr, low_byte, top_byte);
    }
    return illegal;
}

// ------------------------------------------------------------------
//  encode  -  the inverse of decode
// ------------------------------------------------------------------

std::uint32_t Bitcode::encode() const {
    const auto field = [](std::int64_t value, unsigned bits, const char* name) -> std::uint32_t {
        if (!fits_signed(value, bits)) {
            throw BrainfunkException(std::string(name) + " " + std::to_string(value) + " does not fit in " +
                                     std::to_string(bits) + " bits");
        }
        return static_cast<std::uint32_t>(value) & ((std::uint32_t{1} << bits) - 1);
    };

    std::uint32_t word = static_cast<std::uint32_t>(opcode_) << 28;
    switch (opcode_layout(opcode_)) {
    case OperandLayout::None:
        if (opcode_ == Opcode::X) word = static_cast<std::uint32_t>(wide_);
        break;
    case OperandLayout::Data:
        word |= field(addr_offset(), ADDR_OFFSET_BITS, "addr_offset") << 8 | data_;
        break;
    case OperandLayout::Mul:
        word |= field(src_offset(), SRC_OFFSET_BITS, "src_offset") << 20 |
                field(addr_offset(), ADDR_OFFSET_BITS, "addr_offset") << 8 | data_;
        break;
    case OperandLayout::Branch:
        word |= field(test_offset(), TEST_OFFSET_BITS, "test_offset") << 20 |
                field(jump_offset(), LONG_OFFSET_BITS, "jump_offset");
        break;
    case OperandLayout::Scan:
        word |= field(test_offset(), TEST_OFFSET_BITS, "test_offset") << 20 | field(step(), LONG_OFFSET_BITS, "step");
        break;
    case OperandLayout::Move:
        word |= field(move_offset(), LONG_OFFSET_BITS, "move_offset");
        break;
    case OperandLayout::MoveBranch:
        word |= field(test_offset(), TEST_OFFSET_BITS, "test_offset") << 20 |
                field(move_offset(), MJN_MOVE_BITS, "move_offset") << 12 |
                field(jump_offset(), MJN_JUMP_BITS, "jump_offset");
        break;
    case OperandLayout::Fill:
        word |= std::uint32_t{static_cast<std::uint8_t>(narrow_)} << 20 |
                field(addr_offset(), ADDR_OFFSET_BITS, "addr_offset") << 8 | data_;
        break;
    }
    /* What the fields cannot say, such as IO with a data of 2, has no word. */
    if (decode(word) != *this) throw BrainfunkException(to_string() + " has no encoding");
    return word;
}

// ------------------------------------------------------------------
//  parse  -  the inverse of to_string
// ------------------------------------------------------------------

Bitcode Bitcode::parse(std::string_view text) {
    const auto parts = ctre::match<"\\s*+([A-Z]++)\\s*+(.*?)\\s*+">(text);
    if (!parts) throw BrainfunkException("not an instruction: " + std::string(text));
    const auto name = parts.get<1>().to_view();
    const auto operands = parts.get<2>().to_view();

    std::optional<Opcode> found;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Opcode::Count); ++i) {
        if (opcode_name(static_cast<Opcode>(i)) == name) found = static_cast<Opcode>(i);
    }
    if (!found) throw BrainfunkException("unknown opcode " + std::string(name));
    const auto opcode = *found;

    if (opcode == Opcode::X && !operands.empty()) {
        const auto raw = ctre::match<"0x([0-9a-fA-F]{1,8})">(operands);
        if (!raw) throw BrainfunkException("X takes nothing, or the illegal word as 0x...: " + std::string(text));
        const auto digits = raw.get<1>().to_view();
        std::uint32_t word = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), word, 16);
        const auto decoded = decode(word);
        if (decoded.opcode() != Opcode::X || word == 0)
            throw BrainfunkException("not an illegal word: " + std::string(text));
        return decoded;
    }

    /* Decimal operands, separated by commas. */
    std::vector<std::int64_t> values;
    for (std::string_view rest = operands; !rest.empty();) {
        const auto comma = rest.find(',');
        auto item = rest.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        std::int64_t value = 0;
        const auto result = std::from_chars(item.data(), item.data() + item.size(), value);
        if (item.empty() || result.ec != std::errc{} || result.ptr != item.data() + item.size())
            throw BrainfunkException("bad operand in " + std::string(text));
        values.push_back(value);
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
        if (rest.empty()) throw BrainfunkException("bad operand in " + std::string(text));
    }

    const auto layout = opcode_layout(opcode);
    constexpr std::array<std::size_t, 8> arity = {0, 2, 3, 2, 2, 1, 3, 3};
    if (values.size() != arity[static_cast<std::size_t>(layout)])
        throw BrainfunkException(std::string(name) + " takes " + std::to_string(arity[static_cast<std::size_t>(layout)]) +
                                 " operands: " + std::string(text));
    const auto byte = [&](std::int64_t value, const char* what) {
        if (value < 0 || value > 255) throw BrainfunkException(std::string(what) + " " + std::to_string(value) + " is not a byte");
        return static_cast<memory_t>(value);
    };

    Bitcode bitcode;
    switch (layout) {
    case OperandLayout::None: bitcode = none(opcode); break;
    case OperandLayout::Data: bitcode = data_op(opcode, values[0], byte(values[1], "data")); break;
    case OperandLayout::Mul: bitcode = mul(opcode, values[0], values[1], byte(values[2], "factor")); break;
    case OperandLayout::Branch: bitcode = branch(opcode, values[0], values[1]); break;
    case OperandLayout::Scan: bitcode = scan(values[0], values[1]); break;
    case OperandLayout::Move: bitcode = move(values[0]); break;
    case OperandLayout::MoveBranch: bitcode = move_branch(values[0], values[1], values[2]); break;
    case OperandLayout::Fill:
        if (values[2] < 1 || values[2] > 255) throw BrainfunkException("count " + std::to_string(values[2]) + " is not from 1 to 255");
        bitcode = fill(values[0], byte(values[1], "data"), static_cast<unsigned>(values[2]));
        break;
    }
    (void)bitcode.encode(); // the fields that only the binary width limits
    return bitcode;
}

// ------------------------------------------------------------------
//  load_bincode
// ------------------------------------------------------------------

void Brainfunk::load_bincode(std::span<const std::uint32_t> words) {
    std::vector<Bitcode> code;
    code.reserve(words.size());
    for (const auto word : words) code.push_back(Bitcode::decode(word));

    if (code.empty() ||
        (code.back().opcode() != Opcode::H && code.back().opcode() != Opcode::X)) {
        throw BrainfunkException("Program image must end in H or an illegal word");
    }

    const auto size = static_cast<offset_t>(code.size());
    for (offset_t pc = 0; pc < size; ++pc) {
        switch (code[static_cast<std::size_t>(pc)].opcode()) {
        case Opcode::JE:
        case Opcode::JN:
        case Opcode::DJN:
        case Opcode::MJN: {
            const auto target = pc + 1 + code[static_cast<std::size_t>(pc)].jump_offset();
            if (target < 0 || target >= size) {
                throw BrainfunkException("Jump leaves the program image");
            }
            break;
        }
        default:
            break;
        }
    }

    bitcode_ = std::move(code);
    reset_state();
}
