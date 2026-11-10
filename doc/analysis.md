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

`include/brainfunk/trace.hpp` holds the instrumented loop, `run_traced()`, apart from `Brainfunk::run()`, which stays
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
repeat within a tape's length of strides; the traced loop stops there and reports the run `endless`. So
it does at a `JE` or `JN` to itself that is taken, the `[]` some programs end on to wait for ever: it
changes nothing, so it is taken for ever. Neither counts as run.

A sink is anything with `begin`, `event` and `end`; `Sinks<A, B>` hands each event to several, and
`StatSink` is the counts that `stat` reports.

## Trace files

`include/brainfunk/trace_file.hpp` gives the layout. In short, the whole file is one zstd frame, so `zstd -dc` opens it,
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
differ from its defaults. The report has `run` (how it ended, as a trace file's footer says: `pc`,
`base_ptr` and the tape's CRC-32 too), `program` (where it came from) and
`analyses`, a member for each.

| Analysis | Configuration | Reports |
|---|---|---|
| `stat` | | counts per opcode, static and dynamic; cell reads and writes; `F` strides |
| `profile` | `top`: rows, 30; `counts`: the counters, false | the hottest instructions, with how often each branch was taken; coverage: the share of words that ran, and how many branches went both ways, one way, or never ran. With `counts`, `counters`: how often each conditional branch ran and was taken, and where the run entered and was cut, from which every word's count follows; see `coverage.hpp` |
| `ngram` | `n`: lengths, `[2, 3, 4]`; `top`: rows, 30 | the commonest runs of opcodes, each with how its cells relate to the instruction before: `raw` reads a cell it wrote, `same` shares a cell, `near` touches a neighbour |
| `deps` | | how many instructions back a read's cell was written; basic block lengths; instructions between taken branches; loop trip counts |
| `values` | | values read and written, how often a branch's test is zero, how often `F` starts on a zero |
| `issue` | `machines`: a list, below | cycles for each machine, against the first |

### Coverage

Only conditional branches change where a program goes, so `profile` with `counts` keeps just how often
each ran and was taken, with where the run entered and, unless it halted, the word it was cut before.
Every word's count follows, by adding up what flows into it; `coverage.hpp` has the rule, and the unit
tests hold it to counting every word on thousands of random runs, cut short and started late among them.
Runs of one program at one level add up branch by branch.

```shell
build/bin/brainfunk -O0 -m analyze -a profile --config counts.json -f prog.b -o a.json < input-a
build/bin/brainfunk -O0 -m analyze -a profile --config counts.json -f prog.b -o b.json < input-b
build/bin/brainfunk -O0 -m coverage -f prog.b a.json b.json        # counts.json: {"profile": {"counts": true}}
```

`-m coverage` takes the program as the runs had it, and reports for each run the words it reached, the
branch directions it went, and how many of those no other run reached; then the same for all of them,
with their counters added up. `tools/corpus/corpus.py inputs coverage` does this for a corpus program's
inputs.

### `issue`

A trace-driven model of in-order cores that issue up to `width` instructions a cycle, from the dual-issue
study. Its defaults are the scalar core in `rtl/` as it is, its memories included; over the first two
million instructions, it is 0.0% off the cosimulator on `long.b`, 0.1% short on `hanoi.b`, 1.8% on
`fib.b` and 2.2% on `mandel.b`, and 2.6% over on `LostKng.b`, which waits on the instruction cache, and
`tests/hw/test_cosim.py` keeps it there. Over 86 programs of the corpus, with the data cache the core had
then, it was 4.1% off on average and 1.5% short at the median; 61 were within 5%, and the worst 23% short.

Without `--config` it compares `rtl`, the defaults, with the two designs that tag `rtl-dual-issue` built
and doc/rtl.md says why they went: `early resolve`, `early_penalty` 6, and `dual issue`, `width` 2,
`raw` 0, `penalty` 10, `early_penalty` 7 and all four pairing rules. Each machine's fields, all optional:

| Field | Default | Meaning |
|---|---|---|
| `name` | `machine` | |
| `width` | 1 | instructions issued a cycle |
| `reads`, `writes` | 2, 2 | cell reads and writes a cycle |
| `branches` | 1 | branches a cycle |
| `raw` | 1 | a read in the same cycle as a write of its cell: 0 never; 1 when cheap: a branch's zero test, after `S`, a `MUL`'s source, an add after an add; 2 always |
| `mul` | `pair-unit` | `split`: source, a cycle for the multiplier, destination, and a `MUL` after one with the same source takes it from a register; `pair-unit`: factors of 1 and -1 read both cells at once, unless they share a bank of different lines; `single`: every `MUL` does |
| `two_mul` | true | two `MUL` destinations in one cycle |
| `share_source` | false | with `split`, a `MUL` right after one with the same source takes it from a register |
| `fetch_line` | 0 | a group stays within an aligned run of this many words; 0 any two in a row |
| `pair_simple` | false | only `A`, `S`, `M`, `JE` and `JN` share a group |
| `branch_first`, `branch_last` | false, false | a branch only opens a group; a branch closes its group |
| `pair_disjoint` | false | two in a group touch different cells |
| `write_banks` | 0 | two writes in a group are to cells different modulo this; 0 any |
| `early_second` | true | a branch second in its group may resolve early |
| `early_store` | true | with `early_cells`, a store of values; false, only the zero bits |
| `banks` | 16 | cells per line of banks: a `MUL`'s two cells, different and equal modulo this, are not read at once, as the core's tape keeps cells 16 apart in one RAM; 0 none |
| `penalty` | 9 | cycles lost to a misprediction |
| `steer` | 3 | cycles lost when decode redirects fetch |
| `scan_base`, `line_cells`, `stride_limit` | 8, 32, 16 | `F` costs a cycle per line it reaches, plus `scan_base`; a stride of `stride_limit` or more goes a cell at a time |
| `io` | 7 | cycles an `IO` takes |
| `early_penalty` | 0 | cycles a misprediction costs when the branch resolves as soon as its cell's value is known, rather than at X; 0 never |
| `late` | 4 | cycles after its writer issues that a value is known, at X, for `early_penalty` |
| `forward` | `add` | values decode works out itself, known as their writer issues: `none`; `set`, constants; `add`, constants and adds of constants to known values (`A`, `DJN`); `mul`, `MUL` by 1 or -1 of known values too |
| `early_cells` | 4 | cells in a direct-mapped store the early values come from, each cell touched filling its slot, a cell read only once the D-cache has it at X; a branch whose cell is not there resolves at X; 0 the whole tape, wrong paths aside |
| `early_flush` | true | a misprediction empties that store, which the wrong path may have written |
| `early_partial` | false | a branch whose cell's value is known only after it issues resolves when it is; false, at X |
| `zero_bits_penalty` | 0 | what a misprediction the zero bits resolve costs, if not `early_penalty` |
| `early_zero_bits` | true | a branch whose cell is not in the store tests a bit per cell of the whole tape instead, written in order as values are known at X |
| `predictor` | `rtl` | how branches are guessed, below |
| `memory` | the core's | the tape on chip, the instruction cache and SDRAM, below; `false` or `null` for a memory that always hits |

A group closes at a taken branch, a misprediction, and a redirect. The report gives, for each machine,
`cycles`, `cpi`, `speedup` against the first, `joined` (the share of instructions issued alongside an
older one in the same cycle), `not_joined_because`, the share that could not, by reason, and
`branches`: how many were backward or forward, taken or not, and how many of each were mispredicted,
`penalty_cycles`, what mispredictions cost, and `resolved_early`, how many cost only `early_penalty`.
`steers_by` splits the steers: `taken_miss`, a taken guess fetch did not make; `wrong_target`;
`not_taken_hit`, a next-fetch hit decode took back; `not_a_branch`, a hit on a word that is no branch.
`memory` counts `i_misses`, `d_misses`, `writebacks` and `refreshes`, and `i_cycles` and `d_cycles`,
what instructions waited for each cache.

#### Memory

The core keeps its tape on chip, which never misses, and its code in SDRAM behind a cache. Either can be
either: `d_on_chip` 0 puts a data cache in front of the tape, as the core had before, and `i_on_chip` 1
takes the code on chip too. The model does not count the core's restarts of a read that meets a write
to the other cell of its RAM's row, which cost `mandel.b` about 0.4% (rtl.md).

Caches are direct-mapped and share an SDRAM that moves a line at a time, as in the core. An instruction
whose word misses waits for its line, which fetch asked for `fetch_lead` + `i_request` cycles after it
would have fetched the word, and issues `i_resume` cycles after the line's last word. An instruction
whose cell misses issues again `d_resume` cycles after its line's last word, or `penalty` + 1 cycles
after it first issued if that is later; the line asks for the SDRAM `d_request` cycles after the
first issue, and first writes back the line it replaces if that was written. `F` reads a line a
cycle and starts again from a line that misses. The SDRAM takes one request at a time, a read for
`read_busy` cycles with its last word at `read_latency`, a write back for `write_busy`, and refreshes
for `refresh_busy` cycles every `refresh_every` once it is idle. The defaults are measured on the
cosimulator: a run of misses to clean lines, of misses to written ones, and of straight-line code,
each within 0.1%.

| Field | Default | Meaning |
|---|---|---|
| `d_on_chip`, `i_on_chip` | 1, 0 | 1: the tape, or the code, in block RAM, and never a miss; 0: behind a cache |
| `i_lines`, `i_words` | 512, 8 | instruction cache lines, and words in a line |
| `d_lines`, `d_cells` | 512, 32 | data cache lines, and cells in a line |
| `zero_table` | 1 | 1: F reads a line not in the data cache from the zero table, so it never misses; 0: it misses like any read |
| `read_latency`, `read_busy`, `write_busy` | 15, 17, 19 | SDRAM cycles, above |
| `refresh_every`, `refresh_busy` | 1170, 14 | |
| `fetch_lead`, `i_request`, `i_resume` | 5, 4, 12 | |
| `d_request`, `d_resume` | 7, 7 | |

#### Predictors

Branches are guessed in two parts. Fetch has a next-fetch table keyed by the word fetched before: a hit
sends fetch to the target with no gap. Decode has a direction predictor; a taken guess that fetch did
not make costs `steer` cycles, and a wrong direction `penalty`. Every predictor takes `next`, the
next-fetch entries (16), `tag_bits` (8), `forget` (whether a hit on a branch that is not taken drops
the entry, or with `next_counter` one whose counter has run down), and `next_counter` (false; not for
`rtl`): a 2-bit counter in each entry, so that fetch goes to the target only at 2 or more; `lookup`
and `update` (0, 0): the cycles before a branch issues that its tables are read, and after, that its
outcome is written back to counters and per-branch histories. Global histories take an outcome as the
branch issues, as a core does that shifts in its guess and repairs the history after a misprediction.
The core reads its tables in I1 and writes them in W: `lookup` 3, `update` 6. `kind` picks the rest:

| `kind` | Fields, with defaults | Guess |
|---|---|---|
| `rtl` | `direction` 4096, `history` 0 | the core as it is: a next-fetch hit is a taken guess decode does not question; the 2-bit counters, of how far a branch disagrees with backward taken, are read only on a miss. `forget` is true |
| `static` | | backward taken, forward not |
| `bimodal` | `direction` 4096 | 2-bit counters by pc |
| `gshare` | `direction` 4096, `history` 12 | counters by pc exclusive-or the last `history` outcomes |
| `local` | `histories` 256, `history` 6, `direction` 16384 | each branch's own last `history` outcomes, next to its pc bits, index the counters |
| `loop` | `entries` 64, `confidence` 2, `base` (bimodal) | once a loop has gone round the same number of times `confidence` times running, its exit; otherwise `base` |
| `tournament` | `components` (`local`, `gshare`), `chooser` 4096, `chooser_history` 0 | 2-bit counters by pc choose between two components |
| `perfect` | | always right; what is left is the steers |

Every kind but `rtl` overrides fetch: decode's guess stands, and taking back a next-fetch hit that it
does not agree with is a steer. `forget` is false for them unless given. Components (`base`,
`components`) are predictors without `next`, `tag_bits` or `forget`, and not `rtl` or `perfect`.

To compare machines over the corpus rather than one program, `tools/issue_sweep.py run --config
machines.json -o sweep.json` runs every unique 8-bit workload program on each of its inputs, and
`tools/issue_sweep.py report sweep.json` gives each machine's speedup as the geometric mean of the
families' geometric means, with the families split into held-out folds.

```json
{"issue": {"machines": [
    {"name": "rtl"},
    {"name": "dual", "width": 2, "penalty": 10},
    {"name": "dual, aligned fetch", "width": 2, "penalty": 10, "fetch_line": 8},
    {"name": "wide", "width": 4, "reads": 4, "writes": 4, "branches": 2, "raw": 2, "mul": "single"},
    {"name": "rtl, tournament",
     "predictor": {"kind": "tournament", "forget": true, "next": 64,
                   "components": [{"kind": "loop", "base": {"kind": "local"}}, {"kind": "gshare", "history": 8}]}}
]}}
```
