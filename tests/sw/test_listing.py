"""The assembler input: each listing line encodes to its word, the word lists as the same line, and
nothing that does not fit its fields is accepted."""
import pytest

from sw_support import brainfunk

ENCODINGS = [
    ("X", "00000000"),
    ("A 0, 5", "10000005"),
    ("A -1, 5", "100fff05"),
    ("S 2047, 255", "2007ffff"),
    ("MUL 3, 2, 3", "30200303"),
    ("MUL -2048, -128, 255", "380800ff"),
    ("F -1, 1", "4ff00001"),
    ("M -1", "500fffff"),
    ("JE 1, -2", "601ffffe"),
    ("JN 127, 524287", "77f7ffff"),
    ("IO 0, 1", "80000001"),
    ("H", "90000000"),
    ("MULC 3, 2, 3", "a0200303"),
    ("DJN 1, -2", "b01ffffe"),
    ("MJN 1, -2, -3", "c01feffd"),
    ("MJN -128, 127, 2047", "c807f7ff"),
    ("SRUN 5, 7, 3", "d0300507"),
    ("SRUN -1, 0, 255", "dfffff00"),
    ("X 0x9abcdef0", "9abcdef0"),
    ("X 0x00000005", "00000005"),
]

REJECTED = [
    "A 2048, 1", "A -2049, 1", "A 0, 256", "A 0, -1", "A 5", "H 1", "IO 0, 2",
    "MUL 1, 2", "MUL 2048, 0, 1", "MUL 1, 128, 1", "MUL 1, -129, 1", "MUL 1, 0, 256",
    "F 128, 1", "F 0, 524288", "M 524288", "M -524289",
    "JE 128, 0", "JN -129, 0", "JE 0, 524288", "NOPE 1",
    "MULC 1, 128, 1", "DJN 128, 0", "MJN 1, 128, 0", "MJN 1, 0, 2048", "MJN 128, 0, 0", "MJN 1, 2",
    "SRUN 0, 0, 0", "SRUN 0, 0, 256", "SRUN 0, 0, -1", "SRUN 2048, 0, 1", "SRUN 0, 256, 1", "SRUN 0, 0",
    "X 0x10000005", "X 0x0", "X 5",
]


@pytest.mark.parametrize("line,word", ENCODINGS, ids=[line for line, _ in ENCODINGS])
def test_line_encodes_to_its_word(line, word):
    assert brainfunk("-m", "hex", "-b", "-", input=f"0: {line}\n".encode()).stdout.decode().strip() == word


@pytest.mark.parametrize("line,word", ENCODINGS, ids=[line for line, _ in ENCODINGS])
def test_word_lists_as_its_line(line, word):
    listed = brainfunk("-m", "bit", "-b", "-", input=f"{word}\n".encode()).stdout.decode()
    assert listed.replace("\t", " ").removeprefix("0: ").strip() == line


@pytest.mark.parametrize("line", REJECTED)
def test_line_that_does_not_fit_is_rejected(line):
    assert brainfunk("-m", "hex", "-b", "-", input=f"0: {line}\n".encode(), check=False).returncode != 0
