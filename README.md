# Brainfunk: Yet Another Brainfuck Interpreter

![Brainfunk Logo](https://gitlab.com/Neo_Chen/Brainfunk/raw/master/Logo/Logo256px.png "Yes, this is the logo")

# Build

```shell
make
make test
```

# Features

- C++20 interpreter and reusable execution library
- RLE optimizations for `+-` and `><`
- Common loop optimizations: set-to-zero, multiply-with-offset, and find-zero
- C99 code generation with the same 8-bit cells and wrapping tape semantics as the interpreter
- Portable textual LLVM IR generation without linking against LLVM libraries
- ncurses TUI with pause, single-step, speed control, program I/O, and resize support
- `bf`, a small direct interpreter retained as a comparison implementation

# Modes

`bf`: interpret a Brainfuck program

`bfc`: convert a Brainfuck program to C99

`bit`: print optimized bitcode

`llvm`: lower optimized bitcode to portable textual LLVM IR

# Usage

```
# Run a Brainfuck program:
./brainfunk -f <source-file>

# Translate it to C:
./brainfunk -m bfc -f <source-file> -o program.c

# Print optimized bitcode:
./brainfunk -m bit -f <source-file>

# Emit portable LLVM IR:
./brainfunk -m llvm -f <source-file> -o program.ll

# Emit LLVM IR and compile it with the C runtime wrapper:
./compile-llvm-ir.sh <source-file> <executable>

# Open the visualizer (100 ms per optimized instruction):
./visualbrainfunk -f <source-file> -t 100
```

TUI controls:

- `Space`/`p`: pause or resume
- `s`: pause and execute one optimized instruction
- `Up`/`+`, `Down`/`-`: adjust the delay
- `q`/`Esc`: quit

When the Brainfuck program executes `,`, the TUI displays a program-input prompt.
The next byte is delivered to the program rather than interpreted as a TUI command.

The LLVM emitter writes plain text and has no build-time or runtime dependency on LLVM.
`llvm-ir-wrapper.c` supplies the tape, `main()`, and byte I/O callbacks; Clang performs
target selection and optimization only when the emitted IR is compiled. See
[doc/llvm-ir.md](doc/llvm-ir.md) for the ABI and portability contract.

# CPU core

The ultimate Brainfuck CPU
![CPU core](https://github.com/NeoChen1024/Brainfunk/raw/master/CPU/CPU.png)

# Credits

* [brainfuck optimization strategies](http://calmerthanyouare.org/2015/01/07/optimizing-brainfuck.html) by Mats Linander
* [compile-time-regular-expressions](https://github.com/hanickadot/compile-time-regular-expressions) library by Hana Dusíková
