// The EG4S20's embedded SDRAM, with its bidirectional data bus split. Commands and write data
// come from clk; the part runs from sdclk, a PLL output at the phase measured to centre the
// data window; see doc/rtl.md. tb/sdram_model.sv replaces this module in simulation.
module sdram_phy (
    input  logic        clk,
    input  logic        sdclk,
    input  logic        ras_n,
    input  logic        cas_n,
    input  logic        we_n,
    input  logic [10:0] addr,
    input  logic [1:0]  ba,
    input  logic [31:0] dq_out,
    input  logic        dq_oe,
    output logic [31:0] dq_in
);
    wire [31:0] dq;
    assign dq    = dq_oe ? dq_out : 32'bz;
    assign dq_in = dq;

    EG_PHY_SDRAM_2M_32 sdram (
        .clk(sdclk),
        .ras_n(ras_n),
        .cas_n(cas_n),
        .we_n(we_n),
        .addr(addr),
        .ba(ba),
        .dq(dq),
        .cs_n(1'b0),
        .dm0(1'b0),
        .dm1(1'b0),
        .dm2(1'b0),
        .dm3(1'b0),
        .cke(1'b1)
    );
endmodule
