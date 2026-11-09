"""Shared set-up for the hardware tests: tools, images, and what is enabled.

ctest runs these, with labels hw, formal, mutation and board; see tests/CMakeLists.txt. Run alone,
pytest builds what it needs first, in build/ or BRAINFUNK_BUILD:

    pytest -n auto                 what needs neither the board nor much time
    pytest -n auto --full          also long.b and mandel.b, about 40 minutes
    pytest -n auto --mutation      broken designs must be caught, a few minutes
    pytest --board                 the real board, from build/yadan/yadan.bit
    pytest --board --flash         also programs the user flash; it wears out, so ask for this

Everything runs at nice 19. test/ is not part of the repository, and tests that need a program
missing from it are skipped.
"""
import json
import os
import subprocess
from pathlib import Path

import pytest
from filelock import FileLock

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path(os.environ.get("BF_CORPUS", ROOT / "test"))
BUILD = Path(os.environ.get("BRAINFUNK_BUILD", ROOT / "build"))
BIN = BUILD / "bin"
BRAINFUNK = BIN / "brainfunk"
BFLOAD = BIN / "bfload"
COSIM = BIN / "cosim"
RESULTS = BUILD / "hw-test" / "results"


def pytest_addoption(parser):
    parser.addoption("--full", action="store_true", help="also run long.b and mandel.b")
    parser.addoption("--mutation", action="store_true", help="also check that broken designs are caught")
    parser.addoption("--board", action="store_true", help="also test the real board")
    parser.addoption("--flash", action="store_true", help="also program the user flash; implies --board")


def pytest_configure(config):
    # Long jobs should not get in the way of anything else on the machine. Children inherit this.
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    if not os.environ.get("PYTEST_XDIST_WORKER"):
        RESULTS.mkdir(parents=True, exist_ok=True)
        for old in RESULTS.glob("*.json"):
            old.unlink()


def pytest_collection_modifyitems(config, items):
    enabled = {
        "full": config.getoption("--full"),
        "mutation": config.getoption("--mutation"),
        "board": config.getoption("--board") or config.getoption("--flash"),
        "flash": config.getoption("--flash"),
    }
    for item in items:
        for marker, on in enabled.items():
            if item.get_closest_marker(marker) and not on:
                item.add_marker(pytest.mark.skip(reason=f"needs --{marker}"))


def run(*command, cwd=ROOT, check=True, **kwargs):
    """Runs a command; a failure shows what it said."""
    result = subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True, text=True, **kwargs)
    if check and result.returncode != 0:
        pytest.fail(f"{' '.join(map(str, command))} exited {result.returncode}\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
    return result


@pytest.fixture(scope="session")
def tools(tmp_path_factory, worker_id):
    """The programs and the cosimulator, built once however many workers there are. Under ctest
    they are built already, and several pytest runs at once must not build in one tree."""
    if os.environ.get("BRAINFUNK_CTEST"):
        missing = [path for path in (BRAINFUNK, BFLOAD, COSIM) if not path.exists()]
        if missing:
            pytest.fail(f"not built: {', '.join(map(str, missing))}")
        return
    shared = tmp_path_factory.getbasetemp()
    if worker_id != "master":
        shared = shared.parent
    with FileLock(shared / "build.lock"):
        stamp = shared / "built"
        if not stamp.exists():
            run("cmake", "--build", BUILD, "--target", "brainfunk", "bfload", "cosim")
            stamp.touch()


def build_cosim(directory, sources, *options):
    """A cosimulator of its own from these RTL sources, with further Verilator options such as -G and -D."""
    run("verilator", "--cc", "--exe", "--build", "-j", "2", "-O3", "--x-assign", "fast", "--x-initial", "fast",
        "--top-module", "brainfunk_soc_sdram", "-DBRAINFUNK_RETIRE_PORT", *options, "--Mdir", directory / "obj",
        "-CFLAGS", f"-std=c++20 -O1 -I{ROOT} -I{ROOT}/tests -I{ROOT}/3rdparty/json/single_include", *sources, ROOT / "tb" / "sdram_model.sv",
        ROOT / "tb" / "cosim.cpp", ROOT / "libbrainfunk.cpp", ROOT / "optimizer.cpp", ROOT / "llvm_codegen.cpp",
        ROOT / "image.cpp", "-o", "cosim")
    return directory / "obj" / "cosim"


def assemble(source, level, out):
    """A hexadecimal program image from a Brainfuck file."""
    run(BRAINFUNK, f"-O{level}", "-m", "hex", "-f", source, "-o", out)
    return out


def assemble_text(code, level, out):
    run(BRAINFUNK, f"-O{level}", "-m", "hex", "-s", code, "-o", out)
    return out


def corpus_program(name):
    path = CORPUS / name
    if not path.exists():
        pytest.skip(f"{path} not found")
    return path


def cosim_json(*args, cosim=COSIM, **kwargs):
    return json.loads(run(cosim, "--json", *args, **kwargs).stdout)


def pytest_terminal_summary(terminalreporter, config):
    """Cycle counts of the real programs, for comparing one design with another."""
    if os.environ.get("PYTEST_XDIST_WORKER"):
        return
    rows = {}
    for path in sorted(RESULTS.glob("*.json")):
        rows[path.stem] = json.loads(path.read_text())
    if not rows:
        return
    terminalreporter.section("cycles")
    for name, row in rows.items():
        terminalreporter.line(f"{name:24} {row['retired']:>13} retired {row['cycles']:>13} cycles  CPI {row['cycles'] / max(row['retired'], 1):6.3f}")
    (RESULTS.parent / "cycles.json").write_text(json.dumps(rows, indent=1, sort_keys=True))
