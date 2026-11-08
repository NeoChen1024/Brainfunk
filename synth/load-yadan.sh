#!/usr/bin/env sh
# Loads build/yadan/yadan.bit, or YADAN_WORK's, into the FPGA over JTAG. This configures SRAM
# only: it is gone at power-off, and the board's flash is not touched. The yadan-load target runs it.
#
#   TD_HOME=/path/to/TD [YADAN_WORK=dir] sh synth/load-yadan.sh
#
# Tang Dynasty needs read/write access to the cable, USB 336c:1002.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
: "${TD_HOME:?set TD_HOME to the Tang Dynasty install directory}"
work="${YADAN_WORK:-$root/build/yadan}"
cd "$work"
[ -f yadan.bit ] || { echo "no yadan.bit: build the yadan target first" >&2; exit 1; }

cat >load.tcl <<'TCL'
import_device eagle_s20.db -package EG4S20NG88
download -bit yadan.bit -mode jtag -spd 7 -sec 64 -cable 0
TCL

# It crashes with a backtrace, and exits with success, if it cannot open the cable.
"$TD_HOME/bin/td.sh" load.tcl </dev/null >load.log 2>&1
if ! grep -q 'finish command "download' load.log; then
    echo "download failed; see $work/load.log" >&2
    exit 1
fi
