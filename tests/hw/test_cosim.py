"""The core in lock step with the interpreter, in the Verilator cosimulator."""
import json

import pytest

from conftest import BRAINFUNK, RESULTS, assemble, corpus_program, cosim_json, run, COSIM

CHUNK = 100


def seed_ranges(total, base):
    return [(base + start, min(CHUNK, total - start)) for start in range(0, total, CHUNK)]


def batches(mode, total):
    # With and without I/O stalls; each range of seeds is a test of its own, to rerun by name.
    params = []
    for stall, base in ((False, 1), (True, 100000)):
        for seed, count in seed_ranges(total, base):
            tag = "stall-" if stall else ""
            params.append(pytest.param(seed, count, stall, id=f"{tag}seed{seed}"))
    return params


@pytest.mark.parametrize("seed,count,stall", batches("--random", 3000))
def test_random_programs(tools, seed, count, stall):
    result = cosim_json("--random", count, "--seed", seed, *(["--stall"] if stall else []))
    assert result["retired"] > 0


@pytest.mark.parametrize("seed,count,stall", batches("--evict", 1000))
def test_eviction_images(tools, seed, count, stall):
    """No program in the corpus evicts a dirty line, so these are all that test write-back."""
    result = cosim_json("--evict", count, "--seed", seed, *(["--stall"] if stall else []))
    assert result["retired"] > 0


@pytest.mark.parametrize("mode,seed", [("--random", 1), ("--random", 200000), ("--evict", 1), ("--evict", 200000)])
def test_predictor_is_only_a_guess(tools, mode, seed):
    """Guessing at random reaches what a working predictor seldom does: wrong targets, branches that are not."""
    result = cosim_json(mode, CHUNK, "--seed", seed, "--chaos")
    assert result["retired"] > 0


# What the first 2 million instructions of each program may cost, which takes seconds to find out. The
# simulation is cycle-exact, so a limit close to the real figure is not flaky, only a thing to revisit when
# the design changes. Some are close because of what they are there to catch: mandel.b's, adds to a pending
# cell of the early store forgotten (1.2477 as it is, 1.2495 without); hanoi.b's, the zero bits gone (0.7433
# against 0.7457); long.b's, fetch stopping at three queued words even before a pair (1.0660 against 1.0717)
# and pairs only within a fetched block (1.0731).
CPI_LIMITS = {"hanoi.b": 0.745, "mandel.b": 1.249, "LostKng.b": 11.0, "fib.b": 1.05, "long.b": 1.068}
CPI_PROGRAM_LENGTH = 2000000


def program_cpi(cosim, name, directory):
    image = assemble(corpus_program(name), 2, directory / f"{name}.hex")
    source = directory / "input"
    source.write_text("N\nlook\nget matches\n" * 100)
    result = cosim_json("--limit", CPI_PROGRAM_LENGTH, image, source, cosim=cosim)
    return result["cycles"] / result["retired"]


@pytest.mark.parametrize("name", CPI_LIMITS)
def test_prediction_keeps_cpi_down(tools, tmp_path, name):
    assert program_cpi(COSIM, name, tmp_path) < CPI_LIMITS[name]


# F over more tape than the cache holds, in both directions. A miss has to restart it from the line that
# missed: from where it began, it would evict that line getting back there, and never finish.
LONG_SCAN_CELLS = 20000
LONG_SCAN_IMAGE = "".join(f"{word:08x}\n" for word in [
    0x50000000 | LONG_SCAN_CELLS,               # the far end,
    0x20000001,                                 # marked
    0x50000000 | (-LONG_SCAN_CELLS & 0xfffff),  # and back
    0x20000001,                                 # every cell non-zero up to the mark
    0x50000001,
    0x60000000 | (-3 & 0xfffff),
    0x40000000 | 0xfffff,                       # F backward, to the cell before the first
    0x50000001,
    0x40000001,                                 # F forward, to the cell after the mark
    0x90000000,
])


# A MUL that writes its own source, then one with the same source, which must read it again. The second
# image puts a line boundary between them, so that an instruction cache miss empties A in between. A factor
# of 2, because MULs with factors of 1 and -1 read both cells at once and share nothing.
MUL_SELF_WORDS = [0x20000005,   # [0] = 5
                  0x20000101,   # [1] = 1
                  0x30000002,   # [0] += [0] * 2: 15
                  0x30000102,   # [1] += [0] * 2: 31, not 11
                  0x80000101,   # output [1]
                  0x90000000]
MUL_SELF_IMAGES = {
    "adjacent": "".join(f"{word:08x}\n" for word in MUL_SELF_WORDS),
    "across_a_line": "".join(f"{word:08x}\n" for word in MUL_SELF_WORDS[:2] + [0x20000200] * 5 + MUL_SELF_WORDS[2:]),
}

