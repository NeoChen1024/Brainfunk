// Brainfunk core with its Harvard memories, in block RAM.
module brainfunk_soc #(
    parameter int IMEM_ADDR_WIDTH = 18,
    parameter int DMEM_ADDR_WIDTH = 16,
    parameter     IMEM_INIT       = "",   // $readmemh image; empty to load it another way
    parameter bit CLEAR_TAPE_ON_RESET = 1'b0
) (
    input  logic       clk,
    input  logic       rst,

    input  logic       io_in_valid,
    input  logic [7:0] io_in_data,
    output logic       io_in_ready,
    output logic       io_out_valid,
    output logic [7:0] io_out_data,
    input  logic       io_out_ready,

    output logic       halted,
    output logic       trapped
`ifdef BRAINFUNK_RETIRE_PORT
    ,
    output logic                       retire_valid,
    output logic [IMEM_ADDR_WIDTH-1:0] retire_pc,
    output logic [31:0]                retire_instr,
    output logic [IMEM_ADDR_WIDTH-1:0] retire_next_pc,
    output logic [DMEM_ADDR_WIDTH-1:0] retire_base_ptr,
    output logic                       dmem_we_o,
    output logic [DMEM_ADDR_WIDTH-1:0] dmem_addr_o,
    output logic [7:0]                 dmem_wdata_o
`endif
);

    logic                       imem_en;
    logic [IMEM_ADDR_WIDTH-1:0] imem_addr;
    logic [31:0]                imem_rdata;
    logic [DMEM_ADDR_WIDTH-1:0] dmem_raddr, dmem_waddr;
    logic [7:0]                 dmem_rdata, dmem_wdata;
    logic                       dmem_we;

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

    // After reset the core waits for the tape to be cleared, if it is going to be.
    logic core_rst, tape_we;
    logic [DMEM_ADDR_WIDTH-1:0] tape_waddr;
    logic [7:0] tape_wdata;

    generate
        if (CLEAR_TAPE_ON_RESET) begin : g_clear
            logic clearing = 1'b1;
            logic [DMEM_ADDR_WIDTH-1:0] clear_addr = '0;

            always_ff @(posedge clk) begin
                if (rst) begin
                    clearing   <= 1'b1;
                    clear_addr <= '0;
                end else if (clearing) begin
                    clearing   <= clear_addr != '1;
                    clear_addr <= clear_addr + 1'b1;
                end
                core_rst <= rst || clearing;
            end

            assign tape_we    = clearing || dmem_we;
            assign tape_waddr = clearing ? clear_addr : dmem_waddr;
            assign tape_wdata = clearing ? 8'd0 : dmem_wdata;
        end else begin : g_no_clear
            assign core_rst   = rst;
            assign tape_we    = dmem_we;
            assign tape_waddr = dmem_waddr;
            assign tape_wdata = dmem_wdata;
        end
    endgenerate

    brainfunk_core #(
        .PC_WIDTH(IMEM_ADDR_WIDTH),
        .ADDR_WIDTH(DMEM_ADDR_WIDTH)
    ) core (.rst(core_rst), .*);

    // Program memory. A bank holds 2048 words, the depth of two 32 Kb block RAMs.
    localparam int IMEM_BANK_WIDTH = IMEM_ADDR_WIDTH < 11 ? IMEM_ADDR_WIDTH : 11;

    banked_ram #(
        .WIDTH(32),
        .ADDR_WIDTH(IMEM_ADDR_WIDTH),
        .BANK_WIDTH(IMEM_BANK_WIDTH),
        .INIT(IMEM_INIT)
    ) imem (
        .clk(clk),
        .en(imem_en),
        .raddr(imem_addr),
        .rdata(imem_rdata),
        .we(1'b0),
        .waddr('0),
        .wdata('0)
    );

    // Tape: all zero at power-up, as the ISA requires, because Anlogic block RAM configures
    // to zero by default. A bank holds 4096 cells, one 32 Kb block RAM.
    localparam int DMEM_BANK_WIDTH = DMEM_ADDR_WIDTH < 12 ? DMEM_ADDR_WIDTH : 12;

    banked_ram #(
        .WIDTH(8),
        .ADDR_WIDTH(DMEM_ADDR_WIDTH),
        .BANK_WIDTH(DMEM_BANK_WIDTH)
    ) dmem (
        .clk(clk),
        .en(1'b1),
        .raddr(dmem_raddr),
        .rdata(dmem_rdata),
        .we(tape_we),
        .waddr(tape_waddr),
        .wdata(tape_wdata)
    );

endmodule
