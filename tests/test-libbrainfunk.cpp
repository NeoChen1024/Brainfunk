#include "libbrainfunk.hpp"
#include "program_generator.hpp"
#include "trace.hpp"
#include "trace_file.hpp"
#include "analysis.hpp"
#include "coverage.hpp"

#include <catch2/catch_test_macros.hpp>

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

/* A check with what it is for, which Catch2 prints with the values on failure. */
#define EXPECT(condition, message) \
    do {                             \
        INFO(message);               \
        CHECK(condition);            \
    } while (0)

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

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
    StatSink stat;
    (void)run_traced(brainfunk, stat, in, out);
    return stat.stats;
}

} // namespace

TEST_CASE("programs run and translate", "[translate]") {
    EXPECT(run("++++++++[>++++++++<-]>+.+.+.") == "ABC",
          "optimized execution produces expected output");
    EXPECT((run("<+.", {}, 16) == std::string(1, '\x01')),
          "data pointer wraps backwards");
    EXPECT(run(",.") == std::string(1, '\0'),
          "EOF input stores zero");

    EXPECT(dump("[-]").find("S\t0") != std::string::npos,
          "odd decrement loop becomes set-zero");
    EXPECT(dump("[>]").find("F\t0, 1") != std::string::npos,
          "scan loop becomes find-zero");
    EXPECT(dump("[->++<]").find("MUL\t1, 0, 2") != std::string::npos,
          "multiply-offset loop is optimized");

    EXPECT(dump("[->+<+>-<]").find("MUL") == std::string::npos,
          "loop that writes its own induction cell is not a multiply");
    EXPECT(run("++++[->++<->+++<]>.") == std::string(1, '\x0a'),
          "loop that writes its own induction cell keeps its iteration count");
    {
        std::string far_loop = "[-";
        far_loop.append(3000, '>');
        far_loop += '+';
        far_loop.append(3000, '<');
        far_loop += ']';
        EXPECT(dump(far_loop).find("MUL") == std::string::npos,
              "multiply offset beyond 12 bits stays an ordinary loop");
    }

    const auto llvm_ir = dump(",.[>]", Format::LLVM_IR);
    EXPECT(llvm_ir.find("define i32 @brainfunk_program") != std::string::npos,
          "LLVM mode emits the portable runtime entry point");
    EXPECT((llvm_ir.find("\ntarget triple =") == std::string::npos &&
              llvm_ir.find("\ntarget datalayout =") == std::string::npos),
          "portable LLVM IR omits target metadata");
    EXPECT((llvm_ir.find("call i32 %read_byte") != std::string::npos &&
              llvm_ir.find("call void %write_byte") != std::string::npos),
          "LLVM IR uses runtime I/O callbacks");

    INFO("memory size that is not a power of two is rejected");
    CHECK_THROWS_AS(Brainfunk(10), BrainfunkException);
    INFO("unmatched bracket is rejected");
    CHECK_THROWS_AS(Brainfunk().translate("[+"), BrainfunkException);
}

TEST_CASE("execution statistics", "[stats]") {
    {
        const auto stats = count("++[->+<]>.", OPT_NONE, MEMSIZE, {});
        const auto dynamic = [&](Opcode opcode) {
            return stats.dynamic_count[static_cast<std::size_t>(opcode)];
        };
        EXPECT(dynamic(Opcode::A) == 1, "stats count A");
        EXPECT(dynamic(Opcode::MUL) == 1, "stats count MUL");
        EXPECT(dynamic(Opcode::IO) == 1, "stats count IO");
        EXPECT(stats.dynamic_total() == stats.static_total(), "straight-line code runs once");
        /* A: 1R1W, MUL: 2R1W, S: 1W, IO out: 1R */
        EXPECT(stats.memory_reads == 4, "stats count memory reads");
        EXPECT(stats.memory_writes == 3, "stats count memory writes");

        const auto scan = count("+>+>+<<[>]", OPT_NONE, MEMSIZE, {});
        EXPECT(scan.scan_iterations == 3, "stats count scan iterations");
    }
}

