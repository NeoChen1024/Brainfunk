#include "libbrainfunk.hpp"
#include <getopt.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
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
        << "Usage: " << argv[0] << " [-h] [-m mode] [-O level] (-f file | -s code | -b image) [-o out]\n"
        << "\n"
        << "  -f file   Brainfuck source\n"
        << "  -s code   Brainfuck source on the command line\n"
        << "  -b image  a program image instead of source: an image file, a listing, or hexadecimal\n"
        << "            words, told apart by content; '-' is standard input. -O does not apply\n"
        << "  -O level  optimization level, 0 to " << OPT_MAX << " (default " << OPT_MAX << ")\n"
        << "  -o out    where the output of a mode that writes one goes (default standard output)\n"
        << "  -m mode   what to do (default run):\n"
        << "              run     run the program (bf is the same)\n"
        << "              stat    run it, counting what it does; the report goes to -o or stderr\n"
        << "              bit     the program as a listing, one instruction a line\n"
        << "              hex     a bincode image as text, one hexadecimal word a line\n"
        << "              bin     a bincode image file\n"
        << "              packed  a packed image file, compressed, as bfload sends to the board\n"
        << "              bfc     C99\n"
        << "              llvm    portable textual LLVM IR\n"
        << "  -h        show this help\n"
        << "\n"
        << "doc/bincode.txt describes the image formats; bit, hex, bin and packed convert between\n"
        << "them without loss.\n";
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
    constexpr std::string_view modes[] = {"run", "stat", "bit", "hex", "bin", "packed", "bfc", "llvm"};
    if (std::find(std::begin(modes), std::end(modes), mode) == std::end(modes)) {
        cerr << "Unknown mode: " << mode << '\n';
        helpmsg(argv, 1);
    }

    try {
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
        if (!image.empty()) {
            bf.load_bincode(read_image_file(image));
        } else {
            bf.translate(code);
            bf.optimize(opt_level);
        }

        if (mode == "run") {
            bf.run();
        } else if (mode == "stat") {
            /* The program keeps stdout, so the report goes to -o or stderr. */
            const auto stats = bf.run_counted();
            print_stats(output_owner ? *output : cerr, stats);
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
