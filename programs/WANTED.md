# Programs left out for now

Programs we would like in the corpus but have not committed, and why. A program reachable
through a submodule is not listed here: the submodule points at its public upstream instead
of copying it.

## No license found

| Program | Author | Where | Notes |
|---|---|---|---|
| `hi123.b` | Oleg Mazonka | <https://web.archive.org/web/20200808172835/http://mazonka.com/brainf/hi123> | A dummy program printing hello123 in the dbfi dialect, with its input after `!`. The page states no license. |
| `sisihi.b` | built from Cristofani's `dbfi.b` and Mazonka's `hi123` | Mazonka's page describes the `si ! si ! hi123` setup | dbfi running dbfi running hi123. It can be rebuilt locally from `brainfuck.org/dbfi.b` once `hi123` is available. |
| Classic programs in the Esoteric File Archive | various | <http://esoteric.sange.fi/brainfuck/bf-source/prog/> | `99botles.bf`, `PRIME.BF`, `PI16.BF`, `quine505.b`, `rpn.b` and others. Some reach us through `3rdparty/bfcorpus` and `3rdparty/rdebath-Brainfuck`. |

## Too large for the current limits

Programs must run within 1M instruction words and 64 KiB of cells.

| Program | Size | Notes |
|---|---|---|
| ELVM's Brainfuck output (`3rdparty/ELVM`, `make build-bf`) | 7.9 MB to 159 MB of source each, 928 MiB for the 71 tests | `hello.c` becomes 275,395 bitcode words at `-O2`, within the limit, but it touches about 51 million cells: registers start at 0, so the first push wraps the stack pointer to the top of the 24-bit address space, three cells per word. Fitting 64 KiB would need a patch to ELVM so that programs start with a small stack pointer. Not decided yet. |

## Replaced by an upstream copy

The local `test/` corpus that preceded this directory came mostly from a 2013 GitHub bundle
(the same files as `fabianishere/brainfuck` `examples/` and `bibanez/brainfuck-c`
`brainf_progs/`), not from the original authors. Its programs are now read from the
sources below:

| Old file | Now | Difference |
|---|---|---|
| `bench.b` | `3rdparty/bfcorpus/cwfitzgerald/bench.b` | identical code |
| `bottles.b` | `3rdparty/bfcorpus/cwfitzgerald/bottles.b` | identical code |
| `e.b` | `programs/brainfuck.org/e.b` | identical |
| `fib.b` | `programs/brainfuck.org/fib.b` | the old copy lacks the comments |
| `hanoi.b` | `3rdparty/bfcorpus/copy/third_party/hanoi.bf` | identical code |
| `hello.b` | `3rdparty/bfcorpus/cwfitzgerald/hello.b` | identical code |
| `long.b` | `3rdparty/rdebath-Brainfuck/testing/Long.b` | identical code |
| `LostKng.b` | `3rdparty/rdebath-Brainfuck/testing/LostKng.b` | identical |
| `mandel.b` | `3rdparty/rdebath-Brainfuck/testing/Mandelbrot.b` | identical code |
| `si.b` | `programs/brainfuck.org/dbfi.b` | the old copy is Mazonka's reformatting |
| `sierpinski.bf` | `3rdparty/bfcorpus/nyyrikki/triangle.bf` | whitespace only |
| `squares.b` | `programs/brainfuck.org/squares.b` | **the old copy was modified**: it starts with 12 more `+` and prints squares up to 160000, where the original stops at 10000 |
| `hi123.b`, `sisihi.b` | not available | see above |
