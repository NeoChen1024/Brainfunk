#include "libbrainfunk.hpp"
#include "program_generator.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
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

    /* ---- fuse ---- */
    const auto fused = [](std::string_view code) { return dump(code, Format::BIT, OPT_FUSE); };
    {
        const auto listing = fused(",[->+<].");
        check(contains(listing, "MULC\t1, 0, 1"), "multiply loop clears through MULC");
        check(!contains(listing, "S\t"), "MULC replaces the clear");
    }
    {
        const auto listing = fused(",[->+>++<<]");
        check(contains(listing, "MUL\t1, 0, 1") && contains(listing, "MULC\t2, 0, 2"),
              "only the last multiply of a chain clears");
    }
    check(contains(fused(",[-]"), "S\t0, 0"), "a clear with no multiply stays S");
    {
        const auto listing = fused(",[.>+<-]");
        check(contains(listing, "DJN\t0,"), "decrement fuses into the loop branch");
        check(!contains(listing, "A\t0, 255"), "DJN replaces the decrement");
        check(contains(listing, "A\t1, 1"), "DJN leaves other cells alone");
    }
    {
        const auto listing = fused(",[-.]");
        check(!contains(listing, "DJN"), "decrement read before the branch stays");
        check(contains(fused(",[--.,]"), "IO\t0, 0"), "input stays in a loop");
    }
    check(!contains(fused(",[.+]"), "DJN"), "increment does not fuse");
    {
        const auto listing = fused(",[.>]");
        check(contains(listing, "MJN\t0, 1, "), "back-edge move fuses into the branch");
        check(!contains(listing, "\tM\t"), "MJN replaces M");
    }
    {
        const std::string far_move = ",[." + std::string(128, '>') + "]";
        check(!contains(fused(far_move), "MJN"), "move beyond 8 bits stays M");
        const std::string near_move = ",[." + std::string(127, '>') + "]";
        check(contains(fused(near_move), "MJN"), "move of exactly 8 bits fuses");
    }
    {
        std::string long_loop = ",[";
        for (int i = 0; i < 2048; ++i) long_loop += ".,";
        const auto long_listing = fused(long_loop + ">]");
        check(!contains(long_listing, "MJN"), "jump beyond 12 bits stays JN");
    }
    {
        const auto listing = fused(",>,>,<<[-]>[-]>[-]<<");
        check(contains(listing, "SRUN\t0, 0, 3"), "adjacent clears become SRUN");
        check(!contains(listing, "S\t"), "SRUN replaces the sets");
    }
    check(!contains(fused(",>,<[-]>>[-]<<."), "SRUN"), "a gap breaks a run");
    {
        std::string long_run = ",";
        for (int i = 0; i < 300; ++i) long_run += ">,";
        long_run.append(300, '<');
        for (int i = 0; i < 300; ++i) long_run += "[-]>";
        long_run += "<.";
        const auto listing = fused(long_run);
        check(contains(listing, "SRUN\t0, 0, 255"), "a run stops at the count limit");
        const auto input = std::string(301, 'x');
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            check(execute(long_run, level, MEMSIZE, input, 100000).output == std::string(1, '\0'),
                  "long run executes correctly");
        }
    }

    /* ---- program images ---- */
    {
        const auto image = [](std::initializer_list<std::uint32_t> words) {
            Brainfunk brainfunk;
            brainfunk.load_bincode(std::vector<std::uint32_t>(words));
            return brainfunk;
        };
        const auto rejected = [](std::initializer_list<std::uint32_t> words) {
            try {
                Brainfunk brainfunk;
                brainfunk.load_bincode(std::vector<std::uint32_t>(words));
            } catch (const BrainfunkException&) {
                return true;
            }
            return false;
        };
        const auto decoded = [](std::uint32_t word) { return Bitcode::decode(word).to_string(); };

        check(decoded(0x100FFF05) == "A\t-1, 5", "decode A");
        check(decoded(0x2007FFFF) == "S\t2047, 255", "decode S");
        check(decoded(0x380800FF) == "MUL\t-2048, -128, 255", "decode MUL");
        check(decoded(0x4FF00001) == "F\t-1, 1", "decode F");
        check(decoded(0x500FFFFF) == "M\t-1", "decode M");
        check(decoded(0x601FFFFE) == "JE\t1, -2", "decode JE");
        check(decoded(0x77F7FFFF) == "JN\t127, 524287", "decode JN");
        check(decoded(0x80000001) == "IO\t0, 1", "decode IO");
        check(decoded(0x90000000) == "H", "decode H");
        check(decoded(0xA0200303) == "MULC\t3, 2, 3", "decode MULC");
        check(decoded(0xB01FFFFE) == "DJN\t1, -2", "decode DJN");
        check(decoded(0xC01FEFFD) == "MJN\t1, -2, -3", "decode MJN");
        check(decoded(0xD0300507) == "SRUN\t5, 7, 3", "decode SRUN");

        for (const std::uint32_t word : {
                 0x00000001u,   // X with a non-zero field
                 0x10100000u,   // A with a reserved bit
                 0x50100000u,   // M with a reserved bit
                 0x80000002u,   // undefined IO operation
                 0x90000001u,   // H with a non-zero field
                 0xD0000000u,   // SRUN of nothing
                 0xE0000000u,   // reserved opcode
                 0xF0000000u,   // reserved opcode
             }) {
            check(Bitcode::decode(word).opcode() == Opcode::X, "illegal word decodes to X");
            check(Bitcode::decode(word).encode() == word, "illegal word encodes back to itself");
        }

        check(rejected({}), "empty image is rejected");
        check(rejected({0x10000001}), "image that falls off its end is rejected");
        check(rejected({0x60000001, 0x90000000}), "forward jump out of the image is rejected");
        check(rejected({0x700FFFFE, 0x90000000}), "backward jump out of the image is rejected");

        auto trap_image = image({0x10000005, 0xE0000000, 0x90000000});
        std::istringstream in;
        std::ostringstream out;
        check(trap_image.step(in, out), "instruction before the trap runs");
        bool trapped = false;
        try {
            (void)trap_image.step(in, out);
        } catch (const BrainfunkException&) {
            trapped = true;
        }
        check(trapped, "illegal word traps when executed");
        check(trap_image.pc() == 1, "pc stays on the illegal word");
        check(trap_image.memory()[0] == 5, "a trap leaves memory alone");

        auto halt_image = image({0x90000000});
        check(!halt_image.step(in, out) && !halt_image.step(in, out), "halt is sticky");
        check(halt_image.pc() == 0, "pc stays on H");
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

    /* ---- program images ---- */
    {
        /* Runs that copy from 1 word back to the farthest a copy can reach, overlapping themselves. */
        std::uint32_t state = 1;
        const auto next = [&state] { return state = state * 1664525u + 1013904223u; };
        std::vector<std::uint32_t> words;
        for (const std::size_t distance : {1u, 2u, 7u, 8u, 9u, 300u}) {
            for (int k = 0; k < 20; ++k) words.push_back(next());
            for (int k = 0; k < 140; ++k) words.push_back(words[words.size() - distance]);
        }
        while (words.size() < LZ_MAX_DISTANCE + 200) words.push_back(next());
        for (int k = 0; k < 50; ++k) words.push_back(words[words.size() - LZ_MAX_DISTANCE]);

        const auto payload = lz_compress(words);
        check(lz_decompress(payload) == words, "LZ round trip");
        check(payload.size() < words.size() * 4, "LZ makes the image smaller");
        check(lz_decompress(lz_compress(words, 8)) == words, "LZ round trip with a short window");
        check(lz_compress(words, 8).size() > payload.size(), "a shorter window packs worse");

        const auto packed = pack_image(words);
        check(std::equal(PACKED_MAGIC.begin(), PACKED_MAGIC.end(), packed.begin()), "packed image starts with the magic");
        check(unpack_image(packed) == words, "packed image round trip");
        {
            std::istringstream in(std::string(packed.begin(), packed.end()));
            check(read_image(in) == words, "read_image takes a packed image");
        }
        {
            std::istringstream in("80000000\n 0000002a \n9FFFFFFF\n");
            check(read_image(in) == std::vector<std::uint32_t>{0x80000000u, 0x2au, 0x9fffffffu}, "read_image takes hex words");
        }

        const auto rejects = [](auto&& action) {
            try {
                action();
            } catch (const BrainfunkException&) {
                return true;
            }
            return false;
        };
        auto corrupt = packed;
        corrupt[corrupt.size() - 2] ^= 0xFF;
        check(rejects([&] { (void)unpack_image(corrupt); }), "packed image with a wrong CRC is rejected");
        auto truncated = packed;
        truncated.pop_back();
        check(rejects([&] { (void)unpack_image(truncated); }), "packed image cut short is rejected");
        const std::vector<std::uint8_t> too_far = {0x00, 1, 2, 3, 4, 0x80, 0x02, 0x00};
        check(rejects([&] { (void)lz_decompress(too_far); }), "copy from before the start is rejected");
        check(rejects([] {
                  std::istringstream in("1234567\n");
                  (void)read_image(in);
              }),
              "malformed hex image is rejected");
    }

    /* ---- every word and listing line converts back to itself ---- */
    {
        std::uint32_t state = 7;
        const auto next = [&state] {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        std::size_t legal = 0, mismatches = 0;
        for (int i = 0; i < 1000000; ++i) {
            /* Half of them with the reserved bits clear, which most are legal with. */
            std::uint32_t word = next();
            if (i % 2) word &= 0xF00FFFFFu;
            const auto decoded = Bitcode::decode(word);
            legal += decoded.opcode() != Opcode::X;
            bool same = false;
            try {
                same = decoded.encode() == word && Bitcode::parse(decoded.to_string()) == decoded &&
                       Bitcode::parse(decoded.to_string()).to_string() == decoded.to_string();
            } catch (const BrainfunkException&) {
            }
            if (!same && mismatches++ < 5) std::cerr << "FAIL: word " << std::hex << word << std::dec << " does not convert back\n";
        }
        check(mismatches == 0, "words convert to instructions and listing lines and back");
        check(legal > 400000, "the bijection test saw enough legal words");

        std::vector<Bitcode> code;
        for (int i = 0; i < 1000; ++i) code.push_back(Bitcode::decode(next() & 0xF00FFFFFu));
        std::ostringstream listing;
        write_listing(listing, code);
        check(parse_listing(listing.str()) == code, "listing round trip");
        std::ostringstream hex;
        write_hex(hex, encode_bincode(code));
        std::istringstream hex_in(hex.str()), listing_in(listing.str());
        check(read_image(hex_in) == read_image(listing_in), "hex and listing read as the same words");

        bool rejected_far = false;
        try {
            (void)Bitcode::move(1 << 20).encode();
        } catch (const BrainfunkException&) {
            rejected_far = true;
        }
        check(rejected_far, "a move beyond 20 bits does not encode");
    }

    return failures == 0 ? 0 : 1;
}
