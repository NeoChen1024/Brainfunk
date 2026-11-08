#!/usr/bin/env python3
"""Manage the Brainfuck program corpus under programs/.

Every programs/**/*.toml manifest describes one source: where its files come from
(a directory in programs/, or a git submodule under 3rdparty/), its license, and the
programs it contributes. This tool hashes and deduplicates them into programs/index.tsv.

    tools/corpus/corpus.py index           regenerate programs/index.tsv
    tools/corpus/corpus.py verify          check manifests, files and the index
    tools/corpus/corpus.py fetch MANIFEST  download programs that have a url but no file
    tools/corpus/corpus.py list [filters]  print program paths, for other scripts
    tools/corpus/corpus.py draft DIR OUT   draft a manifest for a submodule directory
    tools/corpus/corpus.py inputs ...      inputs, their expected results, and coverage; see below

Inputs come from two places: `cases` in a manifest's program entry, which point at the input and
expected output files a source provides, and inputs.toml next to the manifests, which this project
writes. Every case runs once per EOF mode. inputs.lock, next to them, holds what each run must
give: the output, how it ended, and the tape at the end.
"""

from __future__ import annotations

import dataclasses
import datetime
import fnmatch
import itertools
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile
import tomllib
import urllib.request
from pathlib import Path
from typing import NoReturn

import blake3
import click
from tqdm import tqdm

REPO = Path(__file__).resolve().parents[2]
PROGRAMS = REPO / "programs"
INDEX = PROGRAMS / "index.tsv"
BRAINFUNK = Path(os.environ.get("BRAINFUNK", REPO / "build" / "bin" / "brainfunk"))

COMMANDS = frozenset(b"+-<>[],.")
SHINGLE = 8           # tokens per shingle for near-duplicate detection
NEAR = 0.9            # Jaccard similarity at or above which two programs are near-duplicates

ORIGINS = {"handwritten", "macro", "compiled", "textgen", "unknown"}
ROLES = {"workload", "conformance", "stress", "fragment"}
HALTS = {"always", "with-input", "interactive", "never"}
EOFS = {"0", "255", "keep"}

# Fields a program inherits from its manifest's [defaults] table.
INHERITED = ("author", "license", "origin", "family", "role", "cell_bits", "wraps", "eof",
             "tape_left", "halts")
REQUIRED = ("origin", "family", "role", "halts")

INDEX_COLUMNS = ("path", "source", "family", "origin", "role", "halts", "bytes", "commands",
                 "blake3", "norm_blake3", "cluster", "duplicate")


@dataclasses.dataclass
class Program:
    manifest: Path
    source: str
    path: Path                       # absolute
    meta: dict
    generated: bool = False          # built locally, not committed: see `install`

    @property
    def rel(self) -> str:
        return self.path.relative_to(REPO).as_posix()


def fail(message: str) -> NoReturn:
    raise click.ClickException(message)


INPUTS = "inputs.toml"
LOCK = "inputs.lock"


def manifests() -> list[Path]:
    return sorted(p for p in PROGRAMS.rglob("*.toml") if p.name != INPUTS)


def source_root(manifest: Path, source: dict) -> Path:
    """Directory that program paths are relative to."""
    submodule = source.get("submodule")
    return REPO / submodule if submodule else manifest.parent


def load(manifest: Path) -> tuple[dict, list[Program]]:
    with manifest.open("rb") as f:
        data = tomllib.load(f)
    source = data.get("source") or fail(f"{manifest}: missing [source]")
    defaults = data.get("defaults", {})
    root = source_root(manifest, source)
    programs = []
    for entry in data.get("program", []):
        meta = {key: defaults[key] for key in INHERITED if key in defaults}
        meta.update(entry)
        if "file" not in meta:
            fail(f"{manifest}: program without file: {entry}")
        programs.append(Program(manifest, source["name"], root / meta["file"], meta,
                                bool(source.get("generated"))))
    return source, programs


def load_all() -> list[Program]:
    """Every listed program. Reviewed entries come first, so that a duplicate cluster is
    named after, and `list --unique` keeps, a program whose metadata someone checked."""
    programs = []
    for manifest in manifests():
        programs.extend(load(manifest)[1])
    return sorted(programs, key=lambda p: (p.meta.get("reviewed", True) is False, p.rel))


# -------------------------------------------------------------------- hashing

def normalize(raw: bytes) -> bytes:
    """The program as its interpreter sees it: only the eight commands."""
    return bytes(c for c in raw if c in COMMANDS)


def tokens(code: bytes) -> list[tuple[int, int]]:
    """Run-length tokens, so that +++ and ++++ differ by one token, not by alignment."""
    return [(c, len(list(run))) for c, run in itertools.groupby(code)]


def shingles(code: bytes) -> frozenset[int]:
    toks = tokens(code)
    if len(toks) < SHINGLE:
        return frozenset()
    return frozenset(hash(tuple(toks[i:i + SHINGLE])) for i in range(len(toks) - SHINGLE + 1))


