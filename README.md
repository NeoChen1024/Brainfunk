# Brainfunk: Yet Another Brainfuck Interpreter and RTL core

![Brainfunk Logo](Logo/Logo256px.png "Yes, this is the logo")

A Brainfuck toolchain in C++20, and a CPU that runs Brainfuck on an FPGA. It interprets, optimizes,
translates to C and LLVM IR, and measures what real programs do, so that the hardware is designed
from evidence rather than from a handful of famous programs.

This is a research project. The instruction set, its encoding and the image formats change without
backward compatibility.

## Brainfunk Bitcode

Brainfuck source translates into [Brainfunk Bitcode](doc/bitcode.txt), a small instruction set over
8-bit cells, where every memory access is a signed offset from a base pointer. Runs of `+-` and `><`,
set-to-zero, multiply and scan loops become single instructions at translation; the
[optimizer](doc/optimizer.md) then removes most pointer moves and folds what it can prove.

| Level | What it adds |
| --- | --- |
| `-O0` | The translated bitcode as is. `visualbrainfunk` defaults to it, which is easiest to step through. |
| `-O1` | Lazy moves: the distance a move would cover lives in the next instruction's operand instead. |
| `-O2` | Block-local folding of known values and pending writes. The highest level the core runs. |
| `-O3` | Fused instructions. `brainfunk` defaults to it. |

Every level keeps the program's output, final memory and final pointer. The interpreter is the
reference: the corpus checks every level against recorded results, and the core is compared with the
interpreter at every instruction it retires.

A program has one meaning in several forms: a listing, hexadecimal words, a
[bincode](doc/bincode.txt) image file of 32-bit words, and a compressed packed image. They convert into
each other without loss, and `brainfunk -b` runs any of them.

## CPU core

[rtl/](rtl) is a pipelined SystemVerilog core for the bitcode, with its tape in block RAM and its code
in SDRAM behind an instruction cache. It is verified by lock-step cosimulation with the interpreter, by
formal properties, and by mutants the tests must catch, and runs on a YADAN Board. See
[doc/rtl.md](doc/rtl.md), and [doc/performance.md](doc/performance.md) for its cycles per instruction
beside the interpreter's and native code's.

## Tools

Programs are built into `build/bin`, and each takes `-h`.

| Command | Description |
| --- | --- |
| `brainfunk` | Run, trace or analyze a program, or write it as a listing, an image, C99 or LLVM IR. |
| `visualbrainfunk` | Step through a program in an ncurses TUI, with pause, speed control and program input. |
| `bfconsole` | Load a program onto the board, or write it to the board's flash, and be its console. |
| `bfstrip` | Strip everything but the eight commands from Brainfuck source. |
| `bf` | A small, independent interpreter, kept for comparison. |
| [`tools/compile-llvm-ir.sh`](doc/llvm-ir.md) | Compile a program to a native executable through the portable LLVM IR. |
| `tools/compile.sh` | Compile a program to a native executable through C. |
| [`tools/corpus/corpus.py`](programs/README.md) | Index, verify and list the program corpus. |
| `tools/stat_compare.py` | Compare execution statistics across optimization levels. |
| [`tools/issue_sweep.py`](doc/analysis.md) | Run the configurable issue model over the corpus. |
| [`tools/cpi_compare.py`](doc/performance.md) | Measure cycles per instruction of the core, the interpreter and native code. |
| [`tools/sdram_sweep.py`](doc/rtl.md) | Measure the board's SDRAM data window and timing limits. |

```shell
build/bin/brainfunk -f program.b                              # run it
build/bin/brainfunk -m llvm -f program.b -o program.ll        # LLVM IR, with no LLVM linked in
build/bin/brainfunk -O2 -m analyze -a profile,issue -f program.b -o report.json
build/bin/visualbrainfunk -f program.b -t 100                 # 100 ms per instruction
build/bin/bfconsole -f program.b                              # run it on the board
```

[Traces and analyses](doc/analysis.md) describes trace files, which keep a run so that it can run
again checked, and the analyses: profiles, opcode n-grams, dependence distances, values and cycle
estimates for configurable cores.

## Programs

[programs/](programs/README.md) is a corpus of Brainfuck programs from many authors, for testing and
for measuring which patterns real programs use. Sources with a git repository are submodules under
`3rdparty/`. The programs keep their authors' licenses, not this repository's.

## Building

Requires CMake 3.25+, Ninja, a C++20 compiler, zstd, and ncurses for `visualbrainfunk`. The tests also
need Catch2 3, pytest and pytest-xdist; the hardware ones Verilator, and formal SymbiYosys and
Bitwuzla. Synthesis needs Tang Dynasty.

```shell
git submodule update --init --recursive
cmake --workflow --preset default    # configure, build, run every test that needs no board
ctest --preset corpus                # every corpus program on its inputs, at every level
ctest --preset hw                    # cosimulation, testbenches and formal
```

[tests/CMakeLists.txt](tests/CMakeLists.txt) lists the test labels and the options that register more.

## Documentation

- [Bitcode](doc/bitcode.txt) and [bincode](doc/bincode.txt): the instruction set and its encoding.
- [Optimizer](doc/optimizer.md): what each `-O` level does, and why it stays correct.
- [LLVM IR backend](doc/llvm-ir.md): the emitted module and its runtime ABI.
- [Traces and analyses](doc/analysis.md): trace files, analyses and the issue model.
- [RTL](doc/rtl.md): the core, the memory system, verification and the boards.
- [Performance](doc/performance.md): the core, the interpreter and native code on the same programs.

## License

Original code and documentation are released into the public domain; see [LICENSE](LICENSE). The
corpus under `programs/` and `3rdparty/`, and other third-party components, keep their own licenses.

## Credits

- [Brainfuck optimization strategies](http://calmerthanyouare.org/2015/01/07/optimizing-brainfuck.html)
  by Mats Linander
- [compile-time-regular-expressions](https://github.com/hanickadot/compile-time-regular-expressions)
  by Hana Dusíková
- [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann
