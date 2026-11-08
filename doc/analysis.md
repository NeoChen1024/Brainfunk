# Traces and analyses

`brainfunk` can run a program through an instrumented interpreter that reports every instruction
it runs. On that come trace files, which keep a run so that it can be run again and checked, and
analyses, which say what a run did, as JSON.

```shell
build/bin/brainfunk -O2 -m trace -f program.b -o program.bft < input       # keep a run
build/bin/brainfunk -m analyze -t program.bft                               # run it again, checked against the file
build/bin/brainfunk -O2 -m analyze -a profile,ngram,issue -f program.b -o report.json
build/bin/brainfunk -m analyze -a issue --config machines.json -t program.bft -o report.json
build/bin/brainfunk -O2 -m analyze -a deps --skip 100000000 --limit 10000000 -f program.b
```

`--skip N` runs the first N instructions without reporting them, and `--limit N` stops after N
more, for `analyze` and `trace`. A trace file keeps its own range.

## Events

`trace.hpp` holds the instrumented loop, `run_traced()`, apart from `Brainfunk::run()`, which stays
as fast as it was; `-m analyze` without `-a` runs `stat`, the counts. The unit tests hold the two to the same output, tape and `base_ptr` on thousands
of random programs at every level. Each instruction becomes one `TraceEvent`:

| Field | What it is |
|---|---|
| `index` | instructions run before it, from reset |
| `pc`, `next_pc` | where it is and where the run goes next; `H` has `next_pc == pc`, as the core retires it |
| `base_before`, `base_after` | `base_ptr` around it |
| `read`, `write` | up to two cells each with their values, before the instruction for a read and after it for a write. `MUL` and `MULC` read the source, then the destination |
| `branch`, `taken` | for `JE`, `JN`, `DJN`, `MJN` |
| `steps` | for `F`: strides moved; it tested `steps + 1` cells from `read[0]`, the last zero |
| `fill` | for `SRUN`: cells written from `write[0].cell`, each `write[0].value` |
| `input`, `eof` | for input: the byte is `write[0].value`; at the end of input it is 0 |

`pc`, the word, `next_pc` and `base_after` are what the core's retire port gives. An `F` is one event,
as the core retires it once.

An `F` that never finds a zero would run for ever in `run()`. `F` writes nothing, so the cells it tests
repeat within a tape's length of strides; the traced loop stops there and reports the run `endless`.

A sink is anything with `begin`, `event` and `end`; `Sinks<A, B>` hands each event to several, and
`StatSink` is the counts that `stat` reports.

## Trace files

`trace_file.hpp` gives the layout. In short, the whole file is one zstd frame, so `zstd -dc` opens it,
holding:

- a JSON header: tape size, end-of-input behaviour, range, and where the program came from;
- the program as bincode words, uncompressed, since zstd compresses the whole;
- all of the input;
- the control flow, in chunks: a bit per conditional branch, taken or not, and each `F`'s strides;
- a JSON footer: how the run ended, `pc`, `base_ptr`, and a CRC-32 of the tape.

The program and input determine everything else, so `-t` runs the program again on its input, and
holds every branch, every `F` and the end to what the file says. The control flow alone is enough to
follow `pc` and `base_ptr` without a tape, which is what another implementation's run can be compared
with. Compression runs in a thread of its own, fed through a bounded queue, so that it costs the run
little: `mandel.b` at `-O2`, 1.4 billion instructions, traces in 5 seconds into 6.6 MB.

## Analyses

`-a` takes a comma-separated list; `--config` a JSON object with a member for each analysis that is to
differ from its defaults. The report has `run` (how it ended), `program` (where it came from) and
`analyses`, a member for each.

| Analysis | Configuration | Reports |
|---|---|---|
| `stat` | | counts per opcode, static and dynamic; cell reads and writes; `F` strides |
| `profile` | `top`: rows, 30 | the hottest instructions, with how often each branch was taken |
| `ngram` | `n`: lengths, `[2, 3, 4]`; `top`: rows, 30 | the commonest runs of opcodes, each with how its cells relate to the instruction before: `raw` reads a cell it wrote, `same` shares a cell, `near` touches a neighbour |
| `deps` | | how many instructions back a read's cell was written; basic block lengths; instructions between taken branches; loop trip counts |
| `values` | | values read and written, how often a branch's test is zero, how often `F` starts on a zero |
| `issue` | `machines`: a list, below | cycles for each machine, against the first |

### `issue`

A trace-driven model of in-order cores that issue up to `width` instructions a cycle, from the dual-issue
study. Its defaults are the scalar core in `rtl/` as it is; over the first two million instructions, it
is within 0.01% of the cosimulator on `long.b`, 1% on `hanoi.b`, 2% on `fib.b` and 7% on `mandel.b`, and
`tests/hw/test_cosim.py` keeps it there. It has no caches, so a program that misses, `LostKng.b`, takes
far longer on the core.

Without `--config` it compares `scalar`, the defaults, with `dual`, `width` 2 and `penalty` 10. Each
machine's fields, all optional:

| Field | Default | Meaning |
|---|---|---|
| `name` | `machine` | |
| `width` | 1 | instructions issued a cycle |
| `reads`, `writes` | 2, 2 | cell reads and writes a cycle |
| `branches` | 1 | branches a cycle |
| `raw` | 1 | a read in the same cycle as a write of its cell: 0 never; 1 when cheap: a branch's zero test, after `S`, a `MUL`'s source, an add after an add; 2 always |
| `mul` | `pair-unit` | `split`: source, a cycle for the multiplier, destination, and a `MUL` after one with the same source takes it from a register; `pair-unit`: factors of 1 and -1 read both cells at once, unless they share a bank of different lines; `single`: every `MUL` does |
| `two_mul` | true | two `MUL` destinations in one cycle |
| `fetch_line` | 0 | a group stays within an aligned run of this many words; 0 any two in a row |
| `banks` | 32 | cells per line of banks, two cells in one bank of different lines conflicting; 0 none |
| `penalty` | 9 | cycles lost to a misprediction |
| `steer` | 3 | cycles lost when decode redirects fetch |
| `scan_base`, `line_cells`, `stride_limit` | 8, 32, 16 | `F` costs a cycle per line it reaches, plus `scan_base`; a stride of `stride_limit` or more goes a cell at a time |
| `io` | 7 | cycles an `IO` takes |
| `predictor` | | `next`: next-fetch entries, 16; `tag_bits`: 8; `direction`: counters, 4096; `history`: global history bits mixed into the counter index, 0 |

A group closes at a taken branch, a misprediction, and a redirect. The report gives, for each machine,
`cycles`, `cpi`, `speedup` against the first, `joined` (the share of instructions issued alongside an
older one in the same cycle), and `not_joined_because`, the share that could not, by reason.

```json
{"issue": {"machines": [
    {"name": "scalar"},
    {"name": "dual", "width": 2, "penalty": 10},
    {"name": "dual, aligned fetch", "width": 2, "penalty": 10, "fetch_line": 8},
    {"name": "wide", "width": 4, "reads": 4, "writes": 4, "branches": 2, "raw": 2, "mul": "single"}
]}}
```