TEST_CASE("lazy move", "[optimizer]") {
    EXPECT(dump("+.", Format::BIT, OPT_LAZY_MOVE).find("\tM\t") == std::string::npos,
          "a program without pointer motion emits no M");
    {
        const auto listing = dump(">>+<<.", Format::BIT, OPT_LAZY_MOVE);
        EXPECT(listing.find("A\t2, 1") != std::string::npos,
              "pending motion becomes addr_offset");
        EXPECT(listing.find("\tM\t") == std::string::npos, "balanced motion emits no M");
    }
    EXPECT(dump(">>[->+<]", Format::BIT, OPT_LAZY_MOVE).find("MUL\t3, 2, 1") != std::string::npos,
          "pending motion reaches both MUL operands");
    {
        const auto listing = dump(">>[>+<-]<<", Format::BIT, OPT_LAZY_MOVE);
        EXPECT(listing.find("\tM\t") == std::string::npos, "multiply loop needs no flush");
    }
    {
        const auto listing = dump(">>[.>+<-.]<<", Format::BIT, OPT_LAZY_MOVE);
        EXPECT((listing.find("JE\t2,") != std::string::npos &&
                  listing.find("JN\t2,") != std::string::npos),
              "branches test through test_offset");
        EXPECT(listing.find("\tM\t") == std::string::npos, "balanced loop needs no M");
    }
    {
        const auto listing = dump(">>[.>]", Format::BIT, OPT_LAZY_MOVE);
        EXPECT(listing.find("M\t1") != std::string::npos,
              "loop with net motion keeps M on its back edge");
    }
    EXPECT(dump(">>[>]", Format::BIT, OPT_LAZY_MOVE).find("F\t2, 1") != std::string::npos,
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
        EXPECT(first.str() == second.str(), "lazy move is idempotent");
    }
}

TEST_CASE("fold", "[optimizer]") {
    const auto folded = [](std::string_view code) { return dump(code, Format::BIT, OPT_FOLD); };
    {
        const auto listing = folded(",+>+<+.");
        EXPECT((contains(listing, "A\t0, 2") && contains(listing, "A\t1, 1")),
              "adds to one cell merge across other cells");
    }
    EXPECT(contains(folded(",[-]+++."), "S\t0, 3"), "set then add becomes one set");
    EXPECT(!contains(folded(",[-]++[-]."), "S\t0, 2"), "overwritten set is dropped");
    EXPECT(!contains(folded(",[-]+,."), "S\t0, 1"), "write overwritten by input is dropped");
    {
        const auto listing = folded(",>[-]++[->+++<]");
        EXPECT(!contains(listing, "MUL"), "multiply from a known cell folds away");
        EXPECT(contains(listing, "A\t2, 6"), "folded multiply adds the product");
    }
    EXPECT(!contains(folded(",[.][.]").substr(folded(",[.][.]").find("JN")), "JE"),
          "loop right after an identical test never runs");
    EXPECT(!contains(folded(",[>][-]"), "S\t"), "cell is known zero after a scan");
    EXPECT(!contains(folded(",[>][>]").substr(folded(",[>][>]").find("F\t")+1), "F\t"),
          "scan from a known-zero cell is dropped");
    {
        const auto listing = folded(",[.][-]+.");
        EXPECT(contains(listing, "S\t0, 1"), "cell is known zero after a loop");
        EXPECT(!contains(listing, "S\t0, 0"), "known-zero cell needs no clear");
    }
    {
        /* Offsets 0 and 16 name one cell on a 16-cell tape. */
        std::string aliased = "[-]+";
        aliased.append(16, '>');
        aliased += "+.";
        aliased.append(16, '<');
        aliased += '.';
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            EXPECT((execute(aliased, level, 16, {}, 100).output == std::string(2, '\x02')),
                  "offsets that alias on a small tape are one cell");
        }
        std::string aliased_set = "+";
        aliased_set.append(16, '>');
        aliased_set += "[-]";
        aliased_set.append(16, '<');
        aliased_set += "++.";
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            EXPECT((execute(aliased_set, level, 16, {}, 100).output == std::string(1, '\x02')),
                  "set through an alias overrides the other offset");
        }
        std::string aliased_mul = ",[-]+";
        aliased_mul.append(16, '>');
        aliased_mul += "++";
        aliased_mul.append(16, '<');
        aliased_mul += "[->+<]>.";
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            EXPECT(execute(aliased_mul, level, 16, "x", 100).output == std::string(1, '\x03'),
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
        EXPECT(agrees(",>[-]+++<[->++<]>.", "\x05", "\x0d"),
              "multiply adds to a pending set of its destination");
        EXPECT(agrees(",>[-]<[->+<]>[-].", "\x05", std::string_view("\0", 1)),
              "multiply invalidates what is known about its destination");

        /* Offset overflow is the only way a known value survives an M. */
        std::string far = std::string(3000, '>') + "+" + std::string(3000, '<') + "[-]" +
                          std::string(3000, '>') + "[.-]";
        EXPECT((agrees(far, {}, "\x01")), "known values follow base_ptr across M");
    }
}

