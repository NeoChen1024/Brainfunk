#include "brainfunk/analysis.hpp"
#include "brainfunk/trace_file.hpp"
#include "brainfunk/coverage.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <deque>
#include <memory>
#include <optional>
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
        if (counts_) flow_.begin(bf);
    }
    void end(const Brainfunk& bf, const TraceSummary& summary) override {
        if (counts_) flow_.end(bf, summary);
    }
    void event(const TraceEvent& e) override {
        if (counts_) flow_.event(e);
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
        if (counts_) out["counters"] = flow_.coverage().to_json();
        return out;
    }

private:
    static bool is_branch(const Bitcode& b) {
        return opcode_layout(b.opcode()) == OperandLayout::Branch || opcode_layout(b.opcode()) == OperandLayout::MoveBranch;
    }

    std::size_t                top_;
    bool                       counts_;
    CoverageSink               flow_;
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

/* How branches are guessed, in two parts. Fetch has a next-fetch table keyed by the word fetched
 * before, which the word fetched before is taken to be the one run before: a hit sends fetch to the
 * target with no gap. D has a direction predictor, and a taken guess that fetch did not make costs
 * `steer` cycles to send fetch there; a wrong direction costs `penalty`.
 *
 * Kind `rtl` is rtl/brainfunk_predict.sv as it is: a next-fetch hit is a taken guess that D does not
 * question, and the direction counters are read only on a miss. Every other kind overrides fetch:
 * its direction guess stands, and undoing a hit that it does not agree with is a steer. */

constexpr std::string_view PREDICTOR_KINDS = "rtl, static, bimodal, gshare, local, loop, tournament or perfect";

/* A predictor's fields with every default filled in, so that a report says what ran. */
json predictor_spec(const json& j, bool top) {
    if (!j.is_object()) throw BrainfunkException("issue: a predictor is an object");
    const std::string kind = j.value("kind", std::string(top ? "rtl" : "bimodal"));
    json s{{"kind", kind}};
    const auto field = [&](const char* name, unsigned value) { s[name] = j.value(name, value); };
    if (kind == "rtl") {
        field("direction", 4096);
        field("history", 0);
    } else if (kind == "static" || kind == "perfect") {
    } else if (kind == "bimodal") {
        field("direction", 4096);
    } else if (kind == "gshare") {
        field("direction", 4096);
        field("history", 12);
    } else if (kind == "local") {
        field("histories", 256);
        field("history", 6);
        field("direction", 16384);
    } else if (kind == "loop") {
        field("entries", 64);
        field("confidence", 2);
        s["base"] = predictor_spec(j.value("base", json::object()), false);
    } else if (kind == "tournament") {
        const auto parts = j.value("components", json::array({json{{"kind", "local"}}, json{{"kind", "gshare"}}}));
        if (!parts.is_array() || parts.size() != 2) throw BrainfunkException("issue: a tournament has two components");
        s["components"] = json::array({predictor_spec(parts[0], false), predictor_spec(parts[1], false)});
        field("chooser", 4096);
        field("chooser_history", 0);
    } else {
        throw BrainfunkException("issue: a predictor's kind is " + std::string(PREDICTOR_KINDS) + ", not " + kind);
    }
    if (top) {
        field("next", 16);
        field("tag_bits", 8);
        s["forget"] = j.value("forget", kind == "rtl");
        s["next_counter"] = j.value("next_counter", false);
        field("lookup", 0);
        field("update", 0);
        if (kind == "rtl" && s["next_counter"].get<bool>()) throw BrainfunkException("issue: rtl's next-fetch table has no counters");
    } else if (kind == "rtl" || kind == "perfect") {
        throw BrainfunkException("issue: " + kind + " is not a component");
    }
    return s;
}

unsigned table_size(const json& s, const char* name) {
    const auto n = s.at(name).get<unsigned>();
    if (!std::has_single_bit(n)) throw BrainfunkException(std::string("issue: predictor ") + name + " must be a power of two");
    return n;
}

/* 2-bit counters of how far branches disagree with their static guess, backward taken, so that a
 * table of zeros starts out as that guess. */
class Counters {
public:
    explicit Counters(unsigned n) : c_(n, 0), mask_(n - 1) {}
    [[nodiscard]] bool predict(std::size_t i, bool backward) const { return ((c_[i & mask_] >> 1) & 1) != backward; }
    void update(std::size_t i, bool backward, bool taken) {
        auto& c = c_[i & mask_];
        c = taken == backward ? (c ? c - 1 : 0) : (c < 3 ? c + 1 : 3);
    }

private:
    std::vector<std::uint8_t> c_;
    std::size_t               mask_;
};

/* What a guess looked up, kept until its branch resolves to update the same entries, as the core
 * carries table indices down its pipeline. Each predictor has words of its own in it. */
struct Ticket {
    static constexpr unsigned WORDS = 16;
    std::array<std::uint32_t, WORDS> w{};
};

/* A direction guess at D. Every branch is predicted; then its outcome comes back twice: to global
 * histories as soon as it is guessed, since the core shifts its guess in and repairs the history
 * after a misprediction, so that on the right path it holds outcomes; and to counters and per-branch
 * state once it resolves. */
class Direction {
public:
    virtual ~Direction() = default;
    virtual bool predict(addr_t pc, bool backward, Ticket& t) = 0;
    virtual void speculate(bool taken) { (void)taken; }
    virtual void resolve(addr_t pc, bool backward, bool taken, const Ticket& t) = 0;
};

std::unique_ptr<Direction> make_direction(const json& s, unsigned& words);

unsigned claim(unsigned& words, unsigned n) {
    if (words + n > Ticket::WORDS) throw BrainfunkException("issue: predictor nested too deep");
    words += n;
    return words - n;
}

