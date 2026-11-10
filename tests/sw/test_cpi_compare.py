"""tools/cpi_compare.py: what its report makes of recorded cycles, and that its run still goes through."""
import json
import shutil
import subprocess
import sys

import pytest

from sw_support import CLANG, ROOT, run

pytest.importorskip("blake3", reason="the corpus tool's packages")
pytest.importorskip("click")
pytest.importorskip("tqdm")

TOOL = ROOT / "tools" / "cpi_compare.py"
PROGRAM = "programs/brainfuck.org/squares.b"


def table(text, title):
    """The rows of the table under a title, as lists of cells, by the first cell."""
    lines = text.split(title, 1)[1].split("\n\n")[1].splitlines()
    return {cells[0]: cells[1:] for cells in (line.split() for line in lines)}


def test_report_divides_the_cycles_of_the_run_alone(tmp_path):
    """1000 instructions: 1500 cycles of the core, 8000 of the interpreter after its 200 to start, 2000 of
    native code after 300, and with -march=native 10 after 300, which is lost in the 300."""
    def host(cycles, start):
        return {"cycles": cycles, "start_cycles": start, "host_instructions": 12_100, "start_instructions": 100,
                "runs": [cycles, cycles + 82]}
    results = tmp_path / "cpi.json"
    results.write_text(json.dumps({"source": "recorded", "host": None, "cases": [
        {"key": "a/x.b none eof=0", "program": "a/x.b", "instructions": 1000, "core": {"cycles": 1500},
         "host": {"interp": host(8200, 200), "native": host(2300, 300), "native, this cpu": host(310, 300)}},
        {"key": "a/y.b none eof=0", "program": "a/y.b", "error": "no end within 10 instructions"}]}))
    text = run(sys.executable, TOOL, "report", results, "--min-cycles", 0, "--host-mhz", 4000).stdout.decode()
    text = text.replace("native, this cpu", "native-this-cpu")
    assert table(text, "Cycles per instruction")["x.b"] == ["1,000", "1.500", "8.000", "2.000", "~0"]
    # 168 MHz / 1.5, 4000 MHz / 8, 4000 MHz / 2; then the host's over the core's.
    assert table(text, "Million instructions a second")["x.b"] == ["112", "500", "2,000", "-", "4.464", "17.9", "-"]
    # 12000 host instructions over 1000, and over the cycles of each run.
    assert table(text, "Host instructions for each")["x.b"] == ["12.0", "12.0", "-", "1.500", "6.000", "-"]
    assert "y.b" not in text

    text = run(sys.executable, TOOL, "report", results, "--min-cycles", 8001, check=False)
    assert text.returncode != 0 and b"nothing to report" in text.stderr


def can_count_cycles():
    return bool(shutil.which("perf")) and subprocess.run(["perf", "stat", "-e", "cycles:u", "true"],
                                                         capture_output=True).returncode == 0


def test_run_counts_the_instructions_of_the_o2_program(tmp_path):
    results = tmp_path / "cpi.json"
    run(sys.executable, TOOL, "run", "--no-host", "--no-core", "-o", results, PROGRAM)
    row, = json.loads(results.read_text())["cases"]
    squares = "".join(f"{n * n}\n" for n in range(101))  # what squares.b prints
    assert row["program"] == PROGRAM and row["output_bytes"] == len(squares)
    assert row["instructions"] == sum(row["dynamic"].values()) > 100_000


@pytest.mark.skipif(not shutil.which(CLANG), reason="clang not found")
@pytest.mark.skipif(not can_count_cycles(), reason="perf cannot count cycles:u here")
def test_run_measures_the_host(tmp_path):
    results = tmp_path / "cpi.json"
    run(sys.executable, TOOL, "run", "--no-core", "--repeat", 1, "-o", results, PROGRAM)
    row, = json.loads(results.read_text())["cases"]
    for way in ("interp", "native", "native, this cpu"):
        assert row["host"][way]["cycles"] > 0 and row["host"][way]["start_cycles"] > 0
    # The interpreter takes several of the host's instructions for each of the program's.
    interp = row["host"]["interp"]
    assert interp["host_instructions"] - interp["start_instructions"] > 3 * row["instructions"]
