#include "libbrainfunk.hpp"
#include "trace.hpp"
#include "trace_file.hpp"
#include "analysis.hpp"
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
// print_stats  -  one "name<TAB>static<TAB>dynamic" row per opcode, then totals
// ------------------------------------------------------------------

static void print_stats(std::ostream& os, const ExecutionStats& stats) {
    os << "opcode\tstatic\tdynamic\n";
    for (std::size_t i = 0; i < stats.static_count.size(); ++i) {
        os << opcode_name(static_cast<Opcode>(i)) << '\t' << stats.static_count[i] << '\t'
           << stats.dynamic_count[i] << '\n';
    }
    os << "total\t" << stats.static_total() << '\t' << stats.dynamic_total() << '\n'
       << "scan_iterations\t" << stats.scan_iterations << '\n'
       << "memory_reads\t" << stats.memory_reads << '\n'
       << "memory_writes\t" << stats.memory_writes << '\n';
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
        << "  -O level  optimization level, 0 to " << OPT_MAX << " (default " << OPT_MAX << ")\n"
        << "  -o out    where the output of a mode that writes one goes (default standard output)\n"
        << "  -m mode   what to do (default run):\n"
        << "              run     run the program (bf is the same)\n"
        << "              stat    run it, counting what it does; the report goes to -o or stderr\n"
        << "              trace   run it, writing a trace file to -o; the input is read whole first\n"
        << "              analyze run it through the analyses -a names, a JSON report to -o or stderr\n"
        << "              bit     the program as a listing, one instruction a line\n"
        << "              hex     a bincode image as text, one hexadecimal word a line\n"
        << "              bin     a bincode image file\n"
        << "              packed  a packed image file, compressed, as bfload sends to the board\n"
        << "              bfc     C99\n"
        << "              llvm    portable textual LLVM IR\n"
        << "  -a list   analyses, comma separated: " << [] {
               std::string names;
               for (const auto& name : analysis_names()) names += (names.empty() ? "" : ",") + name;
               return names;
           }() << "\n"
        << "  --config file  JSON with a member per analysis, configuring it; see doc/analysis.md\n"
        << "  --skip N  stat, trace, analyze: run the first N instructions without counting them\n"
        << "  --limit N stat, trace, analyze: stop after counting N instructions\n"
        << "  -h        show this help\n"
        << "\n"
        << "doc/bincode.txt describes the image formats; bit, hex, bin and packed convert between\n"
        << "them without loss. trace_file.hpp describes trace files.\n";
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
    while ((opt = ::getopt_long(argc, argv, "hm:O:s:f:b:o:t:a:", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'a':
            analyses = optarg;
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
    constexpr std::string_view modes[] = {"run", "stat", "trace", "analyze", "bit", "hex", "bin", "packed", "bfc", "llvm"};
    if (std::find(std::begin(modes), std::end(modes), mode) == std::end(modes)) {
        cerr << "Unknown mode: " << mode << '\n';
        helpmsg(argv, 1);
    }

    if (ranged && (mode != "stat" && mode != "trace" && mode != "analyze")) {
        cerr << "--skip and --limit are for stat, trace and analyze\n";
        return 1;
    }
    if (!trace_path.empty() && (ranged || (mode != "stat" && mode != "run" && mode != "analyze"))) {
        cerr << "A trace file runs again with run, stat or analyze, over its own range\n";
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
            std::istringstream in(header.input);
            TraceCheck check(reader);
            if (mode == "stat") {
                StatSink stat;
                Sinks sinks(stat, check);
                (void)run_traced(bf, sinks, in, cout, header.range());
                print_stats(output_owner ? *output : cerr, stat.stats);
            } else if (mode == "analyze") {
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
        } else if (mode == "stat") {
            /* The program keeps stdout, so the report goes to -o or stderr. */
            StatSink stat;
            const auto summary = run_traced(bf, stat, std::cin, cout, range);
            print_stats(output_owner ? *output : cerr, stat.stats);
            if (summary.trapped) throw BrainfunkException("Empty instruction");
        } else if (mode == "analyze") {
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