addr_t shifted(addr_t history, bool taken, unsigned bits) {
    return bits ? ((history << 1) | (taken ? 1 : 0)) & ((addr_t{1} << bits) - 1) : 0;
}

class StaticDirection final : public Direction {
public:
    bool predict(addr_t, bool backward, Ticket&) override { return backward; }
    void resolve(addr_t, bool, bool, const Ticket&) override {}
};

class Bimodal final : public Direction {
public:
    explicit Bimodal(const json& s) : counters_(table_size(s, "direction")) {}
    bool predict(addr_t pc, bool backward, Ticket&) override { return counters_.predict(pc, backward); }
    void resolve(addr_t pc, bool backward, bool taken, const Ticket&) override { counters_.update(pc, backward, taken); }

private:
    Counters counters_;
};

/* Counters indexed by the pc and the last `history` branch outcomes, exclusive-ored. */
class Gshare final : public Direction {
public:
    Gshare(const json& s, unsigned& words)
        : counters_(table_size(s, "direction")), bits_(s.at("history").get<unsigned>()), slot_(claim(words, 1)) {}
    bool predict(addr_t pc, bool backward, Ticket& t) override {
        t.w[slot_] = pc ^ history_;
        return counters_.predict(t.w[slot_], backward);
    }
    void speculate(bool taken) override { history_ = shifted(history_, taken, bits_); }
    void resolve(addr_t, bool backward, bool taken, const Ticket& t) override { counters_.update(t.w[slot_], backward, taken); }

private:
    Counters counters_;
    unsigned bits_, slot_;
    addr_t   history_ = 0;
};

/* Each branch's own last `history` outcomes, from a table of `histories` by pc, next to its pc
 * bits, index counters: the trip counts of short loops, and patterns that repeat. Histories take
 * outcomes as branches resolve. */
class Local final : public Direction {
public:
    Local(const json& s, unsigned& words)
        : histories_(table_size(s, "histories"), 0), counters_(table_size(s, "direction")),
          bits_(s.at("history").get<unsigned>()), slot_(claim(words, 1)) {}
    bool predict(addr_t pc, bool backward, Ticket& t) override {
        t.w[slot_] = static_cast<std::uint32_t>((static_cast<std::size_t>(pc) << bits_) | slot(pc));
        return counters_.predict(t.w[slot_], backward);
    }
    void resolve(addr_t pc, bool backward, bool taken, const Ticket& t) override {
        counters_.update(t.w[slot_], backward, taken);
        slot(pc) = shifted(slot(pc), taken, bits_);
    }

private:
    addr_t& slot(addr_t pc) { return histories_[pc & (histories_.size() - 1)]; }
    std::vector<addr_t> histories_;
    Counters            counters_;
    unsigned            bits_, slot_;
};

/* Counts a backward branch's taken runs: once a loop has gone round the same number of times
 * `confidence` times in a row, it guesses the exit. Otherwise its base guesses. */
class Loop final : public Direction {
public:
    Loop(const json& s, unsigned& words)
        : entries_(table_size(s, "entries")), confidence_(s.at("confidence").get<unsigned>()), base_(make_direction(s.at("base"), words)) {}
    bool predict(addr_t pc, bool backward, Ticket& t) override {
        const bool base = base_->predict(pc, backward, t);
        const Entry& e = entries_[pc & (entries_.size() - 1)];
        if (!backward || !e.valid || e.pc != pc || e.confidence < confidence_) return base;
        return e.iterations != e.trip;
    }
    void speculate(bool taken) override { base_->speculate(taken); }
    void resolve(addr_t pc, bool backward, bool taken, const Ticket& t) override {
        base_->resolve(pc, backward, taken, t);
        if (!backward) return;
        Entry& e = entries_[pc & (entries_.size() - 1)];
        if (!e.valid || e.pc != pc) {
            /* A loop that keeps its count is not given up for another. */
            if (e.valid && e.confidence >= confidence_ && --e.confidence) return;
            e = Entry{.valid = true, .pc = pc};
            e.trip = UNKNOWN;
        }
        if (taken) {
            ++e.iterations;
            return;
        }
        if (e.iterations == e.trip) {
            e.confidence = std::min(e.confidence + 1, 7u);
        } else {
            e.trip = e.iterations;
            e.confidence = 0;
        }
        e.iterations = 0;
    }

private:
    static constexpr std::uint64_t UNKNOWN = std::numeric_limits<std::uint64_t>::max();
    struct Entry {
        bool          valid = false;
        addr_t        pc = 0;
        std::uint64_t trip = 0, iterations = 0;
        unsigned      confidence = 0;
    };
    std::vector<Entry>         entries_;
    unsigned                   confidence_;
    std::unique_ptr<Direction> base_;
};

/* Two predictors, and 2-bit counters by pc, or pc and global history, of which to believe. */
class Tournament final : public Direction {
public:
    Tournament(const json& s, unsigned& words)
        : slot_(claim(words, 2)), first_(make_direction(s.at("components")[0], words)),
          second_(make_direction(s.at("components")[1], words)), chooser_(table_size(s, "chooser"), 1),
          bits_(s.at("chooser_history").get<unsigned>()) {}
    bool predict(addr_t pc, bool backward, Ticket& t) override {
        const bool a = first_->predict(pc, backward, t), b = second_->predict(pc, backward, t);
        t.w[slot_] = (pc ^ history_) & (chooser_.size() - 1);
        t.w[slot_ + 1] = (a ? 1u : 0u) | (b ? 2u : 0u);
        return chooser_[t.w[slot_]] >= 2 ? b : a;
    }
    void speculate(bool taken) override {
        history_ = shifted(history_, taken, bits_);
        first_->speculate(taken);
        second_->speculate(taken);
    }
    void resolve(addr_t pc, bool backward, bool taken, const Ticket& t) override {
        const bool a = t.w[slot_ + 1] & 1u, b = t.w[slot_ + 1] & 2u;
        if (a != b) {
            auto& c = chooser_[t.w[slot_]];
            c = b == taken ? (c < 3 ? c + 1 : 3) : (c ? c - 1 : 0);
        }
        first_->resolve(pc, backward, taken, t);
        second_->resolve(pc, backward, taken, t);
    }

private:
    unsigned                   slot_;
    std::unique_ptr<Direction> first_, second_;
    std::vector<std::uint8_t>  chooser_;
    unsigned                   bits_;
    addr_t                     history_ = 0;
};

