/* Neo_Chen's Brainfuck Interpreter — Modern C++20 Refactored */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/* ================================================================
 * Public type aliases (kept for backward compatibility)
 * ================================================================ */

using addr_t   = std::size_t;
using memory_t = std::uint8_t;
using offset_t = std::ptrdiff_t;

/* ================================================================
 * Constants
 * ================================================================ */

inline constexpr std::size_t MEMSIZE = 1 << 16;   // Default memory size

/* I/O type numbers */
inline constexpr memory_t IO_IN  = 0;
inline constexpr memory_t IO_OUT = 1;

/* Instruction field widths; doc/bincode.txt is the normative layout. */
inline constexpr unsigned ADDR_OFFSET_BITS = 12;
inline constexpr unsigned SRC_OFFSET_BITS  = 8;
inline constexpr unsigned TEST_OFFSET_BITS = 8;
inline constexpr unsigned LONG_OFFSET_BITS = 20;   // jump_offset, step, move_offset

/* Levels for Brainfunk::optimize(); each one includes the previous ones. */
inline constexpr int OPT_NONE      = 0;
inline constexpr int OPT_LAZY_MOVE = 1;
inline constexpr int OPT_MAX       = OPT_LAZY_MOVE;

/* Bitcode format constants (exported for external use) */
enum class BitcodeFormat : std::uint8_t { C, Plain };

/* ================================================================
 * Opcode – strongly typed enum (was: `enum opcodes`)
 * ================================================================ */

enum class Opcode : std::uint8_t {
    X,       // invalid / placeholder
    A,       // mem[EA(addr_offset)] += data
    S,       // mem[EA(addr_offset)]  = data
    MUL,     // mem[EA(addr_offset)] += mem[EA(src_offset)] * data
    F,       // while mem[EA(test_offset)] != 0: base_ptr += step
    M,       // base_ptr += move_offset
    JE,      // if mem[EA(test_offset)] == 0: pc += jump_offset
    JN,      // if mem[EA(test_offset)] != 0: pc += jump_offset
    IO,      // input or output at mem[EA(addr_offset)], selected by data
    H,       // halt
    Count    // sentinel - total number of opcodes
};

/* Friendly names – must match Opcode ordering */
inline constexpr std::string_view opcode_name(Opcode opcode) noexcept {
    constexpr std::array names = {
        std::string_view{"X"}, std::string_view{"A"}, std::string_view{"S"},
        std::string_view{"MUL"}, std::string_view{"F"}, std::string_view{"M"},
        std::string_view{"JE"}, std::string_view{"JN"}, std::string_view{"IO"},
        std::string_view{"H"},
    };
    static_assert(names.size() == static_cast<std::size_t>(Opcode::Count));
    const auto index = static_cast<std::size_t>(opcode);
    return (index < names.size()) ? names[index] : std::string_view{"???"};
}

/* Which operands an opcode carries, and therefore its text and binary layout. */
enum class OperandLayout : std::uint8_t {
    None,    // (no operand)
    Data,    // addr_offset, data
    Mul,     // addr_offset, src_offset, factor
    Branch,  // test_offset, jump_offset
    Scan,    // test_offset, step
    Move     // move_offset
};

inline constexpr OperandLayout opcode_layout(Opcode opcode) noexcept {
    constexpr std::array layouts = {
        OperandLayout::None,   /* X */
        OperandLayout::Data,   /* A */
        OperandLayout::Data,   /* S */
        OperandLayout::Mul,    /* MUL */
        OperandLayout::Scan,   /* F */
        OperandLayout::Move,   /* M */
        OperandLayout::Branch, /* JE */
        OperandLayout::Branch, /* JN */
        OperandLayout::Data,   /* IO */
        OperandLayout::None    /* H */
    };
    static_assert(layouts.size() == static_cast<std::size_t>(Opcode::Count));
    const auto index = static_cast<std::size_t>(opcode);
    return (index < layouts.size()) ? layouts[index] : OperandLayout::None;
}

/* ================================================================
 * Exception
 * ================================================================ */

class BrainfunkException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/* ================================================================
 * Bitcode – single intermediate instruction
 * ================================================================ */

/* Flat 8-byte record, so a cache line holds eight instructions and an operand
 * is a plain load.  Not the binary encoding: that is produced by bit2bin. */
class Bitcode {
public:
    constexpr Bitcode() noexcept = default;

