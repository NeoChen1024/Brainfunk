// The core clock, from the board's 24 MHz: CLOCK_MHZ, a multiple of 24, and the same clock SDRAM_DEGREES
// later, which centres the SDRAM's data window; see doc/rtl.md. The VCO runs at the output frequency times
// OUT_DIV, kept at 1008 MHz or under, where 168 MHz has it. A CPHASE step is one VCO period, 360 / OUT_DIV
// degrees, and an FPHASE step an eighth of one. Simulation passes the input clock through.
module yadan_pll #(
    parameter int CLOCK_MHZ     = 168,
    parameter int SDRAM_DEGREES = 240
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
    localparam int FB_DIV  = CLOCK_MHZ / 24;
    localparam int OUT_DIV = 1008 / CLOCK_MHZ;
    localparam int STEPS   = (SDRAM_DEGREES * 8 * OUT_DIV + 180) / 360;   // fine steps after clk_out
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
        .REFCLK_DIV(1),
        .FBCLK_DIV(FB_DIV),
        .CLKC0_ENABLE("ENABLE"),
        .CLKC0_DIV(OUT_DIV),
        .CLKC0_CPHASE(OUT_DIV - 1),
        .CLKC0_FPHASE(0),
        .CLKC1_ENABLE("ENABLE"),
        .CLKC1_DIV(OUT_DIV),
        .CLKC1_CPHASE((OUT_DIV - 1 + STEPS / 8) % OUT_DIV),
        .CLKC1_FPHASE(STEPS % 8)
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
