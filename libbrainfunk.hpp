/* Neo_Chen's Brainfuck Interpreter — Modern C++20 Refactored */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
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

/* Bitcode format constants (exported for external use) */
enum class BitcodeFormat : std::uint8_t { C, Plain };

/* ================================================================
 * Opcode – strongly typed enum (was: `enum opcodes`)
 * ================================================================ */

enum class Opcode : std::uint8_t {
    X,       // (none)     empty / invalid
    A,       // (imm)      add
    S,       // (imm)      set
    MUL,     // (dual)     multiply-add
    F,       // (offset)   scan-forward
    M,       // (offset)   move pointer
    JE,      // (offset)   jump-if-equal-zero
    JN,      // (offset)   jump-if-not-zero
    IO,      // (imm)      input / output
    H,       // (none)     halt
    Count    // sentinel – total number of opcodes
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

/* Operand type classifier:
 *    N – none, O – offset, M – dual (mul + offset), I – immediate */
inline constexpr char opcode_operand_type(Opcode opcode) noexcept {
    constexpr std::array types = {
        'N', /* X */
        'I', /* A */
        'I', /* S */
        'M', /* MUL */
        'O', /* F */
        'O', /* M */
        'O', /* JE */
        'O', /* JN */
        'I', /* IO */
        'N'  /* H */
    };
    static_assert(types.size() == static_cast<std::size_t>(Opcode::Count));
    const auto index = static_cast<std::size_t>(opcode);
    return (index < types.size()) ? types[index] : '?';
}

/* ================================================================
 * Helper: operand storage
 * ================================================================ */

struct DualOperand {
    memory_t mul;      // multiplication factor (mod 256)
    offset_t offset;   // pointer offset
};

using Operand = std::variant<std::monostate, memory_t, offset_t, DualOperand>;

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

class Bitcode {
public:
    /* ---- constructors ---- */
    Bitcode() noexcept
        : opcode_(Opcode::X), operand_(std::monostate{}) {}

    explicit Bitcode(Opcode op)           // no operand
        : opcode_(op), operand_(std::monostate{}) {}

    Bitcode(Opcode op, memory_t imm)      // immediate operand
        : opcode_(op), operand_(imm) {}

    Bitcode(Opcode op, offset_t off)      // offset operand
        : opcode_(op), operand_(off) {}

    Bitcode(Opcode op, memory_t mul, offset_t off)  // dual operand
        : opcode_(op), operand_(DualOperand{.mul = mul, .offset = off}) {}

    /* ---- accessors ---- */
    [[nodiscard]] Opcode opcode() const noexcept { return opcode_; }

    /* ---- formatting ---- */
    [[nodiscard]] std::string to_string(
        BitcodeFormat format = BitcodeFormat::Plain) const;

    /* ---- execution ---- */
    /* Returns false when halt instruction is reached. */
    [[nodiscard]] bool execute(
        std::span<memory_t>       memory,
        addr_t&                  pc,
        addr_t&                  ptr,
        std::istream&            is = std::cin,
        std::ostream&            os = std::cout
    ) const;

private:
    friend class Brainfunk;

    Opcode   opcode_;
    Operand  operand_;
};

/* ================================================================
 * Brainfunk – the main interpreter / compiler
 * ================================================================ */

enum class Format { BIT, C, LLVM_IR };

class Brainfunk {
public:
    explicit Brainfunk(std::size_t memsize = MEMSIZE);

    Brainfunk(const Brainfunk&)            = delete;
    Brainfunk& operator=(const Brainfunk&) = delete;
    Brainfunk(Brainfunk&&)                 = default;
    Brainfunk& operator=(Brainfunk&&)      = default;

    /* Translate Brainfuck source code into internal bitcode */
    void translate(std::string_view code);

    /* Execute the translated program */
    void run(std::istream& is = std::cin, std::ostream& os = std::cout);

    /* Execute exactly one bitcode instruction. Returns false on halt. */
    [[nodiscard]] bool step(std::istream& is = std::cin, std::ostream& os = std::cout);

    /* Reset execution state (memory + pc + ptr) without clearing bitcode */
    void reset_state();

    /* Reset everything */
    void clear();

    /* ---- const accessors for TUI ---- */
    [[nodiscard]] addr_t                       pc()      const noexcept { return pc_; }
    [[nodiscard]] addr_t                       ptr()     const noexcept { return ptr_; }
    [[nodiscard]] std::span<const memory_t> memory() const noexcept { return memory_; }
    [[nodiscard]] std::span<const Bitcode> bitcode() const noexcept { return bitcode_; }

    /* Dump bitcode to stream */
    void dump(std::ostream& os, Format format = Format::BIT) const;

private:
    addr_t                         ptr_ = 0;
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
