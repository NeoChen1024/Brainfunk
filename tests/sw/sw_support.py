"""What the end-to-end tests of the programs share: where the build is, and running things.

A module of its own rather than a conftest.py, so that it cannot be confused with tests/hw's.
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BRAINFUNK_BUILD", ROOT / "build"))
BIN = BUILD / "bin"
BRAINFUNK = BIN / "brainfunk"

CC = os.environ.get("CC", "cc")
CLANG = os.environ.get("CLANG", "clang")
LLVM_AS = os.environ.get("LLVM_AS", "llvm-as")
LLVM_OPT = os.environ.get("LLVM_OPT", "opt")
HAVE_LLVM = all(shutil.which(tool) for tool in (CLANG, LLVM_AS, LLVM_OPT))
needs_llvm = pytest.mark.skipif(not HAVE_LLVM, reason="clang, llvm-as or opt not found")


def program(name):
    """A program in build/bin, or a failure that says how to get it."""
    path = BIN / name
    if not path.exists():
        pytest.fail(f"{path} not found: build first, cmake --build {BUILD}")
    return path


def run(*command, input=b"", check=True, cwd=ROOT, env_extra=None, **kwargs):
    """Runs a command on bytes, with BRAINFUNK naming the build's for scripts; a failure shows what it said."""
    env = {**os.environ, "BRAINFUNK": str(BRAINFUNK), **(env_extra or {})}
    result = subprocess.run([str(part) for part in command], input=input, capture_output=True, cwd=cwd, env=env,
                            **kwargs)
    if check and result.returncode != 0:
        pytest.fail(f"{' '.join(map(str, command))} exited {result.returncode}\n"
                    f"{result.stderr.decode(errors='replace')[-2000:]}")
    return result


def brainfunk(*args, **kwargs):
    return run(program("brainfunk"), *args, **kwargs)
