"""The command-line programs: what they accept, refuse, and report."""
import pytest

from sw_support import ROOT, brainfunk, program, run


@pytest.mark.parametrize("code", ["[-" + ">+" * 200000 + "<" * 200000 + "]",
                                  "[" + ">+" * 200000 + "<" * 200000 + "-]"], ids=["counter-first", "counter-last"])
def test_long_multiply_loop_does_not_exhaust_the_stack(tmp_path, code):
    source = tmp_path / "pairs.bf"
    source.write_text(code)
    brainfunk("-m", "bit", "-f", source)


def test_multiply_beyond_the_offset_field_still_encodes(tmp_path):
    source = tmp_path / "far-mul.bf"
    source.write_text("+[-" + ">" * 3000 + "+" + "<" * 3000 + "]")
    brainfunk("-m", "hex", "-f", source)


def test_stat_counts_to_stderr():
    report = brainfunk("-O0", "-m", "stat", "-s", "++[->+<]>.").stderr.decode().splitlines()
    assert "MUL\t1\t1" in report
    assert "memory_reads\t4" in report


def test_stat_to_a_file(tmp_path):
    out = tmp_path / "stat.out"
    brainfunk("-m", "stat", "-s", "+", "-o", out)
    assert any(line.startswith("total\t") for line in out.read_text().splitlines())


def test_stat_compare(tmp_path):
    source = tmp_path / "hello.bf"
    source.write_text("++++++++[>++++++++<-]>+.+.+.")
    assert "hello.bf" in run("sh", ROOT / "tests" / "stat-compare.sh", source).stdout.decode()


def test_illegal_word_stops_the_run(tmp_path):
    image = tmp_path / "trap.hex"
    image.write_text("10000005\ne0000000\n90000000\n")
    assert brainfunk("-b", image, check=False).returncode != 0


@pytest.mark.parametrize("level", ["9", "x", "12", ""])
def test_invalid_optimization_level_is_refused(level):
    assert brainfunk(f"-O{level}", "-s", "+", check=False).returncode != 0


def test_visualbrainfunk_refuses_an_invalid_delay():
    assert run(program("visualbrainfunk"), "-s", "+", "-t", "invalid", check=False).returncode != 0
