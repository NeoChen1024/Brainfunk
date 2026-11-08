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
inline constexpr unsigned MJN_MOVE_BITS    = 8;
inline constexpr unsigned MJN_JUMP_BITS    = 12;

/* Levels for Brainfunk::optimize(); each one includes the previous ones. */
inline constexpr int OPT_NONE      = 0;
inline constexpr int OPT_LAZY_MOVE = 1;
inline constexpr int OPT_FOLD      = 2;
inline constexpr int OPT_FUSE      = 3;
inline constexpr int OPT_MAX       = OPT_FUSE;

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
    MULC,    // MUL, then mem[EA(src_offset)] = 0
    DJN,     // mem[EA(test_offset)] -= 1, then JN
    MJN,     // base_ptr += move_offset, then JN
    SRUN,    // mem[EA(addr_offset + i)] = data, for i in 0 .. count-1
    Count    // sentinel - total number of opcodes
};

/* Friendly names – must match Opcode ordering */
inline constexpr std::string_view opcode_name(Opcode opcode) noexcept {
    constexpr std::array names = {
        std::string_view{"X"}, std::string_view{"A"}, std::string_view{"S"},
        std::string_view{"MUL"}, std::string_view{"F"}, std::string_view{"M"},
        std::string_view{"JE"}, std::string_view{"JN"}, std::string_view{"IO"},
        std::string_view{"H"}, std::string_view{"MULC"}, std::string_view{"DJN"},
        std::string_view{"MJN"}, std::string_view{"SRUN"},
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
    Move,    // move_offset
    MoveBranch, // test_offset, move_offset, jump_offset
    Fill     // addr_offset, data, count
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
        OperandLayout::None,   /* H */
        OperandLayout::Mul,    /* MULC */
        OperandLayout::Branch, /* DJN */
        OperandLayout::MoveBranch, /* MJN */
        OperandLayout::Fill    /* SRUN */
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
 * is a plain load.  Not the binary encoding: encode() and decode() convert, and
 * to_string() and parse() do the same with the text of a listing.  Every word
 * decodes to an instruction that encodes back to it, and the same for text in
 * the form to_string() gives. */
class Bitcode {
public:
    constexpr Bitcode() noexcept = default;

    /* ---- construction, one factory per operand layout ---- */
    [[nodiscard]] static Bitcode none(Opcode opcode);
    [[nodiscard]] static Bitcode data_op(Opcode opcode, offset_t addr_offset, memory_t data);
    [[nodiscard]] static Bitcode mul(Opcode opcode, offset_t addr_offset, offset_t src_offset,
                                     memory_t factor);
    [[nodiscard]] static Bitcode branch(Opcode opcode, offset_t test_offset, offset_t jump_offset);
    [[nodiscard]] static Bitcode scan(offset_t test_offset, offset_t step);
    [[nodiscard]] static Bitcode move(offset_t move_offset);
    [[nodiscard]] static Bitcode move_branch(offset_t test_offset, offset_t move_offset,
                                             offset_t jump_offset);
    [[nodiscard]] static Bitcode fill(offset_t addr_offset, memory_t data, unsigned count);

    /* Decodes a bincode word; doc/bincode.txt defines which words are legal.  An
     * illegal word decodes to X, so that it traps if it is ever executed, and X
     * keeps the word, so that it encodes back to it. */
    [[nodiscard]] static Bitcode decode(std::uint32_t word);

    /* The bincode word.  Throws BrainfunkException if a field does not fit its
     * binary width, which a program that is only interpreted may exceed. */
    [[nodiscard]] std::uint32_t encode() const;

    /* One instruction as to_string() prints it, such as "MUL\t3, 2, 255", with
     * any spacing; "X" alone, or "X\t0x..." with an illegal word.  Throws
     * BrainfunkException on anything that would not encode. */
    [[nodiscard]] static Bitcode parse(std::string_view text);

    friend constexpr bool operator==(const Bitcode&, const Bitcode&) noexcept = default;

    /* The same branch, with a different target. */
    [[nodiscard]] Bitcode with_jump_offset(offset_t jump_offset) const;

    /* ---- accessors: each one is valid only for the layouts that carry it ---- */
    [[nodiscard]] constexpr Opcode   opcode()      const noexcept { return opcode_; }
    [[nodiscard]] constexpr memory_t data()        const noexcept { return data_; }
    [[nodiscard]] constexpr offset_t addr_offset() const noexcept { return wide_; }
    [[nodiscard]] constexpr offset_t src_offset()  const noexcept { return narrow_; }
    [[nodiscard]] constexpr offset_t test_offset() const noexcept { return narrow_; }
    [[nodiscard]] constexpr offset_t jump_offset() const noexcept { return wide_; }
    [[nodiscard]] constexpr offset_t step()        const noexcept { return wide_; }
    [[nodiscard]] constexpr offset_t move_offset() const noexcept {
        return opcode_ == Opcode::MJN ? offset_t{short_} : offset_t{wide_};
    }
    [[nodiscard]] constexpr unsigned count() const noexcept {
        return static_cast<std::uint8_t>(narrow_);
    }

    /* Offset of the cell this instruction is about, for display and tracing. */
    [[nodiscard]] constexpr std::optional<offset_t> primary_offset() const noexcept {
        switch (opcode_layout(opcode_)) {
        case OperandLayout::Data:
        case OperandLayout::Mul:
        case OperandLayout::Fill:
            return addr_offset();
        case OperandLayout::Branch:
        case OperandLayout::Scan:
            return test_offset();
        case OperandLayout::MoveBranch:
            return test_offset() + move_offset();   // tests after moving
        case OperandLayout::None:
        case OperandLayout::Move:
            break;
        }
        return std::nullopt;
    }

    /* ---- formatting ---- */
    [[nodiscard]] std::string to_string(
        BitcodeFormat format = BitcodeFormat::Plain) const;

private:
    friend class Brainfunk;

    Opcode        opcode_ = Opcode::X;
    memory_t      data_   = 0;   // data, factor
    std::int8_t   narrow_ = 0;   // src_offset, test_offset, count
    std::int8_t   short_  = 0;   // move_offset of MJN
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

    /* Load a program image, as read_image() gives it.  The image must end in H or an
     * illegal word, and no jump may leave it. */
    void load_bincode(std::span<const std::uint32_t> words);

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
    friend struct TraceRunner;   // the instrumented interpreter loop, trace.hpp

    addr_t                         base_ptr_ = 0;
    addr_t                         pc_  = 0;
    std::vector<memory_t>          memory_;
    std::vector<Bitcode>           bitcode_;

    void dump_llvm_ir(std::ostream& os) const;

    template <bool SingleStep>
    [[nodiscard]] bool interpret_(std::istream& is, std::ostream& os);

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
 * Program images (image.cpp); doc/bincode.txt is the normative format
 * ================================================================ */

/* bincode words from instructions; throws BrainfunkException, naming the pc, if
 * one does not encode. */
[[nodiscard]] std::vector<std::uint32_t> encode_bincode(std::span<const Bitcode> code);

/* Instructions from bincode words; an illegal word becomes X, keeping the word. */
[[nodiscard]] std::vector<Bitcode> decode_bincode(std::span<const std::uint32_t> words);

/* A listing: "pc:<TAB>instruction" a line, pc counting from 0, as to_string()
 * prints the instruction.  parse_listing() takes any spacing. */
void write_listing(std::ostream& os, std::span<const Bitcode> code);
[[nodiscard]] std::vector<Bitcode> parse_listing(std::string_view text);

/* A bincode image as text: one word a line, eight lowercase hexadecimal digits. */
void write_hex(std::ostream& os, std::span<const std::uint32_t> words);

/* Every image file starts with these four bytes, then the format version. */
inline constexpr std::array<std::uint8_t, 4> IMAGE_MAGIC = {'B', 'F', 'I', 'M'};
inline constexpr std::uint8_t IMAGE_VERSION = 1;
inline constexpr std::size_t IMAGE_HEADER_BYTES = 20;

/* How an image's payload holds its words: a bincode image has them as they are,
 * a packed image compresses them. */
enum class Compression : std::uint8_t { None = 0, LZ = 1 };

/* The farthest back an LZ copy can reach: its distance is 16 bits. */
inline constexpr std::size_t LZ_MAX_DISTANCE = 65535;

/* Word-level LZ.  Tokens: 0lllllll, then l + 1 literal words; 1lllllll, then a
 * 16-bit distance, for l + 2 words copied one at a time from that far back.  No
 * copy reaches back more than max_distance. */
[[nodiscard]] std::vector<std::uint8_t> lz_compress(std::span<const std::uint32_t> words,
                                                    std::size_t max_distance = LZ_MAX_DISTANCE);

/* Throws BrainfunkException on a token cut short or a copy from before the start. */
[[nodiscard]] std::vector<std::uint32_t> lz_decompress(std::span<const std::uint8_t> payload);

/* CRC-32 with the common reflected polynomial, as in the image header. */
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;

/* A whole image file: header, then the payload. */
[[nodiscard]] std::vector<std::uint8_t> pack_image(std::span<const std::uint32_t> words,
                                                   Compression compression = Compression::LZ);

/* Throws BrainfunkException unless the magic, version, reserved bits, sizes and
 * CRC are all right. */
[[nodiscard]] std::vector<std::uint32_t> unpack_image(std::span<const std::uint8_t> bytes);

/* Reads a program as words, whatever form it is in, told apart by content: an
 * image file, a listing, or one hexadecimal word a line. */
[[nodiscard]] std::vector<std::uint32_t> read_image(std::istream& in);

/* The same from a file; "-" is standard input. */
[[nodiscard]] std::vector<std::uint32_t> read_image_file(const std::string& filename);

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
