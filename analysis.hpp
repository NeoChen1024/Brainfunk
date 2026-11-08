#pragma once
/* Analyses of a traced run, chosen by name at run time, each reporting JSON.
 *
 *   stat     counts per opcode, cell reads and writes, F strides
 *   profile  how often each instruction ran, and which way each branch went
 *   ngram    runs of 2 to 4 opcodes, with how each instruction's cells relate to the one before
 *   deps     how far back a read's cell was written, basic block lengths, loop trip counts
 *   values   the values read and written, and how often a test finds zero
 *   issue    cycle estimates for configurable in-order cores, scalar and wider; see doc/analysis.md
 *
 * Each takes its part of a JSON configuration, keyed by its name; what it does not find takes
 * a default. */

#include "trace.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Analysis {
public:
    virtual ~Analysis() = default;
    virtual void begin(const Brainfunk&) {}
    virtual void event(const TraceEvent& e) = 0;
    virtual void end(const Brainfunk&, const TraceSummary&) {}
    [[nodiscard]] virtual nlohmann::json report() const = 0;
};

/* The analysis of that name, configured; throws BrainfunkException for a name it does not know. */
[[nodiscard]] std::unique_ptr<Analysis> make_analysis(std::string_view name, const nlohmann::json& config);

/* The names make_analysis knows, in the order above. */
[[nodiscard]] std::vector<std::string> analysis_names();

/* A sink that runs several analyses side by side, and reports them all under their names. */
class AnalysisSet {
public:
    /* names: comma separated. config: an object with a member per analysis, which may be missing. */
    AnalysisSet(std::string_view names, const nlohmann::json& config);

    void begin(const Brainfunk& bf);
    void event(const TraceEvent& e);
    void end(const Brainfunk& bf, const TraceSummary& summary);
    [[nodiscard]] nlohmann::json report() const;

private:
    std::vector<std::pair<std::string, std::unique_ptr<Analysis>>> analyses_;
    nlohmann::json                                                 summary_;
};
