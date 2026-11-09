# Brainfunk — Agent guide

Brainfunk is a Brainfuck toolchain in C++20 (interpreter, optimizer, C and LLVM IR code generators, and
the Brainfunk Bitcode instruction set with its program images) and a pipelined SystemVerilog core that
runs that instruction set on an FPGA.

## Sources of truth

- [README](README.md): build, modes, optimization levels, program images.
- [Bitcode](doc/bitcode.txt) and [bincode](doc/bincode.txt): the instruction set and its encoding,
  which the interpreter and the RTL both implement.
- [Optimizer](doc/optimizer.md): what each `-O` level does.
- [LLVM IR backend](doc/llvm-ir.md): the emitted module and its runtime ABI.
- [Traces and analyses](doc/analysis.md): trace files, analyses, and the issue model.
- [RTL](doc/rtl.md): the core, the memory system, verification, and the boards.
- [Programs](programs/README.md): the corpus, where each program comes from, and its license.
- [Tests](tests/CMakeLists.txt): ctest labels, and the options that register more tests.

Keep this guide about engineering workflow. When a rule changes, update the document that owns it and
link to it from here; do not copy specs, defaults, timings or measurements into this file.

## Layout

- `include/brainfunk/` holds the library's headers, included as `"brainfunk/..."`. `src/` holds the
  library and the programs: `brainfunk`, `bfconsole`, `bf`, `bfstrip`, `visualbrainfunk`.
- `rtl/` is the core, caches, SoC and memory controller; `rtl/board/` is the only board-specific RTL.
  `tb/` has the cosimulator and models, `formal/` the properties, `synth/` the FPGA flow.
- `tests/test-libbrainfunk.cpp` is Catch2; `tests/sw/` and `tests/hw/` are pytest.
- `tools/` has the scripts: the corpus tool, sweeps, diagram rendering, and the compile scripts.
- `programs/` is the corpus; `3rdparty/` holds submodules: corpus sources, nlohmann/json, ctre.
  `test/` has the classic programs that tests and docs name by path.

## Build and verification

```sh
cmake --workflow --preset default   # configure, build into build/, run every test that needs no board
ctest --preset sw                   # library and programs, under a minute
ctest --preset corpus               # every corpus program on its inputs, at every -O level
ctest --preset hw                   # cosimulation, testbenches, formal
```

Programs end up in `build/bin`. Run what a change can reach:

| Change | Run |
|---|---|
| Library, optimizer, code generators | `sw` and `corpus` |
| RTL | `hw`, and mutation (`BRAINFUNK_TEST_MUTATION`); synthesize and read the slack if timing can move |
| Board top, loader, flash, UART | `hw`, then the board tests (`BRAINFUNK_TEST_BOARD`) |
| `doc/diagrams/*.json`, or Mermaid in the docs | `tools/render_diagrams.py`, then `sw` |

Synthesis needs Tang Dynasty: `cmake --build build --target yadan` with `TD_HOME` set, and `yadan-load`
puts the bitstream on the board over JTAG. [doc/rtl.md](doc/rtl.md#verification) has the pytest options.

Do not edit files that a running test, mutation run or synthesis reads; wait for it, or stop it first.

## Rules

- `src/bf.cpp` is a small, independent interpreter kept for comparison. Do not change it.
- Load a board over JTAG. Writing its flash wears the flash and changes what the board boots: do it
  only when the user asks (`--flash`, `BRAINFUNK_TEST_FLASH`).
- Every SymbiYosys run sets a `timeout` in its `.sby`; never stop a solver from outside, which leaves
  it running. TIMEOUT is no answer, and only FAIL counts as a mutant caught. A question that needs a
  deeper bound than the timeout allows is argued from the design, or with a targeted invariant.
- JSON in C++ goes through nlohmann/json, and nothing else.
- When the RTL's timing changes, update the issue model's defaults in `src/analysis.cpp` and
  [doc/analysis.md](doc/analysis.md) to match.
- Corpus programs keep their authors' licenses; read [programs/README.md](programs/README.md) before
  adding one.

## Tests

- The interpreter is the reference. Cosimulation compares the core with it at every retire; the corpus
  compares every `-O` level with the results recorded in `inputs.lock`. Expected results come from
  another implementation or from recorded data, never from the code under test alone.
- New RTL behaviour needs a mutant in `tests/hw/mutants.py` that the tests catch. Logic that no test
  can reach goes in the formal-only list, with the reason; a mutant nothing catches is a gap to close or
  to explain, never one to drop quietly.
- A regression test must fail without its fix. Check that it does.
- A speedup is a measurement: cycles from the cosimulator or the board. The issue model guides
  choices but can be far off.
- Cosimulation does not see the board's timing or the SDRAM's real data window; only the board tests do.

## Writing

- Comments and docs say why, and what holds, not what the next line does.
- Commit subjects are `type(scope): summary` in lowercase prose, such as `feat(rtl): ...` or
  `refactor: ...`; the body says why, and what was measured.
- A measured number in the docs says where it was measured, and how.
