#include "analysis.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>

using nlohmann::json;

namespace {

constexpr std::size_t OPCODES = static_cast<std::size_t>(Opcode::Count);

std::string name_of(Opcode op) { return std::string(opcode_name(op)); }

/* Counts in power-of-two buckets: 1, 2, 3-4, 5-8, ..., keyed by their ranges, in order. */
class Histogram {
public:
    void add(std::uint64_t value, std::uint64_t times = 1) {
        const std::size_t bucket = value <= 1 ? 0 : static_cast<std::size_t>(std::bit_width(value - 1));
        if (bucket >= counts_.size()) counts_.resize(bucket + 1);
        counts_[bucket] += times;
        total_ += times;
    }
    [[nodiscard]] json report() const {
        json buckets = json::array();
        for (std::size_t b = 0; b < counts_.size(); ++b) {
            if (!counts_[b]) continue;
            const std::uint64_t low = b == 0 ? 0 : (std::uint64_t{1} << (b - 1)) + 1, high = b == 0 ? 1 : std::uint64_t{1} << b;
            buckets.push_back({{"from", low}, {"to", high}, {"count", counts_[b]},
                               {"share", static_cast<double>(counts_[b]) / static_cast<double>(total_)}});
        }
        return {{"total", total_}, {"buckets", buckets}};
    }

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t              total_ = 0;
};

double share(std::uint64_t part, std::uint64_t whole) {
    return whole ? static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

/* ---- stat ---- */

class StatAnalysis final : public Analysis {
public:
    void begin(const Brainfunk& bf) override { sink_.begin(bf); }
    void event(const TraceEvent& e) override { sink_.event(e); }
    [[nodiscard]] json report() const override {
        const auto& s = sink_.stats;
        json static_count = json::object(), dynamic_count = json::object();
        for (std::size_t op = 0; op < OPCODES; ++op) {
            static_count[name_of(static_cast<Opcode>(op))] = s.static_count[op];
            dynamic_count[name_of(static_cast<Opcode>(op))] = s.dynamic_count[op];
        }
        return {{"static", static_count}, {"dynamic", dynamic_count}, {"static_total", s.static_total()},
                {"dynamic_total", s.dynamic_total()}, {"memory_reads", s.memory_reads},
                {"memory_writes", s.memory_writes}, {"scan_iterations", s.scan_iterations}};
    }

private:
    StatSink sink_;
};

/* ---- profile ---- */

class ProfileAnalysis final : public Analysis {
public:
    explicit ProfileAnalysis(const json& config)
        : top_(config.value("top", std::size_t{30})), counts_(config.value("counts", false)) {}

    void begin(const Brainfunk& bf) override {
        code_.assign(bf.bitcode().begin(), bf.bitcode().end());
        count_.assign(code_.size(), 0);
        taken_.assign(code_.size(), 0);
    }
    void event(const TraceEvent& e) override {
        ++count_[e.pc];
        ++total_;
        if (e.branch) {
            ++branches_;
            if (e.taken) {
                ++taken_[e.pc];
                ++taken_total_;
            }
        }
    }
    [[nodiscard]] json report() const override {
        std::vector<std::size_t> order(code_.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        const auto shown = std::min(top_, order.size());
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(shown), order.end(),
                          [&](std::size_t a, std::size_t b) { return count_[a] > count_[b]; });
        json hot = json::array();
        for (std::size_t i = 0; i < shown && count_[order[i]]; ++i) {
            const auto pc = order[i];
            json row = {{"pc", pc}, {"instruction", code_[pc].to_string()}, {"count", count_[pc]},
                        {"share", share(count_[pc], total_)}};
            if (is_branch(code_[pc])) row["taken"] = taken_[pc];
            hot.push_back(row);
        }
        /* Coverage: which words ran, and which branches went each way. */
        std::uint64_t reached = 0, static_branches = 0, both = 0, taken_only = 0, not_taken_only = 0;
        for (std::size_t pc = 0; pc < code_.size(); ++pc) {
            reached += count_[pc] != 0;
            if (!is_branch(code_[pc])) continue;
            ++static_branches;
            if (count_[pc] == 0) continue;
            if (taken_[pc] == 0) ++not_taken_only;
            else if (taken_[pc] == count_[pc]) ++taken_only;
            else ++both;
        }
        json out = {{"instructions", total_}, {"words", code_.size()}, {"words_run", reached},
                    {"branches", branches_}, {"taken", taken_total_}, {"hot", hot},
                    {"coverage", {{"words", share(reached, code_.size())},
                                  {"branches", {{"total", static_branches}, {"both_ways", both},
                                                {"taken_only", taken_only}, {"not_taken_only", not_taken_only},
                                                {"never_run", static_branches - both - taken_only - not_taken_only}}}}}};
        if (counts_) {
            out["counts"] = count_;
            out["taken_counts"] = taken_;
        }
        return out;
    }

private:
    static bool is_branch(const Bitcode& b) {
        return opcode_layout(b.opcode()) == OperandLayout::Branch || opcode_layout(b.opcode()) == OperandLayout::MoveBranch;
    }

    std::size_t                top_;
    bool                       counts_;
    std::vector<Bitcode>       code_;
    std::vector<std::uint64_t> count_, taken_;
    std::uint64_t              total_ = 0, branches_ = 0, taken_total_ = 0;
};

/* ---- ngram ---- */

/* How an instruction's cells relate to the one before's, from closest to none. */
enum Relation : std::uint32_t { NONE = 0, RAW = 1, SAME = 2, NEAR = 3 };
constexpr std::string_view RELATION_NAMES[] = {"", "raw", "same", "near"};

/* The cells an event touched, for relating it to the next. */
struct Touched {
    std::array<addr_t, 4> cells{};
    std::uint8_t          count = 0;
    std::array<addr_t, 2> written{};
    std::uint8_t          writes = 0;

    explicit Touched(const TraceEvent& e) {
        for (unsigned i = 0; i < e.reads; ++i) cells[count++] = e.read[i].cell;
        for (unsigned i = 0; i < e.writes; ++i) {
            cells[count++] = e.write[i].cell;
            written[writes++] = e.write[i].cell;
        }
    }
    Touched() = default;
};

Relation relate(const TraceEvent& e, const Touched& before, addr_t mask) {
    for (unsigned i = 0; i < e.reads; ++i)
        for (unsigned j = 0; j < before.writes; ++j)
            if (e.read[i].cell == before.written[j]) return RAW;
    const Touched now(e);
    Relation best = NONE;
    for (unsigned i = 0; i < now.count; ++i) {
        for (unsigned j = 0; j < before.count; ++j) {
            const addr_t a = now.cells[i], b = before.cells[j];
            if (a == b) return SAME;
            if (((a + 1) & mask) == b || ((b + 1) & mask) == a) best = NEAR;
        }
    }
    return best;
}

class NgramAnalysis final : public Analysis {
public:
    explicit NgramAnalysis(const json& config) : top_(config.value("top", std::size_t{30})) {
        for (const auto n : config.value("n", std::vector<unsigned>{2, 3, 4})) {
            if (n < 2 || n > 5) throw BrainfunkException("ngram: n is from 2 to 5");
            counts_.push_back({n, {}});
        }
    }
    void begin(const Brainfunk& bf) override { mask_ = bf.memory().size() - 1; }
    void event(const TraceEvent& e) override {
        const std::uint32_t relation = seen_ ? relate(e, before_, mask_) : NONE;
        code_ = (code_ << ELEMENT) | (relation << 4) | static_cast<std::uint32_t>(e.opcode());
        ++seen_;
        for (auto& [n, counts] : counts_) {
            if (seen_ < n) continue;
            /* The oldest element's relation is to something outside the window. */
            const std::uint64_t key = (code_ & ((std::uint64_t{1} << (ELEMENT * n)) - 1)) &
                                      ~(std::uint64_t{3} << (ELEMENT * (n - 1) + 4));
            ++counts[key];
        }
        before_ = Touched(e);
    }
    [[nodiscard]] json report() const override {
        json out = json::object();
        for (const auto& [n, counts] : counts_) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> rows(counts.begin(), counts.end());
            std::uint64_t total = 0;
            for (const auto& row : rows) total += row.second;
            const auto shown = std::min(top_, rows.size());
            std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(shown), rows.end(),
                              [](const auto& a, const auto& b) { return a.second > b.second; });
            json list = json::array();
            for (std::size_t i = 0; i < shown; ++i) {
                list.push_back({{"pattern", decode(rows[i].first, n)}, {"count", rows[i].second},
                                {"share", share(rows[i].second, total)}});
            }
            out[std::to_string(n)] = {{"total", total}, {"distinct", rows.size()}, {"top", list}};
        }
        out["relations"] = "raw: reads a cell the one before wrote; same: shares a cell with it; near: a cell next to one of its";
        return out;
    }

private:
    static constexpr unsigned ELEMENT = 6;   // 4 bits of opcode, 2 of relation

