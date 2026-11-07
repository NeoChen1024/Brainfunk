#include "libbrainfunk.hpp"
#include <getopt.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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
    cerr << "Usage: " << argv[0]
         << " [-h] [-m bf|bit|bfc|llvm|stat] [-O level] [-s code string] [-f file] [-b image] [-o out]\n"
         << "  -O level  optimization level, 0 to " << OPT_MAX << " (default " << OPT_MAX << ")\n"
         << "  -b image  load a program image, from bit2bin or packed by bfload, instead of source;\n"
         << "            -O does not apply\n";
    std::exit(status);
}

// ==================================================================
//  main
// ==================================================================

int main(int argc, char** argv) {
    string code;
    string mode = "bf";
    int opt_level = OPT_MAX;

    /* Manage output stream – use RAII to avoid leaks */
    std::unique_ptr<std::ostream> output_owner;
    std::ostream* output = &cout;

    bool valid = false;
    string image;

    int opt;
    while ((opt = ::getopt(argc, argv, "hm:O:s:f:b:o:")) != -1) {
        switch (opt) {
        case 'f':
            readcode(code, optarg);
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
                auto fs = std::make_unique<std::ofstream>(optarg);
                if (!fs->is_open()) {
                    cerr << "Failed to open output file: " << optarg << '\n';
                    return 1;
                }
                output_owner = std::move(fs);
                output = output_owner.get();
            }
            break;
        default:
            break;
        }
    }

    if (!valid) {
        cerr << "No input specified.\n";
        helpmsg(argv, 1);
    }

    try {
        Brainfunk bf(MEMSIZE);
        if (!image.empty()) {
            bf.load_bincode(read_image_file(image));
        } else {
            bf.translate(code);
            bf.optimize(opt_level);
        }

        if (mode == "bf") {
            bf.run();
        } else if (mode == "stat") {
            /* The program keeps stdout, so the report goes to -o or stderr. */
            const auto stats = bf.run_counted();
            print_stats(output_owner ? *output : cerr, stats);
        } else if (mode == "bit") {
            bf.dump(*output, Format::BIT);
        } else if (mode == "bfc") {
            bf.dump(*output, Format::C);
        } else if (mode == "llvm") {
            bf.dump(*output, Format::LLVM_IR);
        } else {
            cerr << "Unknown mode: " << mode << '\n';
            return 1;
        }
    } catch (const std::exception& e) {
        cerr << e.what() << '\n';
        return 1;
    }

    return 0;
}
