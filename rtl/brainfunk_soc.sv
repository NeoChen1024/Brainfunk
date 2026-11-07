// Brainfunk core, its caches, and the SDRAM behind them.
module brainfunk_soc #(
    parameter int IMEM_ADDR_WIDTH = 20,
    parameter int DMEM_ADDR_WIDTH = 16,
    parameter bit CLEAR_TAPE_ON_RESET = 1'b0,
    parameter int SDRAM_BOOT_CYCLES   = 16
) (
    input  logic       clk,
    input  logic       sdclk,
    input  logic       por,   // restarts the SDRAM too
    input  logic       rst,

    input  logic       io_in_valid,
    input  logic [7:0] io_in_data,
    output logic       io_in_ready,
    output logic       io_out_valid,
    output logic [7:0] io_out_data,
    input  logic       io_out_ready,

    output logic       halted,
    output logic       trapped,

    // The image loader, which writes code while rst holds everything else still.
    input  logic        load_req_valid,
    input  logic [17:0] load_req_line,
    output logic        load_req_ready,
    output logic        load_wr_fetch,
    input  logic [31:0] load_wr_data,
    output logic        sdram_ready
`ifdef BRAINFUNK_RETIRE_PORT
    ,
    input  logic                       predict_chaos,
    output logic                       retire_valid,
    output logic [IMEM_ADDR_WIDTH-1:0] retire_pc,
    output logic [31:0]                retire_instr,
    output logic [IMEM_ADDR_WIDTH-1:0] retire_next_pc,
    output logic [DMEM_ADDR_WIDTH-1:0] retire_base_ptr,
    output logic                       dmem_we_o,
    output logic [DMEM_ADDR_WIDTH-1:0] dmem_addr_o,
    output logic [7:0]                 dmem_wdata_o,
    output logic                       core_ready
`endif
);

    logic                       imem_en, imem_hit, imem_miss, imem_hold, imem_refetch;
    logic [IMEM_ADDR_WIDTH-1:0] imem_addr, imem_miss_addr;
    logic [31:0]                imem_rdata;
    logic [DMEM_ADDR_WIDTH-1:0] dmem_raddr, dmem_waddr, dmem_miss_addr;
    logic [7:0]                 dmem_rdata, dmem_wdata;
    logic                       dmem_we, dmem_hit, dmem_miss, dmem_busy;
    logic [31:0]                dmem_zero;

`ifndef BRAINFUNK_RETIRE_PORT
    /* verilator lint_off UNUSEDSIGNAL */
    logic                       retire_valid;
    logic [IMEM_ADDR_WIDTH-1:0] retire_pc, retire_next_pc;
    logic [31:0]                retire_instr;
    logic [DMEM_ADDR_WIDTH-1:0] retire_base_ptr;
    /* verilator lint_on UNUSEDSIGNAL */
`else
    assign dmem_we_o    = dmem_we;
    assign dmem_addr_o  = dmem_waddr;
    assign dmem_wdata_o = dmem_wdata;
