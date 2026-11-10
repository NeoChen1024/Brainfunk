#!/usr/bin/env python3
"""Cycles per instruction of the core, the interpreter and native code, over the same -O2 programs.

    tools/cpi_compare.py run [--no-host] [--no-core] [--repeat N] [--cpu C] [--jobs J] -o cpi.json [pattern...]
    tools/cpi_compare.py report cpi.json [--core-mhz F] [--host-mhz F] [--without PATTERN] [--markdown]

An instruction is one the -O2 program executes, counted by `brainfunk -m analyze -a stat`; the
cosimulator retires as many. All three divide their cycles by that count:

    core     the cosimulator's cycles for the whole run (build/bin/cosim, or COSIM)
    interp   `brainfunk -O2` on this machine, in the processor's own cycles
    native   the -O2 program lowered by `brainfunk -m llvm` and compiled by Clang (CLANG), once
             with -O3 -flto and once with -march=native as well

`run` takes the programs named below, or those of the corpus whose paths match a pattern, on each
of their inputs (one EOF mode each), and leaves out a case that does not halt within
--max-instructions, since native code cannot be stopped part way. A native build that prints
anything but what the interpreter printed stops the run.

Host cycles are `perf stat -e cycles:u`: user mode only, and the clock's real ticks, so they do
not depend on what frequency the core ran at. Each command runs --repeat times and the fewest
cycles count, because the other thread of its core, or an interrupt, only ever adds. On a machine
that is doing anything else that is a tenth or more, and the scheduler finds a core whose other
thread is idle more often than a fixed --cpu does; `report` says how far the runs were apart.
What a process costs before its first instruction is measured and taken off: for the interpreter,
the same command writing an image instead of running (`-m bin`), which starts, translates and
optimizes; for native code, the wrapper around an empty program. Output goes to /dev/null. The
host is measured first, then the cosimulations run side by side at nice 19.

With -o naming a file from an earlier run, a case's core result is kept when its image and the
cosimulator are the same files as then, and a host result when --no-host leaves it unmeasured.

`report` turns cycles into instructions a second at each clock: the board's, and the host's
highest. What starting costs is only known to about a million cycles, so programs the interpreter
runs in fewer than --min-cycles are left out, and a native run under a tenth of what starting
costs is shown as ~0: Clang worked the answer out, and what is left cannot be told from nothing.
"""
import argparse
import fnmatch
import hashlib
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "corpus"))
import corpus  # noqa: E402
import issue_sweep  # noqa: E402

LEVEL = 2
ROOT = corpus.REPO
COSIM = Path(os.environ.get("COSIM", corpus.BRAINFUNK.parent / "cosim"))
CLANG = os.environ.get("CLANG", "clang")
WRAPPER = ROOT / "tools" / "llvm-ir-wrapper.c"
# The programs doc/rtl.md compares designs on, and the classics of tools/stat_compare.py, where they halt.
PROGRAMS = [
    "3rdparty/rdebath-Brainfuck/testing/Mandelbrot.b",
    "3rdparty/rdebath-Brainfuck/testing/Long.b",
    "3rdparty/rdebath-Brainfuck/testing/Hanoi.b",
    "3rdparty/rdebath-Brainfuck/testing/Golden.b",
    "3rdparty/rdebath-Brainfuck/testing/Collatz.b",
    "3rdparty/rdebath-Brainfuck/testing/BusyBeaver.b",
    "3rdparty/rdebath-Brainfuck/testing/Bench.b",
    "3rdparty/awib/systest/factor.b",
    "3rdparty/awib/systest/memoryhog.b",
    "3rdparty/bfcorpus/cwfitzgerald/bottles.b",
    "3rdparty/bfcorpus/nyyrikki/triangle.bf",
    "programs/brainfuck.org/squares.b",
    "programs/brainfuck.org/sierpinski.b",
]
NATIVE = {"native": ["-O3", "-flto"], "native, this cpu": ["-O3", "-flto", "-march=native"]}
HOST = ["interp", *NATIVE]
EVENTS = ["cycles:u", "instructions:u", "branches:u", "branch-misses:u", "task-clock"]
# A run of fewer cycles than this share of what starting its process costs is lost in it: reported as ~0.
LOST = 0.1


def digest(path: Path) -> str:
    return hashlib.blake2b(path.read_bytes(), digest_size=16).hexdigest()


