"""Tests are only worth what they can catch: each broken design here must fail them.

A surviving mutant means a gap in the tests, not a passing grade. This is how it came out, once, that
nothing exercised the data cache's write-back. Enabled by --mutation.
"""
import shutil

import pytest

from conftest import CORPUS, ROOT, assemble, build_cosim, run
from mutants import CONSOLE_MUTANTS, FORMAL_MUTANTS, FORMAL_ONLY_MUTANTS, LOADER_MUTANTS, MUTANTS, PERFORMANCE_MUTANTS
from test_cosim import CPI_LIMITS, I_FILL_IMAGE, LONG_SCAN_IMAGE, MUL_CHAIN_IMAGE, MUL_SELF_IMAGES, program_cpi
import test_console_tb
import test_loader
import test_uart_tb

pytestmark = pytest.mark.mutation

RTL = ["brainfunk_core.sv", "brainfunk_predict.sv", "brainfunk_cache.sv", "brainfunk_tape.sv", "tape_ram.sv", "sdram_ctrl.sv", "sdram_memory.sv", "brainfunk_soc.sv", "brainfunk_soc_sdram.sv", "brainfunk_mem.sv"]
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


@pytest.mark.parametrize("name,file,text,replacement", PERFORMANCE_MUTANTS, ids=[mutant[0] for mutant in PERFORMANCE_MUTANTS])
def test_cycle_counts_catch(tools, tmp_path, name, file, text, replacement):
    cosim = build_cosim(tmp_path, mutated(tmp_path, file, text, replacement))
    costs = {program: program_cpi(cosim, program, tmp_path) for program in CPI_LIMITS}
    assert any(costs[program] >= CPI_LIMITS[program] for program in CPI_LIMITS), f"nothing caught: {name}: {costs}"


@pytest.mark.parametrize("name,file,text,replacement", MUTANTS, ids=[mutant[0] for mutant in MUTANTS])
def test_cosimulation_catches(tools, tmp_path, name, file, text, replacement):
    sources = mutated(tmp_path, file, text, replacement)
    cosim = build_cosim(tmp_path, sources)

    commands = [[cosim, "--clash", 200, "--seed", 1], [cosim, "--clash", 200, "--seed", 5000, "--stall"],
                [cosim, "--random", 600, "--seed", 1, "--stall"], [cosim, "--random", 100, "--seed", 1, "--chaos"],
                [cosim, "--clash", 100, "--seed", 1, "--chaos"]]
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
    for layout, text in {**MUL_SELF_IMAGES, "chain": MUL_CHAIN_IMAGE}.items():
        mul = tmp_path / f"mul_{layout}.hex"
        mul.write_text(text)
        if caught([cosim, mul]):
            return
    long_scan = tmp_path / "long_scan.hex"
    long_scan.write_text(LONG_SCAN_IMAGE)
    if caught([cosim, long_scan]):
        return
    i_fill = tmp_path / "i_fill.hex"
    i_fill.write_text(I_FILL_IMAGE)
    if caught([cosim, i_fill]):
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
timeout 600

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
    # sby's own timeout, not run()'s: killing sby would leave its solver running. Only FAIL (2) and a
    # failed induction (4) are catches; a timeout (8) or an error (16) says nothing about the mutant.
    result = run("sby", "-f", "check.sby", cwd=tmp_path, check=False)
    assert result.returncode in (2, 4), f"formal did not catch: {name} (sby exited {result.returncode})"


@pytest.mark.parametrize("name,file,text,replacement", CONSOLE_MUTANTS, ids=[mutant[0] for mutant in CONSOLE_MUTANTS])
def test_console_testbenches_catch(tools, tmp_path, name, file, text, replacement):
    content = (ROOT / "rtl" / file).read_text()
    assert content.count(text) == 1, f"mutant is stale: its text is in {file} {content.count(text)} times"
    path = tmp_path / file
    path.write_text(content.replace(text, replacement))
    original = f"rtl/{file}"
    checks = [(test_console_tb.SOURCES, lambda where, sources: test_console_tb.console_testbench(where, sources=sources)),
              (test_uart_tb.SOURCES, lambda where, sources: test_uart_tb.uart_testbench(where, sources))]
    for number, (originals, check) in enumerate(checks):
        sources = [path if source == original else source for source in originals]
        assert path in sources
        where = tmp_path / str(number)
        where.mkdir()
        try:
            check(where, sources)
        except (Exception, pytest.fail.Exception):  # a failed assertion, pytest.fail, or a timeout
            return
    pytest.fail(f"nothing caught: {name}")


@pytest.mark.parametrize("name,file,text,replacement", LOADER_MUTANTS, ids=[mutant[0] for mutant in LOADER_MUTANTS])
def test_loader_testbenches_catch(tools, tmp_path, name, file, text, replacement):
    content = (ROOT / "rtl" / file).read_text()
    assert content.count(text) == 1, f"mutant is stale: its text is in {file} {content.count(text)} times"
    path = tmp_path / file.split("/")[-1]
    path.write_text(content.replace(text, replacement))
    original = f"rtl/{file}"
    if file == "brainfunk_soc.sv":
        checks = [lambda where, sources: test_uart_tb.uart_testbench(where, sources)]
        sources = [path if source == original else source for source in test_uart_tb.SOURCES]
    else:
        checks = [lambda where, sources: test_loader.test_loader_copies_from_anywhere(None, where, sources),
                  lambda where, sources: test_loader.test_loader_forgets_the_last_image(None, where, sources),
                  lambda where, sources: test_loader.test_loader_with_a_slow_source(None, where, sources),
                  lambda where, sources: test_loader.test_loader_takes_an_image_that_is_not_compressed(None, where, sources)]
        checks += [lambda where, sources, eof=eof: test_loader.test_loader_reports_the_end_of_input(None, where, eof, sources)
                   for eof in ["0", "keep"]]
        checks += [lambda where, sources, at=at: test_loader.test_loader_refuses_a_corrupt_image(None, where, at, sources)
                   for at in [4, 13]]
        checks += [lambda where, sources, change=change: test_loader.test_loader_refuses_what_it_cannot_take(None, where, *change, sources)
                   for change in [(4, 2), (5, 2), (5, 0x11), (5, 0x81), (6, 1), (8, 1), (14, 0x10)]]
        sources = [path if source == original else source for source in test_loader.SOURCES]
    assert path in sources
    for number, check in enumerate(checks):
        where = tmp_path / str(number)
        where.mkdir()
        try:
            check(where, sources)
        except (Exception, pytest.fail.Exception):  # a failed assertion, pytest.fail, or a timeout
            return
    pytest.fail(f"nothing caught: {name}")
