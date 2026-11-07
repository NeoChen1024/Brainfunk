"""The real board, on its USB serial port and JTAG cable. Needs a built build/yadan/yadan.bit and TD_HOME.

Nothing here touches the user flash except the tests marked flash, because it wears out. The board is
one resource, so these all run on one worker in the order written.

Not automatic: pressing K_51 to reload from the flash. Hold it for a moment after --flash has run.
"""
import os
import time

import pytest

from conftest import ROOT, assemble, assemble_text, corpus_program, run

serial = pytest.importorskip("serial")

pytestmark = [pytest.mark.board, pytest.mark.xdist_group("board")]

PORT = os.environ.get("BOARD_PORT", "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0")
PROGRAM_BAUD = 115200
LOAD_BAUD = 2000000


@pytest.fixture(scope="module")
def board(tools):
    if not os.environ.get("TD_HOME"):
        pytest.skip("TD_HOME is not set")
    if not (ROOT / "build" / "yadan" / "yadan.bit").exists():
        pytest.skip("no build/yadan/yadan.bit: run make yadan")
    if not os.path.exists(PORT):
        pytest.skip(f"{PORT} not found")
    run("sh", "synth/load-yadan.sh")
    # minicom leaves this off, and without it closing the port does not drop RTS, so nothing restarts.
    run("stty", "-F", PORT, "hupcl")
    time.sleep(1)


def read_output(count, limit, send=b"", pace=None):
    """Opens the port, which restarts the program, and reads what it prints."""
    with serial.Serial(PORT, PROGRAM_BAUD, timeout=0.05, exclusive=True) as port:
        got = b""
        sent = 0
        start = time.monotonic()
        next_send = start + 0.5
        while time.monotonic() - start < limit and len(got) < count:
            got += port.read(4096)
            if sent < len(send) and time.monotonic() >= next_send:
                chunk = send[sent:sent + (pace or 64)]
                port.write(chunk)
                sent += len(chunk)
                next_send = time.monotonic() + (0.1 if pace else 0.002)
    return got


def expected_output(name, stdin=None, limit=None):
    command = [ROOT / "brainfunk", "-O2", "-f", corpus_program(name)]
    return run(*command, input=stdin or "", check=False, timeout=limit, errors="surrogateescape").stdout.encode(errors="surrogateescape")


def load_ram(image):
    run(ROOT / "bfload", image, timeout=120)


def test_flash_identifies_itself(board):
    assert run(ROOT / "bfload", "-i", timeout=20).stdout.strip() == "c84015"


def test_program_in_code_ram(board, tmp_path):
    source = corpus_program("hanoi.b")
    load_ram(assemble(source, 2, tmp_path / "program.hex"))
    expected = expected_output("hanoi.b")
    assert read_output(len(expected), 30) == expected


def test_reopening_the_port_restarts_it(board):
    expected = expected_output("hanoi.b")
    assert read_output(len(expected), 30) == expected


@pytest.mark.full
def test_mandel_takes_the_time_its_cycles_say(board, tmp_path):
    load_ram(assemble(corpus_program("mandel.b"), 2, tmp_path / "program.hex"))
    expected = expected_output("mandel.b")
    start = time.monotonic()
    assert read_output(len(expected), 60) == expected
    assert 12 < time.monotonic() - start < 18


def test_lost_kingdom_plays(board, tmp_path):
    load_ram(assemble(corpus_program("LostKng.b"), 2, tmp_path / "program.hex"))
    commands = "N\nlook\nget matches\ninventory\n" + "n\ne\ns\nw\nu\nd\nlook\nscore\nhelp\n?\n" * 10
    # There is no flow control, so input must not outrun the FIFO. The interpreter's run never
    # ends at the end of input, so compare what the board printed with the start of it.
    process = run("sh", "-c", f"timeout 10 {ROOT}/brainfunk -O2 -f {corpus_program('LostKng.b')} | head -c 200000",
                  input=commands, check=False, errors="surrogateescape")
    reference = process.stdout.encode(errors="surrogateescape")
    got = read_output(len(reference), 30, send=commands.encode(), pace=8)
    assert len(got) > 1000
    assert got == reference[:len(got)]


def test_corrupted_image_is_refused(board, tmp_path):
    packed = tmp_path / "program.bfi"
    run(ROOT / "bfload", "-o", packed, assemble_text("+.", 0, tmp_path / "program.hex"))
    data = bytearray(packed.read_bytes())
    data[-2] ^= 0xFF
    with serial.Serial(PORT, PROGRAM_BAUD, timeout=3, exclusive=True) as port:
        time.sleep(0.05)
        port.send_break(0.05)
        time.sleep(0.02)
        port.baudrate = LOAD_BAUD
        port.reset_input_buffer()
        port.write(b"R" + bytes(data))
        port.flush()
        answer = port.read(1)
        port.baudrate = PROGRAM_BAUD
    assert answer == b"E"


@pytest.mark.flash
def test_flash_programs_and_boots(board, tmp_path):
    run(ROOT / "bfload", "-r", assemble(corpus_program("hanoi.b"), 2, tmp_path / "program.hex"), timeout=120)
    expected = expected_output("hanoi.b")
    assert read_output(len(expected), 30) == expected


@pytest.mark.flash
def test_power_up_boots_from_flash(board):
    # Reloading the bitstream starts from power-up as far as the design can tell.
    run("sh", "synth/load-yadan.sh")
    expected = expected_output("hanoi.b")
    assert read_output(len(expected), 30) == expected


@pytest.mark.flash
def test_port_reopen_keeps_code_ram_over_flash(board, tmp_path):
    expected = expected_output("hello.b")
    load_ram(assemble(corpus_program("hello.b"), 2, tmp_path / "program.hex"))
    assert read_output(len(expected), 5) == expected
    assert read_output(len(expected), 5) == expected