`endif

    // ---- SDRAM: code in the bottom half, tape in the top half ----
    logic        sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    logic [10:0] sd_addr;
    logic [1:0]  sd_ba;
    logic [31:0] sd_dq_out, sd_dq_in;

    logic        req_valid, req_write, req_ready, rd_valid, wr_fetch;
    logic [17:0] req_line;
    logic [31:0] rd_data, wr_data;

    sdram_ctrl #(.BOOT_CYCLES(SDRAM_BOOT_CYCLES)) sdram (
        .clk(clk), .por(por), .init_done(sdram_ready),
        .req_valid(req_valid), .req_write(req_write), .req_line(req_line), .req_ready(req_ready),
        .rd_valid(rd_valid), .rd_data(rd_data), .wr_fetch(wr_fetch), .wr_data(wr_data),
        .sd_ras_n(sd_ras_n), .sd_cas_n(sd_cas_n), .sd_we_n(sd_we_n), .sd_addr(sd_addr), .sd_ba(sd_ba),
        .sd_dq_out(sd_dq_out), .sd_dq_oe(sd_dq_oe), .sd_dq_in(sd_dq_in)
    );

    sdram_phy phy (
        .clk(clk), .sdclk(sdclk), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .addr(sd_addr), .ba(sd_ba), .dq_out(sd_dq_out), .dq_oe(sd_dq_oe), .dq_in(sd_dq_in)
    );

    // One line at a time. The data cache goes first, because it holds the whole core up, and
    // the owner of a request gets its read data.
    logic        i_req_valid, i_req_ready, d_req_valid, d_req_write, d_req_ready, i_owns_read;
    logic [17:0] i_req_line, d_req_line;
    logic        cache_rst, load_req_active;
    logic [31:0] d_wr_data;
    assign cache_rst = rst || !sdram_ready;

    // Write data comes after the request, so the data path needs to remember whose it was.
    always_ff @(posedge clk) begin
        if (req_ready && req_valid) load_req_active <= load_req_valid;
    end

    assign req_valid      = load_req_valid || d_req_valid || i_req_valid;
    assign req_write      = load_req_valid || (d_req_valid && d_req_write);
    assign req_line       = load_req_valid ? load_req_line : (d_req_valid ? d_req_line : i_req_line);
    assign load_req_ready = req_ready && load_req_valid;
    assign d_req_ready    = req_ready && !load_req_valid && d_req_valid;
    assign i_req_ready    = req_ready && !load_req_valid && !d_req_valid;
    assign load_wr_fetch  = wr_fetch;

    always_ff @(posedge clk) begin
        if (req_ready && req_valid && !req_write) i_owns_read <= !d_req_valid;
    end

    logic i_ready, d_ready, core_rst;

    icache #(.PC_WIDTH(IMEM_ADDR_WIDTH)) icache (
        .clk(clk), .rst(cache_rst), .ready(i_ready),
        .imem_en(imem_en), .imem_addr(imem_addr), .imem_rdata(imem_rdata), .imem_hit(imem_hit),
        .imem_miss(imem_miss), .imem_miss_addr(imem_miss_addr), .imem_hold(imem_hold), .imem_refetch(imem_refetch),
        .req_valid(i_req_valid), .req_line(i_req_line), .req_ready(i_req_ready),
        .rd_valid(rd_valid && i_owns_read), .rd_data(rd_data)
    );

    dcache #(.ADDR_WIDTH(DMEM_ADDR_WIDTH), .CLEAR_TAPE(CLEAR_TAPE_ON_RESET)) dcache (
        .clk(clk), .rst(cache_rst), .ready(d_ready),
        .dmem_raddr(dmem_raddr), .dmem_rdata(dmem_rdata), .dmem_hit(dmem_hit), .dmem_zero(dmem_zero),
        .dmem_we(dmem_we), .dmem_waddr(dmem_waddr), .dmem_wdata(dmem_wdata),
        .dmem_miss(dmem_miss), .dmem_miss_addr(dmem_miss_addr), .dmem_busy(dmem_busy),
        .req_valid(d_req_valid), .req_write(d_req_write), .req_line(d_req_line), .req_ready(d_req_ready),
        .rd_valid(rd_valid && !i_owns_read), .rd_data(rd_data), .wr_fetch(wr_fetch), .wr_data(d_wr_data)
    );
    assign wr_data = load_req_active ? load_wr_data : d_wr_data;

    // The core starts once the caches have invalidated themselves.
    always_ff @(posedge clk) core_rst <= cache_rst || !i_ready || !d_ready;
`ifdef BRAINFUNK_RETIRE_PORT
    assign core_ready = !core_rst;
`endif

    logic                       pred_front_en, next_taken, next_we, next_valid, dir_we;
    logic [IMEM_ADDR_WIDTH-1:0] pred_fetch_pc, pred_i1_pc, next_target, next_key, next_new_target, dir_pc;
    logic [1:0]                 d_counter, dir_counter;

    branch_predictor #(.PC_WIDTH(IMEM_ADDR_WIDTH)) predictor (
        .clk(clk), .rst(core_rst), .front_en(pred_front_en), .fetch_pc(pred_fetch_pc), .i1_pc(pred_i1_pc),
`ifdef BRAINFUNK_RETIRE_PORT
        .chaos(predict_chaos),
`endif
        .next_taken_o(next_taken), .next_target_o(next_target), .d_counter_o(d_counter),
        .next_we(next_we), .next_key(next_key), .next_valid(next_valid), .next_new_target(next_new_target),
        .dir_we(dir_we), .dir_pc(dir_pc), .dir_counter(dir_counter)
    );

    brainfunk_core #(
        .PC_WIDTH(IMEM_ADDR_WIDTH),
        .ADDR_WIDTH(DMEM_ADDR_WIDTH)
    ) core (.rst(core_rst), .*);

endmodule
