// rtl/tape_ram.sv for the EG4S20: one BRAM32K as 2K x 16, which Tang Dynasty does not infer with byte
// writes. Left to infer the portable one, it built each from four ERAM9K and used all 64 of those, which
// leaves none for anything else. Port A reads, and a register in the fabric is the second edge, which can
// sit by the multiplexers that take the line apart; port B writes a byte or both.
module tape_ram #(
    parameter int ROW_BITS = 11
) (
    input  logic                clk,
    input  logic [ROW_BITS-1:0] raddr,
    output logic [15:0]         rdata,
    input  logic [1:0]          we,
    input  logic [ROW_BITS-1:0] waddr,
    input  logic [15:0]         wdata
);
    if (ROW_BITS != 11) $error("a BRAM32K holds 2K rows of 16 bits");

    logic [15:0] ram_q;
    always_ff @(posedge clk) rdata <= ram_q;

    EG_PHY_BRAM32K #(
        .MODE("DP16K"),
        .DATA_WIDTH_A("16"),
        .DATA_WIDTH_B("16"),
        .REGMODE_A("NOREG"),
        .REGMODE_B("NOREG"),
        .WRITEMODE_A("NORMAL"),
        .WRITEMODE_B("NORMAL"),
        .SRMODE("SYNC")
    ) ram (
        .clka(clk), .rsta(1'b0), .csa(1'b1), .wea(1'b0), .ocea(1'b0),
        .addra(raddr), .dia(16'd0), .bytea(1'b0), .bytewea(1'b0), .doa(ram_q),
        // bytewe picks one byte, and byte which: both written, or the high one, or the low one.
        .clkb(clk), .rstb(1'b0), .csb(1'b1), .web(|we), .oceb(1'b0),
        .addrb(waddr), .dib(wdata), .byteb(we[1]), .byteweb(we != 2'b11), .dob()
    );
endmodule
