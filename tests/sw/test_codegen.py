"""Every optimization level, through every backend and every form of program image, must do what the
unoptimized interpreter does."""
import functools

import pytest

from sw_support import CC, CLANG, LLVM_AS, LLVM_OPT, ROOT, brainfunk, needs_llvm, run

EMPTY = b""

# name: (source, input), or (source, input, EOF mode) for what is stored at the end of input
PROGRAMS = {
    "hello": ("++++++++[>++++++++<-]>+.+.+.", EMPTY),
    "io": (",.,.", b"Z"),
    "byte-ff": (",.", b"\xff"),
    "empty": ("", EMPTY),
    "wrap": ("+" + ">" * 65536 + ".", EMPTY),
    "scan": ("+[>]+.", EMPTY),
    "loop": ("+++[.-]", EMPTY),
    "backward": ("+++[<++>-]<.", EMPTY),
    # Operands reached through a pending pointer offset.
    "pending-mul": ("+++>+++[->+<]>.<<.", EMPTY),
    "pending-io": (",>,>,<<.>.>.", b"xyz"),
    "pending-test": (">>>+++[.-]>+<<<<.", EMPTY),
    "pending-scan": ("+>+>+>>>+<<<<<<[>]>>+.", EMPTY),
    "moving-loop": (">>+++[->>+<]+<<.", EMPTY),
    "pending-negative": ("<<<+.>>>.<<<[-].", EMPTY),
    # Instruction fusion.
    "fused-mulc": (",[>+>++<<-]>.>.", b"Z"),
    "fused-djn": ("+++[.>+<-]>.", EMPTY),
    "fused-mjn": ("+>+>+>+>+<<<<[.>]<.", EMPTY),
    "fused-srun": (",>,>,>,<<<[-]>[-]>[-]>[-]<<<.>.>.>.", b"abcd"),
    "fused": (",[>+>++<<-]+++[.>+<-]+>+<[.>]<,>,>,<<[-]>[-]>[-]<<.", b"abcd"),
    # Offsets that overflow each field force an early flush. No real program reaches these.
    "overflow-addr": ("+" + ">" * 2047 + "+" + ">" + "+." + "<" * 2048 + ".", EMPTY),
    "overflow-test": (">" * 128 + "+++[.-]" + "<" * 128 + "+.", EMPTY),
    "fit-src": ("+" + ">" * 127 + "++[->+<]" + ">" * 2 + ".", EMPTY),
    "overflow-src": ("+" + ">" * 128 + "++[->+<]" + ">" + ".", EMPTY),
    "overflow-move": (">" * 100 + "+" + ">" * 524287 + ".", EMPTY),
    # The end of input, in each mode, and the loops that rely on it.
    "eof-0": (",.,.,.", b"A", "0"),
    "eof-255": (",.,.,.", b"A", "255"),
    "eof-keep": (",.,.,.", b"A", "keep"),
    "cat-0": (",[.,]", b"cat", "0"),
    "cat-255": (",+[-.,+]", b"cat", "255"),
    "eof-keep-pending": (">>,<<+>>,<<.>>.", b"Q", "keep"),
}

# Real programs from programs/ and the 3rdparty/ submodules, when checked out.
CORPUS = [
    "3rdparty/bfcorpus/cwfitzgerald/hello.b",
    "programs/brainfuck.org/squares.b",
    "programs/unattributed/bench.b",
    "3rdparty/bfcorpus/cwfitzgerald/bottles.b",
    "3rdparty/bfcorpus/nyyrikki/triangle.bf",
    "3rdparty/bfcorpus/copy/third_party/hanoi.bf",
    "3rdparty/rdebath-Brainfuck/testing/Long.b",
    "3rdparty/rdebath-Brainfuck/testing/Mandelbrot.b",
]
LEVELS = [0, 1, 2, 3]
CASES = [pytest.param(name, level, id=f"{name}-O{level}") for name in [*PROGRAMS, *CORPUS] for level in LEVELS]


@pytest.fixture(scope="module")
def sources(tmp_path_factory):
    directory = tmp_path_factory.mktemp("sources")
    paths = {}
    for name, (code, *_) in PROGRAMS.items():
        paths[name] = directory / f"{name}.bf"
        paths[name].write_text(code)
    for path in CORPUS:
        paths[path] = ROOT / path
    return paths


def source_of(sources, name):
    path = sources[name]
    if not path.exists():
        pytest.skip(f"{name} not found; check out the submodules")
    return path


def input_of(name):
    return PROGRAMS[name][1] if name in PROGRAMS else EMPTY


def eof_of(name):
    """The -e option for a program: its EOF mode, which the code generators build in."""
    return ["-e", PROGRAMS[name][2]] if name in PROGRAMS and len(PROGRAMS[name]) > 2 else []


@functools.cache
def reference(path, stdin, eof=()):
    """What the unoptimized interpreter prints."""
    return brainfunk("-O0", *eof, "-f", path, input=stdin).stdout


@pytest.mark.parametrize("name,level", CASES)
def test_interpreter(sources, name, level):
    path, stdin, eof = source_of(sources, name), input_of(name), eof_of(name)
    assert brainfunk(f"-O{level}", *eof, "-f", path, input=stdin).stdout == reference(path, stdin, tuple(eof))