def text_of(*command) -> str:
    done = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    return done.stdout.strip() if done.returncode == 0 else ""


def brainfunk(case: corpus.Case, *args) -> list[str]:
    return [str(corpus.BRAINFUNK), f"-O{LEVEL}", "-e", case.eof, *map(str, args), "-f", str(case.program.path)]


def compile_native(ir: Path, flags: list[str], out: Path) -> None:
    subprocess.run([CLANG, "-std=c99", "-Wno-override-module", *flags, str(WRAPPER), str(ir), "-o", str(out)],
                   check=True, capture_output=True)


def prepare(case: corpus.Case, directory: Path, host: bool) -> dict:
    """Counts the case's instructions, and builds what runs it: the image, and the native programs."""
    row = {"key": case.key, "program": case.program.rel, "family": case.program.meta["family"]}
    directory.mkdir()
    data = case.data()
    (directory / "input").write_bytes(data)
    report = directory / "report.json"
    done = subprocess.run(["nice", "-n", "19", *brainfunk(case, "-m", "analyze", "-a", "stat", "--limit", case.limit,
                                                          "-o", report)], input=data, capture_output=True)
    if not report.is_file():
        return row | {"error": f"exit {done.returncode}: {done.stderr.decode(errors='replace')[-300:]}"}
    result = json.loads(report.read_text())
    if not result["run"]["halted"]:
        return row | {"error": f"no end within {case.limit:,} instructions"}
    row["instructions"] = result["run"]["executed"]
    stat = result["analyses"]["stat"]
    row["dynamic"] = {op: n for op, n in stat["dynamic"].items() if n}
    row["scan_iterations"] = stat["scan_iterations"]
    row["output_bytes"] = len(done.stdout)
    subprocess.run(brainfunk(case, "-m", "hex", "-o", directory / "image.hex"), check=True)
    row["image"] = digest(directory / "image.hex")
    if host:
        subprocess.run(brainfunk(case, "-m", "llvm", "-o", directory / "program.ll"), check=True)
        for name, flags in NATIVE.items():
            executable = directory / name.replace(", ", "-").replace(" ", "-")
            compile_native(directory / "program.ll", flags, executable)
            if subprocess.run([executable], input=data, capture_output=True).stdout != done.stdout:
                sys.exit(f"{case.key}: {name} does not print what the interpreter prints")
    return row


