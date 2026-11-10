"""The board top as bfconsole sees it: framing, flow control, the end of input, the program's end."""
import pytest

from conftest import BRAINFUNK, run

SOURCES = ["tb/console_tb.sv", "tb/spi_flash_model.sv", "rtl/board/yadan_top.sv", "rtl/board/yadan_pll.sv", "rtl/uart.sv",
           "rtl/loader.sv", "rtl/flash.sv", "rtl/brainfunk_system.sv", "rtl/brainfunk_soc.sv", "rtl/sdram_memory.sv",
           "rtl/brainfunk_cache.sv", "rtl/brainfunk_tape.sv", "rtl/tape_ram.sv", "rtl/sdram_ctrl.sv", "tb/sdram_model.sv", "rtl/brainfunk_mem.sv", "rtl/brainfunk_core.sv",
           "rtl/brainfunk_predict.sv"]

# At -O0, so that the delay loop is a loop: see tb/console_tb.sv.
PROGRAM = "-.>" + "+" * 19 + ".>,[>--[--]<.,]+++++,."
TRAP = "-."
ILLEGAL = "e0000000"

# The image's end-of-input field for each brainfunk -e, and for none.
EOF_FIELDS = {None: 0, "0": 1, "255": 2, "keep": 3}


def image_bytes(tmp_path, name, code, eof=None, last_word=None):
    """An image file's bytes as a file for $readmemh, and how many there are."""
    hexfile = tmp_path / f"{name}.hex"
    run(BRAINFUNK, "-O0", "-m", "hex", "-s", code, "-o", hexfile)
    if last_word:
        words = hexfile.read_text().split()
        words[-1] = last_word
        hexfile.write_text("\n".join(words) + "\n")
    packed = tmp_path / f"{name}.bfi"
    run(BRAINFUNK, "-m", "packed", "-b", hexfile, "-o", packed, *(["-e", eof] if eof else []))
    data = packed.read_bytes()
    bytesfile = tmp_path / f"{name}.bytes"
    bytesfile.write_text("".join(f"{byte:02x}\n" for byte in data))
    return bytesfile, len(data)


def build(tmp_path, sources=SOURCES):
    obj = tmp_path / "obj"
    run("verilator", "--binary", "--timing", "-j", "2", "--top-module", "console_tb", "--Mdir", obj, *sources,
        "-o", "console_tb")
    return obj / "console_tb"


def console_testbench(tmp_path, eofs=tuple(EOF_FIELDS), sources=SOURCES):
    binary = build(tmp_path, sources)
    trap, trap_count = image_bytes(tmp_path, "trap", TRAP, last_word=ILLEGAL)
    for eof in eofs:
        name = f"program-{eof}"
        image, count = image_bytes(tmp_path, name, PROGRAM, eof)
        result = run(binary, f"+image={image}", f"+image_bytes={count}", f"+trap={trap}", f"+trap_bytes={trap_count}",
                     f"+eof={EOF_FIELDS[eof]}", check=False, timeout=900)
        assert "console_tb passed" in result.stdout, f"-e {eof}:\n" + result.stdout[-2000:]


def test_console_testbench(tools, tmp_path):
    console_testbench(tmp_path)
