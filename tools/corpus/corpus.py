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
BRAINFUNK = REPO / "brainfunk"

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


def manifests() -> list[Path]:
    return sorted(PROGRAMS.rglob("*.toml"))


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
        fail("build ./brainfunk first")
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


if __name__ == "__main__":
    cli()
