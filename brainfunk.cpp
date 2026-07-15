#include "libbrainfunk.hpp"
#include <getopt.h>

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
// helpmsg
// ------------------------------------------------------------------

[[noreturn]] static void helpmsg(char** argv, int status = 0) {
    cerr << "Usage: " << argv[0]
         << " [-h] [-m bf|bit|bfc|llvm] [-s code string] [-f file] [-o out]\n";
    std::exit(status);
}

// ==================================================================
//  main
// ==================================================================

int main(int argc, char** argv) {
    string code;
    string mode = "bf";

    /* Manage output stream – use RAII to avoid leaks */
    std::unique_ptr<std::ostream> output_owner;
    std::ostream* output = &cout;

    bool valid = false;

    int opt;
    while ((opt = ::getopt(argc, argv, "hm:s:f:o:")) != -1) {
        switch (opt) {
        case 'f':
            readcode(code, optarg);
            valid = true;
            break;
        case 's':
            code = optarg;
            valid = true;
            break;
        case 'h':
            helpmsg(argv);
            break;
        case 'm':
            mode = optarg;
            break;
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
        bf.translate(code);

        if (mode == "bf") {
            bf.run();
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
