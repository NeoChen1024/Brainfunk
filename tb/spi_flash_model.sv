// Simulation model of a W25Q16 in SPI mode 0, sized down to what uart_tb uses. It stops the
// simulation on what the part would ignore or corrupt: programming without write enable, a
// program crossing a page, or a command while it is busy.
module spi_flash_model #(
    parameter int BUSY_CYCLES = 200
) (
    input  logic cs_n,
    input  logic sck,
    input  logic mosi,
    output logic miso,
    input  logic clk_for_timing
);
    logic [7:0]  mem[0:(1 << 21) - 1];
    logic [7:0]  rx = 0, tx = 0, cmd = 0;
    int          bits = 0, count = 0;
    logic [23:0] addr = 0;
    logic        wel = 0;
    int unsigned now = 0, busy_until = 0;
    logic        busy;
    assign busy = now < busy_until;

    initial for (int i = 0; i < (1 << 21); i++) mem[i] = 8'hFF;

    function automatic void fail(input string what);
        $display("flash model: %s", what);
        $fatal;
    endfunction

    always_ff @(posedge clk_for_timing) now <= now + 1;

    // Commands take effect when CS rises.
    always @(posedge cs_n) begin
        if (cmd == 8'h20 && count == 4) begin
            if (!wel) fail("erase without write enable");
            if (busy) fail("erase while busy");
            for (int i = 0; i < 4096; i++) mem[21'({addr[20:12], 12'd0}) + 21'(i)] = 8'hFF;
            wel        <= 1'b0;
            busy_until <= now + BUSY_CYCLES;
        end
        if (cmd == 8'h02 && count >= 4) begin
            wel        <= 1'b0;
            busy_until <= now + BUSY_CYCLES;
        end
        if (cmd == 8'h06) wel <= 1'b1;
    end

    always @(negedge cs_n) begin
        bits  = 0;
        count = 0;
        cmd   = 0;
    end

    always @(posedge sck) if (!cs_n) begin
        rx = {rx[6:0], mosi};
        bits = bits + 1;
        if (bits == 8) begin
            bits = 0;
            if (count == 0) begin
                cmd = rx;
                if (busy && cmd != 8'h05) fail("command while busy");
                if (cmd == 8'h9F) tx = 8'hEF;
                if (cmd == 8'h05) tx = {7'd0, busy};
            end else if (cmd == 8'h9F) begin
                tx = count == 1 ? 8'h40 : 8'h15;
            end else if (cmd == 8'h05) begin
                tx = {7'd0, busy};
            end else if (count <= 3) begin
                addr = {addr[15:0], rx};
                if (count == 3 && cmd == 8'h03) tx = mem[addr[20:0]];
                if (count == 3 && cmd == 8'h02 && !wel) fail("program without write enable");
            end else if (cmd == 8'h03) begin
                addr = addr + 24'd1;
                tx   = mem[addr[20:0]];
            end else if (cmd == 8'h02) begin
                if (count - 4 >= 256) fail("program of more than a page");
                mem[{addr[20:8], 8'(addr[7:0] + 8'(count - 4))}] &= rx;
            end
            count = count + 1;
        end
    end

    // Mode 0: the part changes its output on the falling edge.
    always @(negedge sck) if (!cs_n) begin
        if (bits == 0) miso = tx[7];
        else           miso = tx[7 - bits];
    end
endmodule
