#pragma once
/* Execution traces: an instrumented interpreter loop that reports every instruction it runs.
 *
 * The loop is apart from Brainfunk::run(), which stays as fast as it is; the differential tests
 * hold the two to the same results. Each instruction becomes one TraceEvent, with the cells it read
 * and wrote and their values, handed to a sink. A sink is any type with
 *
 *     void begin(const Brainfunk&);              before the first event, with the program loaded
 *     void event(const TraceEvent&);
 *     void end(const Brainfunk&, const TraceSummary&);
 *
 * and Sinks<A, B, ...> hands each event to several. The sinks themselves are in analysis.hpp and
 * trace_file.hpp. */

#include "brainfunk/libbrainfunk.hpp"

#include <array>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <tuple>

/* A cell and its value: before the instruction for a read, after it for a write. */
struct TraceAccess {
    addr_t   cell  = 0;
    memory_t value = 0;
};

struct TraceEvent {
    std::uint64_t  index = 0;            // instructions run before this one, from reset
    addr_t         pc = 0, next_pc = 0;  // next_pc == pc for H, as the RTL retires it
    addr_t         base_before = 0, base_after = 0;
    const Bitcode* instruction = nullptr;

    /* Reads and writes in program order of the operands: MUL and MULC read the source, then the
     * destination, and write the destination, then (MULC) the source. */
    std::uint8_t               reads = 0, writes = 0;
    std::array<TraceAccess, 2> read{}, write{};

    bool          branch = false;  // JE, JN, DJN, MJN
    bool          taken  = false;
    std::uint64_t steps  = 0;      // F: strides moved. It read steps + 1 cells from read[0].cell, the
                                   // last zero, the rest not.
    unsigned      fill   = 0;      // SRUN: cells written from write[0].cell on, each write[0].value
    bool          input  = false;  // IO input: the byte is write[0].value
    bool          eof    = false;  // ...and there was none, so it is what the EOF mode says

    [[nodiscard]] Opcode opcode() const noexcept { return instruction->opcode(); }
};

/* Which part of a run to report: the first `skip` instructions run without events, and the run
 * stops after `limit` events. */
struct TraceRange {
    std::uint64_t skip  = 0;
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
};

struct TraceSummary {
    std::uint64_t executed = 0;  // instructions run, skipped ones included
    std::uint64_t reported = 0;  // events
    bool          halted   = false;
    bool          trapped  = false;  // an illegal word, which did not run
    bool          endless  = false;  // an instruction that would run for ever, and is not counted as
                                     // run: an F that never finds a zero, or a branch to itself that is
                                     // taken; run() would spin in it
};

/* Several sinks as one, each event to each in order. */
template <class... S>
struct Sinks {
    std::tuple<S&...> sinks;
    explicit Sinks(S&... s) : sinks(s...) {}
    void begin(const Brainfunk& bf) { std::apply([&](auto&... s) { (s.begin(bf), ...); }, sinks); }
    void event(const TraceEvent& e) { std::apply([&](auto&... s) { (s.event(e), ...); }, sinks); }
    void end(const Brainfunk& bf, const TraceSummary& summary) {
        std::apply([&](auto&... s) { (s.end(bf, summary), ...); }, sinks);
    }
};

/* Counts per opcode, and the cell accesses each instruction made. */
struct StatSink {
    ExecutionStats stats;

    void begin(const Brainfunk& bf) {
        for (const auto& instruction : bf.bitcode()) ++stats.static_count[static_cast<std::size_t>(instruction.opcode())];
    }
    void event(const TraceEvent& e) {
        ++stats.dynamic_count[static_cast<std::size_t>(e.opcode())];
        stats.memory_reads += e.reads + e.steps;   // F tests a cell per stride, and the one it stops on
        stats.memory_writes += e.fill ? e.fill : e.writes;
        stats.scan_iterations += e.steps;
    }
    void end(const Brainfunk&, const TraceSummary&) {}
};


/* Accepts everything and does nothing: what runs the skipped part. */
struct NullSink {
    void begin(const Brainfunk&) {}
    void event(const TraceEvent&) {}
    void end(const Brainfunk&, const TraceSummary&) {}
};

