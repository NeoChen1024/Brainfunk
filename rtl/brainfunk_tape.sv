// The tape: all of it in block RAM, so that nothing misses.
//
// Sixteen block RAMs of 2K x 16, one row of each per 32-cell line: RAM j holds cells j and j + 16 of
// every line, the low byte and the high one. A read gets the whole line at once for F, and the second
// read port takes any RAM the first one does not need, by giving it its own address. On the EG4S20
// these are its sixteen BRAM32K, which hold the 64 KiB tape exactly. Reads take three edges.
//
// A write of one cell and a read of the other cell of the same row, in the same cycle, are a write
// and a read of the same address on the two ports of one block RAM, and nothing promises what that
// read returns, for either byte. The read reports a miss then, and the core restarts the instruction,
// which by the time it reads again has nothing older left to write. Pairing j with j + 16 rather than
// with j + 1 is what makes that rare: the cells next to the one being written are the ones read next.
// Measured in cosimulation over 20 million instructions, mandel.b had 0.35 such reads per thousand
// cycles paired this way, against 5.3 with neighbours, and long.b none, against 48. A read of the
// cell being written is the core's to forward, and no miss. F waits for every older write, so its
// zero bits never meet one.
//
// A reset zeroes the tape, a line a cycle, and holds dmem_busy until it is done.
module dtape #(
    parameter int ADDR_WIDTH = 16   // cells
) (
    input  logic                  clk,
    input  logic                  rst,
    output logic                  ready,

    input  logic [ADDR_WIDTH-1:0] dmem_raddr,
    output logic [7:0]            dmem_rdata,
    output logic                  dmem_hit,      // no write met the read's row in its RAM
    output logic [31:0]           dmem_zero,     // with dmem_rdata: which cells of its line are zero
    input  logic [31:0]           dmem_bank2,    // which cell the second port reads, one-hot, or none
    input  logic [ADDR_WIDTH-1:0] dmem_raddr2,
    output logic [7:0]            dmem_rdata2,
    output logic                  dmem_hit_both, // dmem_hit, and the same for the second port if it read
    input  logic                  dmem_we,
    input  logic [ADDR_WIDTH-1:0] dmem_waddr,
    input  logic [7:0]            dmem_wdata,
    output logic                  dmem_busy
);
    localparam int ROW_BITS = ADDR_WIDTH - 5;

    logic                clearing;
    logic [ROW_BITS-1:0] clear_row;

    always_ff @(posedge clk) begin
        if (rst) begin
            clearing  <= 1'b1;
            clear_row <= '0;
        end else if (clearing) begin
            clear_row <= clear_row + 1'b1;
            if (&clear_row) clearing <= 1'b0;
        end
    end
    assign ready     = !clearing;
    assign dmem_busy = clearing;

    // The second port takes RAM j if it reads either of its cells.
    logic [15:0] second;
    assign second = dmem_bank2[15:0] | dmem_bank2[31:16];

    logic [ROW_BITS-1:0] row, row2, wrow;
    assign row  = dmem_raddr[ADDR_WIDTH-1:5];
    assign row2 = dmem_raddr2[ADDR_WIDTH-1:5];
    assign wrow = clearing ? clear_row : dmem_waddr[ADDR_WIDTH-1:5];

    logic [255:0] line_rdata;   // cell j in byte j, as a line is
    generate
        for (genvar j = 0; j < 16; j++) begin : g_ram
            logic [15:0] rdata;
            tape_ram #(.ROW_BITS(ROW_BITS)) ram (
                .clk(clk),
                .raddr(second[j] ? row2 : row),
                .rdata(rdata),
                .we(clearing ? 2'b11 :
                    {2{dmem_we && dmem_waddr[3:0] == 4'(j)}} & {dmem_waddr[4], !dmem_waddr[4]}),
                .waddr(wrow),
                .wdata(clearing ? 16'd0 : {2{dmem_wdata}})
            );
            assign line_rdata[8*j +: 8]        = rdata[7:0];
            assign line_rdata[8*(j + 16) +: 8] = rdata[15:8];
        end
    endgenerate

    // A write to the other cell of a read's row, in the read's RAM.
    logic clash, clash2;
    assign clash  = dmem_we && !clearing && dmem_waddr[3:0] == dmem_raddr[3:0] &&
                    dmem_waddr[ADDR_WIDTH-1:5] == row && dmem_waddr[4] != dmem_raddr[4];
    assign clash2 = dmem_we && !clearing && |dmem_bank2 && dmem_waddr[3:0] == dmem_raddr2[3:0] &&
                    dmem_waddr[ADDR_WIDTH-1:5] == row2 && dmem_waddr[4] != dmem_raddr2[4];

    logic [4:0] cell_1, cell_2, cell2_1, cell2_2;
    logic       clean_1, clean_2, clean2_1, clean2_2;
    always_ff @(posedge clk) begin
        cell_1      <= dmem_raddr[4:0];
        cell_2      <= cell_1;
        cell2_1     <= dmem_raddr2[4:0];
        cell2_2     <= cell2_1;
        clean_1     <= !clash;
        clean_2     <= clean_1;
        clean2_1    <= !clash2;
        clean2_2    <= clean2_1;
        dmem_rdata  <= line_rdata[8*cell_2 +: 8];
        dmem_rdata2 <= line_rdata[8*cell2_2 +: 8];
        dmem_hit    <= clean_2;
        dmem_hit_both <= clean_2 && clean2_2;
        for (int i = 0; i < 32; i++) dmem_zero[i] <= line_rdata[8*i +: 8] == 8'd0;
    end
endmodule
