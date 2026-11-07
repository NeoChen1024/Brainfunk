"""Tests are only worth what they can catch: each broken design here must fail them.

A surviving mutant means a gap in the tests, not a passing grade. This is how it came out that nothing
exercised write-back. Enabled by --mutation.
"""
import shutil

import pytest

from conftest import CORPUS, ROOT, assemble, run
from mutants import FORMAL_MUTANTS, MUTANTS

pytestmark = pytest.mark.mutation

RTL = ["brainfunk_core.sv", "brainfunk_cache.sv", "sdram_ctrl.sv", "brainfunk_soc.sv", "brainfunk_mem.sv"]
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


@pytest.mark.parametrize("name,file,text,replacement", MUTANTS, ids=[mutant[0] for mutant in MUTANTS])
def test_cosimulation_catches(tools, tmp_path, name, file, text, replacement):
    sources = mutated(tmp_path, file, text, replacement)
    run("verilator", "--cc", "--exe", "--build", "-j", "2", "-O3", "--x-assign", "fast", "--x-initial", "fast",
        "--top-module", "brainfunk_soc", "-DBRAINFUNK_RETIRE_PORT", "--Mdir", tmp_path / "obj",
        "-CFLAGS", f"-std=c++20 -O1 -I{ROOT} -I{ROOT}/tests", *sources, ROOT / "tb" / "sdram_model.sv",
        ROOT / "tb" / "cosim.cpp", ROOT / "libbrainfunk.cpp", ROOT / "optimizer.cpp", ROOT / "llvm_codegen.cpp", "-o", "cosim")
    cosim = tmp_path / "obj" / "cosim"

    commands = [[cosim, "--evict", 200, "--seed", 1], [cosim, "--evict", 200, "--seed", 5000, "--stall"],
                [cosim, "--random", 600, "--seed", 1, "--stall"]]
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
    pytest.fail(f"nothing caught: {name}")


@pytest.mark.parametrize("name", FORMAL_MUTANTS)
def test_formal_catches(tmp_path, name):
    if shutil.which("sby") is None:
        pytest.skip("sby not installed")
    name, file, text, replacement = next(mutant for mutant in MUTANTS if mutant[0] == name)
    assert file == "brainfunk_core.sv"
    mutated(tmp_path, file, text, replacement)
    shutil.copy(ROOT / "formal" / "core_props.sv", tmp_path / "core_props.sv")
    (tmp_path / "check.sby").write_text(f"""[options]
mode bmc
depth 24

[engines]
smtbmc --nopresat yices

[script]
read -formal -sv -DFORMAL -DFORMAL_ABSTRACT_MUL brainfunk_core.sv
read -formal -sv -DFORMAL_ABSTRACT_MUL core_props.sv
prep -top core_props

[files]
{tmp_path}/brainfunk_core.sv
{tmp_path}/core_props.sv
""")
    result = run("sby", "-f", "check.sby", cwd=tmp_path, check=False, timeout=600)
    assert result.returncode != 0, f"formal did not catch: {name}"
