"""The real board, on its USB serial port and JTAG cable. Needs the yadan target built and TD_HOME.

Nothing here touches the user flash except the tests marked flash, because it wears out. The board is
one resource, so these all run on one worker in the order written.

Not automatic: pressing K_51 to reload from the flash. Hold it for a moment after --flash has run.
"""
import glob
import os
import time

import pytest

from conftest import BFLOAD, BRAINFUNK, BUILD, ROOT, assemble, assemble_text, corpus_program, run

serial = pytest.importorskip("serial")

pytestmark = [pytest.mark.board, pytest.mark.xdist_group("board")]


def board_port():
    """$BOARD_PORT, or else the one serial port from the board's CH340, USB vendor 1a86, as bfload finds it."""
    if os.environ.get("BOARD_PORT"):
        return os.environ["BOARD_PORT"]
    found = glob.glob("/dev/serial/by-id/*1a86*")
    return found[0] if len(found) == 1 else None


PORT = board_port()
PROGRAM_BAUD = 115200
LOAD_BAUD = 2000000


@pytest.fixture(scope="module")
def board(tools):
    if not os.environ.get("TD_HOME"):
        pytest.skip("TD_HOME is not set")
    if not (BUILD / "yadan" / "yadan.bit").exists():
        pytest.skip(f"no {BUILD}/yadan/yadan.bit: build the yadan target")
    if PORT is None or not os.path.exists(PORT):
        pytest.skip("no board serial port: set BOARD_PORT")
    run("sh", "synth/load-yadan.sh", env={**os.environ, "YADAN_WORK": str(BUILD / "yadan")})
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


def build_mhz():
    """The clock the bitstream was built for, from the defines synth/build-yadan.sh put first."""
    for line in (BUILD / "yadan" / "yadan_all.sv").read_text().splitlines()[:4]:
        if line.startswith("`define YADAN_MHZ "):
            return int(line.split()[2])
    return 168


def expected_output(name, stdin=None, limit=None):
    command = [BRAINFUNK, "-O2", "-f", corpus_program(name)]
    return run(*command, input=stdin or "", check=False, timeout=limit, errors="surrogateescape").stdout.encode(errors="surrogateescape")


def load_ram(image):
    run(BFLOAD, "-p", PORT, image, timeout=120)


def test_flash_identifies_itself(board):
    assert run(BFLOAD, "-m", "id", "-p", PORT, timeout=20).stdout.strip() == "c84015"


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
    # 12.9 s at 168 MHz, and 11.3 s at 192 with its extra cycle on SDRAM reads.
    scale = 168 / build_mhz()
    assert 12 * scale < time.monotonic() - start < 18 * scale


def test_lost_kingdom_plays(board, tmp_path):
    load_ram(assemble(corpus_program("LostKng.b"), 2, tmp_path / "program.hex"))
    commands = "N\nlook\nget matches\ninventory\n" + "n\ne\ns\nw\nu\nd\nlook\nscore\nhelp\n?\n" * 10
    # There is no flow control, so input must not outrun the FIFO. The interpreter's run never
    # ends at the end of input, so compare what the board printed with the start of it.
    process = run("sh", "-c", f"timeout 10 {BRAINFUNK} -O2 -f {corpus_program('LostKng.b')} | head -c 200000",
                  input=commands, check=False, errors="surrogateescape")
    reference = process.stdout.encode(errors="surrogateescape")
    got = read_output(len(reference), 30, send=commands.encode(), pace=8)
    assert len(got) > 1000
    assert got == reference[:len(got)]


def test_corrupted_image_is_refused(board, tmp_path):
    packed = tmp_path / "program.bfi"
    run(BFLOAD, "-m", "pack", "-o", packed, assemble_text("+.", 0, tmp_path / "program.hex"))
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
    run(BFLOAD, "-m", "flash", "-p", PORT, assemble(corpus_program("hanoi.b"), 2, tmp_path / "program.hex"), timeout=120)
    expected = expected_output("hanoi.b")
    assert read_output(len(expected), 30) == expected


@pytest.mark.flash
def test_power_up_boots_from_flash(board):
    # Reloading the bitstream starts from power-up as far as the design can tell.
    run("sh", "synth/load-yadan.sh", env={**os.environ, "YADAN_WORK": str(BUILD / "yadan")})
    expected = expected_output("hanoi.b")
    assert read_output(len(expected), 30) == expected


@pytest.mark.flash
def test_port_reopen_keeps_code_ram_over_flash(board, tmp_path):
    expected = expected_output("hello.b")
    load_ram(assemble(corpus_program("hello.b"), 2, tmp_path / "program.hex"))
    assert read_output(len(expected), 5) == expected
    assert read_output(len(expected), 5) == expected