def jaccard(a: frozenset[int], b: frozenset[int]) -> float:
    if not a or not b:
        return 0.0
    if min(len(a), len(b)) < NEAR * max(len(a), len(b)):
        return 0.0          # cannot reach the threshold, skip the intersection
    return len(a & b) / len(a | b)


def build_index(programs: list[Program], progress: bool) -> list[dict]:
    rows = []
    norms = []
    for program in tqdm(programs, desc="hash", disable=not progress, leave=False):
        raw = program.path.read_bytes()
        norm = normalize(raw)
        norms.append(norm)
        rows.append({
            "path": program.rel, "source": program.source,
            "family": program.meta.get("family", ""), "origin": program.meta.get("origin", ""),
            "role": program.meta.get("role", ""), "halts": program.meta.get("halts", ""),
            "bytes": len(raw), "commands": len(norm),
            "blake3": blake3.blake3(raw).hexdigest(),
            "norm_blake3": blake3.blake3(norm).hexdigest(),
        })

    # Union-find over exact and near duplicates; the first path in order names a cluster.
    parent = list(range(len(rows)))

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i: int, j: int) -> None:
        i, j = find(i), find(j)
        if i != j:
            parent[max(i, j)] = min(i, j)

    kind = [""] * len(rows)
    first = {}
    for i, row in enumerate(rows):
        j = first.setdefault(row["norm_blake3"], i)
        if j != i:
            union(i, j)
            kind[i] = "exact"

    sets = [shingles(norm) for norm in norms]
    pairs = [(i, j) for i, j in itertools.combinations(range(len(rows)), 2)
             if rows[i]["norm_blake3"] != rows[j]["norm_blake3"]]
    for i, j in tqdm(pairs, desc="near-duplicates", disable=not progress, leave=False):
        if jaccard(sets[i], sets[j]) >= NEAR:
            union(i, j)
            kind[j] = kind[j] or "near"

    for i, row in enumerate(rows):
        root = find(i)
        row["cluster"] = rows[root]["path"]
        row["duplicate"] = kind[i] if root != i else ""
    return rows


def format_index(rows: list[dict]) -> str:
    lines = ["\t".join(INDEX_COLUMNS)]
    lines += ["\t".join(str(row[c]) for c in INDEX_COLUMNS) for row in rows]
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ checks

def check_manifest(manifest: Path) -> list[str]:
    errors = []
    where = manifest.relative_to(REPO)
    source, programs = load(manifest)
    for key in ("name", "license"):
        if key not in source:
            errors.append(f"{where}: [source] has no {key}")
    # Files committed here need a known license, or a recorded decision to keep them anyway.
    if source.get("license", "").lower() in ("", "unknown", "noassertion") and \
            not source.get("submodule") and not source.get("unknown_license_accepted"):
        errors.append(f"{where}: files committed to programs/ need a known license, "
                      "or unknown_license_accepted with the reason")
    for license_file in source.get("license_files", []):
        if not (source_root(manifest, source) / license_file).is_file():
            errors.append(f"{where}: license file {license_file} not found")
    submodule = source.get("submodule")
    if submodule and not (REPO / submodule).is_dir():
        errors.append(f"{where}: submodule {submodule} not checked out")

    seen = set()
    for program in programs:
        name = f"{where}: {program.meta['file']}"
        if program.meta["file"] in seen:
            errors.append(f"{name}: listed twice")
        seen.add(program.meta["file"])
        if program.generated:
            if "blake3" not in program.meta:
                errors.append(f"{name}: a generated program needs its blake3")
            elif program.path.is_file() and \
                    blake3.blake3(program.path.read_bytes()).hexdigest() != program.meta["blake3"]:
                errors.append(f"{name}: built file does not match its blake3; rebuild it")
        elif not program.path.is_file():
            errors.append(f"{name}: file not found")
        for key in REQUIRED:
            if key not in program.meta:
                errors.append(f"{name}: no {key}")
        for key, allowed in (("origin", ORIGINS), ("role", ROLES), ("halts", HALTS)):
            if key in program.meta and program.meta[key] not in allowed:
                errors.append(f"{name}: {key} must be one of {sorted(allowed)}")
        if program.meta.get("cell_bits", 8) not in (8, 16, 32, "any"):
            errors.append(f"{name}: cell_bits must be 8, 16, 32 or \"any\"")
        for eof in program.meta.get("eof", []):
            if eof not in EOFS:
                errors.append(f"{name}: eof must be a list from {sorted(EOFS)}")
        # Inputs and expected outputs are relative to the source root, like `file`.
        root = source_root(manifest, source)
        for case in program.meta.get("cases", []):
            for key in ("input", "expect"):
                if key in case and not (root / case[key]).is_file():
                    errors.append(f"{name}: {key} {case[key]} not found")
        script = program.meta.get("input_script")
        if script and not (root / script).is_file():
            errors.append(f"{name}: input_script {script} not found")
    return errors


