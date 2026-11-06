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
