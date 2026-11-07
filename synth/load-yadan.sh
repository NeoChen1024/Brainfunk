#!/usr/bin/env sh
# Loads build/yadan/yadan.bit into the FPGA over JTAG. This configures SRAM only: it is gone
# at power-off, and the board's flash is not touched.
#
#   TD_HOME=/path/to/TD sh synth/load-yadan.sh
#
# Tang Dynasty needs read/write access to the cable, USB 336c:1002.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
: "${TD_HOME:?set TD_HOME to the Tang Dynasty install directory}"
cd "$root/build/yadan"
[ -f yadan.bit ] || { echo "no yadan.bit: run make yadan first" >&2; exit 1; }

cat >load.tcl <<'TCL'
import_device eagle_s20.db -package EG4S20NG88
download -bit yadan.bit -mode jtag -spd 7 -sec 64 -cable 0
TCL

# It crashes with a backtrace, and exits with success, if it cannot open the cable.
"$TD_HOME/bin/td.sh" load.tcl </dev/null >load.log 2>&1
if ! grep -q 'finish command "download' load.log; then
    echo "download failed; see build/yadan/load.log" >&2
    exit 1
fi