TEST_CASE("fuse", "[optimizer]") {
    const auto fused = [](std::string_view code) { return dump(code, Format::BIT, OPT_FUSE); };
    {
        const auto listing = fused(",[->+<].");
        EXPECT(contains(listing, "MULC\t1, 0, 1"), "multiply loop clears through MULC");
        EXPECT(!contains(listing, "S\t"), "MULC replaces the clear");
    }
    {
        const auto listing = fused(",[->+>++<<]");
        EXPECT((contains(listing, "MUL\t1, 0, 1") && contains(listing, "MULC\t2, 0, 2")),
              "only the last multiply of a chain clears");
    }
    EXPECT(contains(fused(",[-]"), "S\t0, 0"), "a clear with no multiply stays S");
    {
        const auto listing = fused(",[.>+<-]");
        EXPECT(contains(listing, "DJN\t0,"), "decrement fuses into the loop branch");
        EXPECT(!contains(listing, "A\t0, 255"), "DJN replaces the decrement");
        EXPECT(contains(listing, "A\t1, 1"), "DJN leaves other cells alone");
    }
    {
        const auto listing = fused(",[-.]");
        EXPECT(!contains(listing, "DJN"), "decrement read before the branch stays");
        EXPECT(contains(fused(",[--.,]"), "IO\t0, 0"), "input stays in a loop");
    }
    EXPECT(!contains(fused(",[.+]"), "DJN"), "increment does not fuse");
    {
        const auto listing = fused(",[.>]");
        EXPECT(contains(listing, "MJN\t0, 1, "), "back-edge move fuses into the branch");
        EXPECT(!contains(listing, "\tM\t"), "MJN replaces M");
    }
    {
        const std::string far_move = ",[." + std::string(128, '>') + "]";
        EXPECT(!contains(fused(far_move), "MJN"), "move beyond 8 bits stays M");
        const std::string near_move = ",[." + std::string(127, '>') + "]";
        EXPECT(contains(fused(near_move), "MJN"), "move of exactly 8 bits fuses");
    }
    {
        std::string long_loop = ",[";
        for (int i = 0; i < 2048; ++i) long_loop += ".,";
        const auto long_listing = fused(long_loop + ">]");
        EXPECT(!contains(long_listing, "MJN"), "jump beyond 12 bits stays JN");
    }
    {
        const auto listing = fused(",>,>,<<[-]>[-]>[-]<<");
        EXPECT(contains(listing, "SRUN\t0, 0, 3"), "adjacent clears become SRUN");
        EXPECT(!contains(listing, "S\t"), "SRUN replaces the sets");
    }
    EXPECT(!contains(fused(",>,<[-]>>[-]<<."), "SRUN"), "a gap breaks a run");
    {
        std::string long_run = ",";
        for (int i = 0; i < 300; ++i) long_run += ">,";
        long_run.append(300, '<');
        for (int i = 0; i < 300; ++i) long_run += "[-]>";
        long_run += "<.";
        const auto listing = fused(long_run);
        EXPECT(contains(listing, "SRUN\t0, 0, 255"), "a run stops at the count limit");
        const auto input = std::string(301, 'x');
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            EXPECT(execute(long_run, level, MEMSIZE, input, 100000).output == std::string(1, '\0'),
                  "long run executes correctly");
        }
    }
}

TEST_CASE("words decode and images load", "[image]") {
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

        EXPECT(decoded(0x100FFF05) == "A\t-1, 5", "decode A");
        EXPECT(decoded(0x2007FFFF) == "S\t2047, 255", "decode S");
        EXPECT(decoded(0x380800FF) == "MUL\t-2048, -128, 255", "decode MUL");
        EXPECT(decoded(0x4FF00001) == "F\t-1, 1", "decode F");
        EXPECT(decoded(0x500FFFFF) == "M\t-1", "decode M");
        EXPECT(decoded(0x601FFFFE) == "JE\t1, -2", "decode JE");
        EXPECT(decoded(0x77F7FFFF) == "JN\t127, 524287", "decode JN");
        EXPECT(decoded(0x80000001) == "IO\t0, 1", "decode IO");
        EXPECT(decoded(0x90000000) == "H", "decode H");
        EXPECT(decoded(0xA0200303) == "MULC\t3, 2, 3", "decode MULC");
        EXPECT(decoded(0xB01FFFFE) == "DJN\t1, -2", "decode DJN");
        EXPECT(decoded(0xC01FEFFD) == "MJN\t1, -2, -3", "decode MJN");
        EXPECT(decoded(0xD0300507) == "SRUN\t5, 7, 3", "decode SRUN");

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
            EXPECT(Bitcode::decode(word).opcode() == Opcode::X, "illegal word decodes to X");
            EXPECT(Bitcode::decode(word).encode() == word, "illegal word encodes back to itself");
        }

        EXPECT((rejected({})), "empty image is rejected");
        EXPECT((rejected({0x10000001})), "image that falls off its end is rejected");
        EXPECT((rejected({0x60000001, 0x90000000})), "forward jump out of the image is rejected");
        EXPECT((rejected({0x700FFFFE, 0x90000000})), "backward jump out of the image is rejected");

        auto trap_image = image({0x10000005, 0xE0000000, 0x90000000});
        std::istringstream in;
        std::ostringstream out;
        EXPECT(trap_image.step(in, out), "instruction before the trap runs");
        INFO("illegal word traps when executed");
        CHECK_THROWS_AS((void)trap_image.step(in, out), BrainfunkException);
        EXPECT(trap_image.pc() == 1, "pc stays on the illegal word");
        EXPECT(trap_image.memory()[0] == 5, "a trap leaves memory alone");

        auto halt_image = image({0x90000000});
        EXPECT((!halt_image.step(in, out) && !halt_image.step(in, out)), "halt is sticky");
        EXPECT(halt_image.pc() == 0, "pc stays on H");
    }
}