    /* "A raw JN": A, then JN reading what A wrote. */
    static std::string decode(std::uint64_t key, unsigned n) {
        std::string text;
        for (unsigned i = n; i-- > 0;) {
            const auto element = (key >> (ELEMENT * i)) & 63;
            if (!text.empty()) text += ' ';
            if (const auto relation = element >> 4) {
                text += RELATION_NAMES[relation];
                text += ' ';
            }
            text += opcode_name(static_cast<Opcode>(element & 15));
        }
        return text;
    }

    std::size_t                                                            top_;
    std::vector<std::pair<unsigned, std::unordered_map<std::uint64_t, std::uint64_t>>> counts_;
    std::uint64_t code_ = 0, seen_ = 0;
    Touched       before_;
    addr_t        mask_ = 0;
};

/* ---- deps ---- */

class DepsAnalysis final : public Analysis {
public:
    void begin(const Brainfunk& bf) override {
        last_write_.assign(bf.memory().size(), NEVER);
        streak_.assign(bf.bitcode().size(), 0);
        mask_ = bf.memory().size() - 1;
    }
    void event(const TraceEvent& e) override {
        for (unsigned i = 0; i < e.reads; ++i) {
            const auto last = last_write_[e.read[i].cell];
            if (last == NEVER) ++unwritten_;
            else raw_.add(e.index - last);
        }
        for (unsigned i = 0; i < e.writes; ++i) last_write_[e.write[i].cell] = e.index;
        for (unsigned i = 1; i < e.fill; ++i) last_write_[(e.write[0].cell + i) & mask_] = e.index;

        ++block_;
        ++fetch_;
        if (e.branch) {
            blocks_.add(block_);
            block_ = 0;
            if (e.instruction->jump_offset() < 0) {
                if (e.taken) {
                    ++streak_[e.pc];
                } else {
                    trips_.add(streak_[e.pc] + 1);
                    streak_[e.pc] = 0;
                }
            }
        }
        if (e.next_pc != e.pc + 1) {
            fetches_.add(fetch_);
            fetch_ = 0;
        }
    }
    [[nodiscard]] json report() const override {
        return {{"raw_distance", raw_.report()}, {"reads_of_unwritten_cells", unwritten_},
                {"block_length", blocks_.report()}, {"run_between_taken_branches", fetches_.report()},
                {"loop_trips", trips_.report()},
                {"about", "raw_distance: instructions since a read's cell was written. block_length: instructions up to and "
                          "including each branch. run_between_taken_branches: instructions fetched in a row. loop_trips: "
                          "times round a backward branch's loop, counted when it falls through"}};
    }

private:
    static constexpr std::uint64_t NEVER = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> last_write_, streak_;
    Histogram     raw_, blocks_, fetches_, trips_;
    std::uint64_t unwritten_ = 0, block_ = 0, fetch_ = 0;
    addr_t        mask_ = 0;
};

