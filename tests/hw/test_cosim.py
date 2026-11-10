"""The core in lock step with the interpreter, in the Verilator cosimulator."""
import json

import pytest

from conftest import BRAINFUNK, RESULTS, ROOT, assemble, build_cosim, corpus_program, cosim_json, run, COSIM

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


@pytest.mark.parametrize("seed,count,stall", batches("--clash", 1000))
def test_clash_images(tools, seed, count, stall):
    """Reads that meet a write to the other cell of their bank's row, and MULs whose cells share a bank, are
    rare in the corpus: these images are what test them."""
    result = cosim_json("--clash", count, "--seed", seed, *(["--stall"] if stall else []))
    assert result["retired"] > 0


@pytest.mark.parametrize("mode,seed", [("--random", 1), ("--random", 200000), ("--clash", 1), ("--clash", 200000)])
def test_predictor_is_only_a_guess(tools, mode, seed):
    """Guessing at random reaches what a working predictor seldom does: wrong targets, branches that are not."""
    result = cosim_json(mode, CHUNK, "--seed", seed, "--chaos")
    assert result["retired"] > 0


# What the first 2 million instructions of each program may cost, which takes seconds to find out. The
# simulation is cycle-exact, so a limit close to the real figure is not flaky, only a thing to revisit when
# the design changes. Some are close because of what they are there to catch: hanoi.b's, next-fetch false
# hits left in the table (1.0532 as it is, about 1.056 with them); mandel.b's, S restarting when a write
# meets its row, though it uses nothing of what it reads (1.5055 as it is, with the tape's restarts, 1.510
# with S's too), unit MULs that stop reading both cells at once, and the direction table's counters read
# as they are written (1.590 with them).
CPI_LIMITS = {"hanoi.b": 1.055, "mandel.b": 1.508, "LostKng.b": 12.0, "fib.b": 1.30, "long.b": 1.365}
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


# F over 20000 cells in both directions, across hundreds of lines, after writes that it has to wait for.
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


# A MUL that writes its own source, then one with the same source, which must read the new value. The
# second image puts a line boundary between them, so that an instruction cache miss empties A in between.
# A factor of 2, so that both go through the multiplier.
MUL_SELF_WORDS = [0x20000005,   # [0] = 5
                  0x20000101,   # [1] = 1
                  0x30000002,   # [0] += [0] * 2: 15
                  0x30000102,   # [1] += [0] * 2: 31, not 11
                  0x80000101,   # output [1]
                  0x90000000]
# A factor of 1 or -1 with one cell as source and destination: the cells share a bank, being the same,
# so the source is read first, and the destination adds to what it reads then.
MUL_SELF_UNIT_WORDS = [0x20000005,   # [0] = 5
                       0x30000001,   # [0] += [0]: 10
                       0x20000107,   # [1] = 7
                       0x301001ff,   # [1] -= [1]: 0
                       0x30000001,   # [0] += [0]: 20
                       0x80000001, 0x80000101,
                       0x90000000]
MUL_SELF_IMAGES = {
    "adjacent": "".join(f"{word:08x}\n" for word in MUL_SELF_WORDS),
    "unit_factor": "".join(f"{word:08x}\n" for word in MUL_SELF_UNIT_WORDS),
    "across_a_line": "".join(f"{word:08x}\n" for word in MUL_SELF_WORDS[:2] + [0x20000200] * 5 + MUL_SELF_WORDS[2:]),
}

# MULs in a row with one source and factors other than 1 and -1, each with its own factor in the multiplier.
MUL_CHAIN_WORDS = [0x20000003,  # [0] = 3
                   0x20000101,  # [1] = 1
                   0x20000201,  # [2] = 1
                   0x30000102,  # [1] += [0] * 2: 7
                   0x30000205,  # [2] += [0] * 5: 16
                   0x30000307,  # [3] += [0] * 7: 21
                   0x80000101, 0x80000201, 0x80000301,
                   0x90000000]
MUL_CHAIN_IMAGE = "".join(f"{word:08x}\n" for word in MUL_CHAIN_WORDS)


def test_muls_in_a_row_with_one_source(tools, tmp_path):
    image = tmp_path / "mul_chain.hex"
    image.write_text(MUL_CHAIN_IMAGE)
    assert cosim_json(image)["halted"]


@pytest.mark.parametrize("layout", MUL_SELF_IMAGES)
def test_mul_after_one_that_wrote_its_source(tools, tmp_path, layout):
    image = tmp_path / "mul.hex"
    image.write_text(MUL_SELF_IMAGES[layout])
    assert cosim_json(image)["halted"]


