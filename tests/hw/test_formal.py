"""SymbiYosys tasks for the core. Their outputs are separate, so they can run side by side."""
import shutil

import pytest

from conftest import ROOT, run


@pytest.mark.parametrize("task", ["cover", "multiplier", "latency", "bmc", "bmc_miss"])
def test_formal_task(task):
    if shutil.which("sby") is None:
        pytest.skip("sby not installed")
    prefix = ROOT / "build" / "formal" / task
    prefix.parent.mkdir(parents=True, exist_ok=True)
    result = run("sby", "-f", "--prefix", prefix, "core.sby", task, cwd=ROOT / "formal", check=False)
    if result.returncode != 0:
        pytest.fail(f"{task} failed, see {prefix}\n" + "\n".join(line for line in result.stdout.splitlines() if "Assert failed" in line or "Unreached" in line))
