# Performance

The same programs, at `-O2`, run three ways: by the core in [rtl.md](rtl.md), by the interpreter on a
desktop processor, and as native code on that processor, compiled from the [LLVM IR](llvm-ir.md). Each
takes some number of its own clock's cycles for every instruction of the program, and has a clock; this
is what those come to, and why.

## What is counted

An **instruction** is one the `-O2` program executes, as `brainfunk -m analyze -a stat` counts them. The
cosimulator retires exactly as many, and `tools/cpi_compare.py` takes no cycles from a run where it
does not. All three divide by that one count, so native code, which has no such instructions left in
it, still gets a number that compares: its cycles for the work of one. `-O2` because the core does not
run `-O3`'s fused instructions; the interpreter and the IR take the same `-O2` program, though both
could take `-O3`. `F` is one instruction however far it scans.

**Cycles** are, for the core, the cosimulator's for the whole run: the core with its tape on chip and
its code in the SDRAM model behind the instruction cache, as on the board. For the host they are
`perf stat -e cycles:u`: user mode only, and ticks of the clock as it really ran, so that the count does
not depend on the frequency of the moment. What a process costs before the program's first instruction
is measured apart and taken off.

The **clocks** are 168 MHz, where the core closes timing on the YADAN Board, and 4.1 GHz, the highest
the host's processor, a Ryzen 7 5700X3D, is sold to reach. Instructions a second are a clock over
cycles per instruction. The host's is an upper bound: while these ran, its counters made 3.73 to 4.03
billion cycles a second of the task's time.

## Results

Measured on 2026-11-10 with `tools/cpi_compare.py run --repeat 15`: an AMD Ryzen 7 5700X3D, Linux
6.18.60, Clang 23.1.4, the fewest cycles of 15 runs of each command; the core in the Verilator
cosimulator, whole runs. `native` is `clang -O3 -flto`, and `native, this cpu` adds `-march=native`.

Cycles per instruction:

