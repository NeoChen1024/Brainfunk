#!/usr/bin/env sh
# Cosimulation, UART and formal checks for rtl/.
#
#   sh tb/run-hw-tests.sh           random programs, images, and the short corpus programs
#   HW_TEST_FULL=1 sh tb/...        also long.b and mandel.b, about 2 hours
#
# test/ is not part of the repository; programs that are absent are reported as skipped.
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
work="$root/build/hw-test"
rm -rf "$work"
mkdir -p "$work"

sh tb/build.sh >/dev/null
cosim="$root/build/cosim/cosim"

# ---- random programs, with and without I/O stalls ----
"$cosim" --random 3000 --seed 1
"$cosim" --random 3000 --seed 100000 --stall

# ---- images that must trap, in the RTL and the ISS alike ----
check_trap()
{
    printf '10000005\n%s\n90000000\n' "$1" >"$work/trap.hex"
    "$cosim" "$work/trap.hex" | grep -q ' trapped' || {
        echo "image with word $1 did not trap" >&2
        exit 1
    }
}
for word in 00000000 00000001 10100000 20100000 50100000 80000002 800000ff 90000001 e0000000 f0000000; do
    check_trap "$word"
done

# The RTL does not implement the fused instructions yet, and must say so rather
# than run them wrongly.  This goes away when they are implemented.
for word in a0000100 b0000000 c0000000 d0100000; do
    printf '%s\n90000000\n' "$word" >"$work/fused.hex"
    if "$cosim" "$work/fused.hex" >/dev/null 2>"$work/fused.err"; then
        echo "RTL ran fused instruction $word" >&2
        exit 1
    fi
    grep -q 'RTL trapped, ISS did not' "$work/fused.err"
done

# ---- real programs, through the real assembler ----
CORPUS="${BF_CORPUS:-test}"
programs="hello.b squares.b bench.b bottles.b sierpinski.bf hanoi.b"
[ -n "${HW_TEST_FULL:-}" ] && programs="$programs long.b mandel.b"
for program in $programs; do
    if [ ! -f "$CORPUS/$program" ]; then
        echo "SKIP: $CORPUS/$program not found" >&2
        continue
    fi
    for level in 0 1 2; do
        ./brainfunk -O"$level" -m bit -f "$CORPUS/$program" | ./bit2bin >"$work/program.hex" 2>/dev/null
        printf '%s -O%s: ' "$program" "$level"
        "$cosim" "$work/program.hex" | head -1
    done
done
"$cosim" --stall "$work/program.hex" --limit 100000 >/dev/null

# ---- the board top over its UART pins ----
./brainfunk -O0 -m bit -s '+.,+.,+.' | ./bit2bin >build/uart_tb.hex 2>/dev/null
verilator --binary --timing -j "${JOBS:-8}" --top-module uart_tb --Mdir build/uart_tb \
    tb/uart_tb.sv rtl/board/yadan_top.sv rtl/board/yadan_pll.sv rtl/board/uart.sv rtl/brainfunk_soc.sv \
    rtl/brainfunk_mem.sv rtl/brainfunk_core.sv \
    -o uart_tb >/dev/null 2>&1
build/uart_tb/uart_tb | grep -q 'uart_tb passed'

# ---- formal ----
sh formal/run.sh

echo "All hardware tests passed"