std::unique_ptr<Direction> make_direction(const json& s, unsigned& words) {
    const auto kind = s.at("kind").get<std::string>();
    if (kind == "static") return std::make_unique<StaticDirection>();
    if (kind == "bimodal") return std::make_unique<Bimodal>(s);
    if (kind == "gshare" || kind == "rtl") return std::make_unique<Gshare>(s, words);
    if (kind == "local") return std::make_unique<Local>(s, words);
    if (kind == "loop") return std::make_unique<Loop>(s, words);
    if (kind == "tournament") return std::make_unique<Tournament>(s, words);
    throw BrainfunkException("issue: no direction predictor " + kind);
}

class Predictor {
public:
    explicit Predictor(const json& spec)
        : kind_(spec.at("kind").get<std::string>()), table_(table_size(spec, "next")),
          tag_bits_(spec.at("tag_bits").get<unsigned>()), forget_(spec.at("forget").get<bool>()),
          next_counter_(spec.at("next_counter").get<bool>()),
          shift_(static_cast<unsigned>(std::countr_zero(spec.at("next").get<unsigned>()))),
          lookup_(spec.at("lookup").get<unsigned>()), update_(spec.at("update").get<unsigned>()) {
        unsigned words = 0;
        if (kind_ != "perfect") direction_ = make_direction(spec, words);
    }

    /* Why decode sent fetch elsewhere. */
    enum Steer { NO_STEER, TAKEN_MISS, WRONG_TARGET, NOT_TAKEN_HIT, NOT_A_BRANCH, STEERS };
    static constexpr std::string_view STEER_NAMES[] = {"", "taken_miss", "wrong_target", "not_taken_hit", "not_a_branch"};

    struct Guess {
        bool  mispredict = false, steer = false;
        Steer why = NO_STEER;
    };

    /* `issue` is the cycle the word issues from A. */
    Guess see(addr_t pc, addr_t before, bool branch, bool taken, addr_t target, std::uint64_t issue) {
        Guess g;
        Entry& entry = table_[before & (table_.size() - 1)];
        const std::uint32_t tag = static_cast<std::uint32_t>(before >> shift_) & ((1u << tag_bits_) - 1);
        const bool matches = entry.valid && entry.tag == tag;
        const bool hit = matches && (!next_counter_ || entry.counter >= 2);   // fetch goes to the target
        if (!branch) {
            /* Fetch went somewhere a word that is not a branch does not go. */
            if (hit) {
                g.steer = true;
                g.why = NOT_A_BRANCH;
            }
            if (matches) entry.valid = false;
            return g;
        }
        const bool backward = target <= pc;
        const bool guess = direction_ ? direct(pc, backward, taken, issue) : taken;
        if (kind_ == "rtl") {
            /* A hit is taken, unquestioned; the counters guess only on a miss. */
            if (hit) {
                if (!taken || entry.target != target) {
                    g.mispredict = true;
                    if (forget_ || taken) entry.valid = false;
                }
            } else {
                if (guess) {
                    g.steer = true;
                    g.why = TAKEN_MISS;
                    entry = {true, tag, target};
                }
                g.mispredict = guess != taken;
            }
            if (g.mispredict) {
                g.steer = false;
                g.why = NO_STEER;
            }
            return g;
        }

        g.mispredict = guess != taken;
        if (!g.mispredict) {
            /* D sends fetch where it guessed, unless fetch went there. */
            g.why = taken ? (!hit ? TAKEN_MISS : entry.target != target ? WRONG_TARGET : NO_STEER) : hit ? NOT_TAKEN_HIT : NO_STEER;
            g.steer = g.why != NO_STEER;
        }
        if (taken) {
            entry = matches ? Entry{true, tag, target, static_cast<std::uint8_t>(std::min(entry.counter + 1, 3))}
                            : Entry{true, tag, target, 2};
        } else if (matches) {
            if (next_counter_ && entry.counter) --entry.counter;
            if (forget_ && (!next_counter_ || entry.counter == 0)) entry.valid = false;
        }
        return g;
    }

private:
    /* The direction guess, from tables read `lookup` cycles before the branch issues, which hold
     * every outcome that came back by then: a global history's at issue, a counter's `update`
     * cycles after. With both 0, every outcome is back by the next branch. */
    bool direct(addr_t pc, bool backward, bool taken, std::uint64_t issue) {
        Ticket t;
        if (!lookup_ && !update_) {
            const bool guess = direction_->predict(pc, backward, t);
            direction_->speculate(taken);
            direction_->resolve(pc, backward, taken, t);
            return guess;
        }
        const std::uint64_t read = issue > lookup_ ? issue - lookup_ : 0;
        while (!histories_.empty() && histories_.front().first <= read) {
            direction_->speculate(histories_.front().second);
            histories_.pop_front();
        }
        while (!resolving_.empty() && resolving_.front().at <= read) {
            const auto& r = resolving_.front();
            direction_->resolve(r.pc, r.backward, r.taken, r.ticket);
            resolving_.pop_front();
        }
        const bool guess = direction_->predict(pc, backward, t);
        histories_.emplace_back(issue, taken);
        resolving_.push_back({issue + update_, pc, backward, taken, t});
        return guess;
    }