def unlisted(programs: list[Program]) -> list[str]:
    """Program files committed under programs/ that no manifest lists."""
    listed = {p.path for p in programs}
    found = [p for pattern in ("*.b", "*.bf") for p in PROGRAMS.rglob(pattern)]
    return sorted(p.relative_to(REPO).as_posix() for p in found
                  if p not in listed and "inputs" not in p.relative_to(PROGRAMS).parts)


def current_index() -> str:
    """The index of committed and submodule programs. Generated programs are left out, so
    the index is the same whether or not they have been built."""
    programs = [p for p in load_all() if not p.generated]
    return format_index(build_index(programs, progress=sys.stderr.isatty()))


def check_index(text: str) -> None:
    if not INDEX.is_file() or INDEX.read_text() != text:
        fail("programs/index.tsv is out of date: run tools/corpus/corpus.py index")


# --------------------------------------------------------------------- CLI

@click.group()
def cli() -> None:
    """Manage the Brainfuck program corpus."""


@cli.command()
@click.option("--check", is_flag=True, help="Fail if programs/index.tsv is out of date.")
def index(check: bool) -> None:
    """Hash and deduplicate every listed program into programs/index.tsv."""
    text = current_index()
    if check:
        check_index(text)
        return
    INDEX.write_text(text)
    rows = text.count("\n") - 1
    click.echo(f"wrote {INDEX.relative_to(REPO)}: {rows} programs", err=True)


@cli.command()
def verify() -> None:
    """Check manifests, listed files, licenses and the index."""
    errors = [e for manifest in manifests() for e in check_manifest(manifest)]
    errors += [f"{path}: not listed in any manifest" for path in unlisted(load_all())]
    for error in errors:
        click.echo(error, err=True)
    if errors:
        fail(f"{len(errors)} problem(s)")
    text = current_index()
    check_index(text)
    for warning in family_conflicts(text):
        click.echo(f"warning: {warning}", err=True)
    unbuilt = [p for p in load_all() if p.generated and not p.path.is_file()]
    for manifest in sorted({p.manifest for p in unbuilt}):
        count = sum(p.manifest == manifest for p in unbuilt)
        build = load(manifest)[0].get("build", "its build script")
        click.echo(f"note: {count} generated program(s) of {manifest.relative_to(REPO)} "
                   f"not built; run {build}", err=True)
    click.echo("corpus OK", err=True)


def family_conflicts(text: str) -> list[str]:
    """Duplicate clusters whose members disagree on family, which usually means one of them
    is attributed wrongly. Programs with no commands at all are all alike and are skipped."""
    lines = text.splitlines()
    header = lines[0].split("\t")
    families: dict[str, set[str]] = {}
    for row in (dict(zip(header, line.split("\t"))) for line in lines[1:]):
        if row["commands"] != "0":
            families.setdefault(row["cluster"], set()).add(row["family"])
    return [f"cluster {cluster} mixes families {', '.join(sorted(names))}"
            for cluster, names in sorted(families.items()) if len(names) > 1]


@cli.command()
@click.argument("manifest", type=click.Path(exists=True, dir_okay=False, path_type=Path))
def fetch(manifest: Path) -> None:
    """Download each program of MANIFEST that has a url but no file yet.

    Files are saved byte for byte. Record the retrieval date in [source] yourself.
    """
    manifest = manifest.resolve()
    source, programs = load(manifest)
    if source.get("submodule"):
        fail("submodule sources are updated with git, not fetched")
    for program in programs:
        url = program.meta.get("url")
        if not url or program.path.exists():
            continue
        request = urllib.request.Request(url, headers={"User-Agent": "brainfunk-corpus"})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read()
        program.path.parent.mkdir(parents=True, exist_ok=True)
        program.path.write_bytes(data)
        click.echo(f"{blake3.blake3(data).hexdigest()}  {program.rel}  ({len(data)} bytes)")
    click.echo(f"retrieved {datetime.date.today().isoformat()}", err=True)


@cli.command(name="list")
@click.option("--role", multiple=True, help="Only these roles (repeatable).")
@click.option("--halts", multiple=True, help="Only these halting kinds (repeatable).")
@click.option("--eof", help="Only programs that accept this EOF behaviour.")
@click.option("--cell-bits", type=int, help="Only programs written for this cell width.")
@click.option("--unique", is_flag=True, help="One program per duplicate cluster.")
@click.option("--match", "pattern", help="Only paths matching this glob.")
def list_programs(role, halts, eof, cell_bits, unique, pattern) -> None:
    """Print the paths of listed programs, one per line."""
    rows = {}
    if unique:
        if not INDEX.is_file():
            fail("--unique needs programs/index.tsv")
        lines = INDEX.read_text().splitlines()
        header = lines[0].split("\t")
        rows = {r["path"]: r for r in (dict(zip(header, l.split("\t"))) for l in lines[1:])}
    for program in load_all():
        meta = program.meta
        if program.generated and not program.path.is_file():
            continue
        if role and meta.get("role") not in role:
            continue
        if halts and meta.get("halts") not in halts:
            continue
        if eof and "eof" in meta and eof not in meta["eof"]:
            continue
        if cell_bits and meta.get("cell_bits", 8) not in (cell_bits, "any"):
            continue
        if pattern and not fnmatch.fnmatch(program.rel, pattern):
            continue
        if unique and rows.get(program.rel, {}).get("duplicate"):
            continue
        click.echo(program.rel)


