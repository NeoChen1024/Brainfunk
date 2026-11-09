// 8N1 UART, with the bit time as an input so the loader can run faster than the program's I/O.
module uart_tx #(
    parameter int DIVIDER_BITS = 14
) (
    input  logic                    clk,
    input  logic                    rst,
    input  logic [DIVIDER_BITS-1:0] cycles_per_bit,
    input  logic                    valid,
    input  logic [7:0]              data,
    output logic                    ready,
    output logic                    tx
);
    logic [DIVIDER_BITS-1:0] divider, bit_last;
    logic [9:0] shift;   // stop, data, start; empty when zero
    always_ff @(posedge clk) bit_last <= cycles_per_bit - 1'b1;
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
        end else if (divider == bit_last) begin
            divider <= '0;
            shift   <= shift >> 1;
        end else begin
            divider <= divider + 1'b1;
            if (divider == '0) line <= shift[0];
        end
    end
endmodule

module uart_rx #(
    parameter int DIVIDER_BITS = 14
) (
    input  logic                    clk,
    input  logic                    rst,
    input  logic [DIVIDER_BITS-1:0] cycles_per_bit,
    input  logic                    rx,        // synchronized by the caller
    output logic                    valid,     // one cycle, for each good byte
    output logic [7:0]              data
);
    logic [DIVIDER_BITS-1:0] divider, bit_last;
    always_ff @(posedge clk) bit_last <= cycles_per_bit - 1'b1;
    logic [3:0] bits_left;
    logic [7:0] shift;
    logic busy;

    always_ff @(posedge clk) begin
        valid <= 1'b0;
        if (rst) begin
            busy <= 1'b0;
        end else if (!busy) begin
            if (!rx) begin
                // Sample mid-bit: the start bit, 8 data bits, then the stop bit.
                busy      <= 1'b1;
                divider   <= cycles_per_bit >> 1;
                bits_left <= 4'd10;
            end
        end else if (divider == bit_last) begin
            divider   <= '0;
            bits_left <= bits_left - 4'd1;
            if (bits_left == 4'd1) begin
                busy <= 1'b0;   // stop bit
                if (rx) begin
                    valid <= 1'b1;
                    data  <= shift;
                end
            end else begin
                shift <= {rx, shift[7:1]};
            end
        end else begin
            divider <= divider + 1'b1;
        end
    end
endmodule

// First-word-fall-through FIFO in block RAM. Nothing here needs more than a byte every few
// cycles. A byte arriving when it is full is dropped, and overflowed stays set until reset:
// the board has no flow control, because RTS drives its reset net.
module uart_fifo #(
    parameter int ADDR_WIDTH = 10
) (
    input  logic       clk,
    input  logic       rst,
    input  logic       in_valid,
    input  logic [7:0] in_data,
    output logic       in_ready,
    output logic       out_valid,
    output logic [7:0] out_data,
    input  logic       out_ready,
    output logic       overflowed,
    output logic       empty
);
    logic [ADDR_WIDTH-1:0] head, tail;
    logic [1:0]            reading;
    logic [7:0]            ram_rdata;

    // A cycle late: nothing writes here more often than every few cycles.
    always_ff @(posedge clk) begin
        in_ready <= ADDR_WIDTH'(tail + 1'b1) != head;
        empty    <= head == tail && !in_valid;
    end

    ram_bank #(.WIDTH(8), .ADDR_WIDTH(ADDR_WIDTH)) ram (
        .clk(clk),
        .en(1'b1),
        .raddr(head),
        .rdata(ram_rdata),
        .we(in_valid && in_ready),
        .waddr(tail),
        .wdata(in_data)
    );

    always_ff @(posedge clk) begin
        if (rst) begin
            head       <= '0;
            tail       <= '0;
            reading    <= '0;
            out_valid  <= 1'b0;
            overflowed <= 1'b0;
        end else begin
            if (in_valid) begin
                if (in_ready) tail <= tail + 1'b1;
                else          overflowed <= 1'b1;
            end

            if (out_valid) begin
                if (out_ready) begin
                    out_valid <= 1'b0;
                    head      <= head + 1'b1;
                end
            end else if (reading != 2'd0) begin
                reading <= reading - 2'd1;
                if (reading == 2'd1) begin
                    out_valid <= 1'b1;
                    out_data  <= ram_rdata;
                end
            end else if (head != tail) begin
                reading <= 2'd2;
            end
        end
    end
endmodule
