#!/usr/bin/env sh
# Synthesizes, places and routes for the YADAN Board with Tang Dynasty.
#
#   TD_HOME=/path/to/TD sh synth/build-yadan.sh program.hex
#
# Reports and yadan.bit end up in build/yadan.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
: "${TD_HOME:?set TD_HOME to the Tang Dynasty install directory}"
[ "$#" -ge 1 ] || { echo "usage: $0 program.hex" >&2; exit 2; }

image="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work="$root/build/yadan"
rm -rf "$work"
mkdir -p "$work"

# read_verilog elaborates each file as it reads it, so one file must hold them all.
cat "$root"/rtl/brainfunk_core.sv "$root"/rtl/brainfunk_soc.sv "$root"/rtl/board/uart.sv \
    "$root"/rtl/board/yadan_top.sv >"$work/yadan_all.sv"
cp "$root"/synth/yadan.adc "$root"/synth/yadan.sdc "$root"/synth/yadan.tcl "$work"/
cp "$image" "$work/program.hex"
cd "$work"

# Tang Dynasty waits at its prompt after an error, so give it no input to wait for.
"$TD_HOME/bin/td.sh" yadan.tcl </dev/null

# It also exits with success after an error.
if grep -E '(^|[^A-Za-z])ERROR' td_*.log >/dev/null 2>&1 || [ ! -f yadan.bit ]; then
    echo "Tang Dynasty failed; see $work" >&2
    exit 1
fi
