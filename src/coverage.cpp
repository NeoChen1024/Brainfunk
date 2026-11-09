#include "brainfunk/coverage.hpp"

#include <string>

using nlohmann::json;

bool is_conditional_branch(Opcode op) noexcept {
    return op == Opcode::JE || op == Opcode::JN || op == Opcode::DJN || op == Opcode::MJN;
}

std::uint32_t program_crc32(std::span<const Bitcode> code) {
    std::string listing;
    for (const auto& word : code) {
        listing += word.to_string();
        listing += '\n';
    }
    return crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(listing.data()), listing.size()));
}

void Coverage::add(const Coverage& other) {
    if (other.words != words || other.program_crc32 != program_crc32 || other.branch_pc != branch_pc) {
        throw BrainfunkException("coverage: runs of different programs do not add up");
    }
    for (const auto& [pc, n] : other.starts) starts[pc] += n;
    for (const auto& [pc, n] : other.cuts) cuts[pc] += n;
    for (std::size_t i = 0; i < count.size(); ++i) {
        count[i] += other.count[i];
        taken[i] += other.taken[i];
    }
}

std::vector<std::uint64_t> Coverage::expand(std::span<const Bitcode> code) const {
    if (code.size() != words || program_crc32 != ::program_crc32(code)) {
        throw BrainfunkException("coverage: not of this program");
    }
    /* What arrives at each word other than by falling through: starts, and taken branches. */
    std::vector<std::int64_t> arrives(words + 1, 0);
    for (const auto& [pc, n] : starts) arrives[pc] += static_cast<std::int64_t>(n);
    for (const auto& [pc, n] : cuts) arrives[pc] -= static_cast<std::int64_t>(n);
    for (std::size_t i = 0; i < branch_pc.size(); ++i) {
        const auto pc = branch_pc[i];
        const auto target = pc + 1 + static_cast<addr_t>(code[pc].jump_offset());
        if (target > words) throw BrainfunkException("coverage: a branch leaves the program");
        arrives[target] += static_cast<std::int64_t>(taken[i]);
    }

    std::vector<std::uint64_t> counts(words, 0);
    std::size_t next_branch = 0;
    std::int64_t falls = 0;   // from the word before
    for (addr_t pc = 0; pc < words; ++pc) {
        const std::int64_t here = arrives[pc] + falls;
        if (here < 0) throw BrainfunkException("coverage: more runs leave word " + std::to_string(pc) + " than reach it");
        counts[pc] = static_cast<std::uint64_t>(here);
        const Opcode op = code[pc].opcode();
        if (is_conditional_branch(op)) {
            if (next_branch >= branch_pc.size() || branch_pc[next_branch] != pc) {
                throw BrainfunkException("coverage: the branches are not the program's");
            }
            if (count[next_branch] != counts[pc]) {
                throw BrainfunkException("coverage: branch at " + std::to_string(pc) + " ran " +
                                         std::to_string(count[next_branch]) + " times, not the " +
                                         std::to_string(counts[pc]) + " the flow gives");
            }
            falls = static_cast<std::int64_t>(count[next_branch] - taken[next_branch]);
            ++next_branch;
        } else {
            falls = op == Opcode::H ? 0 : here;
        }
    }
    if (next_branch != branch_pc.size()) throw BrainfunkException("coverage: the branches are not the program's");
    return counts;
}

json Coverage::to_json() const {
    const auto table = [](const std::map<addr_t, std::uint64_t>& m) {
        json pcs = json::array(), runs = json::array();
        for (const auto& [pc, n] : m) {
            pcs.push_back(pc);
            runs.push_back(n);
        }
        return json{{"pc", pcs}, {"runs", runs}};
    };
    return {{"words", words}, {"program_crc32", program_crc32}, {"starts", table(starts)}, {"cuts", table(cuts)},
            {"branches", {{"pc", branch_pc}, {"count", count}, {"taken", taken}}}};
}

Coverage Coverage::from_json(const json& j) {
    Coverage c;
    c.words = j.at("words").get<std::size_t>();
    c.program_crc32 = j.at("program_crc32").get<std::uint32_t>();
    const auto table = [&](const json& t, std::map<addr_t, std::uint64_t>& m) {
        const auto pcs = t.at("pc").get<std::vector<addr_t>>();
        const auto runs = t.at("runs").get<std::vector<std::uint64_t>>();
        if (pcs.size() != runs.size()) throw BrainfunkException("coverage: a table's columns differ in length");
        for (std::size_t i = 0; i < pcs.size(); ++i) m[pcs[i]] += runs[i];
    };
    table(j.at("starts"), c.starts);
    table(j.at("cuts"), c.cuts);
    const auto& b = j.at("branches");
    c.branch_pc = b.at("pc").get<std::vector<addr_t>>();
    c.count = b.at("count").get<std::vector<std::uint64_t>>();
    c.taken = b.at("taken").get<std::vector<std::uint64_t>>();
    if (c.count.size() != c.branch_pc.size() || c.taken.size() != c.branch_pc.size()) {
        throw BrainfunkException("coverage: the branch columns differ in length");
    }
    return c;
}

void CoverageSink::begin(const Brainfunk& bf) {
    const auto code = bf.bitcode();
    coverage_ = Coverage{};
    coverage_.words = code.size();
    coverage_.program_crc32 = program_crc32(code);
    slot_.assign(code.size(), 0);
    for (addr_t pc = 0; pc < code.size(); ++pc) {
        if (!is_conditional_branch(code[pc].opcode())) continue;
        slot_[pc] = static_cast<std::uint32_t>(coverage_.branch_pc.size());
        coverage_.branch_pc.push_back(pc);
    }
    coverage_.count.assign(coverage_.branch_pc.size(), 0);
    coverage_.taken.assign(coverage_.branch_pc.size(), 0);
    started_ = false;
}

void CoverageSink::event(const TraceEvent& e) {
    if (!started_) {
        ++coverage_.starts[e.pc];
        started_ = true;
    }
    if (e.branch) {
        const auto i = slot_[e.pc];
        ++coverage_.count[i];
        coverage_.taken[i] += e.taken;
    }
}

void CoverageSink::end(const Brainfunk& bf, const TraceSummary& summary) {
    /* Through H the run leaves by itself; any other way, it is cut before the word it did not run. */
    if (started_ && !summary.halted) ++coverage_.cuts[bf.pc()];
}
