// YADAN Board (Anlogic EG4S20NG88): brainfunk_system with the embedded SDRAM, over the USB UART.
//
// K_51 is the system's button. Opening the serial port pulses RTS, which drives K_51's net, for
// about 137 us: a low that short restarts the program in Code RAM rather than reloading it, so what
// was loaded over the UART stays.
//
// LED_23 is on while running, off when halted, blinks on a trap, and flashes quickly while there is
// no program or one is being loaded.
`ifndef YADAN_MHZ
`define YADAN_MHZ 168
`endif
`ifndef YADAN_SDRAM_DEGREES
`define YADAN_SDRAM_DEGREES 240
`endif
`ifndef YADAN_SDRAM_CAPTURE
`define YADAN_SDRAM_CAPTURE 4
`endif
module yadan_top #(
    parameter int CLOCK_MHZ         = `YADAN_MHZ,     // what yadan_pll makes; synth/build-yadan.sh can set it
    parameter int SDRAM_DEGREES     = `YADAN_SDRAM_DEGREES, // measured for the clock: see doc/rtl.md
    parameter int SDRAM_CAPTURE     = `YADAN_SDRAM_CAPTURE, // cycles from READ to data, with that phase
    parameter int CLOCK_HZ          = CLOCK_MHZ * 1_000_000,
    parameter int BAUD              = 115_200,
    parameter int LOAD_BAUD         = 2_000_000,
    parameter int STARTUP_CYCLES    = CLOCK_HZ / 5,
    parameter int EOF_BREAK_CYCLES     = CLOCK_HZ / 1000, // Breaks: see brainfunk_system
    parameter int FRAME_BREAK_CYCLES   = CLOCK_HZ / 100,
    parameter int COMMAND_BREAK_CYCLES = CLOCK_HZ / 10,
    parameter int LONG_PRESS_CYCLES = CLOCK_HZ / 200, // 5 ms: RTS is 137 us, a finger far longer
    parameter int BOUNCE_CYCLES     = CLOCK_HZ / 25,  // ignore the button for 40 ms after a press
    parameter int SDRAM_BOOT_CYCLES = CLOCK_HZ / 10,
    parameter int IMEM_ADDR_WIDTH   = 20,
    parameter int DMEM_ADDR_WIDTH   = 16
) (
    input  logic clk_24mhz,
    input  logic reset_button_n,
    input  logic uart_rx_pin,
    output logic uart_tx_pin,
    output logic flash_cs_n,
    output logic flash_clk,
    output logic flash_mosi,
    input  logic flash_miso,
    output logic flash_wp_n,
    output logic flash_hold_n,
    output logic led
);
    logic clk, sdclk, locked;
    yadan_pll #(.CLOCK_MHZ(CLOCK_MHZ), .SDRAM_DEGREES(SDRAM_DEGREES)) pll (
        .clk_in(clk_24mhz), .clk_out(clk), .sdclk(sdclk), .locked(locked));

    logic por = 1'b1;
    always_ff @(posedge clk) por <= !locked;

    logic        running, halted, trapped;
    logic        mem_ready, mem_req_valid, mem_req_write, mem_req_ready, mem_rd_valid, mem_wr_fetch;
    logic [17:0] mem_req_line;
    logic [31:0] mem_rd_data, mem_wr_data;

    brainfunk_system #(
        .CLOCK_HZ(CLOCK_HZ), .BAUD(BAUD), .LOAD_BAUD(LOAD_BAUD), .STARTUP_CYCLES(STARTUP_CYCLES),
        .EOF_BREAK_CYCLES(EOF_BREAK_CYCLES), .FRAME_BREAK_CYCLES(FRAME_BREAK_CYCLES),
        .COMMAND_BREAK_CYCLES(COMMAND_BREAK_CYCLES), .LONG_PRESS_CYCLES(LONG_PRESS_CYCLES), .BOUNCE_CYCLES(BOUNCE_CYCLES),
        .IMEM_ADDR_WIDTH(IMEM_ADDR_WIDTH), .DMEM_ADDR_WIDTH(DMEM_ADDR_WIDTH)
    ) system (
        .clk(clk), .por(por), .button_n(reset_button_n), .uart_rx(uart_rx_pin), .uart_tx(uart_tx_pin),
        .flash_cs_n(flash_cs_n), .flash_clk(flash_clk), .flash_mosi(flash_mosi), .flash_miso(flash_miso),
        .running(running), .halted(halted), .trapped(trapped),
        .mem_ready(mem_ready), .mem_req_valid(mem_req_valid), .mem_req_write(mem_req_write), .mem_req_line(mem_req_line),
        .mem_req_ready(mem_req_ready), .mem_rd_valid(mem_rd_valid), .mem_rd_data(mem_rd_data),
        .mem_wr_fetch(mem_wr_fetch), .mem_wr_data(mem_wr_data)
    );

    sdram_memory #(.BOOT_CYCLES(SDRAM_BOOT_CYCLES), .CAPTURE(SDRAM_CAPTURE)) sdram (
        .clk(clk), .sdclk(sdclk), .por(por), .ready(mem_ready),
        .req_valid(mem_req_valid), .req_write(mem_req_write), .req_line(mem_req_line), .req_ready(mem_req_ready),
        .rd_valid(mem_rd_valid), .rd_data(mem_rd_data), .wr_fetch(mem_wr_fetch), .wr_data(mem_wr_data)
    );

    assign flash_wp_n   = 1'b1;
    assign flash_hold_n = 1'b1;

    logic [25:0] blink = '0;
    always_ff @(posedge clk) blink <= blink + 1'b1;
    always_comb led = running ? (trapped ? blink[25] : !halted) : blink[22];
endmodule