def perf(command: list, stdin: Path | None, cpu: int | None, repeat: int) -> dict:
    """The run of fewest cycles out of repeat, as its counters, and every run's cycles."""
    best = None
    runs = []
    pin = ["taskset", "-c", str(cpu)] if cpu is not None else []
    with tempfile.TemporaryDirectory() as directory:
        counters = Path(directory) / "counters"
        for _ in range(repeat):
            with open(stdin or os.devnull, "rb") as source:
                subprocess.run(["perf", "stat", "-x", ",", "-o", str(counters), "-e", ",".join(EVENTS), "--",
                                *pin, *map(str, command)],
                               stdin=source, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            run = {}
            for line in counters.read_text().splitlines():
                fields = line.split(",")
                if len(fields) > 2 and not line.startswith("#"):
                    try:
                        run[fields[2].removesuffix(":u")] = float(fields[0])
                    except ValueError:
                        sys.exit(f"perf did not count {fields[2]}: {fields[0]}")
            runs.append(int(run["cycles"]))
            if best is None or run["cycles"] < best["cycles"]:
                best = run
    return {"cycles": int(best["cycles"]), "host_instructions": int(best["instructions"]),
            "branches": int(best["branches"]), "branch_misses": int(best["branch-misses"]),
            "seconds": best["task-clock"] / 1000, "runs": runs}


def measure_host(case: corpus.Case, directory: Path, empty: dict, cpu: int | None, repeat: int) -> dict:
    stdin = directory / "input"
    start = perf(brainfunk(case, "-m", "bin", "-o", os.devnull), None, cpu, repeat)
    host = {"interp": perf(brainfunk(case), stdin, cpu, repeat) | {"start_cycles": start["cycles"],
                                                                   "start_instructions": start["host_instructions"]}}
    for name in NATIVE:
        executable = directory / name.replace(", ", "-").replace(" ", "-")
        host[name] = perf([executable], stdin, cpu, repeat) | {"start_cycles": empty[name]["cycles"],
                                                               "start_instructions": empty[name]["host_instructions"]}
    return host


def measure_core(case: corpus.Case, directory: Path, row: dict) -> dict:
    done = subprocess.run(["nice", "-n", "19", str(COSIM), "--json", "--eof", case.eof, str(directory / "image.hex"),
                           str(directory / "input")], capture_output=True, text=True)
    if done.returncode != 0:
        return {"error": f"exit {done.returncode}: {(done.stdout + done.stderr)[-300:]}"}
    result = json.loads(done.stdout)
    if not result["halted"] or result["retired"] != row["instructions"]:
        return {"error": f"retired {result['retired']} of {row['instructions']}, halted {result['halted']}"}
    return {"cycles": result["cycles"], "ops": result["ops"]}


def select(patterns: list[str], limit: int) -> list[corpus.Case]:
    """The cases of the programs asked for. A program named here runs whatever the index says it resembles."""
    wanted = patterns or PROGRAMS
    programs = [p for p in corpus.load_all()
                if any(fnmatch.fnmatch(p.rel, pattern) for pattern in wanted) and p.path.is_file()
                and p.meta.get("cell_bits", 8) in (8, "any")]
    cases, _ = issue_sweep.cases_of(programs, limit)
    for pattern in wanted:
        if not any(fnmatch.fnmatch(c.program.rel, pattern) for c in cases):
            print(f"{pattern}: no 8-bit program there with a case to run; is its submodule checked out?",
                  file=sys.stderr)
    return cases


def run(args) -> None:
    for tool, needed in ((corpus.BRAINFUNK, True), (COSIM, not args.no_core)):
        if needed and not tool.is_file():
            sys.exit(f"{tool} not found: build first, cmake --build build")
    host = not args.no_host
    if host:
        for tool in ("perf", CLANG, *(["taskset"] if args.cpu is not None else [])):
            if not shutil.which(tool):
                sys.exit(f"{tool} not found; --no-host leaves the host out")
        if subprocess.run(["perf", "stat", "-e", "cycles:u", "true"], capture_output=True).returncode != 0:
            sys.exit("perf cannot count cycles:u here (see kernel.perf_event_paranoid); --no-host leaves the host out")
    cpu = args.cpu
    before = {}
    if args.output.is_file():
        before = {row["key"]: row for row in json.loads(args.output.read_text())["cases"] if "error" not in row}
    cosim = digest(COSIM) if not args.no_core else None

    cases = select(args.patterns, args.max_instructions)
    with tempfile.TemporaryDirectory() as temporary, ThreadPoolExecutor(args.jobs) as pool:
        top = Path(temporary)
        directories = [top / str(n) for n in range(len(cases))]
        rows = list(corpus.tqdm(pool.map(lambda c, d: prepare(c, d, host), cases, directories), total=len(cases),
                                desc="build", file=sys.stderr))
        good = [(c, d, r) for c, d, r in zip(cases, directories, rows) if "error" not in r]
        for _, _, row in good:
            old = before.get(row["key"], {})
            if not host and old.get("image") == row["image"] and "host" in old:
                row["host"] = old["host"]

        if host:
            # One at a time, before anything else of this tool's is running.
            (top / "empty.ll").write_bytes(subprocess.run([corpus.BRAINFUNK, f"-O{LEVEL}", "-m", "llvm", "-s", ""],
                                                          check=True, capture_output=True).stdout)
            empty = {}
            for name, flags in NATIVE.items():
                compile_native(top / "empty.ll", flags, top / "empty")
                empty[name] = perf([top / "empty"], None, cpu, args.repeat)
            for case, directory, row in corpus.tqdm(good, desc="host", file=sys.stderr):
                row["host"] = measure_host(case, directory, empty, cpu, args.repeat)

        if not args.no_core:
            todo = []
            for case, directory, row in good:
                old = before.get(row["key"], {})
                if old.get("image") == row["image"] and old.get("cosim") == cosim and "core" in old:
                    row["core"], row["cosim"] = old["core"], cosim
                else:
                    todo.append((case, directory, row))
            results = corpus.tqdm(pool.map(lambda item: measure_core(*item), todo), total=len(todo), desc="core",
                                  file=sys.stderr)
            for (_, _, row), result in zip(todo, results):
                if "error" in result:
                    row["core_error"] = result["error"]
                else:
                    row["core"], row["cosim"] = result, cosim

    for row in rows:
        for what in ("error", "core_error"):
            if what in row:
                print(f"{row['key']}: {row[what]}", file=sys.stderr)
    processor = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                      if line.startswith("model name")), platform.processor())
    about = {"level": LEVEL, "max_instructions": args.max_instructions,
             "source": text_of("git", "-C", ROOT, "describe", "--always", "--dirty")}
    if host:
        about["host"] = {"processor": processor, "kernel": platform.release(), "cpu": cpu, "repeat": args.repeat,
                         "events": EVENTS, "clang": text_of(CLANG, "--version").splitlines()[0], "native": NATIVE}
    elif args.output.is_file():
        about["host"] = json.loads(args.output.read_text()).get("host")
    args.output.write_text(json.dumps(about | {"cases": rows}, indent=1) + "\n")


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else float("nan")


