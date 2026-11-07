// Brainfunk core with its Harvard memories, inferred as block RAM.
module brainfunk_soc #(
    parameter int    IMEM_ADDR_WIDTH = 18,
    parameter int    DMEM_ADDR_WIDTH = 16,
    parameter        IMEM_INIT       = ""    // $readmemh image; empty to load it another way
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

    logic [IMEM_ADDR_WIDTH-1:0] imem_addr;
    logic [31:0]                imem_rdata;
    logic                       dmem_en, dmem_we;
    logic [DMEM_ADDR_WIDTH-1:0] dmem_addr;
    logic [7:0]                 dmem_wdata, dmem_rdata;

`ifndef BRAINFUNK_RETIRE_PORT
    /* verilator lint_off UNUSEDSIGNAL */
    logic                       retire_valid;
    logic [IMEM_ADDR_WIDTH-1:0] retire_pc, retire_next_pc;
    logic [31:0]                retire_instr;
    logic [DMEM_ADDR_WIDTH-1:0] retire_base_ptr;
    /* verilator lint_on UNUSEDSIGNAL */
`else
    assign dmem_we_o    = dmem_we;
    assign dmem_addr_o  = dmem_addr;
    assign dmem_wdata_o = dmem_wdata;
`endif

    brainfunk_core #(
        .PC_WIDTH(IMEM_ADDR_WIDTH),
        .ADDR_WIDTH(DMEM_ADDR_WIDTH)
    ) core (.*);

    // Program memory.
    logic [31:0] imem[0:(1 << IMEM_ADDR_WIDTH) - 1] /* verilator public_flat_rw */;

    initial begin
        if (IMEM_INIT != "") $readmemh(IMEM_INIT, imem);
    end

    always_ff @(posedge clk) imem_rdata <= imem[imem_addr];

    // Tape: all zero at power-up, as the ISA requires. Reset does not clear it. Anlogic
    // block RAM configures to zero by default, so only simulation needs to do this, and
    // Tang Dynasty refuses a loop this long.
    logic [7:0] dmem[0:(1 << DMEM_ADDR_WIDTH) - 1] /* verilator public_flat_rw */;

`ifdef VERILATOR
    initial begin
        for (int i = 0; i < (1 << DMEM_ADDR_WIDTH); i++) dmem[i] = 8'd0;
    end
`endif

    always_ff @(posedge clk) begin
        if (dmem_en) begin
            if (dmem_we) dmem[dmem_addr] <= dmem_wdata;
            else         dmem_rdata      <= dmem[dmem_addr];
        end
    end

endmodule