    struct Entry {
        bool          valid = false;
        std::uint32_t tag = 0;
        addr_t        target = 0;
        std::uint8_t  counter = 2;   // with next_counter, taken at 2 or more
    };
    struct Resolving {
        std::uint64_t at;
        addr_t        pc;
        bool          backward, taken;
        Ticket        ticket;
    };
    std::string                kind_;
    std::vector<Entry>         table_;
    unsigned                   tag_bits_;
    bool                       forget_, next_counter_;
    unsigned                   shift_, lookup_, update_;
    std::unique_ptr<Direction> direction_;   // none for perfect
    std::deque<std::pair<std::uint64_t, bool>> histories_;
    std::deque<Resolving>                      resolving_;
};

enum class MulMode { Split, PairUnit, Single };
enum class Forward { None, Set, Add, Mul };
constexpr std::string_view FORWARD_NAMES[] = {"none", "set", "add", "mul"};

/* The memories, as the core has them; doc/analysis.md says what each field means. The tape is on chip, and
 * the code is cached; either can be either. Caches are direct-mapped and share the SDRAM, which moves a line
 * at a time and refreshes on its own. */
struct MemoryConfig {
    bool     on = true;
    unsigned i_lines = 512, i_words = 8;     // instruction cache: lines, and words in a line
    unsigned d_lines = 512, d_cells = 32;    // data cache: lines, and cells in a line; written back
    unsigned d_on_chip = 1, i_on_chip = 0;   // 1: the tape, or the code, in block RAM, which never misses
    unsigned zero_table = 1;                 // 1: F takes a missing line's zero bits from a table, and never misses
    unsigned read_latency = 15;              // SDRAM: a line read, from the request to its last word
    unsigned read_busy = 17, write_busy = 19;   // and until the next request, after a read or a write
    unsigned refresh_every = 1170, refresh_busy = 14;
    unsigned fetch_lead = 5;                 // a word's fetch to its issue
    unsigned i_request = 3, i_resume = 12;   // a fetch that misses to the request; the last word to the issue
    unsigned d_request = 7, d_resume = 7;    // an issue that misses to the request; the last word to the issue again

    static MemoryConfig from(const json& j) {
        MemoryConfig m;
        if (j.is_boolean()) {
            m.on = j.get<bool>();
            return m;
        }
        if (j.is_null()) {
            m.on = false;
            return m;
        }
        if (!j.is_object()) throw BrainfunkException("issue: memory is an object, true, false or null");
        for (const auto& [key, value] : j.items()) {
            if (!field(m, key)) throw BrainfunkException("issue: memory has no field " + key);
            *field(m, key) = value.get<unsigned>();
        }
        if (!m.i_lines || !m.i_words || !m.d_lines || !m.d_cells) throw BrainfunkException("issue: memory sizes are at least 1");
        return m;
    }

    [[nodiscard]] json to_json() const {
        if (!on) return false;
        json out = json::object();
        for (const auto* name : NAMES) out[name] = *field(const_cast<MemoryConfig&>(*this), name);
        return out;
    }

private:
    static constexpr const char* NAMES[] = {"i_lines", "i_words", "d_lines", "d_cells", "d_on_chip", "i_on_chip", "zero_table", "read_latency", "read_busy",
                                            "write_busy", "refresh_every", "refresh_busy", "fetch_lead", "i_request",
                                            "i_resume", "d_request", "d_resume"};
    static unsigned* field(MemoryConfig& m, std::string_view name) {
        unsigned* const fields[] = {&m.i_lines, &m.i_words, &m.d_lines, &m.d_cells, &m.d_on_chip, &m.i_on_chip, &m.zero_table, &m.read_latency, &m.read_busy,
                                    &m.write_busy, &m.refresh_every, &m.refresh_busy, &m.fetch_lead, &m.i_request,
                                    &m.i_resume, &m.d_request, &m.d_resume};
        for (std::size_t i = 0; i < std::size(NAMES); ++i) if (name == NAMES[i]) return fields[i];
        return nullptr;
    }
};

/* An in-order core that issues up to `width` instructions a cycle. The defaults are the scalar core in
 * rtl/ as it is; doc/analysis.md says what each field means. */
