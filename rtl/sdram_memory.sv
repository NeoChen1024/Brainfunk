// The EG4S20's embedded SDRAM as a line memory, the port brainfunk_soc describes: sdram_ctrl, and
// sdram_phy for the pins, which simulation replaces with tb/sdram_model.sv.
module sdram_memory #(
    parameter int BOOT_CYCLES = 16,
    parameter int CAPTURE     = 4     // READ to the first word: CAS latency 3, plus 1 for the pins
) (
    input  logic        clk,
    input  logic        sdclk,
    input  logic        por,      // restarts the SDRAM's initialization
    output logic        ready,

    input  logic        req_valid,
    input  logic        req_write,
    input  logic [17:0] req_line,
    output logic        req_ready,
    output logic        rd_valid,
    output logic [31:0] rd_data,
    output logic        wr_fetch,
    input  logic [31:0] wr_data
);
    logic        sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    logic [10:0] sd_addr;
    logic [1:0]  sd_ba;
    logic [31:0] sd_dq_out, sd_dq_in;

    sdram_ctrl #(.BOOT_CYCLES(BOOT_CYCLES), .CAPTURE(CAPTURE)) ctrl (
        .clk(clk), .por(por), .init_done(ready),
        .req_valid(req_valid), .req_write(req_write), .req_line(req_line), .req_ready(req_ready),
        .rd_valid(rd_valid), .rd_data(rd_data), .wr_fetch(wr_fetch), .wr_data(wr_data),
        .sd_ras_n(sd_ras_n), .sd_cas_n(sd_cas_n), .sd_we_n(sd_we_n), .sd_addr(sd_addr), .sd_ba(sd_ba),
        .sd_dq_out(sd_dq_out), .sd_dq_oe(sd_dq_oe), .sd_dq_in(sd_dq_in)
    );

    sdram_phy phy (
        .clk(clk), .sdclk(sdclk), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .addr(sd_addr), .ba(sd_ba), .dq_out(sd_dq_out), .dq_oe(sd_dq_oe), .dq_in(sd_dq_in)
    );
endmodule