# At 192 MHz the board's SDRAM data comes a cycle later, CAPTURE 5. The line's last word then came after
# the next request had taken the read data over, and went to the wrong owner (when the data cache shared
# the SDRAM with the instruction cache; now the loader does).
SOC_RTL = ["brainfunk_core.sv", "brainfunk_predict.sv", "brainfunk_mem.sv", "brainfunk_cache.sv", "brainfunk_tape.sv", "tape_ram.sv", "sdram_ctrl.sv", "sdram_memory.sv", "brainfunk_soc.sv", "brainfunk_soc_sdram.sv"]


@pytest.mark.parametrize("mode,count", [("--random", 1000), ("--clash", 500)])
def test_sdram_data_a_cycle_late(tools, tmp_path, mode, count):
    cosim = build_cosim(tmp_path, [ROOT / "rtl" / name for name in SOC_RTL], "-GSDRAM_CAPTURE=5", "-DSDRAM_PIN_DELAY=1")
    for stall in ([], ["--stall"]):
        result = run(cosim, mode, count, "--seed", 300000, *stall, check=False)
        assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]


# A fetch that misses, abandoned by a redirect, still fills its line, and nothing is fetched meanwhile: the
# line it replaces keeps its tag until the fill ends, and what fetch read there would be half the new line.
# Word 0 and word 4096 share an index: the JN at 4096 is backward, so it is guessed taken, and fetch goes to
# word 1, which misses and replaces 4096's line; the JN is not taken, and the loop at 4098 is in that line,
# which the fill is writing when the redirect comes.
def i_fill_image(loops=20):
    def field(value, bits):
        return value & ((1 << bits) - 1)

    def data(op, cell, value):
        return op << 28 | field(cell, 12) << 8 | value & 0xFF

    def branch(op, cell, target, pc):
        return op << 28 | field(cell, 8) << 20 | field(target - pc - 1, 20)

    words = [0x90000000] * 4107
    words[0] = data(2, 2, 40)                     # [2] = 40 rounds
    words[1] = branch(6, 1, 4096, 1)              # JE [1] to 4096: [1] stays 0
    words[4096] = branch(7, 1, 1, 4096)           # JN [1] back to 1: never taken
    words[4097] = data(2, 4, loops)
    words[4098] = data(1, 4, 0xFF)
    words[4099] = branch(7, 4, 4098, 4099)        # [4] down to 0, in 4096's line
    for pc in range(4100, 4104):
        words[pc] = data(1, 3, 1)
    words[4104] = data(1, 2, 0xFF)
    words[4105] = branch(7, 2, 1, 4105)
    return "".join(f"{word:08x}\n" for word in words)


I_FILL_IMAGE = i_fill_image()


@pytest.mark.parametrize("chaos", [False, True])
def test_fill_abandoned_by_a_redirect(tools, tmp_path, chaos):
    image = tmp_path / "i_fill.hex"
    image.write_text(I_FILL_IMAGE)
    assert cosim_json(*(["--chaos"] if chaos else []), image)["halted"]


def test_long_scan(tools, tmp_path):
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


# How far the default machine of `brainfunk -m analyze -a issue`, memories included, may be from the core
# over the same first instructions. At the last count it was 0.0% off on long.b, 0.1% short on hanoi.b,
# 1.8% on fib.b and 1.9% on mandel.b, whose restarts of reads that meet a write the model leaves out, and
# 2.8% over on LostKng.b, which waits on the instruction cache. A design change that the model does not
# follow shows up here.
MODEL_TOLERANCE = {"hanoi.b": 0.005, "long.b": 0.005, "fib.b": 0.025, "mandel.b": 0.025, "LostKng.b": 0.035}


@pytest.mark.parametrize("name", MODEL_TOLERANCE)
def test_issue_model_follows_the_core(tools, tmp_path, name):
    image = assemble(corpus_program(name), 2, tmp_path / f"{name}.hex")
    core = cosim_json("--limit", CPI_PROGRAM_LENGTH, image)["cycles"]
    report = tmp_path / "report.json"
    run(BRAINFUNK, "-m", "analyze", "-a", "issue", "--limit", CPI_PROGRAM_LENGTH, "-b", image, "-o", report)
    model = json.loads(report.read_text())["analyses"]["issue"]["machines"][0]["cycles"]
    assert abs(model - core) <= MODEL_TOLERANCE[name] * core, f"model {model}, core {core}"