struct MachineConfig {
    std::string     name = "machine";
    unsigned        width = 1, reads = 2, writes = 2, branches = 1;
    unsigned        raw = 1;            // same-cycle reads of a cell written in the group: 0 never, 1 cheap cases, 2 always
    MulMode         mul = MulMode::PairUnit;
    bool            two_mul = true;     // two MUL destinations in a cycle
    bool            share_source = false;   // a split MUL after one with the same source takes it from a register
    unsigned        fetch_line = 0;     // a group stays within an aligned run of this many words; 0 any
    bool            pair_simple = false;  // only A, S, M, JE and JN share a group
    bool            branch_first = false; // a branch only opens a group
    bool            branch_last = false;  // a branch closes its group
    bool            pair_disjoint = false; // two in a group touch different cells
    unsigned        write_banks = 0;      // two writes in a group are to cells different modulo this; 0 any
    bool            early_second = true;  // a branch second in its group may resolve early
    bool            early_store = true;   // with early_cells, a store of values; else only the zero bits
    unsigned        banks = 16;         // cells in a line of banks: a MUL's two cells in one bank are not read at once; 0 none
    unsigned        penalty = 9, steer = 3;
    unsigned        scan_base = 8, line_cells = 32, stride_limit = 16, io = 7;
    /* Branches resolved before X: once the cell's value is known, a misprediction costs early_penalty
     * (0: never early) plus the cycles until it is. Values are known `late` cycles after their writer
     * issues, at X, unless `forward` works them out in D: constants, or adds of constants too. Values
     * come from a direct-mapped store of `early_cells` cells, or from the whole tape if 0, wrong paths
     * aside. Every cell touched fills its slot, a cell read only once the D-cache has it, at X; a
     * branch whose cell is not there resolves at X. With early_flush, a misprediction empties the
     * store, since the wrong path may have written it. With early_zero_bits, a branch
     * whose cell is not in the store tests a bit per cell of the whole tape instead, written in order
     * as values are known at X. */
    unsigned        early_penalty = 0, late = 4, early_cells = 4;
    unsigned        zero_bits_penalty = 0;   // for a branch the zero bits resolve; 0 as early_penalty
    bool            early_flush = true, early_zero_bits = true;
    bool            early_partial = false;   // a branch whose value comes later resolves then; else at X
    Forward         forward = Forward::Add;   // these early fields are what tag rtl-dual-issue built, once early_penalty is set
    json            predictor = predictor_spec(json::object(), true);
    MemoryConfig    memory;

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
        c.share_source = j.value("share_source", c.share_source);
        c.fetch_line = j.value("fetch_line", c.fetch_line);
        c.pair_simple = j.value("pair_simple", c.pair_simple);
        c.branch_first = j.value("branch_first", c.branch_first);
        c.branch_last = j.value("branch_last", c.branch_last);
        c.pair_disjoint = j.value("pair_disjoint", c.pair_disjoint);
        c.write_banks = j.value("write_banks", c.write_banks);
        c.early_second = j.value("early_second", c.early_second);
        c.early_store = j.value("early_store", c.early_store);
        c.banks = j.value("banks", c.banks);
        c.penalty = j.value("penalty", c.penalty);
        c.steer = j.value("steer", c.steer);
        c.scan_base = j.value("scan_base", c.scan_base);
        c.line_cells = j.value("line_cells", c.line_cells);
        c.stride_limit = j.value("stride_limit", c.stride_limit);
        c.io = j.value("io", c.io);
        c.early_penalty = j.value("early_penalty", c.early_penalty);
        c.late = j.value("late", c.late);
        c.early_cells = j.value("early_cells", c.early_cells);
        c.early_flush = j.value("early_flush", c.early_flush);
        c.early_zero_bits = j.value("early_zero_bits", c.early_zero_bits);
        c.early_partial = j.value("early_partial", c.early_partial);
        c.zero_bits_penalty = j.value("zero_bits_penalty", c.zero_bits_penalty);
        if (c.early_cells && !std::has_single_bit(c.early_cells)) throw BrainfunkException("issue: early_cells must be a power of two");
        const auto forward = j.value("forward", std::string(FORWARD_NAMES[static_cast<int>(c.forward)]));
        const auto known = std::find(std::begin(FORWARD_NAMES), std::end(FORWARD_NAMES), forward);
        if (known == std::end(FORWARD_NAMES)) throw BrainfunkException("issue: forward is none, set, add or mul, not " + forward);
        c.forward = static_cast<Forward>(known - std::begin(FORWARD_NAMES));
        c.predictor = predictor_spec(j.value("predictor", json::object()), true);
        if (j.contains("memory")) c.memory = MemoryConfig::from(j.at("memory"));
        if (c.width == 0 || c.reads == 0 || c.writes == 0) throw BrainfunkException("issue: width, reads and writes are at least 1");
        return c;
    }

    [[nodiscard]] json to_json() const {
        static constexpr std::string_view MUL_NAMES[] = {"split", "pair-unit", "single"};
        return {{"name", name}, {"width", width}, {"reads", reads}, {"writes", writes}, {"branches", branches},
                {"raw", raw}, {"mul", MUL_NAMES[static_cast<int>(mul)]}, {"two_mul", two_mul}, {"share_source", share_source},
                {"fetch_line", fetch_line}, {"pair_simple", pair_simple}, {"branch_first", branch_first},
                {"branch_last", branch_last}, {"pair_disjoint", pair_disjoint}, {"write_banks", write_banks},
                {"early_second", early_second}, {"early_store", early_store}, {"banks", banks}, {"penalty", penalty}, {"steer", steer},
                {"scan_base", scan_base}, {"line_cells", line_cells}, {"stride_limit", stride_limit}, {"io", io},
                {"early_penalty", early_penalty}, {"late", late}, {"early_cells", early_cells}, {"early_flush", early_flush}, {"early_zero_bits", early_zero_bits}, {"early_partial", early_partial}, {"zero_bits_penalty", zero_bits_penalty}, {"forward", FORWARD_NAMES[static_cast<int>(forward)]},
                {"predictor", predictor}, {"memory", memory.to_json()}};
    }
};

/* Why an instruction did not join the group before it. */
enum Reason { JOINED, CLOSED, LATER, FULL, PORTS, BRANCHES, CONFLICT, DEPENDENCE, KIND, REASONS };
constexpr std::string_view REASON_NAMES[] = {"joined", "closed", "later", "full", "ports", "branches", "conflict", "dependence", "kind"};

/* MemoryConfig for one machine: when an instruction whose word or cells miss can go on. The SDRAM
 * takes the misses in the order they come, between its refreshes. */
class MemorySystem {
public:
    explicit MemorySystem(const MemoryConfig& c)
        : c_(c), i_tags_(c.i_lines, NONE), d_tags_(c.d_lines, NONE), d_dirty_(c.d_lines, false),
          next_refresh_(c.refresh_every) {}

