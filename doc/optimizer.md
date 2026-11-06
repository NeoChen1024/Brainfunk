# Bitcode optimizer

`Brainfunk::optimize(level)` rewrites translated bitcode. Each level includes the previous
ones. Every level preserves the program's output, final memory, and final `base_ptr`;
`tests/` checks this against unoptimized execution, on random programs and through every
backend.

## Level 1: lazy moves

Most `M` instructions only exist so that the next data instruction can reach its cell. With
base-relative operands, that distance can live in the operand instead.

The pass tracks `pending`, the distance from `base_ptr` to where the original program's
pointer would be:

```text
original pointer == base_ptr + pending
```

| Instruction | Rewrite |
|---|---|
| `M k` | `pending += k`, emit nothing |
| `A`, `S`, `IO` | add `pending` to `addr_offset` |
| `MUL` | add `pending` to `addr_offset` and to `src_offset` |
| `JE`, `F` | add `pending` to `test_offset` |
| `JN` | emit `M (pending - entry)` if needed, set `pending = entry`, then add it to `test_offset` |
| `H` | emit `M pending`, so that the final `base_ptr` is unchanged |

### Why loops stay correct

A label needs a single value of `pending` on every incoming edge. Jump targets only occur
just after a `JE` or `JN`, so loops are the only merges:

- The body starts with the value `entry` that `pending` had at `JE`.
- `JN` re-enters the body, so `pending` must equal `entry` there. Any difference is the
  distance the loop really moves per iteration, and becomes an `M` on the back edge.
- The code after the loop is reached from `JE` and from `JN`, both with `entry`.

A loop that returns to where it started therefore needs no `M` at all. `base_ptr` carries
real motion only, such as a loop's stride or the unknown distance covered by `F`.

### Flushing

An offset that would not fit its field forces `M pending` first, which resets `pending` to
zero. No program in the test corpus reaches these limits, so `tests/run-tests.sh` covers each
one with a synthetic program.

## Level 2: block-local folding

Between two control-flow boundaries every cell is a constant offset from `base_ptr`, so
aliasing is decided at compile time. This pass needs no change to the instruction set. It
tracks, per cell:

- `pending`: a write not yet emitted (`S v` or `A v`);
- `known`: the exact value, if known.

It uses them to:

| Input | Result |
|---|---|
| `A 0,1 ; A 1,2 ; A 0,3` | `A 0,4 ; A 1,2`, merged across the unrelated cell |
| `S 0,0 ; A 0,5` | `S 0,5` |
| `S 0,1 ; S 0,2` | `S 0,2`, the first write is dead |
| `S 0,1 ; IO 0,0` | `IO 0,0`, input always stores, so the write is dead |
| `S 0,3 ; MUL 1,0,2` | `S 0,3 ; A 1,6` |
| `S` of a value the cell is known to hold | removed |
| `JE` or `F` testing a cell known to be zero | removed, with the loop body |

A pending write reaches memory before anything reads its cell, and all of them before any
boundary, so the order of `IO` and the state at `H` do not change.

### What is known across a boundary

Almost nothing: a loop body can run any number of times, so `known` is cleared at `JE`, `JN`
and `F`. The one exception is what ended the loop. Both `JE` taken and `JN` not taken leave the
tested cell at zero, and the lazy-move pass guarantees they test the same cell, so that one
fact survives. This is what removes the `[-]` that often follows a loop.

### Small tapes

Two different offsets can name one cell when they differ by a multiple of the memory size.
Cells are therefore keyed by their address modulo memory size.
