// brainfunk_soc with the EG4S20's embedded SDRAM as its line memory: what the cosimulator runs.
module brainfunk_soc_sdram #(
    parameter int IMEM_ADDR_WIDTH = 20,
    parameter int DMEM_ADDR_WIDTH = 16,
    parameter bit CLEAR_TAPE_ON_RESET = 1'b0,
    parameter int SDRAM_BOOT_CYCLES   = 16,
    parameter int SDRAM_CAPTURE       = 4     // READ to the first word: CAS latency 3, plus 1 for the pins
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

    input  logic        load_req_valid,
    input  logic        load_req_write,
    input  logic [17:0] load_req_line,
    output logic        load_req_ready,
    output logic        load_rd_valid,
    output logic        load_wr_fetch,
    input  logic [31:0] load_wr_data,
    output logic [31:0] load_rd_data,
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
    logic        mem_req_valid, mem_req_write, mem_req_ready, mem_rd_valid, mem_wr_fetch;
    logic [17:0] mem_req_line;
    logic [31:0] mem_rd_data, mem_wr_data;
    logic        mem_ready;
    assign sdram_ready = mem_ready;

    brainfunk_soc #(
        .IMEM_ADDR_WIDTH(IMEM_ADDR_WIDTH),
        .DMEM_ADDR_WIDTH(DMEM_ADDR_WIDTH),
        .CLEAR_TAPE_ON_RESET(CLEAR_TAPE_ON_RESET)
    ) soc (.*);

    sdram_memory #(.BOOT_CYCLES(SDRAM_BOOT_CYCLES), .CAPTURE(SDRAM_CAPTURE)) sdram (
        .clk(clk), .sdclk(sdclk), .por(por), .ready(mem_ready),
        .req_valid(mem_req_valid), .req_write(mem_req_write), .req_line(mem_req_line), .req_ready(mem_req_ready),
        .rd_valid(mem_rd_valid), .rd_data(mem_rd_data), .wr_fetch(mem_wr_fetch), .wr_data(mem_wr_data)
    );
endmodule
