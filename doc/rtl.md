# RTL

`rtl/brainfunk_core.sv` is a scalar pipelined core for the instruction set in
[bitcode.txt](bitcode.txt) and [bincode.txt](bincode.txt), built for clock speed and checked
against the C++ interpreter at every retire.

It does not implement the fused instructions (`MULC`, `DJN`, `MJN`, `SRUN`) and traps on
them, so programs must come from `-O2` or lower.

```text
rtl/brainfunk_core.sv    the core
rtl/brainfunk_mem.sv     block RAM behind a registered bank multiplexer
rtl/brainfunk_soc.sv     core with instruction and data memory
rtl/board/               YADAN Board top, PLL and UART
tb/                      Verilator cosimulation and UART testbench
formal/                  SymbiYosys properties
synth/                   Tang Dynasty flow for the YADAN Board
```

## Pipeline

```text
IF  I1 I2  D   A   M0 M1 M2  X   W
```

| Stage | Work |
|---|---|
| `IF`, `I1`, `I2` | Instruction memory read, three clock edges |
| `D` | Decode; predict `JE` and `JN` backward taken, forward not taken |
| `A` | `base_ptr` + offset; `M` moves `base_ptr` here |
| `M0`, `M1`, `M2` | Data memory read, three clock edges; `M0` compares addresses with older writes |
| `X` | Result and zero test; resolves branches and the end of `F` |
| `W` | Writes the data memory and retires |

Block RAM sets the shape. On the EG4S20 its clock-to-out is about 4 ns, and each logic level costs
about 1.5 ns with routing, so only a register can follow it. A read is the block RAM, a register
per bank, then a register after the bank multiplexer.

Nothing past `A` stalls, so address comparison in `M0` settles where each operand comes from
before the value exists. `X` only has one multiplexer to go through, because results older
than the instruction right ahead are picked up in `M2`. A read colliding with a write of the same
cell is forwarded, so its undefined result is never used; simulation makes it return wrong data
to keep that true.

Anything younger than an unresolved branch or `F` is speculative. It can move `base_ptr`, which is
restored, and read memory. It cannot write, because the flush reaches `X` before it gets to `W`.
Input waits for an empty pipeline. Output acts in `X`, when everything older has been resolved.

## Timing

| Event | Cycles |
|---|---|
| `A`, `S`, `M`, `JE`, `JN` | 1 |
| `MUL` | 3: source, multiplier, destination |
| `F` | 1 per cell read, plus 5 to end |
| Branch predicted taken | 3 more |
| Branch mispredicted | 9 more |
| `IO` | about 8, plus waiting for the byte |

`F` assumes each cell is non-zero and reads one per cycle, with `base_ptr` stepping ahead. The
read that finds a zero flushes the reads behind it.

Compared with the earlier multi-cycle core on the same images at `-O2`:

| Program | Multi-cycle | Pipeline | CPI |
|---|---|---|---|
| `mandel.b` | 4.96 G cycles | 3.94 G cycles | 3.50 → 2.78 |
| `hanoi.b` | 183.2 M cycles | 141.2 M cycles | 2.68 → 2.07 |

In `mandel.b`, 46% of the cycles are charged to `MUL` and 16% to `F`. The charge includes the
misprediction before an instruction, which is why `MUL` averages 5.4 cycles.

## Verification

```shell
make hw-test                   # cosimulation, UART testbench and formal
HW_TEST_FULL=1 make hw-test    # also long.b and mandel.b, about 2 hours
```

**Cosimulation** runs the core in lock step with `libbrainfunk`. At every retire it compares
pc, next pc, `base_ptr`, the write address and data, and output; at halt it compares the
whole tape. `bit2bin` produces the image, and the interpreter loads the same file with
`load_bincode`, so the assembler and decoder are covered too. Input stalls and output
backpressure can be randomized with `--stall`.

**Formal** checks properties against abstract memories: two tape cells and one instruction word,
at addresses the solver picks, behave like real memory with its latency, and every other read
returns anything. So the properties cover every cell and every pair of cells, and words that no
compiled program contains. They state that whatever reaches `W` is on the program's path and is
the word at that pc; the pc and `base_ptr` rules; results on architectural cell values, which
covers forwarding; writes only at retire and at the effective address; sticky halt and trap,
exactly on illegal words; one I/O byte per instruction; and no lost output.

The checks are bounded. `bmc` runs to 32 steps with the multiplier replaced by an XOR on both sides,
because the multiplier is what the solver spends its time on, and `multiplier` keeps the real one
to 15 steps. There is no unbounded proof: induction needs invariants relating every pipeline
stage, which have not been written. `cover` shows every path is reachable, and `latency` bounds
every instruction except `F` and `IO`.

## YADAN Board

The board has an Anlogic EG4S20NG88. `rtl/board/yadan_pll.sv` makes 168 MHz from its 24 MHz clock.
`rtl/board/yadan_top.sv` runs the image built into the bitstream over the USB UART at 115200 8N1.
`LED_23` is on while running, off when halted, and blinks on a trap.

```shell
./brainfunk -O2 -m bit -f program.b | ./bit2bin > program.hex
export TD_HOME=/path/to/TD
make yadan IMAGE=program.hex    # build/yadan/yadan.bit and reports
make yadan-load                 # JTAG to SRAM; the flash is not touched
```

`K_51` restarts the program. So does opening the serial port, because RTS drives the same net. A
restart clears the tape, then waits about 200 ms, so a host opening the port does not lose the first
output. `make yadan-load` needs read/write access to the cable, USB `336c:1002`.

`IMEM_ADDR_WIDTH` in `yadan_top` is 13, or 8192 instructions, which fits every program in the
test corpus except `LostKng.b`.

| `DMEM_ADDR_WIDTH` | Tape | Minimum period | Block RAM |
|---|---|---|---|
| 16 | 64 KB | about 5.5–5.9 ns | 16 of 32 Kb, 9 of 9 Kb |
| 15 | 32 KB | about 4.9 ns | 11 of 32 Kb |
| 14 | 16 KB | about 5.0 ns | 7 of 32 Kb |

Those are with the `mandel.b` image. Below about 4.8 ns the block RAM's clock-to-out is the limit.

Tang Dynasty's `read_verilog` elaborates each file as it reads it, so the build concatenates
the sources. It stays at its prompt after an error and then exits successfully, so
`synth/build-yadan.sh` closes its input and checks the log. It crashes with a backtrace if it
cannot open the JTAG cable, and exits successfully then too.
