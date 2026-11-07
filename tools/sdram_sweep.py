#!/usr/bin/env python3
"""Measures the SDRAM on the real board: which clock phase has a clean data window, and how far its
timings can go. Uses tools/sdram_memtest.sv.in, Memtest86+-style patterns over all 8 MiB, with every
data bit toggling per word and bit-reversed line order as well. Needs TD_HOME and the board.

    tools/sdram_sweep.py phase                 7.5-degree steps around the working point
    tools/sdram_sweep.py limits                tRCD, tRFC, recovery, CAS latency, refresh at 240 degrees

A clean run has no errors at latency index 2, four cycles after READ. Re-measure after anything that
moves the SDRAM's clock or pads.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[1]


def board_port():
    """$BOARD_PORT, or else the one serial port from the board's CH340, USB vendor 1a86, as bfload finds it."""
    if os.environ.get("BOARD_PORT"):
        return os.environ["BOARD_PORT"]
    found = glob.glob("/dev/serial/by-id/*1a86*")
    return found[0] if len(found) == 1 else None


PORT = board_port()
READ_STEPS = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 20, 21, 23, 24}

ADC = """set_pin_assignment { clk_24mhz }   { LOCATION = P34; IOSTANDARD = LVCMOS33; }
set_pin_assignment { uart_rx_pin } { LOCATION = P54; IOSTANDARD = LVCMOS33; }
set_pin_assignment { uart_tx_pin } { LOCATION = P52; IOSTANDARD = LVCMOS33; }
"""
SDC = "create_clock -name clk_24mhz -period 41.667 [get_ports clk_24mhz]\nderive_pll_clocks\n"
TCL = """import_device eagle_s20.db -package EG4S20NG88
read_verilog -file t.sv -top sdtest
read_adc t.adc
optimize_rtl
read_sdc t.sdc
optimize_gate
legalize_phy_inst
catch {place}
route
report_timing -file timing.rpt
bitgen -bit t.bit
"""


def td(directory, script):
    (directory / "run.tcl").write_text(script)
    home = Path(os.environ["TD_HOME"])
    subprocess.run([home / "bin" / "td.sh", "run.tcl"], cwd=directory, stdin=subprocess.DEVNULL, capture_output=True, timeout=400)


def build(directory, ident, degrees, trcd=4, trp=7, trfc=12, fade=0, cl=3, refresh_us=7):
    directory.mkdir(parents=True, exist_ok=True)
    steps = round(degrees / 7.5)
    source = (ROOT / "tools" / "sdram_memtest.sv.in").read_text()
    for key, value in {"ID": ident, "MHZ": 168, "FB": 7, "DIV1": 5, "DIV": 6, "CP": (5 + steps // 8) % 6, "FP": steps % 8,
                       "TRCD": trcd, "TRP": trp, "TRFC": trfc, "FADE": fade, "CL": cl, "REFI": refresh_us}.items():
        source = source.replace(f"@{key}@", str(value))
    (directory / "t.sv").write_text(source)
    (directory / "t.adc").write_text(ADC)
    (directory / "t.sdc").write_text(SDC)
    td(directory, TCL)


def measure(directory, seconds):
    td(directory, "import_device eagle_s20.db -package EG4S20NG88\ndownload -bit t.bit -mode jtag -spd 7 -sec 64 -cable 0\n")
    with serial.Serial(PORT, 115200, timeout=0.1) as port:
        got = b""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            got += port.read(4096)
    totals = [0, 0, 0, 0]
    reads = 0
    for line in got.decode("latin1").splitlines():
        fields = line.split()
        if len(fields) == 3 and len(fields[2]) == 24 and int(fields[1], 16) in READ_STEPS:
            reads += 1
            for index in range(4):
                totals[index] += int(fields[2][index * 6:index * 6 + 6], 16)
    return reads, totals


def sweep(cases, seconds, work):
    with ThreadPoolExecutor(8) as pool:
        for index, (_, arguments) in enumerate(cases):
            pool.submit(build, work / f"b{index}", index, **arguments)
    for index, (name, _) in enumerate(cases):
        reads, totals = measure(work / f"b{index}", seconds)
        print(f"{name:32} {reads:4} reads, errors by latency index: {totals}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=["phase", "limits"])
    what = parser.parse_args().what
    if "TD_HOME" not in os.environ:
        sys.exit("set TD_HOME")
    if PORT is None:
        sys.exit("no board serial port found: set BOARD_PORT")
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as work:
        if what == "phase":
            cases = [(f"{150 + 7.5 * i:.1f} degrees", {"degrees": 150 + 7.5 * i}) for i in range(24)]
            sweep(cases, 12, Path(work))
        else:
            base = {"degrees": 240}
            cases = [("base", base),
                     ("tRCD 3", {**base, "trcd": 3}), ("tRCD 2", {**base, "trcd": 2}),
                     ("tRFC 10", {**base, "trfc": 10}), ("tRFC 8", {**base, "trfc": 8}), ("tRFC 6", {**base, "trfc": 6}),
                     ("recovery 3", {**base, "trp": 3}), ("recovery 1", {**base, "trp": 1}),
                     ("CAS latency 2", {**base, "cl": 2}),
                     ("refresh 15 us, fade 30 s", {**base, "refresh_us": 15, "fade": 30}),
                     ("refresh 60 us, fade 30 s", {**base, "refresh_us": 60, "fade": 30}),
                     ("refresh 240 us, fade 30 s", {**base, "refresh_us": 240, "fade": 30})]
            sweep(cases, 48, Path(work))


if __name__ == "__main__":
    main()
