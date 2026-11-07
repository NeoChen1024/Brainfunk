#!/usr/bin/env python3
"""Check ELVM's Brainfuck outputs against the corpus limits.

    tools/corpus/check-elvm.py WORKDIR [--timeout SECONDS] [--jobs N]

WORKDIR is where tools/corpus/build-elvm.sh built ELVM. For each WORKDIR/out/*.eir.bf this
counts bitcode words at -O2, runs the program under brainfunk -O3 on each of its test
inputs (test/<name>*.in, as ELVM's runtest.sh finds them, or empty input), and compares the
output with ELVM's own IR interpreter, eli, running the same .eir. Brainfunk's tape wraps at
64 KiB, so a program that needs more cells fails the comparison. Prints one TSV row per
program.
"""

from __future__ import annotations

import concurrent.futures
import os
import subprocess
import sys
import time
from pathlib import Path

import click

REPO = Path(__file__).resolve().parents[2]
BRAINFUNK = REPO / "brainfunk"
MAX_WORDS = 1 << 20


def inputs_for(work: Path, bf: Path) -> list[Path | None]:
    name = bf.name.split(".")[0]
    found = sorted(work.glob(f"*/{name}*.in"))
    return list(found) or [None]


def run(cmd: list[str], stdin: Path | None, timeout: float) -> tuple[str, bytes, float]:
    start = time.monotonic()
    with open(stdin or os.devnull, "rb") as f:
        try:
            result = subprocess.run(cmd, stdin=f, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return "timeout", b"", time.monotonic() - start
    status = "ok" if result.returncode == 0 else f"exit{result.returncode}"
    return status, result.stdout, time.monotonic() - start


def check(work: Path, bf: Path, timeout: float) -> list[str]:
    bitcode = subprocess.run(["nice", "-n", "19", str(BRAINFUNK), "-O2", "-m", "bit", "-f", str(bf)],
                             capture_output=True, check=True).stdout
    words = sum(1 for line in bitcode.splitlines() if line.strip())
    eir = bf.with_suffix("")                   # out/foo.c.eir.bf -> out/foo.c.eir
    results, seconds = [], 0.0
    for stdin in inputs_for(work, bf):
        _, expected, _ = run([str(work / "out" / "eli"), str(eir)], stdin, timeout)
        status, actual, elapsed = run(["nice", "-n", "19", str(BRAINFUNK), "-O3", "-f", str(bf)],
                                      stdin, timeout)
        seconds += elapsed
        results.append(status if status != "ok" else "same" if actual == expected else "DIFF")
    verdict = "pass" if words <= MAX_WORDS and all(r == "same" for r in results) else "fail"
    ins = ",".join(p.name for p in inputs_for(work, bf) if p) or "-"
    return [bf.name, str(bf.stat().st_size), str(words), f"{seconds:.1f}",
            ",".join(results), ins, verdict]


@click.command()
@click.argument("work", type=click.Path(exists=True, file_okay=False, path_type=Path))
@click.option("--timeout", default=600.0, show_default=True, help="Seconds per run.")
@click.option("--jobs", default=os.cpu_count(), show_default=True)
def main(work: Path, timeout: float, jobs: int) -> None:
    files = sorted((work / "out").glob("*.eir.bf"), key=lambda p: p.stat().st_size)
    print("\t".join(("program", "bytes", "words_O2", "seconds", "results", "inputs", "verdict")))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for row in pool.map(lambda bf: check(work, bf, timeout), files):
            print("\t".join(row), flush=True)


if __name__ == "__main__":
    sys.exit(main())