TEST_CASE("every level agrees with the unoptimized program", "[differential]") {
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
                EXPECT(after.dynamic_count[i] <= before.dynamic_count[i], "lazy move adds no M");
            } else {
                EXPECT(after.dynamic_count[i] == before.dynamic_count[i],
                      "lazy move only changes how often M runs");
            }
        }
        EXPECT((after.memory_reads == before.memory_reads &&
                  after.memory_writes == before.memory_writes &&
                  after.scan_iterations == before.scan_iterations),
              "lazy move leaves memory traffic unchanged");

        for (int level = OPT_NONE + 1; level <= OPT_MAX; ++level) {
            /* Optimized code never takes more steps than the original. */
            const auto optimized = execute(code, level, memory_size, input, step_limit);
            if (!(optimized == reference)) FAIL_CHECK("level " << level << " differs on seed " << seed << ": " << code);
            EXPECT(optimized.steps <= reference.steps, "optimized code takes no extra steps");
        }
    }
    EXPECT(halted_programs > 1000, "differential test exercised enough programs");
}

TEST_CASE("image files", "[image]") {
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
        EXPECT(lz_decompress(payload) == words, "LZ round trip");
        EXPECT(payload.size() < words.size() * 4, "LZ makes the image smaller");
        EXPECT(lz_decompress(lz_compress(words, 8)) == words, "LZ round trip with a short window");
        EXPECT(lz_compress(words, 8).size() > payload.size(), "a shorter window packs worse");

        const auto packed = pack_image(words);
        EXPECT(std::equal(IMAGE_MAGIC.begin(), IMAGE_MAGIC.end(), packed.begin()), "packed image starts with the magic");
        EXPECT(unpack_image(packed) == words, "packed image round trip");
        const auto plain = pack_image(words, Compression::None);
        EXPECT(plain.size() == IMAGE_HEADER_BYTES + 4 * words.size(), "bincode image holds the words as they are");
        EXPECT(unpack_image(plain) == words, "bincode image round trip");
        for (const std::size_t at : {std::size_t{4}, std::size_t{5}, std::size_t{6}, std::size_t{7}, std::size_t{12}}) {
            auto changed = plain;
            changed[at] ^= 0x80;
            INFO("image header with a wrong version, flag, reserved bit or count is rejected, byte " << at);
            CHECK_THROWS_AS((void)unpack_image(changed), BrainfunkException);
        }
        {
            std::istringstream in(std::string(packed.begin(), packed.end()));
            EXPECT(read_image(in) == words, "read_image takes a packed image");
        }
        {
            std::istringstream in("80000000\n 0000002a \n9FFFFFFF\n");
            EXPECT((read_image(in) == std::vector<std::uint32_t>{0x80000000u, 0x2au, 0x9fffffffu}), "read_image takes hex words");
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
        EXPECT((rejects([&] { (void)unpack_image(corrupt); })), "packed image with a wrong CRC is rejected");
        auto truncated = packed;
        truncated.pop_back();
        EXPECT((rejects([&] { (void)unpack_image(truncated); })), "packed image cut short is rejected");
        const std::vector<std::uint8_t> too_far = {0x00, 1, 2, 3, 4, 0x80, 0x02, 0x00};
        EXPECT((rejects([&] { (void)lz_decompress(too_far); })), "copy from before the start is rejected");
        EXPECT((rejects([] {
                  std::istringstream in("1234567\n");
                  (void)read_image(in);
              })),
              "malformed hex image is rejected");
    }
}

TEST_CASE("words and listing lines convert back to themselves", "[bijection]") {
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
        EXPECT(mismatches == 0, "words convert to instructions and listing lines and back");
        EXPECT(legal > 400000, "the bijection test saw enough legal words");

        std::vector<Bitcode> code;
        for (int i = 0; i < 1000; ++i) code.push_back(Bitcode::decode(next() & 0xF00FFFFFu));
        std::ostringstream listing;
        write_listing(listing, code);
        EXPECT(parse_listing(listing.str()) == code, "listing round trip");
        std::ostringstream hex;
        write_hex(hex, encode_bincode(code));
        std::istringstream hex_in(hex.str()), listing_in(listing.str());
        EXPECT(read_image(hex_in) == read_image(listing_in), "hex and listing read as the same words");

        INFO("a move beyond 20 bits does not encode");
        CHECK_THROWS_AS((void)Bitcode::move(1 << 20).encode(), BrainfunkException);
    }
}

