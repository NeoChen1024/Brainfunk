#include "brainfunk/libbrainfunk.hpp"
#include "brainfunk/trace.hpp"
#include "brainfunk/trace_file.hpp"
#include "brainfunk/analysis.hpp"
#include "brainfunk/coverage.hpp"
#include <getopt.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using std::cerr;
using std::cout;
using std::string;

// ------------------------------------------------------------------
// readcode  –  read & filter Brainfuck source from a file
// ------------------------------------------------------------------

static void readcode(string& code, const std::string& filename) {
    std::ifstream input(filename);
    if (!input.is_open()) {
        std::perror(filename.c_str());
        std::exit(1);
    }

    char c;
    while (input.get(c)) {
        if (is_brainfuck_instruction(c))
            code += c;
    }
}

// ------------------------------------------------------------------
// helpmsg
// ------------------------------------------------------------------

[[noreturn]] static void helpmsg(char** argv, int status = 0) {
    (status == 0 ? cout : cerr)
        << "Usage: " << argv[0] << " [-h] [-m mode] [-O level] (-f file | -s code | -b image | -t trace) [-o out]\n"
        << "       [--skip N] [--limit N]\n"
        << "\n"
        << "  -f file   Brainfuck source\n"
        << "  -s code   Brainfuck source on the command line\n"
        << "  -b image  a program image instead of source: an image file, a listing, or hexadecimal\n"
        << "            words, told apart by content; '-' is standard input. -O does not apply\n"
        << "  -t trace  a trace file instead: its program runs again on its input, over its range,\n"
        << "            and must go the way it went. -O, --skip and --limit do not apply\n"
        << "  -e mode   what input stores at the end of input: 0 (default), 255, or keep, which\n"
        << "            leaves the cell as it was; bfc and llvm build it in\n"
        << "  -O level  optimization level, 0 to " << OPT_MAX << " (default " << OPT_MAX << ")\n"
        << "  -o out    where the output of a mode that writes one goes (default standard output)\n"
        << "  -m mode   what to do (default run):\n"
        << "              run     run the program (bf is the same)\n"
        << "              analyze run it through the analyses of -a, a JSON report to -o or stderr\n"
        << "              trace   run it, writing a trace file to -o; the input is read whole first\n"
        << "              coverage  add up the counters of analysis reports named after the options,\n"
        << "                      from profile with \"counts\", of this program as translated or loaded,\n"
        << "                      and report what each reached and what only it did, to -o\n"
        << "              bit     the program as a listing, one instruction a line\n"
        << "              hex     a bincode image as text, one hexadecimal word a line\n"
        << "              bin     a bincode image file\n"
        << "              packed  a packed image file, compressed, as bfload sends to the board\n"
        << "              bfc     C99\n"
        << "              llvm    portable textual LLVM IR\n"
        << "  -a list   analyses, comma separated, stat by default: " << [] {
               std::string names;
               for (const auto& name : analysis_names()) names += (names.empty() ? "" : ",") + name;
               return names;
           }() << "\n"
        << "  --config file  JSON with a member per analysis, configuring it; see doc/analysis.md\n"
        << "  --skip N  analyze, trace: run the first N instructions without reporting them\n"
        << "  --limit N analyze, trace: stop after reporting N instructions\n"
        << "  -h        show this help\n"
        << "\n"
        << "doc/bincode.txt describes the image formats; bit, hex, bin and packed convert between\n"
        << "them without loss. include/brainfunk/trace_file.hpp describes trace files.\n";
    std::exit(status);
}

// ==================================================================
//  main
// ==================================================================

