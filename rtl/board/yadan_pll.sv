// The core clock, from the board's 24 MHz: 24 * FB_DIV / REF_DIV MHz, and the same clock at the
// phase that centres the SDRAM's data window, 240 degrees here; see doc/rtl.md. The VCO runs at
// the output frequency times OUT_DIV, and a CPHASE step is one VCO period. Simulation passes the
// input clock through.
module yadan_pll #(
    parameter int REF_DIV = 1,
    parameter int FB_DIV  = 7,
    parameter int OUT_DIV = 6,
    parameter int SDRAM_CPHASE = 3    // 5 is in phase with clk_out; each step ahead is 60 degrees
) (
    input  logic clk_in,
    output logic clk_out,
    output logic sdclk,
    output logic locked
);
`ifdef VERILATOR
    assign clk_out = clk_in;
    assign sdclk   = clk_in;
    assign locked  = 1'b1;
`else
    logic [4:0] clkc;
    logic       feedback;

    EG_PHY_PLL #(
        .DPHASE_SOURCE("DISABLE"),
        .DYNCFG("DISABLE"),
        .FIN("24.000"),
        .FEEDBK_MODE("NORMAL"),
        .FEEDBK_PATH("CLKC0_EXT"),
        .STDBY_ENABLE("DISABLE"),
        .PLLRST_ENA("ENABLE"),
        .SYNC_ENABLE("DISABLE"),
        .DERIVE_PLL_CLOCKS("DISABLE"),
        .GEN_BASIC_CLOCK("DISABLE"),
        .GMC_GAIN(6),
        .ICP_CURRENT(3),
        .KVCO(6),
        .LPF_CAPACITOR(3),
        .LPF_RESISTOR(2),
        .REFCLK_DIV(REF_DIV),
        .FBCLK_DIV(FB_DIV),
        .CLKC0_ENABLE("ENABLE"),
        .CLKC0_DIV(OUT_DIV),
        .CLKC0_CPHASE(OUT_DIV - 1),
        .CLKC0_FPHASE(0),
        .CLKC1_ENABLE("ENABLE"),
        .CLKC1_DIV(OUT_DIV),
        .CLKC1_CPHASE(SDRAM_CPHASE),
        .CLKC1_FPHASE(0)
    ) pll (
        .refclk(clk_in),
        .reset(1'b0),
        .stdby(1'b0),
        .extlock(locked),
        .load_reg(1'b0),
        .psclk(1'b0),
        .psdown(1'b0),
        .psstep(1'b0),
        .psclksel(3'b000),
        .psdone(),
        .dclk(1'b0),
        .dcs(1'b0),
        .dwe(1'b0),
        .di(8'd0),
        .daddr(6'd0),
        .fbclk(feedback),
        .clkc(clkc)
    );

    EG_LOGIC_BUFG feedback_buffer (.i(clkc[0]), .o(feedback));
    assign clk_out = feedback;
    assign sdclk   = clkc[1];
`endif
endmodule
