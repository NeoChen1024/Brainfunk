"""Tests are only worth what they can catch: each broken design here must fail them.

A surviving mutant means a gap in the tests, not a passing grade. This is how it came out that nothing
exercised write-back. Enabled by --mutation.
"""
import shutil

import pytest

from conftest import CORPUS, ROOT, assemble, run
from mutants import FORMAL_MUTANTS, FORMAL_ONLY_MUTANTS, MUTANTS, PERFORMANCE_MUTANTS
from test_cosim import CPI_LIMITS, LONG_SCAN_IMAGE, MUL_SELF_IMAGES, program_cpi

pytestmark = pytest.mark.mutation

RTL = ["brainfunk_core.sv", "brainfunk_predict.sv", "brainfunk_cache.sv", "sdram_ctrl.sv", "brainfunk_soc.sv", "brainfunk_mem.sv"]
TIMEOUT = 90


def mutated(directory, file, text, replacement):
    sources = []
    for name in RTL:
        content = (ROOT / "rtl" / name).read_text()
        if name == file:
            assert content.count(text) == 1, f"mutant is stale: its text is in {file} {content.count(text)} times"
            content = content.replace(text, replacement)
        path = directory / name
        path.write_text(content)
        sources.append(path)
    return sources


def caught(command):
    try:
        return run(*command, check=False, timeout=TIMEOUT).returncode != 0
    except Exception:  # a hang counts: the simulation never finishing is a failure too
        return True


def build_cosim(directory, sources):
    run("verilator", "--cc", "--exe", "--build", "-j", "2", "-O3", "--x-assign", "fast", "--x-initial", "fast",
        "--top-module", "brainfunk_soc", "-DBRAINFUNK_RETIRE_PORT", "--Mdir", directory / "obj",
        "-CFLAGS", f"-std=c++20 -O1 -I{ROOT} -I{ROOT}/tests", *sources, ROOT / "tb" / "sdram_model.sv",
        ROOT / "tb" / "cosim.cpp", ROOT / "libbrainfunk.cpp", ROOT / "optimizer.cpp", ROOT / "llvm_codegen.cpp", "-o", "cosim")
    return directory / "obj" / "cosim"


@pytest.mark.parametrize("name,file,text,replacement", PERFORMANCE_MUTANTS, ids=[mutant[0] for mutant in PERFORMANCE_MUTANTS])
def test_cycle_counts_catch(tools, tmp_path, name, file, text, replacement):
    cosim = build_cosim(tmp_path, mutated(tmp_path, file, text, replacement))
    costs = {program: program_cpi(cosim, program, tmp_path) for program in CPI_LIMITS}
    assert any(costs[program] >= CPI_LIMITS[program] for program in CPI_LIMITS), f"nothing caught: {name}: {costs}"


@pytest.mark.parametrize("name,file,text,replacement", MUTANTS, ids=[mutant[0] for mutant in MUTANTS])
def test_cosimulation_catches(tools, tmp_path, name, file, text, replacement):
    sources = mutated(tmp_path, file, text, replacement)
    cosim = build_cosim(tmp_path, sources)

    commands = [[cosim, "--evict", 200, "--seed", 1], [cosim, "--evict", 200, "--seed", 5000, "--stall"],
                [cosim, "--random", 600, "--seed", 1, "--stall"], [cosim, "--random", 100, "--seed", 1, "--chaos"],
                [cosim, "--evict", 100, "--seed", 1, "--chaos"]]
    for word in ("10100000", "80000002", "90000001", "e0000000"):
        trap = tmp_path / f"trap_{word}.hex"
        trap.write_text(f"10000005\n{word}\n90000000\n")
    if any(caught(command) for command in commands):
        return
    for word in ("10100000", "80000002", "90000001", "e0000000"):
        if " trapped" not in run(cosim, tmp_path / f"trap_{word}.hex", check=False, timeout=TIMEOUT).stdout:
            return
    if (CORPUS / "hanoi.b").exists():
        image = assemble(CORPUS / "hanoi.b", 2, tmp_path / "hanoi.hex")
        if caught([cosim, "--limit", 1000000, image]):
            return
    for layout, text in MUL_SELF_IMAGES.items():
        mul = tmp_path / f"mul_{layout}.hex"
        mul.write_text(text)
        if caught([cosim, mul]):
            return
    long_scan = tmp_path / "long_scan.hex"
    long_scan.write_text(LONG_SCAN_IMAGE)
    if caught([cosim, long_scan]):
        return
    pytest.fail(f"nothing caught: {name}")


@pytest.mark.parametrize("name", FORMAL_MUTANTS)
def test_formal_catches(tmp_path, name):
    if shutil.which("sby") is None or shutil.which("bitwuzla") is None:
        pytest.skip("sby or bitwuzla not installed")
    name, file, text, replacement = next(mutant for mutant in MUTANTS + FORMAL_ONLY_MUTANTS if mutant[0] == name)
    assert file == "brainfunk_core.sv"
    mutated(tmp_path, file, text, replacement)
    shutil.copy(ROOT / "formal" / "core_props.svh", tmp_path / "core_props.svh")
    shutil.copy(ROOT / "formal" / "core_invariants.svh", tmp_path / "core_invariants.svh")
    (tmp_path / "check.sby").write_text(f"""[options]
mode prove
depth 3

[engines]
smtbmc --nopresat bitwuzla

[script]
read -formal -sv -DFORMAL -DFORMAL_ABSTRACT_MUL -DFORMAL_INVARIANTS brainfunk_core.sv
chparam -set PC_WIDTH 5 -set ADDR_WIDTH 4 -set LINE_BITS 2 -set STRIDE_BITS 2 brainfunk_core
prep -top brainfunk_core

[files]
{tmp_path}/brainfunk_core.sv
{tmp_path}/core_props.svh
{tmp_path}/core_invariants.svh
""")
    result = run("sby", "-f", "check.sby", cwd=tmp_path, check=False, timeout=600)
    assert result.returncode != 0, f"formal did not catch: {name}"