int main(int argc, char** argv) {
    string code;
    string mode = "run";
    int opt_level = OPT_MAX;

    /* Manage output stream – use RAII to avoid leaks */
    std::unique_ptr<std::ostream> output_owner;
    std::ostream* output = &cout;

    bool valid = false;
    string image, trace_path, source_name;
    TraceRange range;
    bool ranged = false;
    string analyses = "stat", config_path;
    EofMode eof = EofMode::Zero;

    const auto count_of = [&](const char* text) -> std::uint64_t {
        std::size_t used = 0;
        try {
            const auto value = std::stoull(text, &used, 0);
            if (used == std::string_view{text}.size()) return value;
        } catch (const std::exception&) {
        }
        cerr << "Invalid count: " << text << '\n';
        std::exit(1);
    };

    static const option long_options[] = {{"skip", required_argument, nullptr, 'K'},
                                          {"limit", required_argument, nullptr, 'L'},
                                          {"config", required_argument, nullptr, 'C'},
                                          {"help", no_argument, nullptr, 'h'},
                                          {nullptr, 0, nullptr, 0}};
    int opt;
    while ((opt = ::getopt_long(argc, argv, "hm:O:s:f:b:o:t:a:e:", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'a':
            analyses = optarg;
            break;
        case 'e':
            if (const auto mode = parse_eof_mode(optarg)) {
                eof = *mode;
            } else {
                cerr << "Invalid EOF mode: " << optarg << " (0, 255 or keep)\n";
                return 1;
            }
            break;
        case 'C':
            config_path = optarg;
            break;
        case 't':
            trace_path = optarg;
            valid = true;
            break;
        case 'K':
            range.skip = count_of(optarg);
            ranged = true;
            break;
        case 'L':
            range.limit = count_of(optarg);
            ranged = true;
            break;
        case 'f':
            readcode(code, optarg);
            source_name = optarg;
            valid = true;
            break;
        case 's':
            code = optarg;
            valid = true;
            break;
        case 'b':
            image = optarg;
            valid = true;
            break;
        case 'h':
            helpmsg(argv);
            break;
        case 'm':
            mode = optarg;
            break;
        case 'O': {
            const auto level = parse_opt_level(optarg);
            if (!level) {
                cerr << "Invalid optimization level: " << optarg << '\n';
                return 1;
            }
            opt_level = *level;
            break;
        }
        case 'o':
            if (std::string_view{optarg} == "-") {
                output = &cout;
            } else {
                auto fs = std::make_unique<std::ofstream>(optarg, std::ios::binary);
                if (!fs->is_open()) {
                    cerr << "Failed to open output file: " << optarg << '\n';
                    return 1;
                }
                output_owner = std::move(fs);
                output = output_owner.get();
            }
            break;
        default:
            helpmsg(argv, 1);
        }
    }

    if (!valid) {
        cerr << "No input specified.\n";
        helpmsg(argv, 1);
    }
    if (mode == "bf") mode = "run";
    constexpr std::string_view modes[] = {"run", "analyze", "trace", "coverage", "bit", "hex", "bin", "packed", "bfc", "llvm"};
    if (std::find(std::begin(modes), std::end(modes), mode) == std::end(modes)) {
        cerr << "Unknown mode: " << mode << '\n';
        helpmsg(argv, 1);
    }

    if (ranged && mode != "trace" && mode != "analyze") {
        cerr << "--skip and --limit are for analyze and trace\n";
        return 1;
    }
    if (!trace_path.empty() && (ranged || (mode != "run" && mode != "analyze"))) {
        cerr << "A trace file runs again with run or analyze, over its own range\n";
        return 1;
    }
    if (mode == "trace" && !output_owner) {
        cerr << "trace needs a file to write to: -o\n";
        return 1;
    }

    try {
        nlohmann::json config = nlohmann::json::object();
        if (!config_path.empty()) {
            std::ifstream file(config_path);
            if (!file) throw BrainfunkException("cannot open " + config_path);
            config = nlohmann::json::parse(file);
        }
        std::optional<AnalysisSet> analysis;
        if (mode == "analyze") analysis.emplace(analyses, config);
        const auto report = [&](nlohmann::json about) {
            auto out = analysis->report();
            out["program"] = std::move(about);
            (output_owner ? *output : cerr) << out.dump(1) << '\n';
        };

        /* A trace file brings its program, input and range, and the run must go its way again. */
        if (!trace_path.empty()) {
            std::ifstream file(trace_path, std::ios::binary);
            if (!file) throw BrainfunkException("cannot open " + trace_path);
            TraceReader reader(file);
            const auto& header = reader.header();
            Brainfunk bf(header.meta.at("memory").get<std::size_t>());
            bf.load_bincode(header.words);
            const auto recorded = parse_eof_mode(header.meta.at("eof").get<std::string>());
            if (!recorded) throw BrainfunkException("the trace file's EOF mode is not known");
            bf.set_eof(*recorded);
            std::istringstream in(header.input);
            TraceCheck check(reader);
            if (mode == "analyze") {
                Sinks sinks(*analysis, check);
                (void)run_traced(bf, sinks, in, cout, header.range());
                auto about = header.meta;
                about["trace"] = trace_path;
                report(about);
            } else {
                (void)run_traced(bf, check, in, cout, header.range());
            }
            return 0;
        }

        /* The forms of an image convert as they are, without the checks that running needs. */
        if (mode == "bit" || mode == "hex" || mode == "bin" || mode == "packed") {
            std::vector<std::uint32_t> words;
            std::vector<Bitcode> program;
            if (!image.empty()) {
                words = read_image_file(image);
                if (mode == "bit") program = decode_bincode(words);
            } else {
                Brainfunk bf(MEMSIZE);
                bf.translate(code);
                bf.optimize(opt_level);
                program.assign(bf.bitcode().begin(), bf.bitcode().end());
                if (mode != "bit") words = encode_bincode(program);
            }
            if (mode == "bit") {
                write_listing(*output, program);
            } else if (mode == "hex") {
                write_hex(*output, words);
            } else {
                const auto bytes = pack_image(words, mode == "packed" ? Compression::LZ : Compression::None);
                output->write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            output->flush();
            if (!*output) throw BrainfunkException("cannot write the output");
            return 0;
        }

        Brainfunk bf(MEMSIZE);
        bf.set_eof(eof);
        std::vector<std::uint32_t> words;
        if (!image.empty()) {
            words = read_image_file(image);
            bf.load_bincode(words);
        } else {
            bf.translate(code);
            bf.optimize(opt_level);
        }

        if (mode == "run") {
            bf.run();
        } else if (mode == "coverage") {
            const std::vector<string> reports(argv + optind, argv + argc);
            if (reports.empty()) throw BrainfunkException("coverage needs reports to add up");
            const auto code = bf.bitcode();

            /* What a run reached: each word that ran, then each branch direction taken. */
            const auto reached = [&](const Coverage& flow) {
                const auto counts = flow.expand(code);
                std::vector<bool> r(code.size() + 2 * flow.branch_pc.size());
                for (std::size_t pc = 0; pc < code.size(); ++pc) r[pc] = counts[pc] != 0;
                for (std::size_t i = 0; i < flow.branch_pc.size(); ++i) {
                    r[code.size() + 2 * i] = flow.taken[i] != 0;
                    r[code.size() + 2 * i + 1] = flow.count[i] != flow.taken[i];
                }
                return r;
            };
            const auto tally = [&](const std::vector<bool>& r) {
                std::size_t words = 0, ways = 0;
                for (std::size_t k = 0; k < r.size(); ++k) (k < code.size() ? words : ways) += r[k];
                return std::pair{words, ways};
            };

            std::vector<Coverage> flows;
            for (const auto& path : reports) {
                std::ifstream file(path);
                if (!file) throw BrainfunkException("cannot open " + path);
                const auto report = nlohmann::json::parse(file);
                flows.push_back(Coverage::from_json(report.contains("analyses")
                                                        ? report.at("analyses").at("profile").at("counters")
                                                        : report));
            }
            std::vector<std::vector<bool>> sets;
            Coverage all = flows.front();
            for (std::size_t i = 0; i < flows.size(); ++i) {
                sets.push_back(reached(flows[i]));
                if (i) all.add(flows[i]);
            }
            nlohmann::json inputs = nlohmann::json::array();
            for (std::size_t i = 0; i < sets.size(); ++i) {
                std::size_t only = 0;
                for (std::size_t k = 0; k < sets[i].size(); ++k) {
                    bool elsewhere = false;
                    for (std::size_t j = 0; j < sets.size() && !elsewhere; ++j) elsewhere = j != i && sets[j][k];
                    only += sets[i][k] && !elsewhere;
                }
                const auto [words, ways] = tally(sets[i]);
                inputs.push_back({{"report", reports[i]}, {"words_run", words}, {"ways", ways}, {"only", only}});
            }
            const auto [words, ways] = tally(reached(all));
            const nlohmann::json out = {{"words", code.size()}, {"branches", all.branch_pc.size()}, {"inputs", inputs},
                                        {"all", {{"words_run", words}, {"ways", ways}, {"counters", all.to_json()}}}};
            *output << out.dump(1) << '\n';
        } else if (mode == "analyze") {
            /* The program keeps stdout, so the report goes to -o or stderr. */
            const auto summary = run_traced(bf, *analysis, std::cin, cout, range);
            nlohmann::json about = {{"level", image.empty() ? nlohmann::json(opt_level) : nlohmann::json(nullptr)},
                                    {"words", bf.bitcode().size()}, {"skip", range.skip},
                                    {"limit", ranged && range.limit != TraceRange{}.limit ? nlohmann::json(range.limit)
                                                                                         : nlohmann::json(nullptr)}};
            if (!source_name.empty()) about["source"] = source_name;
            if (!image.empty()) about["image"] = image;
            report(about);
            if (summary.trapped) throw BrainfunkException("Empty instruction");
        } else if (mode == "trace") {
            /* The input goes into the file, so all of it is read first. */
            const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
            if (words.empty()) words = encode_bincode(bf.bitcode());
            nlohmann::json about = {{"level", image.empty() ? nlohmann::json(opt_level) : nlohmann::json(nullptr)}};
            if (!source_name.empty()) about["source"] = source_name;
            if (!image.empty()) about["image"] = image;
            TraceWriter writer(*output, words, input, range, about);
            std::istringstream in(input);
            const auto summary = run_traced(bf, writer, in, cout, range);
            if (summary.trapped) throw BrainfunkException("Empty instruction");
        } else if (mode == "bfc") {
            bf.dump(*output, Format::C);
        } else {
            bf.dump(*output, Format::LLVM_IR);
        }
    } catch (const std::exception& e) {
        cerr << e.what() << '\n';
        return 1;
    }

    return 0;
}