TEST_CASE("traced runs agree with the interpreter, and their events follow each other", "[trace]") {
    /* Checks each event against the one before and against a shadow of the tape. */
    struct Check {
        std::vector<memory_t> tape;
        addr_t pc = 0, base = 0;
        std::uint64_t index = 0;
        bool consistent = true;
        void begin(const Brainfunk& bf) { tape.assign(bf.memory().size(), 0); }
        void event(const TraceEvent& e) {
            consistent = consistent && e.index == index++ && e.pc == pc && e.base_before == base;
            for (unsigned i = 0; i < e.reads; ++i) consistent = consistent && tape[e.read[i].cell] == e.read[i].value;
            for (unsigned i = 0; i < e.writes; ++i) tape[e.write[i].cell] = e.write[i].value;
            for (unsigned i = 1; i < e.fill; ++i) tape[(e.write[0].cell + i) & (tape.size() - 1)] = e.write[0].value;
            pc = e.next_pc;
            base = e.base_after;
        }
        void end(const Brainfunk& bf, const TraceSummary&) {
            consistent = consistent && std::equal(tape.begin(), tape.end(), bf.memory().begin());
        }
    };

    constexpr std::string_view input = "Az\xff\x01?";
    int compared = 0;
    for (std::uint32_t seed = 1; seed <= 3000; ++seed) {
        ProgramGenerator generator(seed);
        const auto code = generator.program();
        const std::size_t memory_size = seed % 2 == 0 ? std::size_t{16} : MEMSIZE;
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            Brainfunk plain(memory_size), traced(memory_size);
            plain.translate(code);
            plain.optimize(level);
            traced.translate(code);
            traced.optimize(level);

            /* The plain run, as far as the traced one may go: a program that never halts is cut. */
            std::istringstream plain_in{std::string(input)}, traced_in{std::string(input)};
            std::ostringstream plain_out, traced_out;
            Check check;
            const auto summary = run_traced(traced, check, traced_in, traced_out, {0, 20000});
            for (std::uint64_t i = 0; i < summary.executed; ++i) {
                if (scan_never_ends(plain)) break;
                (void)plain.step(plain_in, plain_out);
            }
            if (summary.endless) {
                INFO("seed " << seed << " level " << level << ": an endless F or branch, which the step loop must agree on");
                CHECK((scan_never_ends(plain) || branches_to_itself(plain)));
            }
            if (!summary.halted) continue;
            ++compared;
            INFO("seed " << seed << " level " << level << ": " << code);
            CHECK(check.consistent);
            CHECK(traced_out.str() == plain_out.str());
            CHECK(traced.base_ptr() == plain.base_ptr());
            CHECK(std::equal(traced.memory().begin(), traced.memory().end(), plain.memory().begin()));
        }
    }
    CHECK(compared > 4000);
}

TEST_CASE("a branch to itself that is taken ends the run as endless", "[trace]") {
    for (const char* code : {"+.[]", "+.[[]-]"}) {
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            Brainfunk bf;
            bf.translate(code);
            bf.optimize(level);
            INFO(code << " at level " << level);
            NullSink sink;
            std::istringstream in;
            std::ostringstream out;
            const auto summary = run_traced(bf, sink, in, out, {});
            CHECK(summary.endless);
            CHECK_FALSE(summary.halted);
            CHECK(out.str() == "\x01");
            CHECK(branches_to_itself(bf));
        }
    }
    /* Not taken, it is an ordinary branch. */
    Brainfunk bf;
    bf.translate("[]+");
    NullSink sink;
    std::istringstream in;
    std::ostringstream out;
    const auto summary = run_traced(bf, sink, in, out, {});
    CHECK(summary.halted);
    CHECK_FALSE(summary.endless);
}

TEST_CASE("a trace range skips and stops", "[trace]") {
    struct Count {
        std::uint64_t events = 0, first = 0;
        void begin(const Brainfunk&) {}
        void event(const TraceEvent& e) { if (events++ == 0) first = e.index; }
        void end(const Brainfunk&, const TraceSummary&) {}
    };
    Brainfunk bf;
    bf.translate("++++++++[>+.<-]");   // a loop with output in it stays a loop
    std::istringstream in;
    std::ostringstream out;
    Count count;
    const auto summary = run_traced(bf, count, in, out, {10, 5});
    CHECK(count.events == 5);
    CHECK(count.first == 10);
    CHECK(summary.executed == 15);
    CHECK(!summary.halted);

    Count all;
    std::ostringstream whole;
    const auto full = run_traced(bf, all, in, whole, {});
    CHECK(full.halted);
    CHECK(full.reported == full.executed);
    CHECK(whole.str() == "\x01\x02\x03\x04\x05\x06\x07\x08");
}

