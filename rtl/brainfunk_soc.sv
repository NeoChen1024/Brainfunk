// Brainfunk core, its instruction cache and its tape, in front of a line memory for the code: whatever a
// board has, behind a controller that moves 8-word lines one at a time. brainfunk_soc_sdram puts the
// EG4S20's SDRAM there. The tape is on chip (brainfunk_tape.sv), and never goes to the line memory.
//
// The memory's side of the port, which sdram_ctrl keeps to:
//   - mem_ready rises once the memory can take requests, and stays high.
//   - A request is taken in a cycle where mem_req_valid and mem_req_ready are both high.
//   - read:  mem_rd_valid is then high 8 times, not necessarily in a row, with the line's words
//            in order on mem_rd_data. The next request is not taken before the last of them: the
//            word goes to whoever made the latest read request.
//   - write: mem_wr_fetch is then high 8 times, and word k of the line is on mem_wr_data three
//            cycles after the k-th of them.
//   - mem_req_line is 18 bits, the line. The code is in the bottom half; the loader may use the rest.
module brainfunk_soc #(
    parameter int IMEM_ADDR_WIDTH = 20,
    parameter int DMEM_ADDR_WIDTH = 16
) (
    input  logic       clk,
    input  logic       rst,

    input  logic       io_in_valid,
    input  logic [7:0] io_in_data,
    input  logic       io_in_keep,
    output logic       io_in_ready,
    output logic       io_out_valid,
    output logic [7:0] io_out_data,
    input  logic       io_out_ready,

    output logic       halted,
    output logic       trapped,

    // The image loader, which writes code, and reads it back, while rst holds everything else still.
    input  logic        load_req_valid,
    input  logic        load_req_write,
    input  logic [17:0] load_req_line,
    output logic        load_req_ready,
    output logic        load_rd_valid,
    output logic        load_wr_fetch,
    input  logic [31:0] load_wr_data,
    output logic [31:0] load_rd_data,

    // The line memory.
    input  logic        mem_ready,
    output logic        mem_req_valid,
    output logic        mem_req_write,
    output logic [17:0] mem_req_line,
    input  logic        mem_req_ready,
    input  logic        mem_rd_valid,
    input  logic [31:0] mem_rd_data,
    input  logic        mem_wr_fetch,
    output logic [31:0] mem_wr_data
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
    logic [DMEM_ADDR_WIDTH-1:0] dmem_raddr, dmem_waddr;
    logic [7:0]                 dmem_rdata, dmem_wdata;
    logic                       dmem_we, dmem_hit, dmem_busy;
    logic [31:0]                dmem_zero;
    logic                       dmem_hit_both;
    logic [31:0]                dmem_bank2;
    logic [DMEM_ADDR_WIDTH-1:0] dmem_raddr2;
    logic [7:0]                 dmem_rdata2;

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

    // ---- the line memory: code ----
    logic        req_valid, req_write, req_ready, rd_valid, wr_fetch;
    logic [17:0] req_line;
    logic [31:0] rd_data, wr_data;
    assign mem_req_valid = req_valid;
    assign mem_req_write = req_write;
    assign mem_req_line  = req_line;
    assign req_ready     = mem_req_ready;
    assign rd_valid      = mem_rd_valid;
    assign rd_data       = mem_rd_data;
    assign wr_fetch      = mem_wr_fetch;
    assign mem_wr_data   = wr_data;

    // One line at a time, the loader's first; the owner of a read request gets its data. Only the
    // loader writes.
    logic        i_req_valid, i_req_ready;
    logic [17:0] i_req_line;
    logic        cache_rst, load_owns_read = 1'b0;
    assign cache_rst = rst || !mem_ready;

    assign req_valid      = load_req_valid || i_req_valid;
    assign req_write      = load_req_valid && load_req_write;
    assign req_line       = load_req_valid ? load_req_line : i_req_line;
    assign load_req_ready = req_ready && load_req_valid;
    assign i_req_ready    = req_ready && !load_req_valid;
    assign load_wr_fetch  = wr_fetch;
    assign load_rd_valid  = rd_valid && load_owns_read;
    assign load_rd_data   = rd_data;
    assign wr_data        = load_wr_data;

    always_ff @(posedge clk) begin
        if (req_ready && req_valid && !req_write) load_owns_read <= load_req_valid;
    end

    logic i_ready, d_ready, core_rst;

    icache #(.PC_WIDTH(IMEM_ADDR_WIDTH)) icache (
        .clk(clk), .rst(cache_rst), .ready(i_ready),
        .imem_en(imem_en), .imem_addr(imem_addr), .imem_rdata(imem_rdata), .imem_hit(imem_hit),
        .imem_miss(imem_miss), .imem_miss_addr(imem_miss_addr), .imem_hold(imem_hold), .imem_refetch(imem_refetch),
        .req_valid(i_req_valid), .req_line(i_req_line), .req_ready(i_req_ready),
        .rd_valid(rd_valid && !load_owns_read), .rd_data(rd_data)
    );

    dtape #(.ADDR_WIDTH(DMEM_ADDR_WIDTH)) tape (
        .clk(clk), .rst(cache_rst), .ready(d_ready),
        .dmem_raddr(dmem_raddr), .dmem_rdata(dmem_rdata), .dmem_hit(dmem_hit), .dmem_zero(dmem_zero),
        .dmem_bank2(dmem_bank2), .dmem_raddr2(dmem_raddr2), .dmem_rdata2(dmem_rdata2), .dmem_hit_both(dmem_hit_both),
        .dmem_we(dmem_we), .dmem_waddr(dmem_waddr), .dmem_wdata(dmem_wdata), .dmem_busy(dmem_busy)
    );

    // The core starts once the cache has invalidated itself and the tape is clear.
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
