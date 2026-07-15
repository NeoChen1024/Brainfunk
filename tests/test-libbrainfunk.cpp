#include "libbrainfunk.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string run(std::string_view code, std::string_view input = {},
                std::size_t memory_size = MEMSIZE) {
    Brainfunk brainfunk(memory_size);
    brainfunk.translate(code);
    std::istringstream in{std::string(input)};
    std::ostringstream out;
    brainfunk.run(in, out);
    return out.str();
}

std::string dump(std::string_view code, Format format = Format::BIT) {
    Brainfunk brainfunk;
    brainfunk.translate(code);
    std::ostringstream out;
    brainfunk.dump(out, format);
    return out.str();
}

} // namespace

int main() {
    check(run("++++++++[>++++++++<-]>+.+.+.") == "ABC",
          "optimized execution produces expected output");
    check(run("<+.", {}, 16) == std::string(1, '\x01'),
          "data pointer wraps backwards");
    check(run(",.") == std::string(1, '\0'),
          "EOF input stores zero");

    check(dump("[-]").find("S\t0") != std::string::npos,
          "odd decrement loop becomes set-zero");
    check(dump("[>]").find("F\t1") != std::string::npos,
          "scan loop becomes find-zero");
    check(dump("[->++<]").find("MUL\t1, 2") != std::string::npos,
          "multiply-offset loop is optimized");

    const auto llvm_ir = dump(",.[>]", Format::LLVM_IR);
    check(llvm_ir.find("define i32 @brainfunk_program") != std::string::npos,
          "LLVM mode emits the portable runtime entry point");
    check(llvm_ir.find("\ntarget triple =") == std::string::npos &&
              llvm_ir.find("\ntarget datalayout =") == std::string::npos,
          "portable LLVM IR omits target metadata");
    check(llvm_ir.find("call i32 %read_byte") != std::string::npos &&
              llvm_ir.find("call void %write_byte") != std::string::npos,
          "LLVM IR uses runtime I/O callbacks");

    bool rejected = false;
    try {
        Brainfunk brainfunk;
        brainfunk.translate("[+");
    } catch (const BrainfunkException&) {
        rejected = true;
    }
    check(rejected, "unmatched bracket is rejected");

    return failures == 0 ? 0 : 1;
}