/* ---- values ---- */

class ValuesAnalysis final : public Analysis {
public:
    void event(const TraceEvent& e) override {
        for (unsigned i = 0; i < e.reads; ++i) ++read_[e.read[i].value];
        for (unsigned i = 0; i < e.writes; ++i) ++written_[e.write[i].value];
        if (e.branch) {
            ++tests_;
            zero_tests_ += e.read[0].value == 0 || (e.writes && e.write[0].value == 0);
        }
        if (e.opcode() == Opcode::F) {
            ++scans_;
            immediate_ += e.steps == 0;
        }
    }
    [[nodiscard]] json report() const override {
        const auto table = [](const std::array<std::uint64_t, 256>& counts) {
            std::uint64_t total = 0;
            for (const auto c : counts) total += c;
            json top = json::array();
            std::vector<unsigned> order(256);
            for (unsigned i = 0; i < 256; ++i) order[i] = i;
            std::partial_sort(order.begin(), order.begin() + 8, order.end(), [&](unsigned a, unsigned b) { return counts[a] > counts[b]; });
            for (unsigned i = 0; i < 8 && counts[order[i]]; ++i) top.push_back({{"value", order[i]}, {"share", share(counts[order[i]], total)}});
            return json{{"total", total}, {"zero", share(counts[0], total)}, {"top", top}, {"counts", counts}};
        };
        return {{"read", table(read_)}, {"written", table(written_)},
                {"branch_tests", tests_}, {"branch_tests_zero", share(zero_tests_, tests_)},
                {"scans", scans_}, {"scans_already_on_zero", share(immediate_, scans_)}};
    }

private:
    std::array<std::uint64_t, 256> read_{}, written_{};
    std::uint64_t tests_ = 0, zero_tests_ = 0, scans_ = 0, immediate_ = 0;
};

