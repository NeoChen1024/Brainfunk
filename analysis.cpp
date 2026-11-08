#include "analysis.hpp"
#include "trace_file.hpp"
#include "coverage.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
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

/* A direction guess at D. Every branch is predicted, then updated with how it went. */
class Direction {
public:
    virtual ~Direction() = default;
    virtual bool predict(addr_t pc, bool backward) = 0;
    virtual void update(addr_t pc, bool backward, bool taken) = 0;
};

std::unique_ptr<Direction> make_direction(const json& s);

class StaticDirection final : public Direction {
public:
    bool predict(addr_t, bool backward) override { return backward; }
    void update(addr_t, bool, bool) override {}
};

class Bimodal final : public Direction {
public:
    explicit Bimodal(const json& s) : counters_(table_size(s, "direction")) {}
    bool predict(addr_t pc, bool backward) override { return counters_.predict(pc, backward); }
    void update(addr_t pc, bool backward, bool taken) override { counters_.update(pc, backward, taken); }

private:
    Counters counters_;
};

/* Counters indexed by the pc and the last `history` branch outcomes, exclusive-ored. */
class Gshare final : public Direction {
public:
    explicit Gshare(const json& s) : counters_(table_size(s, "direction")), bits_(s.at("history").get<unsigned>()) {}
    bool predict(addr_t pc, bool backward) override { return counters_.predict(pc ^ history_, backward); }
    void update(addr_t pc, bool backward, bool taken) override {
        counters_.update(pc ^ history_, backward, taken);
        if (bits_) history_ = ((history_ << 1) | (taken ? 1 : 0)) & ((addr_t{1} << bits_) - 1);
    }

private:
    Counters counters_;
    unsigned bits_;
    addr_t   history_ = 0;
};

/* Each branch's own last `history` outcomes, from a table of `histories` by pc, next to its pc
 * bits, index counters: the trip counts of short loops, and patterns that repeat. */
class Local final : public Direction {
public:
    explicit Local(const json& s)
        : histories_(table_size(s, "histories"), 0), counters_(table_size(s, "direction")), bits_(s.at("history").get<unsigned>()) {}
    bool predict(addr_t pc, bool backward) override { return counters_.predict(index(pc), backward); }
    void update(addr_t pc, bool backward, bool taken) override {
        counters_.update(index(pc), backward, taken);
        auto& h = slot(pc);
        if (bits_) h = ((h << 1) | (taken ? 1 : 0)) & ((addr_t{1} << bits_) - 1);
    }

private:
    addr_t& slot(addr_t pc) { return histories_[pc & (histories_.size() - 1)]; }
    std::size_t index(addr_t pc) { return (static_cast<std::size_t>(pc) << bits_) | slot(pc); }
    std::vector<addr_t> histories_;
    Counters            counters_;
    unsigned            bits_;
};

/* Counts a backward branch's taken runs: once a loop has gone round the same number of times
 * `confidence` times in a row, it guesses the exit. Otherwise its base guesses. */
class Loop final : public Direction {
public:
    explicit Loop(const json& s)
        : entries_(table_size(s, "entries")), confidence_(s.at("confidence").get<unsigned>()), base_(make_direction(s.at("base"))) {}
    bool predict(addr_t pc, bool backward) override {
        const bool base = base_->predict(pc, backward);
        const Entry& e = entries_[pc & (entries_.size() - 1)];
        if (!backward || !e.valid || e.pc != pc || e.confidence < confidence_) return base;
        return e.iterations != e.trip;
    }
    void update(addr_t pc, bool backward, bool taken) override {
        base_->update(pc, backward, taken);
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
    explicit Tournament(const json& s)
        : first_(make_direction(s.at("components")[0])), second_(make_direction(s.at("components")[1])),
          chooser_(table_size(s, "chooser"), 1), bits_(s.at("chooser_history").get<unsigned>()) {}
    bool predict(addr_t pc, bool backward) override {
        a_ = first_->predict(pc, backward);
        b_ = second_->predict(pc, backward);
        return chooser_[index(pc)] >= 2 ? b_ : a_;
    }
    void update(addr_t pc, bool backward, bool taken) override {
        if (a_ != b_) {
            auto& c = chooser_[index(pc)];
            c = b_ == taken ? (c < 3 ? c + 1 : 3) : (c ? c - 1 : 0);
        }
        first_->update(pc, backward, taken);
        second_->update(pc, backward, taken);
        if (bits_) history_ = ((history_ << 1) | (taken ? 1 : 0)) & ((addr_t{1} << bits_) - 1);
    }

private:
    std::size_t index(addr_t pc) const { return (pc ^ history_) & (chooser_.size() - 1); }
    std::unique_ptr<Direction> first_, second_;
    std::vector<std::uint8_t>  chooser_;
    unsigned                   bits_;
    addr_t                     history_ = 0;
    bool                       a_ = false, b_ = false;
};

std::unique_ptr<Direction> make_direction(const json& s) {
    const auto kind = s.at("kind").get<std::string>();
    if (kind == "static") return std::make_unique<StaticDirection>();
    if (kind == "bimodal") return std::make_unique<Bimodal>(s);
    if (kind == "gshare") return std::make_unique<Gshare>(s);
    if (kind == "local") return std::make_unique<Local>(s);
    if (kind == "loop") return std::make_unique<Loop>(s);
    if (kind == "tournament") return std::make_unique<Tournament>(s);
    throw BrainfunkException("issue: no direction predictor " + kind);
}

class Predictor {
public:
    explicit Predictor(const json& spec)
        : kind_(spec.at("kind").get<std::string>()), table_(table_size(spec, "next")),
          tag_bits_(spec.at("tag_bits").get<unsigned>()), forget_(spec.at("forget").get<bool>()),
          shift_(static_cast<unsigned>(std::countr_zero(spec.at("next").get<unsigned>()))) {
        if (kind_ == "rtl") {
            counters_ = std::make_unique<Counters>(table_size(spec, "direction"));
            history_bits_ = spec.at("history").get<unsigned>();
        } else if (kind_ != "perfect") {
            direction_ = make_direction(spec);
        }
    }