def digits(value: float) -> str:
    """Three decimals of a cycle count, and three figures of anything larger."""
    return f"{value:,.0f}" if value >= 100 else f"{value:.1f}" if value >= 10 else f"{value:.3f}"


def summarize(data: dict, min_cycles: int, without: list[str]) -> tuple[list[dict], list[str]]:
    """A row per program, its cases added up: for each way of running it that every case has, the
    cycles of the run alone, and for the host's ways their instructions and what starting cost."""
    programs: dict[str, dict] = {}
    for row in data["cases"]:
        if "error" in row or any(fnmatch.fnmatch(row["program"], pattern) for pattern in without):
            continue
        p = programs.setdefault(row["program"], {"program": row["program"], "instructions": 0, "ways": {},
                                                 "missing": set()})
        p["instructions"] += row["instructions"]
        measured = {"core": row.get("core")} | {name: row.get("host", {}).get(name) for name in HOST}
        for name, way in measured.items():
            if way is None:
                p["missing"].add(name)
                continue
            total = p["ways"].setdefault(name, {"cycles": 0, "start": 0, "host_instructions": 0})
            total["cycles"] += way["cycles"] - way.get("start_cycles", 0)
            total["start"] += way.get("start_cycles", 0)
            total["host_instructions"] += way.get("host_instructions", 0) - way.get("start_instructions", 0)
    kept, small = [], []
    for p in programs.values():
        for name in p.pop("missing"):
            p["ways"].pop(name, None)
        if "interp" in p["ways"] and p["ways"]["interp"]["cycles"] < min_cycles:
            small.append(p["program"])
        else:
            kept.append(p)
    kept.sort(key=lambda p: -p["instructions"])
    return kept, small


