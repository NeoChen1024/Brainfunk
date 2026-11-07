// 8N1 UART, one byte at a time, no FIFO.
module uart_tx #(
    parameter int CYCLES_PER_BIT = 208
) (
    input  logic       clk,
    input  logic       rst,
    input  logic       valid,
    input  logic [7:0] data,
    output logic       ready,
    output logic       tx
);
    logic [$clog2(CYCLES_PER_BIT)-1:0] divider;
    logic [9:0] shift;   // stop, data, start; empty when zero
    assign ready = shift == 10'd0;

    // Idle from power-up: a low line would read as a start bit before the first reset.
    logic line = 1'b1;
    assign tx = line;

    always_ff @(posedge clk) begin
        if (rst) begin
            shift   <= 10'd0;
            divider <= '0;
            line    <= 1'b1;
        end else if (ready) begin
            if (valid) begin
                shift   <= {1'b1, data, 1'b0};
                divider <= '0;
            end
        end else if (divider == $bits(divider)'(CYCLES_PER_BIT - 1)) begin
            divider <= '0;
            shift   <= shift >> 1;
        end else begin
            divider <= divider + 1'b1;
            if (divider == '0) line <= shift[0];
        end
    end
endmodule

module uart_rx #(
    parameter int CYCLES_PER_BIT = 208
) (
    input  logic       clk,
    input  logic       rst,
    input  logic       rx,
    output logic       valid,   // holds until ready; a byte arriving meanwhile is lost
    output logic [7:0] data,
    input  logic       ready
);
    logic [1:0] sync;
    logic [$clog2(CYCLES_PER_BIT)-1:0] divider;
    logic [3:0] bits_left;
    logic [7:0] shift;
    logic busy;

    always_ff @(posedge clk) begin
        if (rst) begin
            sync  <= 2'b11;
            busy  <= 1'b0;
            valid <= 1'b0;
        end else begin
            sync <= {sync[0], rx};
            if (valid && ready) valid <= 1'b0;

            if (!busy) begin
                if (!sync[1]) begin
                    // Sample mid-bit: the start bit, 8 data bits, then the stop bit.
                    busy      <= 1'b1;
                    divider   <= $bits(divider)'(CYCLES_PER_BIT / 2);
                    bits_left <= 4'd10;
                end
            end else if (divider == $bits(divider)'(CYCLES_PER_BIT - 1)) begin
                divider   <= '0;
                bits_left <= bits_left - 4'd1;
                if (bits_left == 4'd1) begin
                    busy <= 1'b0;   // stop bit
                    if (sync[1]) begin
                        valid <= 1'b1;
                        data  <= shift;
                    end
                end else begin
                    shift <= {sync[1], shift[7:1]};
                end
            end else begin
                divider <= divider + 1'b1;
            end
        end
    end
endmodule