    struct Guess {
        bool mispredict = false, steer = false;
    };

    Guess see(addr_t pc, addr_t before, bool branch, bool taken, addr_t target) {
        Guess g;
        Entry& entry = table_[before & (table_.size() - 1)];
        const std::uint32_t tag = static_cast<std::uint32_t>(before >> shift_) & ((1u << tag_bits_) - 1);
        const bool hit = entry.valid && entry.tag == tag;
        if (!branch) {
            /* Fetch went somewhere a word that is not a branch does not go. */
            if (hit) {
                g.steer = true;
                entry.valid = false;
            }
            return g;
        }
        const bool backward = target <= pc;
        if (kind_ == "rtl") {
            const auto index = pc ^ history_;
            if (hit) {
                if (!taken || entry.target != target) {
                    g.mispredict = true;
                    if (forget_ || taken) entry.valid = false;
                }
            } else {
                const bool predict = counters_->predict(index, backward);
                if (predict) {
                    g.steer = true;
                    entry = {true, tag, target};
                }
                g.mispredict = predict != taken;
            }
            if (g.mispredict) g.steer = false;
            counters_->update(index, backward, taken);
            if (history_bits_) history_ = ((history_ << 1) | (taken ? 1 : 0)) & ((addr_t{1} << history_bits_) - 1);
            return g;
        }

        const bool guess = direction_ ? direction_->predict(pc, backward) : taken;
        if (direction_) direction_->update(pc, backward, taken);
        g.mispredict = guess != taken;
        const bool fetched = hit && entry.target == target;   // fetch went to the target with no gap
        if (!g.mispredict) g.steer = taken ? !fetched : hit;  // D sends fetch where it guessed
        if (taken) {
            entry = {true, tag, target};
        } else if (hit && forget_) {
            entry.valid = false;
        }
        return g;
    }

private:
    struct Entry {
        bool          valid = false;
        std::uint32_t tag = 0;
        addr_t        target = 0;
    };
    std::string                kind_;
    std::vector<Entry>         table_;
    unsigned                   tag_bits_;
    bool                       forget_;
    unsigned                   shift_;
    std::unique_ptr<Counters>  counters_;   // rtl
    unsigned                   history_bits_ = 0;
    addr_t                     history_ = 0;
    std::unique_ptr<Direction> direction_;  // the others; none for perfect
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
    json            predictor = predictor_spec(json::object(), true);

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
        c.predictor = predictor_spec(j.value("predictor", json::object()), true);
        if (c.width == 0 || c.reads == 0 || c.writes == 0) throw BrainfunkException("issue: width, reads and writes are at least 1");
        return c;
    }

    [[nodiscard]] json to_json() const {
        static constexpr std::string_view MUL_NAMES[] = {"split", "pair-unit", "single"};
        return {{"name", name}, {"width", width}, {"reads", reads}, {"writes", writes}, {"branches", branches},
                {"raw", raw}, {"mul", MUL_NAMES[static_cast<int>(mul)]}, {"two_mul", two_mul},
                {"fetch_line", fetch_line}, {"banks", banks}, {"penalty", penalty}, {"steer", steer},
                {"scan_base", scan_base}, {"line_cells", line_cells}, {"stride_limit", stride_limit}, {"io", io},
                {"predictor", predictor}};
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
        if (e.branch) {
            const int kind = (target <= e.pc ? 0 : 2) + (e.taken ? 0 : 1);
            ++branch_kinds_[kind];
            mispredicts_by_[kind] += guess.mispredict;
        }
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
        static constexpr std::string_view KINDS[] = {"backward_taken", "backward_not_taken", "forward_taken", "forward_not_taken"};
        json branches_by = json::object();
        for (int k = 0; k < 4; ++k) branches_by[std::string(KINDS[k])] = {{"count", branch_kinds_[k]}, {"mispredicts", mispredicts_by_[k]}};
        const auto total = cycles();
        return {{"config", c_.to_json()}, {"cycles", total},
                {"cpi", share(total, instructions_)},
                {"speedup", baseline ? static_cast<double>(baseline) / static_cast<double>(total) : 1.0},
                {"groups", groups_}, {"joined", share(reasons_[JOINED], instructions_)},
                {"mispredicts", mispredicts_}, {"steers", steers_}, {"not_joined_because", reasons},
                {"branches", branches_by}};
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
    std::array<std::uint64_t, 4> branch_kinds_{}, mispredicts_by_{};   // backward or forward, taken or not
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
    summary_ = trace_footer(bf, summary);   // how it ended, where, and the tape's CRC-32
}

json AnalysisSet::report() const {
    json out = json::object();
    for (const auto& [name, a] : analyses_) out[name] = a->report();
    return {{"run", summary_}, {"analyses", out}};
}