struct TraceRunner {
    /* Runs the loaded program from reset. */
    template <class Sink>
    static TraceSummary run(Brainfunk& bf, Sink& sink, std::istream& is, std::ostream& os, TraceRange range) {
        bf.reset_state();
        sink.begin(bf);
        TraceSummary summary;

        const Bitcode* const code = bf.bitcode_.data();
        const std::size_t size = bf.bitcode_.size();
        memory_t* const memory = bf.memory_.data();
        const addr_t mask = bf.memory_.size() - 1;
        addr_t pc = 0, base = 0;
        const auto ea = [&](addr_t from, offset_t offset) { return (from + static_cast<addr_t>(offset)) & mask; };

        while (pc < size && summary.reported < range.limit) {
            const Bitcode& in = code[pc];
            TraceEvent e;
            e.index = summary.executed;
            e.pc = pc;
            e.base_before = base;
            e.instruction = &in;
            addr_t next = pc + 1;

            switch (in.opcode()) {
            case Opcode::A: {
                const auto cell = ea(base, in.addr_offset());
                e.reads = e.writes = 1;
                e.read[0] = {cell, memory[cell]};
                memory[cell] = static_cast<memory_t>(memory[cell] + in.data());
                e.write[0] = {cell, memory[cell]};
                break;
            }
            case Opcode::S: {
                const auto cell = ea(base, in.addr_offset());
                memory[cell] = in.data();
                e.writes = 1;
                e.write[0] = {cell, memory[cell]};
                break;
            }
            case Opcode::MUL:
            case Opcode::MULC: {
                const auto src = ea(base, in.src_offset()), dst = ea(base, in.addr_offset());
                const memory_t source = memory[src];
                e.reads = 2;
                e.read[0] = {src, source};
                e.read[1] = {dst, memory[dst]};
                memory[dst] = static_cast<memory_t>(memory[dst] + source * in.data());
                e.writes = 1;
                e.write[0] = {dst, memory[dst]};
                if (in.opcode() == Opcode::MULC) {
                    memory[src] = 0;
                    e.writes = 2;
                    e.write[1] = {src, 0};
                }
                break;
            }
            case Opcode::F: {
                /* F writes nothing, so the cells it tests repeat within a tape's length of strides:
                 * one that has gone that far without a zero never finds one. */
                e.reads = 1;
                e.read[0] = {ea(base, in.test_offset()), memory[ea(base, in.test_offset())]};
                while (memory[ea(base, in.test_offset())] != 0) {
                    if (e.steps > mask) {
                        summary.endless = true;
                        break;
                    }
                    base = ea(base, in.step());
                    ++e.steps;
                }
                break;
            }
            case Opcode::M:
                base = ea(base, in.move_offset());
                break;
            case Opcode::JE:
            case Opcode::JN:
            case Opcode::DJN:
            case Opcode::MJN: {
                if (in.opcode() == Opcode::MJN) base = ea(base, in.move_offset());
                const auto cell = ea(base, in.test_offset());
                e.reads = 1;
                e.read[0] = {cell, memory[cell]};
                if (in.opcode() == Opcode::DJN) {
                    memory[cell] = static_cast<memory_t>(memory[cell] - 1);
                    e.writes = 1;
                    e.write[0] = {cell, memory[cell]};
                }
                const bool zero = memory[cell] == 0;
                e.branch = true;
                e.taken = in.opcode() == Opcode::JE ? zero : !zero;
                if (e.taken) next = pc + 1 + static_cast<addr_t>(in.jump_offset());
                /* A JE or JN that jumps to itself changes nothing, so once taken it is taken for ever. */
                if (e.taken && next == pc && (in.opcode() == Opcode::JE || in.opcode() == Opcode::JN)) {
                    summary.endless = true;
                }
                break;
            }
            case Opcode::SRUN: {
                e.writes = 1;
                e.fill = in.count();
                e.write[0] = {ea(base, in.addr_offset()), in.data()};
                for (unsigned i = 0; i < e.fill; ++i) memory[ea(base, in.addr_offset() + static_cast<offset_t>(i))] = in.data();
                break;
            }
            case Opcode::IO: {
                const auto cell = ea(base, in.addr_offset());
                if (in.data() == IO_IN) {
                    memory_t byte = 0;
                    is >> std::noskipws >> byte;
                    e.eof = is.eof();
                    if (e.eof) byte = bf.eof_ == EofMode::Zero ? 0 : bf.eof_ == EofMode::MinusOne ? 255 : memory[cell];
                    memory[cell] = byte;
                    e.input = true;
                    e.writes = 1;
                    e.write[0] = {cell, byte};
                } else {
                    e.reads = 1;
                    e.read[0] = {cell, memory[cell]};
                    os << static_cast<char>(memory[cell]) << std::flush;
                }
                break;
            }
            case Opcode::H:
                next = pc;
                break;
            case Opcode::X:
            case Opcode::Count:
                summary.trapped = true;
                break;
            }
            if (summary.trapped || summary.endless) {
                base = e.base_before;   // as it was, at the instruction that did not finish
                break;
            }

            e.next_pc = next;
            e.base_after = base;
            if (summary.executed >= range.skip) {
                sink.event(e);
                ++summary.reported;
            }
            ++summary.executed;
            pc = next;
            if (in.opcode() == Opcode::H) {
                summary.halted = true;
                break;
            }
        }

        bf.pc_ = pc;
        bf.base_ptr_ = base;
        sink.end(bf, summary);
        return summary;
    }
};

/* The loaded program from reset, each instruction an event for the sink. */
template <class Sink>
TraceSummary run_traced(Brainfunk& bf, Sink& sink, std::istream& is, std::ostream& os, TraceRange range = {}) {
    return TraceRunner::run(bf, sink, is, os, range);
}