@cli.command()
def summary() -> None:
    """Count programs by source, role and halting kind, from programs/index.tsv."""
    if not INDEX.is_file():
        fail("run tools/corpus/corpus.py index first")
    lines = INDEX.read_text().splitlines()
    header = lines[0].split("\t")
    rows = [dict(zip(header, line.split("\t"))) for line in lines[1:]]
    unique = [r for r in rows if not r["duplicate"]]
    size = sum(int(r["bytes"]) for r in rows)
    click.echo(f"{len(rows)} programs, {len(unique)} after removing duplicates, "
               f"{len({r['family'] for r in unique})} families, {size / 2**20:.1f} MiB")
    for title, key in (("source", lambda r: r["source"]),
                       ("role / halts", lambda r: f"{r['role']} / {r['halts']}"),
                       ("origin", lambda r: r["origin"])):
        counts: dict[str, int] = {}
        for row in unique:
            counts[key(row)] = counts.get(key(row), 0) + 1
        click.echo(f"\nunique programs by {title}:")
        for name, count in sorted(counts.items(), key=lambda item: -item[1]):
            click.echo(f"  {count:5}  {name}")


@cli.command()
@click.argument("manifest", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.argument("built", type=click.Path(exists=True, file_okay=False, path_type=Path))
def install(manifest: Path, built: Path) -> None:
    """Copy the generated programs of MANIFEST from the directory BUILT.

    Each program is looked up in BUILT by its file name, and must match the blake3 that
    the manifest records, so a different generator or patch is caught here.
    """
    source, programs = load(manifest.resolve())
    if not source.get("generated"):
        fail(f"{manifest} is not a generated source")
    bad = []
    for program in programs:
        data = (built / program.path.name).read_bytes()
        if blake3.blake3(data).hexdigest() != program.meta["blake3"]:
            bad.append(program.path.name)
            continue
        program.path.parent.mkdir(parents=True, exist_ok=True)
        program.path.write_bytes(data)
    if bad:
        fail(f"{len(bad)} program(s) differ from their recorded blake3: {', '.join(bad)}")
    click.echo(f"installed {len(programs)} programs", err=True)


# ------------------------------------------------------------------- draft

def probe(path: Path, timeout: float) -> tuple[str, int]:
    """Run a program once on empty input. Returns ("ok" | "timeout" | "error", output bytes)."""
    try:
        result = subprocess.run(["nice", "-n", "19", str(BRAINFUNK), "-O3", "-f", str(path)],
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return "timeout", len(e.stdout or b"")
    return ("ok" if result.returncode == 0 else "error"), len(result.stdout)


def nearest_license(path: Path, top: Path) -> str:
    """The first "License:" line of the nearest README.md between path and top."""
    for directory in [path.parent, *path.parent.parents]:
        readme = directory / "README.md"
        if readme.is_file():
            for line in readme.read_text(errors="replace").splitlines():
                if line.lower().startswith(("license:", "licenses:")):
                    return line.split(":", 1)[1].strip()
        if directory == top:
            break
    return ""


def toml_value(value) -> str:
    return json.dumps(value, ensure_ascii=False) if isinstance(value, str) else \
        str(value).lower() if isinstance(value, bool) else str(value)


@cli.command()
@click.argument("directory", type=click.Path(exists=True, file_okay=False, path_type=Path))
@click.argument("out", type=click.Path(dir_okay=False, path_type=Path))
@click.option("--timeout", default=5.0, show_default=True, help="Seconds per probe run.")
@click.option("--jobs", default=os.cpu_count(), show_default=True)
@click.option("--force", is_flag=True, help="Overwrite OUT.")
def draft(directory: Path, out: Path, timeout: float, jobs: int, force: bool) -> None:
    """Draft a manifest for the programs under DIRECTORY, inside a submodule.

    Each program runs once on empty input to guess `halts`; a program that contains `,`
    is assumed to need input. The family is the first directory below DIRECTORY, and the
    nearest README.md's License: line is kept verbatim as license_note. Every entry is
    marked reviewed = false: check it before relying on it.
    """
    if out.exists() and not force:
        fail(f"{out} exists; pass --force to overwrite it")
    if not BRAINFUNK.is_file():
        fail(f"{BRAINFUNK} not found: build first, cmake --build build")
    directory = directory.resolve()
    submodule = next((p for p in [directory, *directory.parents]
                      if (p / ".git").is_file() and p.is_relative_to(REPO) and p != REPO),
                     None)
    if submodule is None:
        fail(f"{directory} is not inside a submodule")
    files = sorted(p for pattern in ("*.b", "*.bf") for p in directory.rglob(pattern)
                   if p.is_file() and not p.is_symlink() and ".git" not in p.parts)
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(tqdm(pool.map(lambda p: probe(p, timeout), files), total=len(files),
                            desc="probe", disable=not sys.stderr.isatty()))

    lines = [f"# Drafted by tools/corpus/corpus.py draft from {directory.relative_to(REPO)}.",
             f"# halts was guessed from one run on empty input with a {timeout:g} s limit.",
             "", "[source]",
             f"name = {toml_value(submodule.name)}",
             f"submodule = {toml_value(submodule.relative_to(REPO).as_posix())}",
             'license = "NOASSERTION"', "",
             "[defaults]", 'origin = "unknown"', 'role = "workload"', "cell_bits = 8", ""]
    for path, (status, _output) in zip(files, results):
        reads = b"," in normalize(path.read_bytes())
        entry: dict[str, str | bool] = {"file": path.relative_to(submodule).as_posix(),
                 "family": path.relative_to(directory).parts[0] if path.parent != directory
                 else directory.name}
        if "third_party" in path.relative_to(directory).parts:
            # Collected from someone else: the directory says who copied it, not who wrote it.
            entry["family"] = f"unknown-{path.stem}"
        if status == "error":
            entry |= {"role": "fragment", "halts": "always",
                      "notes": "brainfunk rejects it, probably unbalanced brackets."}
        elif status == "ok":
            entry["halts"] = "with-input" if reads else "always"
        else:
            entry["halts"] = "with-input" if reads else "never"
        note = nearest_license(path, directory)
        if note:
            entry["license_note"] = note
        entry["reviewed"] = False
        lines.append("[[program]]")
        lines += [f"{key} = {toml_value(value)}" for key, value in entry.items()]
        lines.append("")
    out.write_text("\n".join(lines))
    click.echo(f"wrote {out}: {len(files)} programs", err=True)


# ------------------------------------------------------------------ inputs

GENERATORS = {"random", "repeat", "concat"}
INPUT_SOURCES = ("text", "hex", "file", "generate")
TIMEOUT = 600.0


@dataclasses.dataclass
class Case:
    """One input of one program, under one EOF mode."""
    program: Program
    name: str
    eof: str
    spec: dict                       # the inputs.toml entry, or the manifest case
    base: Path                       # what file paths in spec are relative to
    lock: Path                       # the inputs.lock this case's result goes in
    limit: int | None = None
    expect: Path | None = None       # an expected output the source provides
    siblings: dict | None = None     # the program's other inputs by name, for concat

    @property
    def key(self) -> str:
        return f"{self.program.rel} {self.name} eof={self.eof}"

    def data(self, seen: tuple[str, ...] = ()) -> bytes:
        spec = self.spec
        if "text" in spec:
            return spec["text"].encode()
        if "hex" in spec:
            return bytes.fromhex(spec["hex"])
        if "file" in spec:
            return (self.base / spec["file"]).read_bytes()
        gen = spec["generate"]
        kind = gen["kind"]
        if kind == "random":
            # BLAKE3 as an extendable output function: the same bytes on every machine and version.
            raw = blake3.blake3(f"brainfunk input {gen.get('seed', 0)}".encode()).digest(length=gen["bytes"])
            alphabet = gen.get("alphabet")
            if alphabet is None:
                return raw
            symbols = alphabet.encode()
            return bytes(symbols[b % len(symbols)] for b in raw)
        if kind == "repeat":
            unit = gen["text"].encode() if "text" in gen else bytes.fromhex(gen["hex"])
            return unit * gen["count"]
        if kind == "concat":
            out = b""
            for part in gen["parts"]:
                if isinstance(part, str):
                    if part in seen or part not in (self.siblings or {}):
                        fail(f"{self.key}: concat of {part}, which is not another input of the program")
                    out += self.siblings[part].data(seen + (self.name,))
                else:
                    out += part["text"].encode() if "text" in part else bytes.fromhex(part["hex"])
            return out
        fail(f"{self.key}: unknown generator {kind}")


def program_eofs(program: Program) -> list[str]:
    return list(program.meta.get("eof", ["0"]))


def input_errors(spec: dict, where: str, base: Path) -> list[str]:
    """What is wrong with an input's definition, before running anything."""
    errors = []
    given = [key for key in INPUT_SOURCES if key in spec]
    if len(given) != 1:
        errors.append(f"{where}: give exactly one of {', '.join(INPUT_SOURCES)}")
    if "file" in spec and not (base / spec["file"]).is_file():
        errors.append(f"{where}: file {spec['file']} not found")
    if "hex" in spec:
        try:
            bytes.fromhex(spec["hex"])
        except ValueError:
            errors.append(f"{where}: hex is not hexadecimal")
    gen = spec.get("generate")
    if gen is not None:
        kind = gen.get("kind")
        if kind not in GENERATORS:
            errors.append(f"{where}: generate.kind must be one of {sorted(GENERATORS)}")
        elif kind == "random" and not isinstance(gen.get("bytes"), int):
            errors.append(f"{where}: random needs bytes, a count")
        elif kind == "repeat" and not (("text" in gen or "hex" in gen) and isinstance(gen.get("count"), int)):
            errors.append(f"{where}: repeat needs text or hex, and count")
        elif kind == "concat" and not isinstance(gen.get("parts"), list):
            errors.append(f"{where}: concat needs parts, a list of input names and {{text}} or {{hex}} tables")
    if "limit" in spec and not (isinstance(spec["limit"], int) and spec["limit"] > 0):
        errors.append(f"{where}: limit must be a positive number of instructions")
    return errors


def load_cases(programs: list[Program] | None = None) -> tuple[list[Case], list[str]]:
    """Every case of every program, and what is wrong with their definitions."""
    programs = programs if programs is not None else load_all()
    errors: list[str] = []
    cases: list[Case] = []

    # From the manifests: the inputs and expected outputs a source ships.
    for program in programs:
        source, _ = load(program.manifest)
        root = source_root(program.manifest, source)
        for case in program.meta.get("cases", []):
            name = Path(case.get("input") or case.get("expect") or "empty").stem
            spec = {"file": case["input"]} if "input" in case else {"text": ""}
            # An expected output for wider cells, or one known not to match, is not compared.
            expect = (root / case["expect"] if "expect" in case and "expect_cell_bits" not in case
                      and "expect_unmatched" not in case else None)
            for eof in program_eofs(program):
                cases.append(Case(program, name, eof, spec, root, program.manifest.parent / LOCK, expect=expect))

    # From inputs.toml: the ones written here.
    by_dir: dict[Path, list[Program]] = {}
    for program in programs:
        by_dir.setdefault(program.manifest.parent, []).append(program)
    for inputs in sorted(PROGRAMS.rglob(INPUTS)):
        where = inputs.relative_to(REPO)
        with inputs.open("rb") as f:
            entries = tomllib.load(f).get("input", [])
        mine: dict[str, list[tuple[dict, Program]]] = {}
        for spec in entries:
            label = f"{where}: {spec.get('program', '?')} {spec.get('name', '?')}"
            if "program" not in spec or "name" not in spec:
                errors.append(f"{label}: an input needs program and name")
                continue
            found = [p for p in by_dir.get(inputs.parent, []) if p.meta["file"] == spec["program"] and
                     ("manifest" not in spec or p.manifest.name == spec["manifest"])]
            if len(found) != 1:
                errors.append(f"{label}: {'no' if not found else 'more than one'} program of that file next to it"
                              + ("; say which manifest" if found else ""))
                continue
            errors += input_errors(spec, label, inputs.parent)
            program = found[0]
            eofs = spec.get("eof", program_eofs(program))
            eofs = [eofs] if isinstance(eofs, str) else eofs
            for eof in eofs:
                if eof not in EOFS or ("eof" in program.meta and eof not in program.meta["eof"]):
                    errors.append(f"{label}: eof {eof} is not one the program handles")
            mine.setdefault(program.rel, []).append((spec, program))
        for rel, specs in mine.items():
            names = [spec["name"] for spec, _ in specs]
            for name in sorted({n for n in names if names.count(n) > 1}):
                errors.append(f"{where}: {rel} has two inputs called {name}")
            siblings: dict[str, Case] = {}
            for spec, program in specs:
                eofs = spec.get("eof", program_eofs(program))
                for eof in [eofs] if isinstance(eofs, str) else eofs:
                    case = Case(program, spec["name"], eof, spec, inputs.parent, inputs.parent / LOCK,
                                limit=spec.get("limit"), siblings=siblings)
                    siblings.setdefault(spec["name"], case)
                    cases.append(case)
    return cases, errors


def run_case(case: Case, level: int, fast: bool = False) -> dict:
    """The case at a level: through the analyzing loop, or with fast, through run() alone."""
    if not BRAINFUNK.is_file():
        fail(f"{BRAINFUNK} not found: build first, cmake --build build")
    data = case.data()
    command = [str(BRAINFUNK), f"-O{level}", "-e", case.eof]
    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.json"
        if not fast:
            command += ["-m", "analyze", "-a", "stat", "-o", str(report)]
            if case.limit:
                command += ["--limit", str(case.limit)]
        command += ["-f", str(case.program.path)]
        try:
            done = subprocess.run(command, input=data, capture_output=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return {"error": f"no end within {TIMEOUT:g} s"}
        result = {"input": blake3.blake3(data).hexdigest(), "output": blake3.blake3(done.stdout).hexdigest(),
                  "output_bytes": len(done.stdout)}
        if fast:
            result["exit"] = done.returncode
            return result
        if not report.is_file():
            return {"error": f"exit {done.returncode}: {done.stderr.decode(errors='replace')[-300:]}"}
        run = json.loads(report.read_text())["run"]
    result["ending"] = ("trapped" if run["trapped"] else "endless" if run["endless"] else
                        "halted" if run["halted"] else "limit")
    if result["ending"] != "limit":
        result |= {"tape_crc32": run["memory_crc32"], "base_ptr": run["base_ptr"]}
    if case.expect is not None and level == 0:
        result["expected_output"] = case.expect.read_bytes() == done.stdout
    return result


def read_lock(path: Path) -> dict:
    return json.loads(path.read_text()) if path.is_file() else {}


def write_lock(path: Path, entries: dict) -> None:
    path.write_text(json.dumps(dict(sorted(entries.items())), indent=1, sort_keys=True) + "\n")


def selected(cases: list[Case], pattern: str | None) -> list[Case]:
    return [c for c in cases if not pattern or fnmatch.fnmatch(c.program.rel, pattern)]


def runnable(cases: list[Case]) -> tuple[list[Case], int]:
    """The cases whose program is there: generated ones may not have been built."""
    present = [c for c in cases if c.program.path.is_file()]
    return present, len(cases) - len(present)


@cli.group()
def inputs() -> None:
    """Inputs of the programs, what they must give, and how much of the programs they reach."""


@inputs.command(name="verify")
def inputs_verify() -> None:
    """Check input definitions, and that inputs.lock has a current entry for every case."""
    cases, errors = load_cases()
    unbuilt = 0
    for case in cases:
        if case.program.generated and not case.program.path.is_file():
            unbuilt += 1
            continue
        lock = read_lock(case.lock)
        entry = lock.get(case.key)
        if entry is None:
            errors.append(f"{case.key}: not in {case.lock.relative_to(REPO)}; run inputs lock")
        else:
            try:
                if entry.get("input") != blake3.blake3(case.data()).hexdigest():
                    errors.append(f"{case.key}: its input changed; run inputs lock")
            except click.ClickException as error:
                errors.append(error.message)
    keys = {c.key for c in cases}
    for lock in sorted(PROGRAMS.rglob(LOCK)):
        for key in read_lock(lock):
            if key not in keys:
                errors.append(f"{lock.relative_to(REPO)}: {key} has no case any more")
    for error in errors:
        click.echo(error, err=True)
    if errors:
        fail(f"{len(errors)} problem(s)")
    if unbuilt:
        click.echo(f"note: {unbuilt} case(s) of programs not built were not checked", err=True)
    click.echo(f"{len(cases) - unbuilt} cases OK", err=True)


@inputs.command(name="lock")
@click.option("--match", "pattern", help="Only programs whose path matches this glob.")
@click.option("--jobs", default=os.cpu_count(), show_default=True)
def inputs_lock(pattern: str | None, jobs: int) -> None:
    """Run cases at -O0 and record what they give in inputs.lock."""
    cases, errors = load_cases()
    if errors:
        fail("\n".join(errors))
    current = {c.key for c in cases}
    cases, missing = runnable(selected(cases, pattern))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(tqdm(pool.map(lambda c: run_case(c, 0), cases), total=len(cases), desc="lock",
                            disable=not sys.stderr.isatty()))
    problems = []
    locks: dict[Path, dict] = {}
    for case, result in zip(cases, results):
        if "error" in result:
            problems.append(f"{case.key}: {result['error']}")
            continue
        if result.pop("expected_output", True) is False:
            problems.append(f"{case.key}: output differs from {case.expect.relative_to(REPO)}")
        locks.setdefault(case.lock, read_lock(case.lock))[case.key] = result
    for path, entries in locks.items():
        # Entries of cases that are gone: an input removed or renamed, or an EOF mode dropped.
        write_lock(path, {key: value for key, value in entries.items() if key in current})
        click.echo(f"wrote {path.relative_to(REPO)}", err=True)
    if missing:
        click.echo(f"note: {missing} case(s) of programs not built were left as they were", err=True)
    for problem in problems:
        click.echo(problem, err=True)
    if problems:
        fail(f"{len(problems)} problem(s)")


@inputs.command(name="check")
@click.option("--match", "pattern", help="Only programs whose path matches this glob.")
@click.option("--levels", default="0,1,2,3", show_default=True, help="Optimization levels, comma separated.")
@click.option("--jobs", default=os.cpu_count(), show_default=True)
def inputs_check(pattern: str | None, levels: str, jobs: int) -> None:
    """Run every case at every level, analyzed and plain, against inputs.lock.

    A run cut at its limit stops at another point at each level, so only -O0's output is held to
    the lock; the other levels' must agree with it as far as both go."""
    cases, errors = load_cases()
    if errors:
        fail("\n".join(errors))
    cases, missing = runnable(selected(cases, pattern))
    wanted = [int(level) for level in levels.split(",")]
    # The slowest first, so that the longest run does not start last: -O0, and analyzed.
    jobs_list = sorted(((c, level, fast) for c in cases for level in wanted for fast in (False, True)
                        if not (fast and c.limit)), key=lambda job: (job[1], job[2]))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(tqdm(pool.map(lambda job: run_case(*job), jobs_list), total=len(jobs_list), desc="check",
                            disable=not sys.stderr.isatty()))
    problems = []
    for (case, level, fast), result in zip(jobs_list, results):
        how = f"{case.key} -O{level}{' run' if fast else ''}"
        expected = read_lock(case.lock).get(case.key)
        if expected is None:
            problems.append(f"{how}: not in inputs.lock; run inputs lock")
        elif "error" in result:
            problems.append(f"{how}: {result['error']}")
        elif result["input"] != expected["input"]:
            problems.append(f"{how}: its input changed; run inputs lock")
        elif expected["ending"] == "limit":
            if level == 0 and not fast and result["output"] != expected["output"]:
                problems.append(f"{how}: output differs")
        else:
            for key in ("output", "output_bytes", "ending", "tape_crc32", "base_ptr"):
                if key in result and key in expected and result[key] != expected[key]:
                    problems.append(f"{how}: {key} is {result[key]}, not {expected[key]}")
    for problem in problems:
        click.echo(problem, err=True)
    if missing:
        click.echo(f"note: {missing} case(s) of programs not built were skipped", err=True)
    if problems:
        fail(f"{len(problems)} problem(s)")
    click.echo(f"{len(jobs_list)} runs of {len(cases)} cases agree with inputs.lock", err=True)


@inputs.command(name="show")
@click.argument("program")
@click.argument("name")
@click.option("--eof", default=None, help="Which EOF mode's case, if it matters.")
def inputs_show(program: str, name: str, eof: str | None) -> None:
    """Write a case's input to standard output."""
    cases, _ = load_cases()
    found = [c for c in cases if c.program.rel == program and c.name == name and (eof is None or c.eof == eof)]
    if not found:
        fail(f"no input {name} of {program}")
    sys.stdout.buffer.write(found[0].data())


@inputs.command(name="coverage")
@click.argument("program")
@click.option("--jobs", default=os.cpu_count(), show_default=True)
def inputs_coverage(program: str, jobs: int) -> None:
    """How much of a program each of its inputs reaches at -O0, and what each adds to the others.

    Counts words of the program and branch directions; an input that adds nothing is a candidate
    to drop, and what none reaches is where the next input should go."""
    cases, errors = load_cases()
    if errors:
        fail("\n".join(errors))
    cases = [c for c in cases if c.program.rel == program]
    if not cases:
        fail(f"no inputs of {program}")

    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config.json"
        config.write_text(json.dumps({"profile": {"counts": True}}))

        def report(index_case: tuple[int, Case]) -> Path:
            index, case = index_case
            out = Path(directory) / f"{index}.json"
            command = [str(BRAINFUNK), "-O0", "-e", case.eof, "-m", "analyze", "-a", "profile", "--config",
                       str(config), "-o", str(out), "-f", str(case.program.path)]
            if case.limit:
                command += ["--limit", str(case.limit)]
            subprocess.run(command, input=case.data(), capture_output=True, timeout=TIMEOUT)
            if not out.is_file():
                fail(f"{case.key}: no report")
            return out

        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            reports = list(pool.map(report, enumerate(cases)))
        # brainfunk adds the counters up, and works out each word's count from them.
        done = subprocess.run([str(BRAINFUNK), "-O0", "-m", "coverage", "-f", str(cases[0].program.path),
                               *map(str, reports)], capture_output=True, text=True)
        if done.returncode != 0:
            fail(done.stderr.strip())
        result = json.loads(done.stdout)

    click.echo(f"{program}: {len(cases)} cases, {result['words']} words and {result['branches']} branches at -O0")
    click.echo(f"{'case':40} {'words':>8} {'ways':>8} {'only it':>8}")
    for case, row in zip(cases, result["inputs"]):
        click.echo(f"{case.name + ' eof=' + case.eof:40} {row['words_run']:8} {row['ways']:8} {row['only']:8}")
    everything = result["all"]
    click.echo(f"{'all':40} {everything['words_run']:8} {everything['ways']:8}"
               f"   {everything['words_run'] / result['words']:.1%} of words, "
               f"{everything['ways'] / max(2 * result['branches'], 1):.1%} of branch directions")


if __name__ == "__main__":
    cli()
