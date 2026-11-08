#!/usr/bin/env python3
"""Runs the issue model over the corpus, and sums it up by family so that no one program decides.

    tools/issue_sweep.py run [--config machines.json] [--limit N] [--jobs J] -o sweep.json
    tools/issue_sweep.py report sweep.json [--against NAME] [--folds K] [--worst N]

`run` takes every unique 8-bit workload program of the corpus, on each of its inputs (one EOF mode
each), or on no input when it needs none; programs that need input but have none written are left
out, and counted. Each case runs at -O2 through `brainfunk -m analyze -a issue`, cut off at --limit
instructions. The machines are those of --config, as for brainfunk (doc/analysis.md), or its defaults.

`report` gives each machine's speedup over the first (or --against): per program, over all its
cases' cycles; per family, the geometric mean of its programs; and overall the geometric mean of the
families, so that a family of seventy programs counts as much as one of a single program. --folds
splits the families into K groups by a hash of their name, as held-out sets: a choice that helps in
only some of them is tuned to those. Programs that ran fewer than --min-instructions are left out.
"""
import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "corpus"))
import corpus  # noqa: E402

LEVEL = 2


def index_rows() -> dict[str, dict]:
    lines = corpus.INDEX.read_text().splitlines()
    header = lines[0].split("\t")
    return {r["path"]: r for r in (dict(zip(header, line.split("\t"))) for line in lines[1:])}


def select_cases(limit: int) -> tuple[list[corpus.Case], dict[str, int]]:
    """One case per program and input, the program's first EOF mode, every one cut at limit."""
    rows = index_rows()
    skipped = {"duplicate": 0, "not 8-bit": 0, "needs input": 0, "not built": 0}
    programs = []
    for program in corpus.load_all():
        meta = program.meta
        if meta.get("role") != "workload" or meta.get("halts") == "interactive":
            continue
        if rows.get(program.rel, {}).get("duplicate"):
            skipped["duplicate"] += 1
        elif meta.get("cell_bits", 8) not in (8, "any"):
            skipped["not 8-bit"] += 1
        elif not program.path.is_file():
            skipped["not built"] += 1
        else:
            programs.append(program)
    by_program: dict[str, list[corpus.Case]] = {}
    for case in corpus.load_cases(programs)[0]:
        named = by_program.setdefault(case.program.rel, [])
        if all(c.name != case.name for c in named):
            named.append(case)
    cases = []
    for program in programs:
        mine = by_program.get(program.rel)
        if not mine:
            if program.meta.get("halts") == "with-input":
                skipped["needs input"] += 1
                continue
            mine = [corpus.Case(program, "none", corpus.program_eofs(program)[0], {"text": ""},
                                program.path.parent, program.manifest.parent / corpus.LOCK)]
        for case in mine:
            case.limit = min(case.limit or limit, limit)
            cases.append(case)
    return cases, skipped


def run_case(case: corpus.Case, config: Path | None) -> dict:
    row = {"key": case.key, "program": case.program.rel, "family": case.program.meta["family"],
           "source": case.program.source}
    command = ["nice", "-n", "19", str(corpus.BRAINFUNK), f"-O{LEVEL}", "-e", case.eof, "-m", "analyze",
               "-a", "issue", "--limit", str(case.limit)]
    if config:
        command += ["--config", str(config)]
    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.json"
        command += ["-o", str(report), "-f", str(case.program.path)]
        try:
            done = subprocess.run(command, input=case.data(), capture_output=True, timeout=corpus.TIMEOUT)
        except subprocess.TimeoutExpired:
            return row | {"error": f"no end within {corpus.TIMEOUT:g} s"}
        if not report.is_file():
            return row | {"error": f"exit {done.returncode}: {done.stderr.decode(errors='replace')[-300:]}"}
        result = json.loads(report.read_text())
    run = result["run"]
    row["instructions"] = run["executed"]
    row["ending"] = ("trapped" if run["trapped"] else "endless" if run["endless"] else
                     "halted" if run["halted"] else "limit")
    row["machines"] = {m["name"]: {k: m[k] for k in ("cycles", "joined", "mispredicts", "steers",
                                                      "not_joined_because")}
                       for m in result["analyses"]["issue"]["machines"]}
    row["order"] = [m["name"] for m in result["analyses"]["issue"]["machines"]]
    return row


def run(args) -> None:
    if not corpus.BRAINFUNK.is_file():
        sys.exit(f"{corpus.BRAINFUNK} not found: build first, cmake --build build")
    cases, skipped = select_cases(args.limit)
    print(f"{len(cases)} cases; left out: " + ", ".join(f"{n} {why}" for why, n in skipped.items()),
          file=sys.stderr)
    with ThreadPoolExecutor(args.jobs) as pool:
        rows = list(corpus.tqdm(pool.map(lambda c: run_case(c, args.config), cases), total=len(cases),
                                file=sys.stderr))
    for row in rows:
        if "error" in row:
            print(f"{row['key']}: {row['error']}", file=sys.stderr)
    config = json.loads(args.config.read_text()) if args.config else None
    out = {"level": LEVEL, "limit": args.limit, "config": config, "skipped": skipped, "cases": rows}
    args.output.write_text(json.dumps(out, indent=1) + "\n")


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else float("nan")


