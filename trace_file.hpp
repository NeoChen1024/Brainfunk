#pragma once
/* Trace files: what it takes to run a program again, and the control flow it took.
 *
 * A run is determined by its program and its input, so a trace file carries both, and with them
 * every value any instruction read or wrote: running it again gives every event back. On top of
 * that it records which way each conditional branch went and how far each F went, a bit and a
 * byte or two each. That much lets a reader follow pc and base_ptr without a tape, compare another
 * implementation's run with this one, and check that a run again went the same way.
 *
 * The whole file is one zstd frame. Inside it, every value little endian:
 *
 *   "BFTR", version 1, 3 reserved bytes
 *   u32 length, then the header, JSON: "memory" cells, "eof", "skip", "limit" (null for none),
 *       "words", "input" (byte counts), and anything else the writer was given
 *   the program, "words" bincode words of 4 bytes, as they are
 *   the input, "input" bytes, all of it, whether the program read it all or not
 *   chunks, each starting with a kind byte:
 *     1  control: u32 events, u32 branches, the branches' bits (bit i of byte i / 8 is branch i,
 *        1 taken), u32 scans, each F's strides as a LEB128 number
 *     2  end: u32 length, then JSON: "executed", "reported", "halted", "trapped", "endless",
 *        "pc", "base_ptr", "memory_crc32"
 *
 * Only the events in the header's range are in the chunks. */

#include "libbrainfunk.hpp"
#include "trace.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

inline constexpr std::uint8_t TRACE_VERSION = 1;

struct TraceHeader {
    nlohmann::json             meta;
    std::vector<std::uint32_t> words;
    std::string                input;

    [[nodiscard]] TraceRange range() const;
};

/* A sink that writes a trace file. The program and input are the ones the run uses. */
class TraceWriter {
public:
    TraceWriter(std::ostream& out, std::span<const std::uint32_t> words, std::string_view input, TraceRange range,
                nlohmann::json extra = nlohmann::json::object());
    ~TraceWriter();
    TraceWriter(const TraceWriter&)            = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    void begin(const Brainfunk& bf);
    void event(const TraceEvent& e);
    void end(const Brainfunk& bf, const TraceSummary& summary);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Reads a trace file: the header at once, the control flow as asked for. */
class TraceReader {
public:
    explicit TraceReader(std::istream& in);
    ~TraceReader();
    TraceReader(const TraceReader&)            = delete;
    TraceReader& operator=(const TraceReader&) = delete;

    [[nodiscard]] const TraceHeader& header() const noexcept;

    /* The next branch's direction and the next F's strides, in the order of the run. Throw
     * BrainfunkException if the file has no more. */
    [[nodiscard]] bool          next_branch();
    [[nodiscard]] std::uint64_t next_scan();

    /* The end chunk, once every control chunk has been read; throws if there is more. */
    [[nodiscard]] const nlohmann::json& footer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* A sink that holds a run to a trace file's: each branch and F, and the state at the end. */
class TraceCheck {
public:
    explicit TraceCheck(TraceReader& reader) : reader_(reader) {}
    void begin(const Brainfunk&) {}
    void event(const TraceEvent& e);
    void end(const Brainfunk& bf, const TraceSummary& summary);

private:
    TraceReader&  reader_;
    std::uint64_t events_ = 0;
};

/* What the end chunk says about a run. */
[[nodiscard]] nlohmann::json trace_footer(const Brainfunk& bf, const TraceSummary& summary);