def report(args) -> None:
    data = json.loads(args.results.read_text())
    programs, small = summarize(data, args.min_cycles, args.without)
    if not programs:
        sys.exit("nothing to report")
    ways = [name for name in ("core", *HOST) if any(name in p["ways"] for p in programs)]
    hosted = [name for name in ways if name != "core"]
    clock = {name: (args.core_mhz if name == "core" else args.host_mhz) for name in ways}
    names = [Path(p["program"]).name for p in programs]
    for p, name in zip(programs, names):
        p["name"] = name if names.count(name) == 1 else p["program"]

    def cpi(p: dict, name: str) -> float | None:
        """None where it was not measured, and 0 where the run is lost in what starting costs."""
        way = p["ways"].get(name)
        if way is None:
            return None
        return 0.0 if way["cycles"] < LOST * way["start"] else way["cycles"] / p["instructions"]

    def mips(p: dict, name: str) -> float | None:
        return clock[name] / cpi(p, name) if cpi(p, name) else None

    def times(p: dict, name: str) -> float | None:
        return mips(p, name) / mips(p, "core") if "core" in ways and mips(p, name) and mips(p, "core") else None

    def each(p: dict, name: str) -> float | None:
        return p["ways"][name]["host_instructions"] / p["instructions"] if cpi(p, name) else None

    def a_cycle(p: dict, name: str) -> float | None:
        return p["ways"][name]["host_instructions"] / p["ways"][name]["cycles"] if cpi(p, name) else None

    def column(value, name: str, mean: bool = True) -> list[str]:
        """A column of the table, with its geometric mean over the programs that have a value."""
        values = [value(p, name) for p in programs]
        cells = [digits(v) if v else "~0" if value is cpi and v == 0 else "-" for v in values]
        return cells + ([digits(geomean([v for v in values if v])) if any(values) else "-"] if mean else [])

    first = [p["name"] for p in programs]
    kept = {p["program"] for p in programs}
    tables = [
        ("Cycles per instruction", ["program", "instructions", *ways],
         [first + ["geometric mean"], [f"{p['instructions']:,}" for p in programs] + [""],
          *(column(cpi, name) for name in ways)]),
        (f"Million instructions a second: the core at {args.core_mhz:g} MHz, the host at {args.host_mhz:g} MHz",
         ["program", *ways, *(f"{name} / core" for name in hosted if "core" in ways)],
         [first + ["geometric mean"], *(column(mips, name) for name in ways),
          *(column(times, name) for name in hosted if "core" in ways)]),
        ("Host instructions for each instruction, and in a cycle",
         ["program", *(f"{name}, each" for name in hosted), *(f"{name}, a cycle" for name in hosted)],
         [first, *(column(each, name, mean=False) for name in hosted),
          *(column(a_cycle, name, mean=False) for name in hosted)]),
    ]
    for title, header, columns in tables:
        if len(header) == 1:
            continue
        lines = [header] + [list(line) for line in zip(*columns)]
        print(f"{title}\n")
        if args.markdown:
            lines = [["`" + line[0] + "`" if "." in line[0] else line[0]] + line[1:] for line in lines]
            lines.insert(1, ["---"] + ["---:"] * (len(header) - 1))
            for line in lines:
                print("| " + " | ".join(line) + " |")
        else:
            widths = [max(len(line[n]) for line in lines) for n in range(len(header))]
            for line in lines:
                print("  ".join(text.ljust(w) if n == 0 else text.rjust(w)
                                for n, (text, w) in enumerate(zip(line, widths))).rstrip())
        print()
    host = data.get("host")
    if host:
        where = "wherever the scheduler put them" if host["cpu"] is None else f"on CPU {host['cpu']}"
        print(f"host: {host['processor']}, Linux {host['kernel']}, {host['clang']}; the fewest cycles of "
              f"{host['repeat']} runs, {where}")
        measured = [way for row in data["cases"] if row["program"] in kept for way in row.get("host", {}).values()]
        spread = [sorted(way["runs"])[len(way["runs"]) // 2] / way["cycles"] - 1 for way in measured if "runs" in way]
        if spread:
            print(f"the median run took up to {max(spread):.1%} more cycles than the fewest, "
                  f"{sorted(spread)[len(spread) // 2]:.1%} at the median")
        # What the clock was while they ran; time in the kernel, which cycles:u leaves out, makes it look slower.
        clocks = [way["cycles"] / way["seconds"] / 1e6 for way in measured if way.get("seconds", 0) >= 0.1]
        if clocks:
            print(f"runs of 0.1 s or more made {min(clocks):,.0f} to {max(clocks):,.0f} million cycles a second")
    print(f"source: {data['source']}")
    if small:
        print(f"left out, under {args.min_cycles:,} cycles in the interpreter: " + ", ".join(sorted(small)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(required=True)
    p = sub.add_parser("run", help="measure")
    p.add_argument("patterns", nargs="*", help="corpus paths, as shell patterns; the named programs by default")
    p.add_argument("--no-host", action="store_true", help="leave the interpreter and native code out")
    p.add_argument("--no-core", action="store_true", help="leave the cosimulator out")
    p.add_argument("--repeat", type=int, default=5, help="runs of each host command; the fewest cycles count")
    p.add_argument("--cpu", type=int, help="a CPU to keep host commands on; the scheduler's choice by default")
    p.add_argument("--max-instructions", type=int, default=4_000_000_000,
                   help="a case that runs longer is left out")
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("-o", "--output", type=Path, required=True)
    p.set_defaults(func=run)
    p = sub.add_parser("report", help="tables from a run")
    p.add_argument("results", type=Path)
    p.add_argument("--core-mhz", type=float, default=168, help="the core's clock")
    p.add_argument("--host-mhz", type=float, default=4100, help="the host's clock")
    p.add_argument("--min-cycles", type=int, default=20_000_000,
                   help="programs the interpreter runs in fewer cycles are left out")
    p.add_argument("--without", action="append", default=[], metavar="PATTERN",
                   help="leave out the programs whose paths match; may be given again")
    p.add_argument("--markdown", action="store_true")
    p.set_defaults(func=report)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
