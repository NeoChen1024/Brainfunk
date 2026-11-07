"""The board top over its pins, with SDRAM and flash models: load, refuse, restart, program, boot."""
from conftest import ROOT, assemble_text, run

SOURCES = ["tb/uart_tb.sv", "tb/spi_flash_model.sv", "rtl/board/yadan_top.sv", "rtl/board/yadan_pll.sv", "rtl/board/uart.sv",
           "rtl/board/loader.sv", "rtl/board/flash.sv", "rtl/brainfunk_soc.sv", "rtl/brainfunk_cache.sv", "rtl/sdram_ctrl.sv",
           "tb/sdram_model.sv", "rtl/brainfunk_mem.sv", "rtl/brainfunk_core.sv", "rtl/brainfunk_predict.sv"]

# The loop is skipped, and is only there so that the loader copies from lines it has written already.
PROGRAM = "[" + "+>-<" * 10 + "]+.,+.,+."


def uart_testbench(tmp_path, sources=SOURCES):
    image = assemble_text(PROGRAM, 0, tmp_path / "program.hex")
    packed = tmp_path / "program.bfi"
    run(ROOT / "bfload", "-m", "pack", "-o", packed, image)
    data = packed.read_bytes()
    (tmp_path / "program.bytes").write_text("".join(f"{byte:02x}\n" for byte in data))
    run("verilator", "--binary", "--timing", "-j", "2", "--top-module", "uart_tb", "--Mdir", tmp_path / "obj",
        f"-GIMAGE_BYTES={len(data)}", f'-GIMAGE_FILE="{tmp_path}/program.bytes"', *sources, "-o", "uart_tb")
    result = run(tmp_path / "obj" / "uart_tb", check=False, timeout=600)
    assert "uart_tb passed" in result.stdout, result.stdout[-2000:]


def test_uart_testbench(tools, tmp_path):
    uart_testbench(tmp_path)