/* ---- issue ---- */

/* The core's guesses, as rtl/brainfunk_predict.sv makes them, with sizes to vary: a next-fetch
 * table keyed by the word fetched before, and 2-bit counters of how far a branch disagrees with
 * its static guess, backward taken. The word fetched before is taken to be the one run before. */
struct PredictorConfig {
    unsigned next = 16, tag_bits = 8, direction = 4096, history = 0;
};

class Predictor {
public:
    explicit Predictor(const PredictorConfig& c) : c_(c), table_(c.next), counters_(c.direction, 0) {
        if (!std::has_single_bit(c.next) || !std::has_single_bit(c.direction)) {
            throw BrainfunkException("issue: predictor table sizes must be powers of two");
        }
        shift_ = static_cast<unsigned>(std::countr_zero(c.next));
    }

    struct Guess {
        bool mispredict = false, steer = false;
    };

    Guess see(addr_t pc, addr_t before, bool branch, bool taken, addr_t target) {
        Guess g;
        Entry& entry = table_[before & (c_.next - 1)];
        const std::uint32_t tag = static_cast<std::uint32_t>(before >> shift_) & ((1u << c_.tag_bits) - 1);
        const bool hit = entry.valid && entry.tag == tag;
        if (!branch) {
            if (hit) {
                g.steer = true;
                entry.valid = false;
            }
            return g;
        }
        const bool backward = target <= pc;
        std::uint8_t& counter = counters_[(pc ^ history_) & (c_.direction - 1)];
        if (hit) {
            if (!taken || entry.target != target) {
                g.mispredict = true;
                entry.valid = false;
            }
        } else {
            const bool predict = ((counter >> 1) & 1) != backward;
            if (predict) {
                g.steer = true;
                entry = {true, tag, target};
            }
            g.mispredict = predict != taken;
        }
        if (g.mispredict) g.steer = false;
        const bool agrees = taken == backward;
        counter = agrees ? (counter ? counter - 1 : 0) : (counter < 3 ? counter + 1 : 3);
        if (c_.history) history_ = ((history_ << 1) | (taken ? 1 : 0)) & ((addr_t{1} << c_.history) - 1);
        return g;
    }

private:
    struct Entry {
        bool          valid = false;
        std::uint32_t tag = 0;
        addr_t        target = 0;
    };
    PredictorConfig           c_;
    std::vector<Entry>        table_;
    std::vector<std::uint8_t> counters_;
    unsigned                  shift_ = 0;
    addr_t                    history_ = 0;
};

enum class MulMode { Split, PairUnit, Single };

/* An in-order core that issues up to `width` instructions a cycle. The defaults are the scalar
 * core in rtl/ as it is; doc/analysis.md says what each field means. */