| program | instructions | core | interp | native | native, this cpu |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Collatz.b` | 1,593,138,658 | 1.885 | 5.951 | 1.407 | 1.396 |
| `Mandelbrot.b` | 1,415,963,057 | 1.503 | 7.589 | 1.497 | 1.364 |
| `Long.b` | 670,380,977 | 1.360 | 5.151 | 1.032 | 1.038 |
| `Hanoi.b` | 68,379,803 | 1.039 | 5.473 | 2.532 | 2.515 |
| `Golden.b` | 3,433,972 | 3.841 | 19.4 | 8.535 | 8.520 |
| `factor.b` | 2,945,185 | 1.584 | 10.2 | 1.359 | 1.334 |
| `BusyBeaver.b` | 81,933 | 79.3 | 6,371 | 3,024 | 2,461 |
| geometric mean |  | 2.947 | 20.6 | 5.690 | 5.429 |
| without `BusyBeaver.b` |  | 1.703 | 7.943 | 1.999 | 1.959 |

Million instructions a second, the core at 168 MHz and the host at 4100 MHz, and the host's over the
core's:

| program | core | interp | native | native, this cpu | interp / core | native / core | native, this cpu / core |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `Collatz.b` | 89.1 | 689 | 2,914 | 2,938 | 7.730 | 32.7 | 33.0 |
| `Mandelbrot.b` | 112 | 540 | 2,739 | 3,007 | 4.834 | 24.5 | 26.9 |
| `Long.b` | 124 | 796 | 3,971 | 3,949 | 6.444 | 32.1 | 32.0 |
| `Hanoi.b` | 162 | 749 | 1,619 | 1,630 | 4.632 | 10.0 | 10.1 |
| `Golden.b` | 43.7 | 211 | 480 | 481 | 4.836 | 11.0 | 11.0 |
| `factor.b` | 106 | 403 | 3,016 | 3,072 | 3.800 | 28.4 | 29.0 |
| `BusyBeaver.b` | 2.119 | 0.644 | 1.356 | 1.666 | 0.304 | 0.640 | 0.786 |
| geometric mean | 57.0 | 199 | 721 | 755 | 3.483 | 12.6 | 13.2 |
| without `BusyBeaver.b` | 98.7 | 516 | 2,051 | 2,093 | 5.231 | 20.8 | 21.2 |

The host's instructions for each instruction of the program, and how many of them it ran in a cycle:

| program | interp, each | native, each | interp, a cycle | native, a cycle |
| --- | ---: | ---: | ---: | ---: |
| `Collatz.b` | 11.9 | 3.427 | 2.004 | 2.436 |
| `Mandelbrot.b` | 14.8 | 5.060 | 1.944 | 3.381 |
| `Long.b` | 11.5 | 1.811 | 2.237 | 1.754 |
| `Hanoi.b` | 11.7 | 2.022 | 2.137 | 0.799 |
| `Golden.b` | 49.3 | 28.1 | 2.542 | 3.296 |
| `factor.b` | 11.3 | 2.533 | 1.113 | 1.864 |
| `BusyBeaver.b` | 21,847 | 14,561 | 3.429 | 4.816 |

These are seven programs, each counting once, not the corpus: the means say how these compare, and
[rtl.md](rtl.md#tried-and-removed) has what the corpus says of the core.

## What decides each

**The core** takes a cycle for an instruction, and more for what [rtl.md](rtl.md#timing) lists: a
misprediction, a `MUL` whose factor is not 1 or -1, an `F`, an instruction cache miss. `Hanoi.b` is
nearly all the first kind, at 1.04. `Mandelbrot.b`'s 1.50 is its `MUL`s, 23% of its instructions at
1.53 cycles, and the mispredictions charged to what follows them. `Golden.b`'s 3.84 is `F`: 28% of
its instructions, 9.0 cycles each for 15 cells.

**The interpreter** spends 11 to 15 of the host's instructions on one of the program's, where no `F`
scans far, and the host runs about two of them a cycle: 5 to 8 cycles. Each instruction ends in an
indirect jump to the next one's handler, and a wrong guess there is what separates the programs:
`Long.b` has 4 mispredicted branches in a thousand instructions and `Mandelbrot.b` 60.

**Native code** needs 2 to 5 of the host's instructions for one of the program's, at up to 3.4 a
cycle, which comes to 1.0 to 1.5 cycles: what the core takes. So where the two differ by the ratio of
their clocks, 24.4, the clock is all there is between them: 24.5 on `Mandelbrot.b`. It is 32 where
native code takes fewer cycles than the core, and 10 or less where it takes more:

- **`Hanoi.b`**: 90% of its instructions are one delay loop, `A 14, 255`, `S 15, 0`, `JN 14, -3`.
  Clang keeps the counter in a register, and still stores it, stores the zero, and loads the counter
  back every time round: addresses wrap by masking, and it does not work out that the two cells
  differ. So the count goes through memory on every pass, and a pass takes about 8 cycles where the
  core, which forwards a write to the read behind it, takes 3.
- **`BusyBeaver.b`**: 18,201 `F`s look at 198.8 million cells between them. The core reads a 32-cell
  line a cycle and takes 6.34 million cycles for them, 31 cells a cycle. Native code takes 1.25 cycles
  a cell, a byte at a time, and the interpreter 2.6. At their clocks that is 38.7 ms for the core,
  60.4 ms for native code and 127 ms for the interpreter: the core at 168 MHz is 1.56 times as fast as
  native code at 4.1 GHz, and 3.3 times as fast as the interpreter. `-march=native` gets native code to
  a cycle a cell, 49.2 ms.
- **`Golden.b`** is the same in small: its `F`s cross 15 cells each, which the core does in a line or
  two and the host a byte at a time.

A compiler may also leave nothing to count. `Bench.b`'s 1.57 million instructions are loops that Clang
works out, and its native run cannot be told from that of an empty program; the report shows such a
run as `~0`.

## Measuring

```shell
tools/cpi_compare.py run -o cpi.json            # about 25 minutes, nearly all of it cosimulation
tools/cpi_compare.py report cpi.json
tools/cpi_compare.py report cpi.json --without '*BusyBeaver*' --markdown
tools/cpi_compare.py run --no-core -o host.json 'programs/brainfuck.org/*.b'
```

`run` takes its thirteen programs, or the corpus programs whose paths match, on each of their inputs.
It needs `perf`, Clang and the cosimulator; `--no-host` and `--no-core` do without. A case must halt,
since native code cannot be stopped part way, and a native build that prints anything but what the
interpreter printed stops the run. Run again into the same file, it keeps a case's core result while
the image and the cosimulator are the files they were.

What limits the host's numbers:

- **Other work on the machine.** A thread sharing the core adds cycles and never takes any away, so the
  fewest of the runs count. On a desktop in use the median run took 1.6% more than the fewest at the
  median, and up to 16%. The scheduler found a quiet core more often than pinning to one CPU did, which
  `--cpu` still does.
- **Starting.** The interpreter's start is the same command writing an image instead of running, and
  native code's is the wrapper around an empty program. Fifteen runs of the interpreter's start were
  0.3 to 1.4 million cycles apart, so `report` leaves out a program the interpreter runs in under 20
  million. That took six of the thirteen here: `Bench.b`, `bottles.b`, `squares.b`, `sierpinski.b`,
  `triangle.bf` and `memoryhog.b`. The smallest left, `factor.b`, is 30 million.
- **Input and output.** Only user mode is counted, and output goes to `/dev/null`. The wrapper flushes
  after every byte, which is user-mode work that native code is charged for; the core's `IO` takes
  about 8 cycles with nothing holding it up, as the cosimulator has no UART.
- **The cosimulator is not the board.** It gives the core's cycles exactly, and 168 MHz is where timing
  closes; the product is not a time anyone measured. `Mandelbrot.b`'s 2.129 billion cycles are 12.67 s
  at 168 MHz, and [rtl.md](rtl.md#yadan-board) has the board at 12.87 s for the same program.