    /* The word at pc, which would issue at `at` from the cache: when it can. A miss holds fetch, which
     * was `fetch_lead` cycles ahead of issue, until the line is in. */
    std::uint64_t fetch(addr_t pc, std::uint64_t at) {
        if (c_.i_on_chip) return at;
        const std::uint64_t line = pc / c_.i_words;
        auto& tag = i_tags_[line % c_.i_lines];
        if (tag == line) return at;
        tag = line;
        ++i_misses_;
        const auto fetched = at > c_.fetch_lead ? at - c_.fetch_lead : 0;
        const auto ready = std::max(at, read(fetched + c_.i_request) + c_.i_resume);
        i_cycles_ += ready - at;
        return ready;
    }

    /* A cell of an instruction that would issue at `at`: when it can. A miss is found at X and
     * restarts the instruction, `restart` cycles at least, once the line is in, after writing back the
     * one it replaces if that was written. */
    std::uint64_t data(addr_t cell, bool write, std::uint64_t at, unsigned restart) {
        if (c_.d_on_chip) return at;
        const std::uint64_t line = cell / c_.d_cells;
        const auto index = line % c_.d_lines;
        std::uint64_t ready = at;
        if (d_tags_[index] != line) {
            ++d_misses_;
            auto request = at + c_.d_request;
            if (d_tags_[index] != NONE && d_dirty_[index]) {
                ++writebacks_;
                request = write_back(request);
            }
            ready = std::max(at + 1 + restart, read(request) + c_.d_resume);
            d_tags_[index] = line;
            d_dirty_[index] = false;
            d_cycles_ += ready - at;
        }
        if (write) d_dirty_[index] = true;
        return ready;
    }

    [[nodiscard]] json report() const {
        return {{"i_misses", i_misses_}, {"d_misses", d_misses_}, {"writebacks", writebacks_},
                {"i_cycles", i_cycles_}, {"d_cycles", d_cycles_}, {"refreshes", refreshes_}};
    }

private:
    /* When the SDRAM takes a request made at t: once it is free, and has refreshed if that was due. */
    std::uint64_t start(std::uint64_t t) {
        t = std::max(t, free_);
        while (c_.refresh_every && t >= next_refresh_) {
            const auto at = std::max(next_refresh_, free_);
            free_ = at + c_.refresh_busy;
            next_refresh_ = at + c_.refresh_every;
            t = std::max(t, free_);
            ++refreshes_;
        }
        return t;
    }
    /* A line read: when its last word is in. */
    std::uint64_t read(std::uint64_t t) {
        const auto s = start(t);
        free_ = s + c_.read_busy;
        return s + c_.read_latency;
    }
    /* A line written back: when the next request can be made, which waits for the SDRAM anyway. */
    std::uint64_t write_back(std::uint64_t t) {
        const auto s = start(t);
        free_ = s + c_.write_busy;
        return s;
    }

    static constexpr std::uint64_t NONE = std::numeric_limits<std::uint64_t>::max();
    MemoryConfig               c_;
    std::vector<std::uint64_t> i_tags_, d_tags_;   // the line each holds
    std::vector<bool>          d_dirty_;
    std::uint64_t              free_ = 0, next_refresh_ = 0;
    std::uint64_t              i_misses_ = 0, d_misses_ = 0, writebacks_ = 0, i_cycles_ = 0, d_cycles_ = 0, refreshes_ = 0;
};

class Machine {
public:
    explicit Machine(MachineConfig config) : c_(std::move(config)), predictor_(c_.predictor) {
        if (c_.memory.on) memory_.emplace(c_.memory);
    }

    void see(const TraceEvent& e, addr_t before, addr_t mask) {
        mask_ = mask;
        ++instructions_;
        const Bitcode& in = *e.instruction;
        const Opcode op = in.opcode();
        const addr_t target = e.branch ? e.pc + 1 + static_cast<addr_t>(in.jump_offset()) : 0;
        pc_ = e.pc;
        std::uint64_t at = ready_;
        std::uint64_t scan_misses = 0;   // what F's reads wait for the data cache
        if (memory_) {
            at = memory_->fetch(e.pc, at);
            if (op == Opcode::F && c_.memory.zero_table) {
                /* A line a cycle, from the cache or else the zero table: nothing to wait for. */
            } else if (op == Opcode::F) {
                /* A line a cycle; a miss restarts F from that line once it is in. */
                const auto stride = static_cast<std::int64_t>(in.step());
                std::uint64_t t = at, last = std::numeric_limits<std::uint64_t>::max();
                for (std::uint64_t i = 0; i <= e.steps; ++i) {
                    const auto cell = static_cast<addr_t>((static_cast<std::int64_t>(e.read[0].cell) + static_cast<std::int64_t>(i) * stride)) & mask;
                    const std::uint64_t line = cell / c_.memory.d_cells;
                    if (line == last) continue;
                    last = line;
                    const auto ready = memory_->data(cell, false, t, c_.penalty);
                    scan_misses += ready - t;
                    t = ready + 1;
                }
            } else {
                for (unsigned i = 0; i < e.reads; ++i) at = memory_->data(e.read[i].cell, false, at, c_.penalty);
                for (unsigned i = 0; i < e.writes; ++i) at = memory_->data(e.write[i].cell, true, at, c_.penalty);
                for (unsigned i = 1; i < e.fill; ++i) at = memory_->data((e.write[0].cell + i) & mask, true, at, c_.penalty);
            }
        }

        const bool mul = op == Opcode::MUL || op == Opcode::MULC;
        bool chained = false, pair = false;
        if (mul) {
            const bool unit = in.data() == 1 || in.data() == 255;
            const addr_t src = e.read[0].cell, dst = e.read[1].cell;
            const bool conflict = c_.banks && (src % c_.banks) == (dst % c_.banks);
            pair = c_.mul == MulMode::Single || (c_.mul == MulMode::PairUnit && unit && !conflict);
            chained = !pair && c_.share_source && prev_split_mul_ && in.src_offset() == prev_src_offset_;
            prev_split_mul_ = !pair && src != dst && op == Opcode::MUL;
            prev_src_offset_ = in.src_offset();
            mul_unit_ = unit;
        } else {
            prev_split_mul_ = false;
        }

        if (op == Opcode::F || op == Opcode::IO || op == Opcode::H) {
            open(next_cycle(at));
            closed_ = true;
            ready_ = cycle_ + (op == Opcode::F ? scan_cost(e) + scan_misses : op == Opcode::IO ? c_.io : 1);
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
                u.simple = op == Opcode::A || op == Opcode::S || op == Opcode::M || e.branch;
                u.reader = e.branch ? READER_BRANCH : op == Opcode::A ? READER_ADD : READER_OTHER;
                u.writer = op == Opcode::S ? WRITER_SET : (op == Opcode::A || op == Opcode::DJN) ? WRITER_ADD : WRITER_OTHER;
            }
            place(u, at);
            if (op == Opcode::MULC) place(Uop{.writes = 1, .write = e.read[0].cell, .writer = WRITER_SET}, cycle_);
            ready_ = cycle_;
        }