struct MachineConfig {
    std::string     name = "machine";
    unsigned        width = 1, reads = 2, writes = 2, branches = 1;
    unsigned        raw = 1;            // same-cycle reads of a cell written in the group: 0 never, 1 cheap cases, 2 always
    MulMode         mul = MulMode::PairUnit;
    bool            two_mul = true;     // two MUL destinations in a cycle
    unsigned        fetch_line = 0;     // a group stays within an aligned run of this many words; 0 any
    unsigned        banks = 32;         // cells in a line of banks: two in one bank of different lines conflict; 0 none
    unsigned        penalty = 9, steer = 3;
    unsigned        scan_base = 8, line_cells = 32, stride_limit = 16, io = 7;
    PredictorConfig predictor;

    static MachineConfig from(const json& j) {
        MachineConfig c;
        c.name = j.value("name", c.name);
        c.width = j.value("width", c.width);
        c.reads = j.value("reads", c.reads);
        c.writes = j.value("writes", c.writes);
        c.branches = j.value("branches", c.branches);
        c.raw = j.value("raw", c.raw);
        const auto mul = j.value("mul", std::string("pair-unit"));
        if (mul == "split") c.mul = MulMode::Split;
        else if (mul == "pair-unit") c.mul = MulMode::PairUnit;
        else if (mul == "single") c.mul = MulMode::Single;
        else throw BrainfunkException("issue: mul is split, pair-unit or single, not " + mul);
        c.two_mul = j.value("two_mul", c.two_mul);
        c.fetch_line = j.value("fetch_line", c.fetch_line);
        c.banks = j.value("banks", c.banks);
        c.penalty = j.value("penalty", c.penalty);
        c.steer = j.value("steer", c.steer);
        c.scan_base = j.value("scan_base", c.scan_base);
        c.line_cells = j.value("line_cells", c.line_cells);
        c.stride_limit = j.value("stride_limit", c.stride_limit);
        c.io = j.value("io", c.io);
        if (j.contains("predictor")) {
            const auto& p = j.at("predictor");
            c.predictor.next = p.value("next", c.predictor.next);
            c.predictor.tag_bits = p.value("tag_bits", c.predictor.tag_bits);
            c.predictor.direction = p.value("direction", c.predictor.direction);
            c.predictor.history = p.value("history", c.predictor.history);
        }
        if (c.width == 0 || c.reads == 0 || c.writes == 0) throw BrainfunkException("issue: width, reads and writes are at least 1");
        return c;
    }

    [[nodiscard]] json to_json() const {
        static constexpr std::string_view MUL_NAMES[] = {"split", "pair-unit", "single"};
        return {{"name", name}, {"width", width}, {"reads", reads}, {"writes", writes}, {"branches", branches},
                {"raw", raw}, {"mul", MUL_NAMES[static_cast<int>(mul)]}, {"two_mul", two_mul},
                {"fetch_line", fetch_line}, {"banks", banks}, {"penalty", penalty}, {"steer", steer},
                {"scan_base", scan_base}, {"line_cells", line_cells}, {"stride_limit", stride_limit}, {"io", io},
                {"predictor", {{"next", predictor.next}, {"tag_bits", predictor.tag_bits},
                               {"direction", predictor.direction}, {"history", predictor.history}}}};
    }
};

/* Why an instruction did not join the group before it. */
enum Reason { JOINED, CLOSED, LATER, FULL, PORTS, BRANCHES, CONFLICT, DEPENDENCE, REASONS };
constexpr std::string_view REASON_NAMES[] = {"joined", "closed", "later", "full", "ports", "branches", "conflict", "dependence"};

class Machine {
public:
    explicit Machine(MachineConfig config) : c_(std::move(config)), predictor_(c_.predictor) {}

