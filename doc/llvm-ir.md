# Portable LLVM IR backend

Brainfunk's LLVM backend is a text emitter. The `brainfunk` executable does not include
LLVM headers, link against LLVM libraries, invoke LLVM tools, select a target, or run LLVM
optimization passes.

```text
Brainfuck source -> Brainfunk Bitcode -> portable program.ll
                                           +
                                     llvm-ir-wrapper.c
                                           |
                              target Clang optimization
                                           |
                                    native executable
```

## Runtime ABI

The generated module exports:

```llvm
define i32 @brainfunk_program(
    ptr %tape,
    i64 %tape_size,
    ptr %io_context,
    ptr %read_byte,
    ptr %write_byte
)
```

The corresponding C declaration is:

```c
typedef int32_t (*brainfunk_read_fn)(void *context);
typedef void (*brainfunk_write_fn)(void *context, uint32_t byte);

int32_t brainfunk_program(uint8_t *tape,
                          uint64_t tape_size,
                          void *io_context,
                          brainfunk_read_fn read_byte,
                          brainfunk_write_fn write_byte);
```

`read_byte` returns a byte value or `-1` for EOF. `write_byte` receives the zero-extended
cell value. The wrapper may replace stdio with a TUI, embedded UART, WASI host function,
or another byte-oriented interface.

The generated function returns:

- `0`: halted normally
- `1`: null tape or unexpected tape size
- `2`: invalid placeholder instruction was reached

The tape size must equal the size used when the Brainfunk program was translated. The
provided wrapper uses the default 65,536-byte tape.

## Portability profile

Portable output intentionally:

- omits `target triple` and `target datalayout`;
- uses opaque `ptr` and fixed-width `i1`, `i8`, `i32`, and `i64` values;
- accesses tape cells only as `i8`;
- avoids C structure layout across the ABI;
- avoids libc calls, LLVM intrinsics, inline assembly, target attributes, and module flags;
- uses ordinary control flow, integer arithmetic, loads, and stores;
- receives all platform-dependent services through callbacks.

Brainfunk emits direct, verifier-valid IR and does not perform LLVM-level optimization.
The final compiler may promote local state to SSA, simplify wrapping arithmetic, optimize
loops, and select instructions using the actual target DataLayout and CPU model.

Opaque pointers require a modern LLVM toolchain. Brainfunk Bitcode remains the canonical
target-independent representation; textual LLVM IR is a regeneratable lowering artifact.

The test matrix assembles and lowers the same module for 32- and 64-bit x86, ARM, AArch64,
RISC-V, big- and little-endian PowerPC, s390x, Windows, Darwin, and 32- and 64-bit WebAssembly.
Native execution is compared with the interpreter where the host can run the result.

## Usage

Emit IR only:

```sh
./brainfunk -m llvm -f program.bf -o program.ll
llvm-as program.ll -o program.bc
opt -passes=verify program.ll -disable-output
```

Compile through the provided pipeline:

```sh
./compile-llvm-ir.sh program.bf program
```

`compile-llvm-ir.sh` follows the same two-argument convention as `compile.sh`. Its compiler
and flags are configurable:

```sh
CLANG=clang CFLAGS='-O3 -target aarch64-linux-gnu --sysroot=/path/to/sysroot' \
    ./compile-llvm-ir.sh program.bf program-aarch64
```

Clang warns when it assigns a concrete target to target-neutral IR; the pipeline suppresses
that expected `-Woverride-module` diagnostic.