# MULs in a row with one source and factors other than 1 and -1, so that each takes it from the one before
# and needs its own factor in the multiplier.
MUL_CHAIN_WORDS = [0x20000003,  # [0] = 3
                   0x20000101,  # [1] = 1
                   0x20000201,  # [2] = 1
                   0x30000102,  # [1] += [0] * 2: 7
                   0x30000205,  # [2] += [0] * 5: 16
                   0x30000307,  # [3] += [0] * 7: 21
                   0x80000101, 0x80000201, 0x80000301,
                   0x90000000]
MUL_CHAIN_IMAGE = "".join(f"{word:08x}\n" for word in MUL_CHAIN_WORDS)


def test_muls_sharing_a_source(tools, tmp_path):
    image = tmp_path / "mul_chain.hex"
    image.write_text(MUL_CHAIN_IMAGE)
    assert cosim_json(image)["halted"]


@pytest.mark.parametrize("layout", MUL_SELF_IMAGES)
def test_mul_after_one_that_wrote_its_source(tools, tmp_path, layout):
    image = tmp_path / "mul.hex"
    image.write_text(MUL_SELF_IMAGES[layout])
    assert cosim_json(image)["halted"]


def test_long_scan_crosses_the_cache(tools, tmp_path):
    image = tmp_path / "long_scan.hex"
    image.write_text(LONG_SCAN_IMAGE)
    assert cosim_json(image)["halted"]


@pytest.mark.parametrize("word", ["00000000", "00000001", "10100000", "20100000", "50100000", "80000002",
                                  "800000ff", "90000001", "e0000000", "f0000000"])
def test_illegal_word_traps(tools, tmp_path, word):
    image = tmp_path / "trap.hex"
    image.write_text(f"10000005\n{word}\n90000000\n")
    assert cosim_json(image)["trapped"]


@pytest.mark.parametrize("word", ["a0000100", "b0000000", "c0000000", "d0100000"])
def test_fused_instruction_is_not_run(tools, tmp_path, word):
    """The RTL does not implement these yet, and must say so rather than run them wrongly."""
    image = tmp_path / "fused.hex"
    image.write_text(f"{word}\n90000000\n")
    result = run(COSIM, image, check=False)
    assert result.returncode != 0 and "RTL trapped, ISS did not" in result.stderr


SHORT = ["hello.b", "squares.b", "bench.b", "bottles.b", "sierpinski.bf", "hanoi.b"]
LONG = ["long.b", "mandel.b"]


def corpus_params():
    for level in (0, 1, 2):
        for name in SHORT:
            yield pytest.param(name, level, id=f"{name}-O{level}")
        for name in LONG:
            yield pytest.param(name, level, id=f"{name}-O{level}", marks=pytest.mark.full)


@pytest.mark.parametrize("name,level", list(corpus_params()))
def test_corpus_program(tools, tmp_path, name, level):
    image = assemble(corpus_program(name), level, tmp_path / "program.hex")
    result = cosim_json(image)
    assert result["halted"]
    (RESULTS / f"{name}-O{level}.json").write_text(json.dumps(result))


def test_stalls_on_a_real_program(tools, tmp_path):
    image = assemble(corpus_program("hello.b"), 2, tmp_path / "program.hex")
    assert cosim_json("--stall", image)["halted"]


# How far the default machine of `brainfunk -m analyze -a issue` may be from the core, over the same first
# instructions. The model has no caches, so LostKng.b, which waits on the instruction cache, is not here,
# and mandel.b's branches cost the core more than the model sees; nor does it have the core's fetch, two
# words a block into a queue of four. At the last count, with dual issue, the model was 0.9% short on
# long.b, 3.1% on hanoi.b, 3.8% on mandel.b and 5.0% on fib.b. A design change that the model does not
# follow shows up here.
MODEL_TOLERANCE = {"hanoi.b": 0.04, "long.b": 0.015, "fib.b": 0.06, "mandel.b": 0.05}


@pytest.mark.parametrize("name", MODEL_TOLERANCE)
def test_issue_model_follows_the_core(tools, tmp_path, name):
    image = assemble(corpus_program(name), 2, tmp_path / f"{name}.hex")
    core = cosim_json("--limit", CPI_PROGRAM_LENGTH, image)["cycles"]
    report = tmp_path / "report.json"
    run(BRAINFUNK, "-m", "analyze", "-a", "issue", "--limit", CPI_PROGRAM_LENGTH, "-b", image, "-o", report)
    model = json.loads(report.read_text())["analyses"]["issue"]["machines"][0]["cycles"]
    assert abs(model - core) <= MODEL_TOLERANCE[name] * core, f"model {model}, core {core}"