    void see(const TraceEvent& e, addr_t before, addr_t mask) {
        mask_ = mask;
        ++instructions_;
        const Bitcode& in = *e.instruction;
        const Opcode op = in.opcode();
        const addr_t target = e.branch ? e.pc + 1 + static_cast<addr_t>(in.jump_offset()) : 0;
        const auto guess = predictor_.see(e.pc, before, e.branch, e.taken, target);
        mispredicts_ += guess.mispredict;
        steers_ += guess.steer;
        pc_ = e.pc;
        const std::uint64_t at = ready_;

        const bool mul = op == Opcode::MUL || op == Opcode::MULC;
        bool chained = false, pair = false;
        if (mul) {
            const bool unit = in.data() == 1 || in.data() == 255;
            const addr_t src = e.read[0].cell, dst = e.read[1].cell;
            const bool conflict = c_.banks && src != dst && (src % c_.banks) == (dst % c_.banks);
            pair = c_.mul == MulMode::Single || (c_.mul == MulMode::PairUnit && unit && !conflict);
            chained = !pair && prev_split_mul_ && in.src_offset() == prev_src_offset_;
            prev_split_mul_ = !pair && src != dst && op == Opcode::MUL;
            prev_src_offset_ = in.src_offset();
            mul_unit_ = unit;
        } else {
            prev_split_mul_ = false;
        }

        if (op == Opcode::F || op == Opcode::IO || op == Opcode::H) {
            open(next_cycle(at));
            closed_ = true;
            ready_ = cycle_ + (op == Opcode::F ? scan_cost(e) : op == Opcode::IO ? c_.io : 1);
        } else if (op == Opcode::SRUN) {
            for (unsigned i = 0; i < e.fill; ++i) {
                place(Uop{.writes = 1, .write = (e.write[0].cell + i) & mask, .writer = WRITER_SET}, i ? cycle_ : at);
            }
            ready_ = cycle_;
        } else if (mul && !pair) {
            const unsigned gap = mul_unit_ ? 1 : 2;
            std::uint64_t dst_at = at;
            if (!chained) {
                place(Uop{.reads = 1, .read = {e.read[0].cell}, .reader = READER_SOURCE}, at);
                source_cycle_ = cycle_;
                dst_at = cycle_ + gap;
            } else {
                dst_at = std::max(at, source_cycle_ + gap);
                if (!c_.two_mul && !closed_ && slots_ > 0) dst_at = std::max(dst_at, cycle_ + 1);
            }
            place(Uop{.reads = 1, .read = {e.read[1].cell}, .writes = 1, .write = e.read[1].cell, .reader = READER_OTHER,
                      .writer = WRITER_OTHER}, dst_at);
            if (op == Opcode::MULC) place(Uop{.writes = 1, .write = e.read[0].cell, .writer = WRITER_SET}, cycle_);
            ready_ = cycle_;
        } else {
            Uop u;
            if (mul) {
                u.reads = chained ? 1 : 2;
                u.read = chained ? std::array<addr_t, 2>{e.read[1].cell, 0} : std::array<addr_t, 2>{e.read[0].cell, e.read[1].cell};
                u.writes = 1;
                u.write = e.read[1].cell;
                u.reader = READER_OTHER;
                u.writer = WRITER_OTHER;
            } else {
                u.reads = e.reads;
                u.read = {e.read[0].cell, e.read[1].cell};
                u.writes = e.writes ? 1 : 0;
                u.write = e.write[0].cell;
                u.branch = e.branch;
                u.reader = e.branch ? READER_BRANCH : op == Opcode::A ? READER_ADD : READER_OTHER;
                u.writer = op == Opcode::S ? WRITER_SET : (op == Opcode::A || op == Opcode::DJN) ? WRITER_ADD : WRITER_OTHER;
            }
            place(u, at);
            if (op == Opcode::MULC) place(Uop{.writes = 1, .write = e.read[0].cell, .writer = WRITER_SET}, cycle_);
            ready_ = cycle_;
        }

        if (e.branch && e.taken) closed_ = true;
        if (guess.mispredict) {
            closed_ = true;
            ready_ = cycle_ + 1 + c_.penalty;
        } else if (guess.steer) {
            closed_ = true;
            ready_ = cycle_ + 1 + c_.steer;
        }
    }

    [[nodiscard]] std::uint64_t cycles() const { return std::max(cycle_ + 1, ready_); }