def fold_of(family: str, folds: int) -> int:
    return int.from_bytes(hashlib.blake2b(family.encode(), digest_size=4).digest(), "little") % folds


def report(args) -> None:
    data = json.loads(args.sweep.read_text())
    rows = [r for r in data["cases"] if "error" not in r]
    order = rows[0]["order"]
    base = args.against or order[0]
    if base not in order:
        sys.exit(f"no machine {base}; there are {', '.join(order)}")

    # Per program: all its cases' cycles and instructions, added up.
    programs: dict[str, dict] = {}
    for row in rows:
        p = programs.setdefault(row["program"], {"family": row["family"], "instructions": 0, "cases": 0,
                                                 "cycles": dict.fromkeys(order, 0)})
        p["instructions"] += row["instructions"]
        p["cases"] += 1
        for name in order:
            p["cycles"][name] += row["machines"][name]["cycles"]
    small = [k for k, p in programs.items() if p["instructions"] < args.min_instructions]
    for k in small:
        del programs[k]
    families: dict[str, list[str]] = {}
    for k, p in programs.items():
        families.setdefault(p["family"], []).append(k)

    def speedups(name: str, members: list[str]) -> list[float]:
        return [programs[k]["cycles"][base] / programs[k]["cycles"][name] for k in members]

    def stratified(name: str, fams: list[str]) -> float:
        return geomean([geomean(speedups(name, families[f])) for f in fams])

    fams = sorted(families)
    limited = sum(1 for r in rows if r["ending"] == "limit")
    print(f"{len(programs)} programs in {len(fams)} families, {len(rows)} cases ({limited} cut at "
          f"{data['limit']:,} instructions); {len(small)} programs under {args.min_instructions:,} "
          f"instructions left out; speedup over {base}\n")

    width = max(len(n) for n in order)
    pooled_base = sum(p["cycles"][base] for p in programs.values())
    header = f"{'machine':<{width}}  {'families':>8}  {'programs':>8}  {'pooled':>7}  {'worst family':>20}  {'best family':>20}"
    if args.folds > 1:
        header += "  " + "  ".join(f"{'fold ' + str(i):>7}" for i in range(args.folds))
    print(header)
    for name in order:
        per_family = {f: geomean(speedups(name, families[f])) for f in fams}
        worst = min(per_family, key=per_family.get)
        best = max(per_family, key=per_family.get)
        pooled = pooled_base / sum(p["cycles"][name] for p in programs.values())
        line = (f"{name:<{width}}  {stratified(name, fams):8.3f}  {geomean(speedups(name, list(programs))):8.3f}  "
                f"{pooled:7.3f}  {worst[:13]:>13} {per_family[worst]:6.3f}  {best[:13]:>13} {per_family[best]:6.3f}")
        if args.folds > 1:
            for i in range(args.folds):
                fold = [f for f in fams if fold_of(f, args.folds) == i]
                line += f"  {stratified(name, fold):7.3f}" if fold else f"  {'-':>7}"
        print(line)

    if args.families:
        print(f"\nper family: programs, and each machine's geometric mean")
        print(f"{'family':<20} {'n':>3}  " + "  ".join(f"{n[:9]:>9}" for n in order))
        for f in fams:
            print(f"{f[:20]:<20} {len(families[f]):3d}  " +
                  "  ".join(f"{geomean(speedups(n, families[f])):9.3f}" for n in order))

    if args.worst:
        for name in order:
            if name == base:
                continue
            ranked = sorted(programs, key=lambda k: programs[k]["cycles"][base] / programs[k]["cycles"][name])
            print(f"\n{name}: the {args.worst} programs it speeds up least")
            for k in ranked[:args.worst]:
                p = programs[k]
                print(f"  {p['cycles'][base] / p['cycles'][name]:6.3f}  {p['instructions']:>13,}  {k}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(required=True)
    p = sub.add_parser("run", help="run the issue model over the corpus")
    p.add_argument("--config", type=Path, help="machines, as for brainfunk --config")
    p.add_argument("--limit", type=int, default=100_000_000, help="instructions per case at most")
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("-o", "--output", type=Path, required=True)
    p.set_defaults(func=run)
    p = sub.add_parser("report", help="sum a sweep up by family")
    p.add_argument("sweep", type=Path)
    p.add_argument("--against", help="the machine speedups are over; the first by default")
    p.add_argument("--folds", type=int, default=4, help="held-out groups of families")
    p.add_argument("--worst", type=int, default=10, help="programs each machine speeds up least")
    p.add_argument("--families", action="store_true", help="every family's speedups")
    p.add_argument("--min-instructions", type=int, default=10_000)
    p.set_defaults(func=report)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