        const auto guess = predictor_.see(e.pc, before, e.branch, e.taken, target, cycle_);
        mispredicts_ += guess.mispredict;
        if (e.branch) {
            const int kind = (target <= e.pc ? 0 : 2) + (e.taken ? 0 : 1);
            ++branch_kinds_[kind];
            mispredicts_by_[kind] += guess.mispredict;
        }
        steers_ += guess.steer;
        ++steers_by_[guess.why];

        bool held = true;   // the branch's cell is in the early store
        if (c_.early_penalty) {
            if (known_.size() != std::size_t{mask} + 1) {
                known_.assign(std::size_t{mask} + 1, 0);
                final_.assign(std::size_t{mask} + 1, 0);
            }
            for (unsigned i = 0; i < e.writes; ++i) final_[e.write[i].cell] = cycle_ + c_.late;
            for (unsigned i = 1; i < e.fill; ++i) final_[(e.write[0].cell + i) & mask] = cycle_ + c_.late;
            if (c_.early_cells) {
                if (slots_of_.size() != c_.early_cells) slots_of_.assign(c_.early_cells, NONE);
                const auto fill = [&](addr_t cell, bool read) {
                    auto& slot = slots_of_[cell & (c_.early_cells - 1)];
                    if (slot == cell) return;
                    slot = cell;
                    if (read) known_[cell] = std::max(known_[cell], cycle_ + c_.late);
                };
                if (e.branch) held = c_.early_store && slots_of_[e.read[0].cell & (c_.early_cells - 1)] == e.read[0].cell;
                /* As the core keeps them: not F's cells, and a MUL's source only when it reads it alone. */
                if (op != Opcode::F) {
                    const unsigned reads = mul ? (pair || chained ? 0 : 1) : e.reads;
                    for (unsigned i = 0; i < reads; ++i) fill(e.read[i].cell, true);
                    if (mul) fill(e.read[1].cell, true);
                    for (unsigned i = 0; i < e.writes; ++i) fill(e.write[i].cell, false);
                }
            }
            note_values(e, op, mask);
        }
        if (e.branch && e.taken) closed_ = true;
        if (guess.mispredict) {
            closed_ = true;
            unsigned penalty = c_.penalty;
            if (c_.early_penalty && (held || c_.early_zero_bits) && (c_.early_second || !second_)) {
                const auto known = held ? known_[e.read[0].cell] : final_[e.read[0].cell];
                const auto wait = known > cycle_ ? known - cycle_ : 0;
                const unsigned early = !held && c_.zero_bits_penalty ? c_.zero_bits_penalty : c_.early_penalty;
                if (c_.early_partial || wait == 0) {
                    penalty = static_cast<unsigned>(std::min<std::uint64_t>(penalty, early + wait));
                }
                early_ += penalty == early;
            }
            if (c_.early_penalty && c_.early_cells && c_.early_flush) {
                std::fill(slots_of_.begin(), slots_of_.end(), NONE);
                ++flushes_;
            }
            penalty_cycles_ += penalty;
            ready_ = cycle_ + 1 + penalty;
        } else if (guess.steer) {
            closed_ = true;
            ready_ = cycle_ + 1 + c_.steer;
        }
    }

    [[nodiscard]] std::uint64_t cycles() const { return std::max(cycle_ + 1, ready_); }

    /* When the values this instruction writes are known, for branches that resolve early. Its last
     * part issued at cycle_. */
    void note_values(const TraceEvent& e, Opcode op, addr_t mask) {
        if (!e.writes) return;
        const std::uint64_t t = cycle_, late = t + c_.late;
        const bool sets = c_.forward != Forward::None, adds = c_.forward >= Forward::Add, muls = c_.forward == Forward::Mul;
        const auto chained = [&](std::initializer_list<addr_t> cells) {
            std::uint64_t at = t;
            for (const auto cell : cells) at = std::max(at, known_[cell]);
            return at;
        };
        switch (op) {
        case Opcode::S:
            known_[e.write[0].cell] = sets ? t : late;
            break;
        case Opcode::SRUN:
            for (unsigned i = 0; i < e.fill; ++i) known_[(e.write[0].cell + i) & mask] = sets ? t : late;
            break;
        case Opcode::A:
        case Opcode::DJN:
            known_[e.write[0].cell] = adds ? chained({e.write[0].cell}) : late;
            break;
        case Opcode::MUL:
        case Opcode::MULC: {
            const bool unit = e.instruction->data() == 1 || e.instruction->data() == 255;
            known_[e.write[0].cell] = muls && unit ? chained({e.read[0].cell, e.read[1].cell}) : late;
            if (op == Opcode::MULC) known_[e.write[1].cell] = sets ? t : late;
            break;
        }
        default:
            known_[e.write[0].cell] = late;
            break;
        }
    }

    [[nodiscard]] json report(std::uint64_t baseline) const {
        json reasons = json::object();
        for (int r = CLOSED; r < REASONS; ++r) reasons[std::string(REASON_NAMES[r])] = share(reasons_[r], instructions_);
        static constexpr std::string_view KINDS[] = {"backward_taken", "backward_not_taken", "forward_taken", "forward_not_taken"};
        json branches_by = json::object();
        for (int k = 0; k < 4; ++k) branches_by[std::string(KINDS[k])] = {{"count", branch_kinds_[k]}, {"mispredicts", mispredicts_by_[k]}};
        json steers_by = json::object();
        for (int k = Predictor::TAKEN_MISS; k < Predictor::STEERS; ++k) steers_by[std::string(Predictor::STEER_NAMES[k])] = steers_by_[k];
        const auto total = cycles();
        return {{"config", c_.to_json()}, {"cycles", total},
                {"cpi", share(total, instructions_)},
                {"speedup", baseline ? static_cast<double>(baseline) / static_cast<double>(total) : 1.0},
                {"groups", groups_}, {"joined", share(reasons_[JOINED], instructions_)},
                {"mispredicts", mispredicts_}, {"steers", steers_}, {"steers_by", steers_by}, {"not_joined_because", reasons},
                {"branches", branches_by}, {"penalty_cycles", penalty_cycles_}, {"resolved_early", early_}, {"early_flushes", flushes_},
                {"memory", memory_ ? memory_->report() : json(nullptr)}};
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
        bool                   simple = false;   // A, S, M, JE or JN
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
        if (u.branch && c_.branch_first && slots_ > 0) return BRANCHES;
        if (c_.pair_simple && slots_ > 0 && !(u.simple && simple_)) return KIND;
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
        if (c_.pair_disjoint) {
            for (const auto t : touched_) {
                for (unsigned i = 0; i < u.reads; ++i) if (t == u.read[i]) return DEPENDENCE;
                if (u.writes && t == u.write) return DEPENDENCE;
            }
        }
        if (c_.write_banks && u.writes) {
            for (const auto& [cell, writer] : written_) if (cell % c_.write_banks == u.write % c_.write_banks) return CONFLICT;
        }
        return JOINED;
    }

    void place(const Uop& u, std::uint64_t at) {
        const Reason why = why_not(u, at);
        ++reasons_[why];
        if (why != JOINED) open(next_cycle(at));
        if (slots_ == 0) simple_ = u.simple;
        second_ = slots_ > 0;
        ++slots_;
        reads_ += u.reads;
        for (unsigned i = 0; i < u.reads; ++i) touched_.push_back(u.read[i]);
        if (u.writes) {
            ++writes_;
            written_.emplace_back(u.write, u.writer);
            touched_.push_back(u.write);
        }
        if (u.branch) ++branches_;
        if (u.branch && c_.branch_last) closed_ = true;
    }

    MachineConfig               c_;
    Predictor                   predictor_;
    std::optional<MemorySystem> memory_;
    addr_t        mask_ = 0, pc_ = 0, first_pc_ = 0;

    std::uint64_t cycle_ = 0, ready_ = 0, source_cycle_ = 0;
    bool          closed_ = true;
    bool          simple_ = false, second_ = false;   // the group's first uop is simple; the last one placed joined
    unsigned      slots_ = 0, reads_ = 0, writes_ = 0, branches_ = 0;
    std::vector<std::pair<addr_t, Writer>> written_;
    std::vector<addr_t>                    touched_;

    bool     prev_split_mul_ = false, mul_unit_ = false;
    offset_t prev_src_offset_ = 0;

    std::uint64_t instructions_ = 0, groups_ = 0, mispredicts_ = 0, steers_ = 0, penalty_cycles_ = 0, early_ = 0, flushes_ = 0;
    std::vector<std::uint64_t> known_;   // by cell, the cycle its value is known, for early_penalty
    std::vector<std::uint64_t> final_;   // and the cycle it is known at X, for early_zero_bits
    static constexpr addr_t    NONE = std::numeric_limits<addr_t>::max();
    std::vector<addr_t>        slots_of_;   // the cell each slot of the early store holds
    std::array<std::uint64_t, 4> branch_kinds_{}, mispredicts_by_{};
    std::array<std::uint64_t, Predictor::STEERS> steers_by_{};   // backward or forward, taken or not
    std::array<std::uint64_t, REASONS> reasons_{};
};

class IssueAnalysis final : public Analysis {
public:
    explicit IssueAnalysis(const json& config) {
        const auto machines = config.value("machines", json::array({
            json{{"name", "rtl"}},
            json{{"name", "early resolve"}, {"early_penalty", 6}},
            json{{"name", "dual issue"}, {"width", 2}, {"raw", 0}, {"penalty", 10}, {"early_penalty", 7}, {"pair_simple", true},
                 {"branch_last", true}, {"pair_disjoint", true}, {"write_banks", 8}}}));
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
    summary_ = trace_footer(bf, summary);   // how it ended, where, and the tape's CRC-32
}

json AnalysisSet::report() const {
    json out = json::object();
    for (const auto& [name, a] : analyses_) out[name] = a->report();
    return {{"run", summary_}, {"analyses", out}};
}