    [[nodiscard]] json report(std::uint64_t baseline) const {
        json reasons = json::object();
        for (int r = CLOSED; r < REASONS; ++r) reasons[std::string(REASON_NAMES[r])] = share(reasons_[r], instructions_);
        const auto total = cycles();
        return {{"config", c_.to_json()}, {"cycles", total},
                {"cpi", share(total, instructions_)},
                {"speedup", baseline ? static_cast<double>(baseline) / static_cast<double>(total) : 1.0},
                {"groups", groups_}, {"joined", share(reasons_[JOINED], instructions_)},
                {"mispredicts", mispredicts_}, {"steers", steers_}, {"not_joined_because", reasons}};
    }

    [[nodiscard]] const MachineConfig& config() const { return c_; }

private:
    enum Reader : std::uint8_t { READER_ADD, READER_BRANCH, READER_SOURCE, READER_OTHER };
    enum Writer : std::uint8_t { WRITER_SET, WRITER_ADD, WRITER_OTHER };

    /* One slot of a group: a whole instruction, or one part of a MUL that takes several. */
    struct Uop {
        unsigned               reads = 0;
        std::array<addr_t, 2>  read{};
        unsigned               writes = 0;
        addr_t                 write = 0;
        bool                   branch = false;
        Reader                 reader = READER_OTHER;
        Writer                 writer = WRITER_OTHER;
    };

    std::uint64_t scan_cost(const TraceEvent& e) const {
        const auto stride = static_cast<std::int64_t>(e.instruction->step());
        const auto magnitude = static_cast<std::uint64_t>(stride < 0 ? -stride : stride);
        std::uint64_t lines = e.steps + 1;
        if (magnitude > 0 && magnitude < c_.stride_limit) {
            const auto first = static_cast<std::int64_t>(e.read[0].cell);
            const auto last = first + static_cast<std::int64_t>(e.steps) * stride;
            const auto cells = static_cast<std::int64_t>(c_.line_cells);
            const auto line = [&](std::int64_t cell) { return cell >= 0 ? cell / cells : (cell - cells + 1) / cells; };
            lines = static_cast<std::uint64_t>(std::abs(line(last) - line(first))) + 1;
        }
        return lines + c_.scan_base;
    }

    /* The first cycle a new group can have: the next one, or the first of all. */
    [[nodiscard]] std::uint64_t next_cycle(std::uint64_t at) const { return groups_ ? std::max(at, cycle_ + 1) : at; }

    void open(std::uint64_t at) {
        cycle_ = at;
        ++groups_;
        slots_ = reads_ = writes_ = branches_ = 0;
        closed_ = false;
        written_.clear();
        touched_.clear();
        first_pc_ = pc_;
    }

    bool conflicts(addr_t cell) const {
        if (!c_.banks) return false;
        for (const auto t : touched_)
            if (t != cell && t % c_.banks == cell % c_.banks && t / c_.banks != cell / c_.banks) return true;
        return false;
    }

    Reason why_not(const Uop& u, std::uint64_t at) const {
        if (closed_) return CLOSED;
        if (at > cycle_) return LATER;
        if (slots_ >= c_.width) return FULL;
        if (reads_ + u.reads > c_.reads || writes_ + u.writes > c_.writes) return PORTS;
        if (u.branch && branches_ >= c_.branches) return BRANCHES;
        if (c_.fetch_line && pc_ / c_.fetch_line != first_pc_ / c_.fetch_line) return CONFLICT;
        for (unsigned i = 0; i < u.reads; ++i) {
            for (const auto& [cell, writer] : written_) {
                if (cell != u.read[i]) continue;
                if (c_.raw == 0) return DEPENDENCE;
                /* A zero test compares with minus the addend, a constant folds in D, a MUL source
                 * is a register loaded from the adder, and two adds of constants fold in D. */
                const bool cheap = u.reader == READER_BRANCH || writer == WRITER_SET || u.reader == READER_SOURCE ||
                                   (u.reader == READER_ADD && writer == WRITER_ADD);
                if (c_.raw == 1 && !cheap) return DEPENDENCE;
            }
            if (conflicts(u.read[i])) return CONFLICT;
        }
        if (u.writes && conflicts(u.write)) return CONFLICT;
        return JOINED;
    }

