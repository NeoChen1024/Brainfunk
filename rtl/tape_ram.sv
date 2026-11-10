// The tape's block RAM, as any tool can infer it: 2K x 16 with a write enable per byte, read in two edges
// as ram_bank is (brainfunk_mem.sv). rtl/board/eg4_tape_ram.sv is the same for the EG4S20, whose tools
// would not make byte writes of a BRAM32K from this. A read of the row being written returns undefined
// data in both bytes, as it may on a device; simulation returns wrong data so that nothing comes to
// depend on it.
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
    logic [15:0] mem[0:(1 << ROW_BITS) - 1];
    logic [15:0] ram_q;

    always_ff @(posedge clk) begin
        if (we[0]) mem[waddr][7:0]  <= wdata[7:0];
        if (we[1]) mem[waddr][15:8] <= wdata[15:8];
`ifdef VERILATOR
        ram_q <= (|we && waddr == raddr) ? ~mem[raddr] : mem[raddr];
`else
        ram_q <= mem[raddr];
`endif
        rdata <= ram_q;
    end

`ifdef VERILATOR
    // The cosimulator's view of the tape.
    export "DPI-C" function tape_peek;

    function automatic int tape_peek(input int row);
        return int'(mem[row[ROW_BITS-1:0]]);
    endfunction
`endif
endmodule