@pytest.mark.parametrize("name,level", CASES)
def test_image_forms(sources, tmp_path, name, level):
    """Every form of the image converts to every other without loss, and runs the same."""
    path, stdin, eof = source_of(sources, name), input_of(name), eof_of(name)
    base = tmp_path / "program"
    listing = brainfunk(f"-O{level}", "-m", "bit", "-f", path).stdout
    hexwords = brainfunk(f"-O{level}", "-m", "hex", "-f", path).stdout
    (bit := base.with_suffix(".bit")).write_bytes(listing)
    (hexfile := base.with_suffix(".hex")).write_bytes(hexwords)
    assert brainfunk("-b", bit, "-m", "hex").stdout == hexwords
    assert brainfunk("-b", hexfile, "-m", "bit").stdout == listing
    binary, packed = base.with_suffix(".bin"), base.with_suffix(".bfi")
    brainfunk("-b", hexfile, "-m", "bin", "-o", binary)
    brainfunk("-b", binary, "-m", "packed", "-o", packed)
    assert brainfunk("-b", packed, "-m", "bin").stdout == binary.read_bytes()
    assert brainfunk("-b", packed, "-m", "bit").stdout == listing
    for image in (bit, hexfile, binary, packed):
        assert brainfunk(*eof, "-b", image, input=stdin).stdout == reference(path, stdin, tuple(eof)), image.suffix


@pytest.mark.parametrize("name,level", CASES)
def test_c_backend(sources, tmp_path, name, level):
    path, stdin, eof = source_of(sources, name), input_of(name), eof_of(name)
    c_file, executable = tmp_path / "program.c", tmp_path / "program"
    brainfunk(f"-O{level}", *eof, "-m", "bfc", "-f", path, "-o", c_file)
    run(CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic", c_file, "-o", executable)
    assert run(executable, input=stdin).stdout == reference(path, stdin, tuple(eof))


@needs_llvm
@pytest.mark.parametrize("name,level", CASES)
def test_llvm_backend(sources, tmp_path, name, level):
    path, stdin, eof = source_of(sources, name), input_of(name), eof_of(name)
    ir, executable = tmp_path / "program.ll", tmp_path / "program"
    brainfunk(f"-O{level}", *eof, "-m", "llvm", "-f", path, "-o", ir)
    assert not any(line.startswith(("target triple", "target datalayout")) for line in ir.read_text().splitlines()), \
        "portable LLVM IR contains target metadata"
    run(LLVM_AS, ir, "-o", tmp_path / "program.bc")
    run(LLVM_OPT, "-passes=verify", ir, "-disable-output")
    run(CLANG, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-override-module",
        ROOT / "tools" / "llvm-ir-wrapper.c", ir, "-o", executable)
    assert run(executable, input=stdin).stdout == reference(path, stdin, tuple(eof))


@pytest.mark.parametrize("mnemonic", ["MULC", "DJN", "MJN", "SRUN"])
def test_fused_instructions_reach_c(sources, mnemonic):
    assert mnemonic in brainfunk("-O3", "-m", "bfc", "-f", sources["fused"]).stdout.decode()


LLVM_TARGETS = ["i386-unknown-linux-gnu", "armv7-unknown-linux-gnueabihf", "aarch64-unknown-linux-gnu",
                "riscv64-unknown-linux-gnu", "powerpc64-unknown-linux-gnu", "powerpc64le-unknown-linux-gnu",
                "s390x-unknown-linux-gnu", "x86_64-pc-windows-msvc", "x86_64-apple-darwin", "arm64-apple-darwin",
                "wasm32-unknown-unknown", "wasm64-unknown-unknown"]


@needs_llvm
@pytest.mark.parametrize("target", LLVM_TARGETS)
def test_llvm_ir_is_portable(sources, tmp_path, target):
    ir = tmp_path / "fused.ll"
    brainfunk("-O3", "-m", "llvm", "-f", sources["fused"], "-o", ir)
    run(CLANG, "-target", target, "-O2", "-Wno-override-module", "-c", ir, "-o", tmp_path / "fused.o")


@needs_llvm
def test_compile_llvm_ir_script(sources, tmp_path):
    executable = tmp_path / "hello"
    run("sh", ROOT / "tools" / "compile-llvm-ir.sh", sources["hello"], executable, env_extra={"CFLAGS": "-O2 -pipe"})
    assert run(executable).stdout == reference(sources["hello"], EMPTY)


def test_compile_c_script(sources, tmp_path):
    executable = tmp_path / "hello"
    run("sh", ROOT / "tools" / "compile.sh", sources["hello"], executable)
    assert run(executable).stdout == reference(sources["hello"], EMPTY)


@pytest.mark.parametrize("mode,expected", [("0", b"A\0\0"), ("255", b"A\xff\xff"), ("keep", b"AAA")])
def test_eof_mode_stores(mode, expected):
    """The reference itself: what each mode stores once input runs out."""
    assert brainfunk("-O0", "-e", mode, "-s", ",.,.,.", input=b"A").stdout == expected


def test_eof_mode_is_checked():
    assert brainfunk("-e", "-1", "-s", ",", check=False).returncode != 0