namespace {

/* A trace of a program on an input, into a string. */
std::string trace_of(std::string_view code, const std::string& input, TraceRange range = {}) {
    Brainfunk bf;
    bf.translate(code);
    bf.optimize(OPT_FOLD);
    const auto words = encode_bincode(bf.bitcode());
    std::ostringstream file;
    TraceWriter writer(file, words, input, range);
    std::istringstream in(input);
    std::ostringstream out;
    (void)run_traced(bf, writer, in, out, range);
    return file.str();
}

/* Runs a trace file's program again, on the input given, against the file. */
void replay(const std::string& file, std::optional<std::string> input = std::nullopt) {
    std::istringstream stream(file);
    TraceReader reader(stream);
    const auto& header = reader.header();
    Brainfunk bf(header.meta.at("memory").get<std::size_t>());
    bf.load_bincode(header.words);
    std::istringstream in(input.value_or(header.input));
    std::ostringstream out;
    TraceCheck check(reader);
    (void)run_traced(bf, check, in, out, header.range());
}

} // namespace

TEST_CASE("a trace file runs again the way it went", "[trace]") {
    /* A word count: branches that depend on the input, and F. */
    const std::string code = ",[>+<[>-]>[>]<<[-]>[-]+>,]" "+[>]<[<]";
    const std::string input = "brainfunk traces this";
    const auto file = trace_of(code, input);

    std::istringstream stream(file);
    TraceReader reader(stream);
    CHECK(reader.header().input == input);
    CHECK(reader.header().meta.at("skip") == 0);
    CHECK(reader.header().meta.at("limit").is_null());
    CHECK_NOTHROW(replay(file));

    INFO("another input sends the branches another way");
    CHECK_THROWS_AS(replay(file, std::string("short")), BrainfunkException);

    INFO("a range is kept and runs again");
    CHECK_NOTHROW(replay(trace_of(code, input, {7, 30})));

    INFO("a file cut short is refused");
    CHECK_THROWS_AS(replay(file.substr(0, file.size() - 8)), BrainfunkException);

    INFO("something that is not a trace is refused");
    CHECK_THROWS_AS(replay(std::string(64, 'x')), BrainfunkException);
}

TEST_CASE("a trace file records which way branches went and how far F went", "[trace]") {
    const auto file = trace_of("+>+>+<<[>]+++[-]", "");
    std::istringstream stream(file);
    TraceReader reader(stream);
    CHECK(reader.next_scan() == 3);     // F 0, 1 from cell 0 to cell 3
    CHECK(reader.footer().at("halted") == true);
}

namespace {

/* An analysis report of a program given as bincode words. */
nlohmann::json analyze(std::initializer_list<std::uint32_t> words, std::string_view names, const nlohmann::json& config = {}) {
    Brainfunk bf;
    bf.load_bincode(std::vector<std::uint32_t>(words));
    AnalysisSet set(names, config.is_null() ? nlohmann::json::object() : config);
    std::istringstream in;
    std::ostringstream out;
    (void)run_traced(bf, set, in, out);
    return set.report();
}

std::uint64_t cycles_of(const nlohmann::json& report, std::size_t machine) {
    return report.at("analyses").at("issue").at("machines").at(machine).at("cycles").get<std::uint64_t>();
}

constexpr std::uint32_t add(unsigned cell) { return 0x10000000u | (cell << 8) | 1u; }
constexpr std::uint32_t HALT = 0x90000000u;

} // namespace

TEST_CASE("the issue model counts cycles as the cores would take them", "[analysis]") {
    /* Eight adds to eight cells, then H, which waits for everything. */
    const auto independent = analyze({add(0), add(1), add(2), add(3), add(4), add(5), add(6), add(7), HALT}, "issue");
    CHECK(cycles_of(independent, 0) == 9);    // scalar: one a cycle
    CHECK(cycles_of(independent, 1) == 5);    // dual: in pairs

    /* Adds to one cell, one after another: a pair would chain two adders, which only raw 2 allows. */
    const nlohmann::json config = {{"issue", {{"machines", {{{"name", "strict"}, {"width", 2}, {"raw", 1}},
                                                           {{"name", "chained"}, {"width", 2}, {"raw", 2}}}}}}};
    const auto chain = analyze({add(0), 0x10000002u, add(0), 0x10000002u, HALT}, "issue", config);
    CHECK(cycles_of(chain, 0) == 3);   // A+A of constants folds in D, so even raw 1 pairs them
    CHECK(cycles_of(chain, 1) == 3);

    const nlohmann::json narrow = {{"issue", {{"machines", {{{"width", 2}, {"writes", 1}}}}}}};
    CHECK(cycles_of(analyze({add(0), add(1), add(2), add(3), HALT}, "issue", narrow), 0) == 5);   // one write a cycle

    CHECK_THROWS_AS(analyze({HALT}, "issue", {{"issue", {{"machines", {{{"mul", "fast"}}}}}}}), BrainfunkException);
    CHECK_THROWS_AS(analyze({HALT}, "nonsense"), BrainfunkException);
}

