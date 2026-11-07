// YADAN Board (Anlogic EG4S20NG88): runs the program image in imem over the USB UART.
//
// 115200 8N1. Input is blocking: there is no end of input on a UART, so the core waits.
// K_51 resets the core; the tape keeps its contents. LED_23 is on while running,
// off when halted, and blinks on a trap.
module yadan_top #(
    parameter int    CLOCK_HZ        = 24_000_000,
    parameter int    BAUD            = 115_200,
    parameter int    IMEM_ADDR_WIDTH = 13,    // 8192 instructions
    parameter        IMEM_INIT       = "program.hex"
) (
    input  logic clk,
    input  logic reset_button_n,
    input  logic uart_rx_pin,
    output logic uart_tx_pin,
    output logic led
);
    localparam int CYCLES_PER_BIT = CLOCK_HZ / BAUD;

    // Power-on reset, and a synchronised button.
    logic [3:0] por = 4'd0;
    logic [1:0] button_sync = 2'b00;
    logic rst;
    assign rst = !por[3];

    always_ff @(posedge clk) begin
        button_sync <= {button_sync[0], reset_button_n};
        if (!button_sync[1]) por <= 4'd0;
        else if (!por[3])    por <= por + 4'd1;
    end

    logic       in_valid, in_ready, out_valid, out_ready, halted, trapped;
    logic [7:0] in_data, out_data;

    brainfunk_soc #(
        .IMEM_ADDR_WIDTH(IMEM_ADDR_WIDTH),
        .DMEM_ADDR_WIDTH(16),
        .IMEM_INIT(IMEM_INIT)
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

    logic [22:0] blink = '0;
    always_ff @(posedge clk) blink <= blink + 1'b1;

    assign led = trapped ? blink[22] : !halted;
endmodule
