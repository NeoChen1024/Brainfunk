// YADAN Board (Anlogic EG4S20NG88): runs the program image in imem over the USB UART.
//
// 115200 8N1. Input is blocking: there is no end of input on a UART, so the core waits.
//
// K_51 restarts the program, and so does opening the serial port, because RTS drives the same
// net. A restart clears the tape first. The program starts about 200 ms after reset, so that
// a host opening the port, or the JTAG download finishing, does not lose its first output.
// LED_23 is on while running, off when halted, and blinks on a trap.
module yadan_top #(
    parameter int CLOCK_HZ        = 168_000_000,    // what yadan_pll makes
    parameter int BAUD            = 115_200,
    parameter int STARTUP_CYCLES  = CLOCK_HZ / 5,
    parameter int IMEM_ADDR_WIDTH = 13,             // 8192 instructions
    parameter int DMEM_ADDR_WIDTH = 16,
    parameter     IMEM_INIT       = "program.hex"
) (
    input  logic clk_24mhz,
    input  logic reset_button_n,
    input  logic uart_rx_pin,
    output logic uart_tx_pin,
    output logic led
);
    localparam int CYCLES_PER_BIT = CLOCK_HZ / BAUD;

    logic clk, locked;
    yadan_pll pll (.clk_in(clk_24mhz), .clk_out(clk), .locked(locked));

    // Reset while the PLL is not locked or the button is down, and for a while after.
    localparam int STARTUP_BITS = $clog2(STARTUP_CYCLES + 1);
    logic [STARTUP_BITS-1:0] startup = '0;
    logic [1:0] button_sync = 2'b00;
    logic rst = 1'b1;

    always_ff @(posedge clk) begin
        button_sync <= {button_sync[0], reset_button_n};
        if (!locked || !button_sync[1]) begin
            startup <= '0;
            rst     <= 1'b1;
        end else if (startup == STARTUP_BITS'(STARTUP_CYCLES)) begin
            rst     <= 1'b0;
        end else begin
            startup <= startup + 1'b1;
        end
    end

    logic       in_valid, in_ready, out_valid, out_ready, halted, trapped;
    logic [7:0] in_data, out_data;

    brainfunk_soc #(
        .IMEM_ADDR_WIDTH(IMEM_ADDR_WIDTH),
        .DMEM_ADDR_WIDTH(DMEM_ADDR_WIDTH),
        .IMEM_INIT(IMEM_INIT),
        .CLEAR_TAPE_ON_RESET(1'b1)
    ) soc (
        .clk(clk),
        .rst(rst),
        .io_in_valid(in_valid),
        .io_in_data(in_data),
        .io_in_ready(in_ready),
        .io_out_valid(out_valid),
        .io_out_data(out_data),
        .io_out_ready(out_ready),
        .halted(halted),
        .trapped(trapped)
    );

    uart_rx #(.CYCLES_PER_BIT(CYCLES_PER_BIT)) receiver (
        .clk(clk), .rst(rst), .rx(uart_rx_pin), .valid(in_valid), .data(in_data), .ready(in_ready)
    );

    uart_tx #(.CYCLES_PER_BIT(CYCLES_PER_BIT)) transmitter (
        .clk(clk), .rst(rst), .valid(out_valid), .data(out_data), .ready(out_ready), .tx(uart_tx_pin)
    );

    logic [25:0] blink = '0;
    always_ff @(posedge clk) blink <= blink + 1'b1;

    assign led = trapped ? blink[25] : !halted;
endmodule
