/* Neo_Chen's Brainfuck Interpreter — Modern C++20 Refactored */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
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
inline constexpr int BITCODE_FORMAT_C     = 0;
inline constexpr int BITCODE_FORMAT_PLAIN = 1;

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
    _COUNT   // sentinel – total number of opcodes
};

/* Friendly names – must match Opcode ordering */
inline constexpr const char* opcode_name(std::size_t i) noexcept {
    constexpr const char* names[] = {
        "X", "A", "S", "MUL", "F", "M", "JE", "JN", "IO", "H"
    };
    return (i < std::size(names)) ? names[i] : "???";
}

/* Operand type classifier:
 *    N – none, O – offset, M – dual (mul + offset), I – immediate */
inline constexpr char opcode_operand_type(std::size_t i) noexcept {
    constexpr char types[] = {
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
    return (i < std::size(types)) ? types[i] : '?';
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

class BrainfunkException : public std::exception {
public:
    explicit BrainfunkException(const std::string& msg)
        : msg_(msg) {}

    [[nodiscard]] const char* what() const noexcept override {
        return msg_.c_str();
    }

private:
    std::string msg_;
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
        : opcode_(op), operand_(DualOperand{mul, off}) {}

    /* ---- accessors ---- */
    [[nodiscard]] Opcode opcode() const noexcept { return opcode_; }

    /* ---- formatting ---- */
    [[nodiscard]] std::string to_string(int format = BITCODE_FORMAT_PLAIN) const;

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
    Opcode   opcode_;
    Operand  operand_;
};

/* ================================================================
 * Brainfunk – the main interpreter / compiler
 * ================================================================ */

enum class Format { BIT, C };

class Brainfunk {
public:
    explicit Brainfunk(std::size_t memsize = MEMSIZE);
    ~Brainfunk();

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
    [[nodiscard]] const std::vector<memory_t>& memory()  const noexcept { return memory_; }
    [[nodiscard]] const std::vector<Bitcode>&  bitcode() const noexcept { return bitcode_; }

    /* Dump bitcode to stream */
    void dump(std::ostream& os, Format format = Format::BIT) const;

private:
    addr_t                         ptr_ = 0;
    addr_t                         pc_  = 0;
    std::vector<memory_t>          memory_;
    std::vector<Bitcode>           bitcode_;

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
    return {ctr, i};
}

/* Modular-wrapping address arithmetic. */
[[nodiscard]] constexpr addr_t wrap_addr(addr_t address,
                                          addr_t size) noexcept {
    return (address < size) ? address : address % size;
}