    /* ---- construction, one factory per operand layout ---- */
    [[nodiscard]] static Bitcode none(Opcode opcode);
    [[nodiscard]] static Bitcode data_op(Opcode opcode, offset_t addr_offset, memory_t data);
    [[nodiscard]] static Bitcode mul(offset_t addr_offset, offset_t src_offset, memory_t factor);
    [[nodiscard]] static Bitcode branch(Opcode opcode, offset_t test_offset, offset_t jump_offset);
    [[nodiscard]] static Bitcode scan(offset_t test_offset, offset_t step);
    [[nodiscard]] static Bitcode move(offset_t move_offset);

    /* ---- accessors: each one is valid only for the layouts that carry it ---- */
    [[nodiscard]] constexpr Opcode   opcode()      const noexcept { return opcode_; }
    [[nodiscard]] constexpr memory_t data()        const noexcept { return data_; }
    [[nodiscard]] constexpr offset_t addr_offset() const noexcept { return wide_; }
    [[nodiscard]] constexpr offset_t src_offset()  const noexcept { return narrow_; }
    [[nodiscard]] constexpr offset_t test_offset() const noexcept { return narrow_; }
    [[nodiscard]] constexpr offset_t jump_offset() const noexcept { return wide_; }
    [[nodiscard]] constexpr offset_t step()        const noexcept { return wide_; }
    [[nodiscard]] constexpr offset_t move_offset() const noexcept { return wide_; }

    /* Offset of the cell this instruction is about, for display and tracing. */
    [[nodiscard]] constexpr std::optional<offset_t> primary_offset() const noexcept {
        switch (opcode_layout(opcode_)) {
        case OperandLayout::Data:
        case OperandLayout::Mul:
            return addr_offset();
        case OperandLayout::Branch:
        case OperandLayout::Scan:
            return test_offset();
        case OperandLayout::None:
        case OperandLayout::Move:
            break;
        }
        return std::nullopt;
    }

    /* ---- formatting ---- */
    [[nodiscard]] std::string to_string(
        BitcodeFormat format = BitcodeFormat::Plain) const;

    /* ---- execution ---- */
    /* Returns false when halt instruction is reached. */
    [[nodiscard]] bool execute(
        std::span<memory_t>       memory,
        addr_t&                  pc,
        addr_t&                  base_ptr,
        std::istream&            is = std::cin,
        std::ostream&            os = std::cout
    ) const;

private:
    friend class Brainfunk;

    Opcode        opcode_ = Opcode::X;
    memory_t      data_   = 0;   // data, factor
    std::int8_t   narrow_ = 0;   // src_offset, test_offset
    std::int8_t   spare_  = 0;
    std::int32_t  wide_   = 0;   // addr_offset, jump_offset, step, move_offset
};

static_assert(sizeof(Bitcode) == 8);

/* ================================================================
 * Execution statistics
 * ================================================================ */

struct ExecutionStats {
    using OpcodeCounts = std::array<std::uint64_t, static_cast<std::size_t>(Opcode::Count)>;

    OpcodeCounts  static_count{};      // instructions in the program, per opcode
    OpcodeCounts  dynamic_count{};     // instructions executed, per opcode
    std::uint64_t scan_iterations = 0; // pointer steps taken inside F
    std::uint64_t memory_reads    = 0;
    std::uint64_t memory_writes   = 0;

    [[nodiscard]] std::uint64_t static_total() const noexcept;
    [[nodiscard]] std::uint64_t dynamic_total() const noexcept;
};

/* ================================================================
 * Brainfunk – the main interpreter / compiler
 * ================================================================ */

enum class Format { BIT, C, LLVM_IR };

class Brainfunk {
public:
    /* `memsize` must be a power of two: addresses wrap by masking, as they do
     * in hardware. */
    explicit Brainfunk(std::size_t memsize = MEMSIZE);

    Brainfunk(const Brainfunk&)            = delete;
    Brainfunk& operator=(const Brainfunk&) = delete;
    Brainfunk(Brainfunk&&)                 = default;
    Brainfunk& operator=(Brainfunk&&)      = default;

    /* Translate Brainfuck source code into internal bitcode */
    void translate(std::string_view code);

    /* Rewrite the translated program; see doc/optimizer.md.  Output, final memory
     * and final base_ptr stay the same at every level. */
    void optimize(int level);

    /* Execute the translated program */
    void run(std::istream& is = std::cin, std::ostream& os = std::cout);

    /* Execute the program from reset while counting what it does.  Much slower
     * than run(). */
    [[nodiscard]] ExecutionStats run_counted(std::istream& is = std::cin,
                                             std::ostream& os = std::cout);

    /* Execute exactly one bitcode instruction. Returns false on halt. */
    [[nodiscard]] bool step(std::istream& is = std::cin, std::ostream& os = std::cout);

