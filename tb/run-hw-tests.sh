#!/usr/bin/env sh
# Cosimulation, UART and formal checks for rtl/.
#
#   sh tb/run-hw-tests.sh           random programs, images, and the short corpus programs
#   HW_TEST_FULL=1 sh tb/...        also long.b and mandel.b, about 40 minutes
#
# Everything that can runs across $JOBS processes, by default one per CPU, at nice 19.
#
# test/ is not part of the repository; programs that are absent are reported as skipped.
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
work="$root/build/hw-test"
rm -rf "$work"
mkdir -p "$work"
. "$root/tb/parallel.sh"

sh tb/build.sh >/dev/null
cosim="$root/build/cosim/cosim"

# Count programs split across the processes, each range of seeds its own.
seeded()
{
    mode=$1; total=$2; base=$3; extra=${4:-}
    each=$((total / JOBS + 1))
    start=0
    while [ "$start" -lt "$total" ]; do
        echo "'$cosim' $mode $each --seed $((base + start)) $extra"
        start=$((start + each))
    done
}

# ---- random programs, with and without I/O stalls, and images built to evict dirty lines,
# ---- which no program in the corpus does
{
    seeded --random 3000 1
    seeded --random 3000 100000 --stall
    seeded --evict 1000 1
    seeded --evict 1000 100000 --stall
} | run_jobs "$work"

# ---- images that must trap, in the RTL and the ISS alike ----
for word in 00000000 00000001 10100000 20100000 50100000 80000002 800000ff 90000001 e0000000 f0000000; do
    printf '10000005\n%s\n90000000\n' "$word" >"$work/trap_$word.hex"
    echo "'$cosim' '$work/trap_$word.hex' | grep -q ' trapped'"
done | run_jobs "$work" || {
    echo "an illegal word did not trap" >&2
    exit 1
}

# The RTL does not implement the fused instructions yet, and must say so rather
# than run them wrongly.  This goes away when they are implemented.
for word in a0000100 b0000000 c0000000 d0100000; do
    printf '%s\n90000000\n' "$word" >"$work/fused_$word.hex"
    echo "! '$cosim' '$work/fused_$word.hex' 2>&1 | grep -q 'RTL trapped, ISS did not' && exit 1; exit 0"
done | run_jobs "$work" || {
    echo "the RTL ran a fused instruction" >&2
    exit 1
}

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
        image="$work/$program.O$level.hex"
        ./brainfunk -O"$level" -m bit -f "$CORPUS/$program" | ./bit2bin >"$image" 2>/dev/null
        echo "printf '%s -O%s: ' '$program' '$level'; result=\$('$cosim' '$image') || exit 1; echo \"\$result\" | head -1"
    done
done | run_jobs "$work"
if [ -f "$work/hello.b.O2.hex" ]; then
    "$cosim" --stall "$work/hello.b.O2.hex" >/dev/null
fi

# ---- the board top over its UART pins, including loading the program ----
./brainfunk -O0 -m bit -s '+.,+.,+.' | ./bit2bin >build/uart_tb.hex 2>/dev/null
./bfload -o build/uart_tb.bfi build/uart_tb.hex 2>/dev/null
od -An -v -tx1 -w1 build/uart_tb.bfi >build/uart_tb.bytes
verilator --binary --timing -j "$JOBS" --top-module uart_tb --Mdir build/uart_tb \
    -GIMAGE_BYTES="$(wc -l <build/uart_tb.bytes)" \
    tb/uart_tb.sv tb/spi_flash_model.sv rtl/board/yadan_top.sv rtl/board/yadan_pll.sv rtl/board/uart.sv \
    rtl/board/loader.sv rtl/board/flash.sv \
    rtl/brainfunk_soc.sv rtl/brainfunk_cache.sv rtl/sdram_ctrl.sv tb/sdram_model.sv \
    rtl/brainfunk_mem.sv rtl/brainfunk_core.sv -o uart_tb >/dev/null 2>&1
build/uart_tb/uart_tb | grep -q 'uart_tb passed'

# ---- formal ----
sh formal/run.sh

echo "All hardware tests passed"
