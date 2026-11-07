# RTL

`rtl/brainfunk_core.sv` is a multi-cycle core for the instruction set in
[bitcode.txt](bitcode.txt) and [bincode.txt](bincode.txt). It is the baseline for later
microarchitecture work: small, one instruction at a time, and checked against the C++
interpreter at every retire.

It does not implement the fused instructions (`MULC`, `DJN`, `MJN`, `SRUN`) and traps on
them, so programs must come from `-O2` or lower.

```text
rtl/brainfunk_core.sv    the core
rtl/brainfunk_soc.sv     core with instruction and data block RAM
rtl/board/               YADAN Board top and UART
tb/                      Verilator cosimulation and UART testbench
formal/                  SymbiYosys properties
synth/                   Tang Dynasty flow for the YADAN Board
```

## Timing

Memories are synchronous, one access per cycle. An instruction spends one cycle fetching and
one decoding, in which it can make its first memory access.

| Instruction | Cycles |
|---|---|
| `S`, `M`, `H` | 2 |
| `A`, `JE`, `JN` | 3 |
| `MUL` | 4 |
| `F` | 3 + 2 per pointer step |
| `IO` input | 3, plus waiting for a byte |
| `IO` output | 4, plus waiting for the byte to be taken |

## Verification

```shell
make hw-test                   # cosimulation, UART testbench and formal
HW_TEST_FULL=1 make hw-test    # also long.b and mandel.b, about 40 minutes
```

**Cosimulation** runs the core in lock step with `libbrainfunk`. At every retire it compares
pc, next pc, `base_ptr`, the write address and data, and output; at halt it compares the
whole tape. `bit2bin` produces the image, and the interpreter loads the same file with
`load_bincode`, so the assembler and decoder are covered too. Input stalls and output
backpressure can be randomized with `--stall`.

**Formal** proves properties for arbitrary memory contents and I/O, which includes illegal
words that no compiled program contains: pc and `base_ptr` update rules, writes only at the
effective address and only at retire, sticky halt and trap, trapping exactly on illegal words,
and no lost output. `prove` is unbounded. `cover` shows that every path is reachable, and
`latency` bounds every instruction except `F` and `IO`.

## YADAN Board

The board has an Anlogic EG4S20NG88 and a 24 MHz clock. `rtl/board/yadan_top.sv` runs the image
built into the bitstream over the USB UART at 115200 8N1. `K_51` resets the core, without clearing
the tape. `LED_23` is on while running, off when halted, and blinks on a trap.

```shell
./brainfunk -O2 -m bit -f program.b | ./bit2bin > program.hex
TD_HOME=/path/to/TD make yadan IMAGE=program.hex    # build/yadan/yadan.bit and reports
```

`IMEM_ADDR_WIDTH` in `yadan_top` is 13, or 8192 instructions, which fits every program in the
test corpus except `LostKng.b`. The 64 KB tape takes half of the board's block RAM: Tang Dynasty
places it in 32 of the 9 Kb blocks and 8 of the 32 Kb blocks.

Tang Dynasty's `read_verilog` elaborates each file as it reads it, so the build concatenates
the sources. It stays at its prompt after an error and then exits successfully, so
`synth/build-yadan.sh` closes its input and checks the log.
