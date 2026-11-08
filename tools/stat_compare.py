#!/usr/bin/env python3
"""Compares execution statistics across optimization levels, as a table on standard output.

    tools/stat_compare.py program.b...

Programs must halt; they read end-of-input from stdin. With no arguments, runs eight classic
programs from programs/ and the 3rdparty/ submodules, and reports any that are absent. For a
wider set, pass a list from the corpus tool:

    tools/stat_compare.py $(tools/corpus/corpus.py list --role workload --halts always --cell-bits 8 --unique)

Percentages are relative to -O0 of the same program. The interpreter is build/bin/brainfunk, or
BRAINFUNK.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRAINFUNK = Path(os.environ.get("BRAINFUNK", ROOT / "build" / "bin" / "brainfunk"))
CLASSICS = [
    "3rdparty/bfcorpus/cwfitzgerald/hello.b",
    "programs/brainfuck.org/squares.b",
    "programs/unattributed/bench.b",
    "3rdparty/bfcorpus/cwfitzgerald/bottles.b",
    "3rdparty/bfcorpus/nyyrikki/triangle.bf",
    "3rdparty/bfcorpus/copy/third_party/hanoi.bf",
    "3rdparty/rdebath-Brainfuck/testing/Long.b",
    "3rdparty/rdebath-Brainfuck/testing/Mandelbrot.b",
]
COLUMNS = ["program", "level", "static", "static_M", "dynamic", "dyn%", "dynamic_M", "M%", "reads", "reads%",
           "writes", "writes%"]


def max_level():
    """The highest optimization level, from the usage text."""
    usage = subprocess.run([BRAINFUNK, "-h"], capture_output=True, text=True).stdout
    found = re.search(r"level, 0 to (\d+)", usage)
    if not found:
        sys.exit(f"{BRAINFUNK} -h does not give the optimization levels")
    return int(found.group(1))


def stat(program, level):
    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.json"
        subprocess.run([BRAINFUNK, f"-O{level}", "-m", "analyze", "-a", "stat", "-f", program, "-o", report],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, check=True)
        return json.loads(report.read_text())["analyses"]["stat"]


def change(value, reference):
    return "-" if reference == 0 else f"{(value / reference - 1) * 100:+.1f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("programs", nargs="*")
    programs = parser.parse_args().programs
    if not programs:
        for program in CLASSICS:
            if (ROOT / program).is_file():
                programs.append(str(ROOT / program))
            else:
                print(f"SKIP: {program} not found", file=sys.stderr)
    if not BRAINFUNK.is_file():
        sys.exit(f"{BRAINFUNK} not found: build first, cmake --build build")

    levels = range(max_level() + 1)
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        stats = {(p, l): pool.submit(stat, p, l) for p in programs for l in levels}

    print("\t".join(COLUMNS))
    for program in programs:
        first = stats[(program, 0)].result()
        for level in levels:
            s = stats[(program, level)].result()
            print("\t".join(map(str, [
                Path(program).name, level, s["static_total"], s["static"]["M"],
                s["dynamic_total"], change(s["dynamic_total"], first["dynamic_total"]),
                s["dynamic"]["M"], change(s["dynamic"]["M"], first["dynamic"]["M"]),
                s["memory_reads"], change(s["memory_reads"], first["memory_reads"]),
                s["memory_writes"], change(s["memory_writes"], first["memory_writes"]),
            ])))


if __name__ == "__main__":
    main()