    /* Reset execution state (memory + pc + base_ptr) without clearing bitcode */
    void reset_state();

    /* Reset everything */
    void clear();

    /* ---- const accessors for TUI ---- */
    [[nodiscard]] addr_t                       pc()      const noexcept { return pc_; }
    [[nodiscard]] addr_t                       base_ptr()     const noexcept { return base_ptr_; }
    [[nodiscard]] std::span<const memory_t> memory() const noexcept { return memory_; }
    [[nodiscard]] std::span<const Bitcode> bitcode() const noexcept { return bitcode_; }

    /* Dump bitcode to stream */
    void dump(std::ostream& os, Format format = Format::BIT) const;

private:
    addr_t                         base_ptr_ = 0;
    addr_t                         pc_  = 0;
    std::vector<memory_t>          memory_;
    std::vector<Bitcode>           bitcode_;

    void dump_llvm_ir(std::ostream& os) const;

    /* ---- internal helpers for translate() ---- */

    /* Try to parse a mul-offset loop like [->>++++<<] or [>>++++<<-].
     * On success, appends bitcode and returns true. */
    bool try_parse_mul_offset_loop_(std::string_view& code);

    /* Try to parse a scan loop like [><] (F instruction). */
    bool try_parse_scan_loop_(std::string_view& code);

    /* Try to parse a set-to-zero loop like [+-] or [-+]. */
    bool try_parse_set_zero_loop_(std::string_view& code);
};

/* ================================================================
 * Free helpers (exposed for testing / other translation units)
 * ================================================================ */

/* Count net occurrences of two complementary characters over the
 * whole string.  `sym[0]` counts positive, `sym[1]` counts negative.
 * Requires `sym.size() == 2`. */
[[nodiscard]] constexpr std::ptrdiff_t count_net(std::string_view text,
                                                  std::string_view sym) noexcept {
    std::ptrdiff_t ctr = 0;
    for (auto ch : text) {
        if (ch == sym[0]) ++ctr;
        else if (ch == sym[1]) --ctr;
    }
    return ctr;
}

[[nodiscard]] constexpr bool is_brainfuck_instruction(char ch) noexcept {
    constexpr std::string_view instructions = "+-><[].,";
    return instructions.find(ch) != std::string_view::npos;
}

/* Scan the longest leading run of `sym[0]`/`sym[1]` characters.
 * Returns the net count and the number of consumed characters. */
struct LeadingRun {
    std::ptrdiff_t net;     // net count of (+)-minus(-)
    std::size_t    length;  // characters consumed
};

[[nodiscard]] constexpr LeadingRun leading_run(std::string_view text,
                                                std::string_view sym) noexcept {
    std::ptrdiff_t ctr = 0;
    std::size_t    i   = 0;
    for (; i < text.size(); ++i) {
        if (text[i] == sym[0])      { ++ctr; }
        else if (text[i] == sym[1]) { --ctr; }
        else                        { break; }
    }
    return {.net = ctr, .length = i};
}

/* True when `value` fits in a two's-complement field of `bits` bits. */
[[nodiscard]] constexpr bool fits_signed(std::int64_t value, unsigned bits) noexcept {
    const std::int64_t limit = std::int64_t{1} << (bits - 1);
    return value >= -limit && value < limit;
}

/* Parses the argument of a -O option. */
[[nodiscard]] constexpr std::optional<int> parse_opt_level(std::string_view text) noexcept {
    if (text.size() != 1 || text[0] < '0' || text[0] > '9') return std::nullopt;
    const int level = text[0] - '0';
    if (level > OPT_MAX) return std::nullopt;
    return level;
}

/* Effective address; `size` is a power of two. */
[[nodiscard]] constexpr addr_t effective_address(addr_t base_ptr, offset_t offset,
                                                 addr_t size) noexcept {
    return (base_ptr + static_cast<addr_t>(offset)) & (size - 1);
}

/* Modular-wrapping address arithmetic. */
[[nodiscard]] constexpr addr_t wrap_addr(addr_t address,
                                          addr_t size) noexcept {
    return (address < size) ? address : address % size;
}

[[nodiscard]] constexpr addr_t wrap_offset(addr_t address,
                                           offset_t offset,
                                           addr_t size) noexcept {
    if (size == 0) return 0;

    const auto base = address % size;
    if (offset >= 0) {
        return (base + (static_cast<addr_t>(offset) % size)) % size;
    }

    const auto backward = static_cast<addr_t>(-(offset + 1)) + 1;
    return (base + size - (backward % size)) % size;
}
