"""The command-line programs: what they accept, refuse, and report."""
import json
import sys

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


def test_analyze_counts_to_stderr():
    report = json.loads(brainfunk("-O0", "-m", "analyze", "-s", "++[->+<]>.").stderr)
    stat = report["analyses"]["stat"]
    assert stat["dynamic"]["MUL"] == 1
    assert stat["memory_reads"] == 4


def test_stat_compare(tmp_path):
    source = tmp_path / "hello.bf"
    source.write_text("++++++++[>++++++++<-]>+.+.+.")
    table = run(sys.executable, ROOT / "tools" / "stat_compare.py", source).stdout.decode().splitlines()
    assert table[0].startswith("program\tlevel")
    assert [row.split("\t")[:2] for row in table[1:]] == [["hello.bf", str(level)] for level in range(4)]


def test_illegal_word_stops_the_run(tmp_path):
    image = tmp_path / "trap.hex"
    image.write_text("10000005\ne0000000\n90000000\n")
    assert brainfunk("-b", image, check=False).returncode != 0


@pytest.mark.parametrize("level", ["9", "x", "12", ""])
def test_invalid_optimization_level_is_refused(level):
    assert brainfunk(f"-O{level}", "-s", "+", check=False).returncode != 0


def test_visualbrainfunk_refuses_an_invalid_delay():
    assert run(program("visualbrainfunk"), "-s", "+", "-t", "invalid", check=False).returncode != 0


def test_trace_runs_again(tmp_path):
    """A trace keeps the program and its input, and a run from it must go the same way."""
    source = tmp_path / "echo.bf"
    source.write_text(",[.,]")
    trace = tmp_path / "echo.bft"
    assert brainfunk("-m", "trace", "-f", source, "-o", trace, input=b"traced").stdout == b"traced"
    replayed = brainfunk("-m", "analyze", "-t", trace)
    assert replayed.stdout == b"traced"
    assert json.loads(replayed.stderr)["analyses"]["stat"]["dynamic_total"] > 0
    partial = tmp_path / "part.bft"
    brainfunk("-m", "trace", "--skip", "3", "--limit", "4", "-f", source, "-o", partial, input=b"traced")
    brainfunk("-t", partial)


@pytest.mark.parametrize("args", [["-m", "trace", "-s", "+"],               # no file to write to
                                  ["-m", "run", "--skip", "1", "-s", "+"],  # a range is for stat and trace
                                  ["-m", "analyze", "--limit", "x", "-s", "+"],
                                  ["-m", "stat", "-s", "+"]],
                         ids=["trace-without-o", "range-with-run", "bad-count", "stat-is-analyze-now"])
def test_trace_options_are_checked(args):
    assert brainfunk(*args, check=False).returncode != 0


def test_analyze_reports_json(tmp_path):
    source = tmp_path / "count.bf"
    source.write_text("++++++++[>+++++<-]>[<+>-]<.")
    report = tmp_path / "report.json"
    config = tmp_path / "config.json"
    config.write_text(json.dumps({"issue": {"machines": [{"name": "one"}, {"name": "two", "width": 2}]},
                                  "ngram": {"n": [2]}}))
    brainfunk("-m", "analyze", "-a", "stat,profile,ngram,deps,values,issue", "--config", config, "-f", source,
              "-o", report)
    result = json.loads(report.read_text())
    assert result["run"]["halted"]
    assert result["program"]["source"] == str(source)
    assert [m["name"] for m in result["analyses"]["issue"]["machines"]] == ["one", "two"]
    assert list(result["analyses"]["ngram"]) == ["2", "relations"]

    trace = tmp_path / "count.bft"
    brainfunk("-m", "trace", "-f", source, "-o", trace)
    again = tmp_path / "again.json"
    brainfunk("-m", "analyze", "-a", "stat", "-t", trace, "-o", again)
    assert json.loads(again.read_text())["analyses"]["stat"] == result["analyses"]["stat"]


def test_analyze_refuses_an_unknown_analysis():
    assert brainfunk("-m", "analyze", "-a", "nonsense", "-s", "+", check=False).returncode != 0


def test_trace_keeps_the_eof_mode(tmp_path):
    """Run again from a trace, input at its end stores what the traced run stored."""
    source = tmp_path / "keep.bf"
    source.write_text(",[.,]")   # with keep, the last byte repeats until the limit
    trace = tmp_path / "keep.bft"
    traced = brainfunk("-e", "keep", "-m", "trace", "--limit", "20", "-f", source, "-o", trace, input=b"ab")
    assert traced.stdout.startswith(b"abb")
    assert brainfunk("-m", "analyze", "-t", trace).stdout == traced.stdout