    void place(const Uop& u, std::uint64_t at) {
        const Reason why = why_not(u, at);
        ++reasons_[why];
        if (why != JOINED) open(next_cycle(at));
        ++slots_;
        reads_ += u.reads;
        for (unsigned i = 0; i < u.reads; ++i) touched_.push_back(u.read[i]);
        if (u.writes) {
            ++writes_;
            written_.emplace_back(u.write, u.writer);
            touched_.push_back(u.write);
        }
        if (u.branch) ++branches_;
    }

    MachineConfig c_;
    Predictor     predictor_;
    addr_t        mask_ = 0, pc_ = 0, first_pc_ = 0;

    std::uint64_t cycle_ = 0, ready_ = 0, source_cycle_ = 0;
    bool          closed_ = true;
    unsigned      slots_ = 0, reads_ = 0, writes_ = 0, branches_ = 0;
    std::vector<std::pair<addr_t, Writer>> written_;
    std::vector<addr_t>                    touched_;

    bool     prev_split_mul_ = false, mul_unit_ = false;
    offset_t prev_src_offset_ = 0;

    std::uint64_t instructions_ = 0, groups_ = 0, mispredicts_ = 0, steers_ = 0;
    std::array<std::uint64_t, REASONS> reasons_{};
};

class IssueAnalysis final : public Analysis {
public:
    explicit IssueAnalysis(const json& config) {
        const auto machines = config.value("machines", json::array({json{{"name", "scalar"}},
                                                                     json{{"name", "dual"}, {"width", 2}, {"penalty", 10}}}));
        for (const auto& m : machines) machines_.emplace_back(MachineConfig::from(m));
        if (machines_.empty()) throw BrainfunkException("issue: no machines");
    }
    void begin(const Brainfunk& bf) override { mask_ = bf.memory().size() - 1; }
    void event(const TraceEvent& e) override {
        for (auto& m : machines_) m.see(e, before_, mask_);
        before_ = e.pc;
    }
    [[nodiscard]] json report() const override {
        json out = json::array();
        const auto baseline = machines_.front().cycles();
        for (const auto& m : machines_) {
            auto row = m.report(baseline);
            row["name"] = m.config().name;
            out.push_back(row);
        }
        return {{"machines", out}, {"baseline", machines_.front().config().name}};
    }

private:
    std::vector<Machine> machines_;
    addr_t               before_ = 0, mask_ = 0;
};

} // namespace

std::vector<std::string> analysis_names() { return {"stat", "profile", "ngram", "deps", "values", "issue"}; }

std::unique_ptr<Analysis> make_analysis(std::string_view name, const json& config) {
    if (name == "stat") return std::make_unique<StatAnalysis>();
    if (name == "profile") return std::make_unique<ProfileAnalysis>(config);
    if (name == "ngram") return std::make_unique<NgramAnalysis>(config);
    if (name == "deps") return std::make_unique<DepsAnalysis>();
    if (name == "values") return std::make_unique<ValuesAnalysis>();
    if (name == "issue") return std::make_unique<IssueAnalysis>(config);
    throw BrainfunkException("no analysis is called " + std::string(name));
}

AnalysisSet::AnalysisSet(std::string_view names, const json& config) {
    std::size_t start = 0;
    while (start <= names.size()) {
        const auto comma = names.find(',', start);
        const auto name = std::string(names.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!name.empty()) {
            const json& part = config.is_object() && config.contains(name) ? config.at(name) : json::object();
            analyses_.emplace_back(name, make_analysis(name, part));
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    if (analyses_.empty()) throw BrainfunkException("no analyses named");
}

void AnalysisSet::begin(const Brainfunk& bf) {
    for (auto& [name, a] : analyses_) a->begin(bf);
}

void AnalysisSet::event(const TraceEvent& e) {
    for (auto& [name, a] : analyses_) a->event(e);
}

void AnalysisSet::end(const Brainfunk& bf, const TraceSummary& summary) {
    for (auto& [name, a] : analyses_) a->end(bf, summary);
    summary_ = {{"executed", summary.executed}, {"reported", summary.reported}, {"halted", summary.halted},
                {"trapped", summary.trapped}, {"endless", summary.endless}};
}

json AnalysisSet::report() const {
    json out = json::object();
    for (const auto& [name, a] : analyses_) out[name] = a->report();
    return {{"run", summary_}, {"analyses", out}};
}
