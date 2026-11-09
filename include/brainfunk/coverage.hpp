#pragma once
/* Coverage, kept compact: only conditional branches change the flow of a program, so how often each
 * ran and how often it was taken determine how often every other word ran.
 *
 * Every run enters at a word, pc 0 unless the first part was skipped, and leaves one way or another.
 * Through H it leaves after running it: nothing falls through. Otherwise it leaves before a word it
 * did not run: the one after the limit, an illegal word, an F that never ends. Words then count
 *
 *   count[p] = taken branches to p + runs entering at p - runs cut before p + what falls through from p - 1
 *
 * where a branch lets fall through what it ran and was not taken, H nothing, and any other word all
 * it ran. One pass forward gives every count, and checks each branch's against its own. Runs of one
 * program add up, branch by branch. */

#include "brainfunk/libbrainfunk.hpp"
#include "brainfunk/trace.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <vector>

struct Coverage {
    std::size_t                     words = 0;
    std::uint32_t                   program_crc32 = 0;  // of the listing, to keep runs of different programs apart
    std::map<addr_t, std::uint64_t> starts, cuts;       // runs entering at, and cut before, each word
    std::vector<addr_t>             branch_pc;          // every conditional branch of the program, in order
    std::vector<std::uint64_t>      count, taken;       // and how often each ran, and was taken

    /* Adds another run's, of the same program; throws BrainfunkException for another program's. */
    void add(const Coverage& other);

    /* How often each word ran. Throws BrainfunkException if the counts do not add up. */
    [[nodiscard]] std::vector<std::uint64_t> expand(std::span<const Bitcode> code) const;

    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static Coverage from_json(const nlohmann::json& j);
};

/* The listing's CRC-32, which identifies a program as translated and optimized. */
[[nodiscard]] std::uint32_t program_crc32(std::span<const Bitcode> code);

[[nodiscard]] bool is_conditional_branch(Opcode op) noexcept;

/* A sink that collects a run's coverage. */
class CoverageSink {
public:
    void begin(const Brainfunk& bf);
    void event(const TraceEvent& e);
    void end(const Brainfunk& bf, const TraceSummary& summary);
    [[nodiscard]] const Coverage& coverage() const noexcept { return coverage_; }

private:
    Coverage                   coverage_;
    std::vector<std::uint32_t> slot_;   // index into branch_pc, by pc
    bool                       started_ = false;
};
