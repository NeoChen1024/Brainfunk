#!/usr/bin/env sh
# Synthesizes, places and routes for the YADAN Board with Tang Dynasty.
#
#   TD_HOME=/path/to/TD sh synth/build-yadan.sh
#
# Reports and yadan.bit end up in build/yadan.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
renice -n 19 -p $$ >/dev/null 2>&1 || true
: "${TD_HOME:?set TD_HOME to the Tang Dynasty install directory}"
work="$root/build/yadan"
rm -rf "$work"
mkdir -p "$work"

# read_verilog elaborates each file as it reads it, so one file must hold them all.
cat "$root"/rtl/brainfunk_core.sv "$root"/rtl/brainfunk_predict.sv "$root"/rtl/brainfunk_mem.sv "$root"/rtl/brainfunk_cache.sv "$root"/rtl/sdram_ctrl.sv \
    "$root"/rtl/brainfunk_soc.sv "$root"/rtl/board/sdram_phy.sv "$root"/rtl/board/uart.sv "$root"/rtl/board/loader.sv "$root"/rtl/board/flash.sv \
    "$root"/rtl/board/yadan_pll.sv "$root"/rtl/board/yadan_top.sv >"$work/yadan_all.sv"
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
