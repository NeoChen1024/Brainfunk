#!/usr/bin/env sh
# Synthesizes, places and routes for the YADAN Board with Tang Dynasty. The yadan target runs it.
#
#   TD_HOME=/path/to/TD [YADAN_WORK=dir] [YADAN_MHZ=168] [YADAN_SDRAM_DEGREES=240] [YADAN_SDRAM_CAPTURE=4] sh synth/build-yadan.sh
#
# Reports and yadan.bit end up in YADAN_WORK, build/yadan by default. YADAN_MHZ is the clock, a
# multiple of 24; one other than 168 is beyond what timing has been closed at, and wants the SDRAM's
# phase measured for it with tools/sdram_sweep.py and given as YADAN_SDRAM_DEGREES, and the
# cycles from READ to data at that phase, the sweep's latency index plus 2, as YADAN_SDRAM_CAPTURE.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
renice -n 19 -p $$ >/dev/null 2>&1 || true
: "${TD_HOME:?set TD_HOME to the Tang Dynasty install directory}"
work="${YADAN_WORK:-$root/build/yadan}"
rm -rf "$work"
mkdir -p "$work"

# read_verilog elaborates each file as it reads it, so one file must hold them all.
{ echo "\`define YADAN_MHZ ${YADAN_MHZ:-168}"; echo "\`define YADAN_SDRAM_DEGREES ${YADAN_SDRAM_DEGREES:-240}"
  echo "\`define YADAN_SDRAM_CAPTURE ${YADAN_SDRAM_CAPTURE:-4}"; } >"$work/yadan_all.sv"
cat "$root"/rtl/brainfunk_core.sv "$root"/rtl/brainfunk_predict.sv "$root"/rtl/brainfunk_mem.sv "$root"/rtl/brainfunk_cache.sv "$root"/rtl/sdram_ctrl.sv \
    "$root"/rtl/brainfunk_soc.sv "$root"/rtl/sdram_memory.sv "$root"/rtl/brainfunk_system.sv "$root"/rtl/board/sdram_phy.sv "$root"/rtl/uart.sv "$root"/rtl/loader.sv "$root"/rtl/flash.sv \
    "$root"/rtl/board/yadan_pll.sv "$root"/rtl/board/yadan_top.sv >>"$work/yadan_all.sv"
cp "$root"/synth/yadan.adc "$root"/synth/yadan.sdc "$root"/synth/yadan.tcl "$work"/
cd "$work"

# Tang Dynasty waits at its prompt after an error, so give it no input to wait for.
"$TD_HOME/bin/td.sh" yadan.tcl </dev/null

# It also exits with success after an error. The SDRAM's pads get constraints of their own that
# report violations, for a data path the design handles with its measured capture phase; only
# the rest of the design has to meet timing, which timing.rpt shows under the PLL clock.
if grep -E '(^|[^A-Za-z])ERROR' td_*.log >/dev/null 2>&1 || [ ! -f yadan.bit ]; then
    echo "Tang Dynasty failed; see $work" >&2
    exit 1
fi
