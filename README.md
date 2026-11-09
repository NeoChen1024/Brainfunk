# Brainfunk: Yet Another Brainfuck Interpreter

![Brainfunk Logo](https://gitlab.com/Neo_Chen/Brainfunk/raw/master/Logo/Logo256px.png "Yes, this is the logo")

# Build

CMake 3.25 or later, Ninja, and a C++20 compiler. The tests need Catch2 3, pytest and
pytest-xdist; the hardware ones also Verilator, and formal SymbiYosys and Bitwuzla. python-wavedrom
renders the timing diagrams in `doc/diagrams/`, and checks the committed ones; mermaid-cli checks that
the Mermaid diagrams in the docs parse.

```shell
cmake --workflow --preset default    # configure, build into build/, run every test that needs no board
cmake --workflow --preset sw         # the software only: unit and end-to-end tests, under a minute
ctest --preset corpus                # the corpus programs on their inputs, at every level, 4 minutes
ctest --preset hw                    # the hardware: cosimulation, testbenches and formal
cmake --workflow --preset sanitize   # the software under ASan and UBSan, in build-sanitize/
```

Programs end up in `build/bin`. Tests are labelled `unit`, `sw`, `corpus`, `hw`, `formal`, and, when enabled
at configure time, `mutation` and `board`; see [tests/CMakeLists.txt](tests/CMakeLists.txt).

# Features

- C++20 interpreter and reusable execution library
- RLE optimizations for `+-` and `><`
- Common loop optimizations: set-to-zero, multiply-with-offset, and find-zero
- Lazy pointer moves and block-local folding; see [doc/optimizer.md](doc/optimizer.md)
- C99 code generation with the same 8-bit cells and wrapping tape semantics as the interpreter
- Portable textual LLVM IR generation without linking against LLVM libraries
- ncurses TUI with pause, single-step, speed control, program I/O, and resize support
- `bf`, a small direct interpreter retained as a comparison implementation

# Program images

A program also takes the form of a listing (`-m bit`), hexadecimal words (`-m hex`), a bincode
image file (`-m bin`), or a packed image file, compressed with word-level LZ (`-m packed`). They
convert into each other without loss, and `brainfunk -b` runs any of them; see
[doc/bincode.txt](doc/bincode.txt). `bfload` sends a program to the YADAN board; see
[doc/rtl.md](doc/rtl.md).

```shell
build/bin/brainfunk -O2 -m packed -f program.b -o program.bfi
build/bin/brainfunk -m bit -b program.bfi
build/bin/brainfunk -b program.bfi
build/bin/bfload -t -f program.b
```

# Optimization levels

`-O0` runs the translated bitcode as is. `-O1` adds lazy moves. `-O2` adds block-local
folding. `-O3` adds fused instructions. `brainfunk` defaults to the
highest level; `visualbrainfunk` defaults to `-O0`, which is easier to follow while stepping.

# Modes

`run`, or `bf`: interpret a Brainfuck program

`bfc`: convert a Brainfuck program to C99

`bit`: print optimized bitcode as a listing

`hex`, `bin`, `packed`: write the program image as hexadecimal words, a bincode image file, or a
packed image file

`llvm`: lower optimized bitcode to portable textual LLVM IR

`stat`: run the program and report per-opcode static and dynamic instruction counts, scan
iterations, and memory reads and writes

`-m trace` keeps a run in a file that runs again checked, and `-m analyze` reports what a run did,
as JSON: profiles, opcode n-grams, dependence distances, values, and cycle estimates for configurable
cores. See [doc/analysis.md](doc/analysis.md).

`tools/stat_compare.py` compares those numbers across optimization levels:

```shell
tools/stat_compare.py 3rdparty/rdebath-Brainfuck/testing/Mandelbrot.b
```

# Usage

```
# Run a Brainfuck program:
build/bin/brainfunk -f <source-file>

# Translate it to C:
build/bin/brainfunk -m bfc -f <source-file> -o program.c

# Print optimized bitcode:
build/bin/brainfunk -m bit -f <source-file>

# Emit portable LLVM IR:
build/bin/brainfunk -m llvm -f <source-file> -o program.ll

# Emit LLVM IR and compile it with the C runtime wrapper:
tools/compile-llvm-ir.sh <source-file> <executable>

# Open the visualizer (100 ms per optimized instruction):
build/bin/visualbrainfunk -f <source-file> -t 100
```

TUI controls:

- `Space`/`p`: pause or resume
- `s`: pause and execute one optimized instruction
- `Up`/`+`, `Down`/`-`: adjust the delay
- `q`/`Esc`: quit

When the Brainfuck program executes `,`, the TUI displays a program-input prompt.
The next byte is delivered to the program rather than interpreted as a TUI command.

The LLVM emitter writes plain text and has no build-time or runtime dependency on LLVM.
`tools/llvm-ir-wrapper.c` supplies the tape, `main()`, and byte I/O callbacks; Clang performs
target selection and optimization only when the emitted IR is compiled. See
[doc/llvm-ir.md](doc/llvm-ir.md) for the ABI and portability contract.

# Programs

[programs/](programs/README.md) is a corpus of Brainfuck programs from many authors, for
testing and for measuring which patterns real programs use. Sources that have a git
repository are submodules under `3rdparty/`:

```shell
git submodule update --init 3rdparty/awib 3rdparty/bfcorpus 3rdparty/rdebath-Brainfuck
tools/corpus/corpus.py list --role workload --halts always --cell-bits 8 --unique
```

The programs keep their authors' licenses, not this repository's. See
[programs/README.md](programs/README.md).

# CPU core

[rtl/](rtl) is a pipelined SystemVerilog core for the current instruction set, verified against the
interpreter by lock-step cosimulation and formal checks, and run on a YADAN Board at 168 MHz. See
[doc/rtl.md](doc/rtl.md).

# Credits

* [brainfuck optimization strategies](http://calmerthanyouare.org/2015/01/07/optimizing-brainfuck.html) by Mats Linander
* [compile-time-regular-expressions](https://github.com/hanickadot/compile-time-regular-expressions) library by Hana Dusíková