TEST_CASE("predictors guess branches as their kinds do", "[analysis]") {
    /* Forty times round a loop whose inner loop goes round five times; the . keeps it a loop. */
    Brainfunk bf;
    bf.translate(std::string(40, '+') + "[>+++++[-.]<-]");
    const auto machine = [](const char* name, nlohmann::json predictor) {
        return nlohmann::json{{"name", name}, {"predictor", std::move(predictor)}};
    };
    const nlohmann::json config = {{"issue", {{"machines", {
        machine("rtl", nlohmann::json::object()),
        machine("bimodal", {{"kind", "bimodal"}}),
        machine("loop", {{"kind", "loop"}}),
        machine("tournament", {{"kind", "tournament"}, {"components", {{{"kind", "loop"}}, {{"kind", "gshare"}}}}}),
        machine("perfect", {{"kind", "perfect"}}),
    }}}}};
    AnalysisSet set("issue", config);
    std::istringstream in;
    std::ostringstream out;
    (void)run_traced(bf, set, in, out);
    const auto machines = set.report().at("analyses").at("issue").at("machines");
    const auto exits = [&](std::size_t m) {
        return machines.at(m).at("branches").at("backward_not_taken").at("mispredicts").get<std::uint64_t>();
    };
    CHECK(machines.at(0).at("branches").at("backward_not_taken").at("count") == 41);   // forty inner exits, one outer
    CHECK(exits(1) == 41);                // counters always guess the loop goes on
    CHECK(exits(2) <= 4);                 // the loop predictor learns that the inner one goes round five times
    CHECK(exits(3) < exits(1));
    CHECK(machines.at(4).at("mispredicts") == 0);
    CHECK(machines.at(4).at("cycles") < machines.at(0).at("cycles"));
    CHECK(machines.at(0).at("config").at("predictor").at("kind") == "rtl");    // defaults filled in

    const auto refused = [](nlohmann::json predictor) {
        return nlohmann::json{{"issue", {{"machines", {{{"predictor", std::move(predictor)}}}}}}};
    };
    CHECK_THROWS_AS(analyze({HALT}, "issue", refused({{"kind", "oracle"}})), BrainfunkException);
    CHECK_THROWS_AS(analyze({HALT}, "issue", refused({{"kind", "tournament"}, {"components", {{{"kind", "rtl"}}, {{"kind", "bimodal"}}}}})),
                    BrainfunkException);
    CHECK_THROWS_AS(analyze({HALT}, "issue", refused({{"kind", "gshare"}, {"direction", 1000}})), BrainfunkException);
}

TEST_CASE("branches resolve early once their cell's value is known", "[analysis]") {
    /* A loop counted down by adds, each exit a misprediction for counters that guess it goes on. */
    Brainfunk bf;
    bf.translate(std::string(20, '+') + "[>+++[-.>+<]<-]");
    const auto machine = [](const char* name, unsigned early, const char* forward, unsigned cells) {
        return nlohmann::json{{"name", name}, {"early_penalty", early}, {"forward", forward}, {"early_cells", cells}};
    };
    const nlohmann::json config = {{"issue", {{"machines", {machine("late", 0, "none", 0), machine("none", 5, "none", 0),
                                                           machine("add", 5, "add", 0), machine("add, 1 cell", 5, "add", 1)}}}}};
    AnalysisSet set("issue", config);
    std::istringstream in;
    std::ostringstream out;
    (void)run_traced(bf, set, in, out);
    const auto machines = set.report().at("analyses").at("issue").at("machines");
    const auto field = [&](std::size_t m, const char* name) { return machines.at(m).at(name).get<std::uint64_t>(); };
    const auto mispredicts = field(0, "mispredicts");
    REQUIRE(mispredicts >= 20);
    CHECK(field(0, "penalty_cycles") == 9 * mispredicts);
    CHECK(field(0, "resolved_early") == 0);
    CHECK(field(2, "penalty_cycles") == 5 * mispredicts);    // an add of a known value is known in D
    CHECK(field(2, "resolved_early") == mispredicts);
    CHECK(field(1, "penalty_cycles") > field(2, "penalty_cycles"));   // without, the decrement just before waits for X
    CHECK(field(1, "penalty_cycles") < field(0, "penalty_cycles"));
    CHECK(field(3, "penalty_cycles") > field(2, "penalty_cycles"));   // one cell holds the counter or the cell after it, not both
    CHECK(field(2, "cycles") < field(0, "cycles"));

    const auto refused = [](const char* name, nlohmann::json value) {
        return nlohmann::json{{"issue", {{"machines", {{{name, std::move(value)}}}}}}};
    };
    CHECK_THROWS_AS(analyze({HALT}, "issue", refused("forward", "all")), BrainfunkException);
    CHECK_THROWS_AS(analyze({HALT}, "issue", refused("early_cells", 24)), BrainfunkException);
}

TEST_CASE("analyses report what the run did", "[analysis]") {
    /* [0] = 3, then count it down: S, then A and JN three times, then H. */
    const auto report = analyze({0x20000003u, 0x100000FFu, 0x700FFFFEu, HALT}, "stat,profile,ngram,deps,values");
    const auto& a = report.at("analyses");
    CHECK(report.at("run").at("halted") == true);
    CHECK(a.at("stat").at("dynamic_total") == 8);
    CHECK(a.at("stat").at("dynamic").at("JN") == 3);
    CHECK(a.at("profile").at("branches") == 3);
    CHECK(a.at("profile").at("taken") == 2);
    CHECK(a.at("profile").at("hot").at(0).at("count") == 3);
    CHECK(a.at("profile").at("words_run") == 4);
    CHECK(a.at("profile").at("coverage").at("branches").at("both_ways") == 1);
    CHECK(!a.at("profile").contains("counts"));

    /* Every counter, for adding up runs; the JN at pc 2 ran three times and went back twice. */
    const auto counted = analyze({0x20000003u, 0x100000FFu, 0x700FFFFEu, HALT}, "profile",
                                 {{"profile", {{"counts", true}}}});
    const auto& profile = counted.at("analyses").at("profile");
    const auto counters = Coverage::from_json(profile.at("counters"));
    CHECK(counters.branch_pc == std::vector<addr_t>{2});
    CHECK(counters.count == std::vector<std::uint64_t>{3});
    CHECK(counters.taken == std::vector<std::uint64_t>{2});
    Brainfunk loaded;
    loaded.load_bincode(std::vector<std::uint32_t>{0x20000003u, 0x100000FFu, 0x700FFFFEu, HALT});
    CHECK(counters.expand(loaded.bitcode()) == std::vector<std::uint64_t>{1, 3, 3, 1});

    /* A forward branch that always falls through went one way only. */
    const auto skipped = analyze({0x20000001u, 0x60000001u, 0x20000002u, HALT}, "profile");
    CHECK(skipped.at("analyses").at("profile").at("coverage").at("branches").at("not_taken_only") == 1);
    CHECK(a.at("ngram").at("2").at("top").at(0).at("pattern") == "A raw JN");
    CHECK(a.at("deps").at("loop_trips").at("total") == 1);
    CHECK(a.at("values").at("branch_tests") == 3);
}

TEST_CASE("every word's count follows from the branches', and runs add up", "[coverage]") {
    /* Counts each word as it runs, for comparing with what the branches give. */
    struct Count {
        std::vector<std::uint64_t> counts;
        void begin(const Brainfunk& bf) { counts.assign(bf.bitcode().size(), 0); }
        void event(const TraceEvent& e) { ++counts[e.pc]; }
        void end(const Brainfunk&, const TraceSummary&) {}
    };
    const auto run = [](Brainfunk& bf, std::string_view input, TraceRange range, CoverageSink& flow, Count& count) {
        Sinks sinks(flow, count);
        std::istringstream in{std::string(input)};
        std::ostringstream out;
        return run_traced(bf, sinks, in, out, range);
    };

    int cut = 0, halted = 0, endless = 0;
    for (std::uint32_t seed = 1; seed <= 1500; ++seed) {
        ProgramGenerator generator(seed);
        const auto code = generator.program();
        for (int level = OPT_NONE; level <= OPT_MAX; ++level) {
            Brainfunk bf(seed % 2 == 0 ? std::size_t{16} : MEMSIZE);
            bf.translate(code);
            bf.optimize(level);
            INFO("seed " << seed << " level " << level << ": " << code);

            /* Whole, cut short, and a stretch from the middle. */
            for (const TraceRange range : {TraceRange{0, 20000}, TraceRange{0, 1 + seed % 37}, TraceRange{seed % 11, 1 + seed % 23}}) {
                CoverageSink flow;
                Count count;
                const auto summary = run(bf, "Az\xff", range, flow, count);
                halted += summary.halted;
                endless += summary.endless;
                cut += !summary.halted;
                CHECK(flow.coverage().expand(bf.bitcode()) == count.counts);
            }

            /* Two runs on different input, added up. */
            CoverageSink a, b;
            Count ca, cb;
            (void)run(bf, "Az", {0, 20000}, a, ca);
            (void)run(bf, "\x01\x02\x03", {0, 20000}, b, cb);
            auto both = a.coverage();
            both.add(Coverage::from_json(nlohmann::json::parse(b.coverage().to_json().dump())));
            std::vector<std::uint64_t> sum(ca.counts.size());
            for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = ca.counts[i] + cb.counts[i];
            CHECK(both.expand(bf.bitcode()) == sum);
        }
    }
    CHECK(halted > 1000);
    CHECK(cut > 1000);
    CHECK(endless > 0);

    /* Counters of another program, or that do not add up, are refused. */
    Brainfunk one, other;
    one.translate("+++[.-]");    // a loop that stays one, with its branches
    other.translate("+++[.-]>");
    CoverageSink flow;
    Count count;
    (void)run(one, "", {}, flow, count);
    CHECK_THROWS_AS(flow.coverage().expand(other.bitcode()), BrainfunkException);
    auto broken = flow.coverage();
    REQUIRE(broken.count.size() == 2);
    ++broken.count.back();
    CHECK_THROWS_AS(broken.expand(one.bitcode()), BrainfunkException);
}
