// What fetch and decode guess. They are only guesses: the core checks every one, so nothing in
// here affects a program's results, however wrong or uninitialized.
//
// Next-fetch table: 16 entries, looked up with the address fetched one slot before the word it
// predicts. The result is in registers by the time that word is fetched, so a hit sends the
// following fetch to the target with no gap. The slot before can be anything, a word on a wrong
// path included, because the same path through the program tends to give the same slots.
//
// Direction table: a 2-bit counter per word, read to arrive with the word in D. It counts how far
// the branch disagrees with its static guess, backward taken, forward not taken, so that a RAM of
// zeros starts out as that guess, and branches sharing a counter mostly agree with each other.
module branch_predictor #(
    parameter int PC_WIDTH = 20,
    parameter int NEXT_TAG = 8,  // address bits above the index an entry is matched on
    parameter int DIR_BITS = 12
) (
`ifdef BRAINFUNK_RETIRE_PORT
    input  logic                chaos,        // guess at random, to show that guesses are only guesses
`endif
    input  logic                clk,
    input  logic                rst,
    input  logic                front_en,
    input  logic [PC_WIDTH-1:0] fetch_pc,
    input  logic [PC_WIDTH-1:0] i1_pc,

    output logic                next_taken_o, // for the word fetch_pc addresses
    output logic [PC_WIDTH-1:0] next_target_o,
    output logic [1:0]          d_counter_o,  // for the word in D

    input  logic                next_we,
    input  logic [PC_WIDTH-1:0] next_key,
    input  logic                next_valid,
    input  logic [PC_WIDTH-1:0] next_new_target,

    input  logic                dir_we,
    input  logic [PC_WIDTH-1:0] dir_pc,
    input  logic [1:0]          dir_counter
);
    localparam int ENTRY_WIDTH = 1 + NEXT_TAG + PC_WIDTH;

    logic                next_taken;
    logic [PC_WIDTH-1:0] next_target;
    logic [1:0]          d_counter;

`ifdef BRAINFUNK_RETIRE_PORT
    logic [31:0] noise = 32'h1234_ABCD;
    always_ff @(posedge clk) noise <= {noise[30:0], noise[31] ^ noise[21] ^ noise[1] ^ noise[0]};
    assign next_taken_o  = chaos ? noise[1:0] == 2'd0 : next_taken;
    assign next_target_o = chaos ? fetch_pc + PC_WIDTH'(noise[9:4]) - PC_WIDTH'(32) : next_target;
    assign d_counter_o   = chaos ? noise[11:10] : d_counter;
`else
    assign next_taken_o  = next_taken;
    assign next_target_o = next_target;
    assign d_counter_o   = d_counter;
`endif

    logic [ENTRY_WIDTH-1:0] next_table[0:15];
    logic [ENTRY_WIDTH-1:0] entry;

    assign entry = next_table[fetch_pc[3:0]];

    always_ff @(posedge clk) begin
        if (next_we) next_table[next_key[3:0]] <= {next_valid, next_key[4 + NEXT_TAG - 1:4], next_new_target};
        if (rst) begin
            next_taken <= 1'b0;
        end else if (front_en) begin
            next_taken <= entry[ENTRY_WIDTH - 1] && entry[PC_WIDTH + NEXT_TAG - 1:PC_WIDTH] == fetch_pc[4 + NEXT_TAG - 1:4];
        end
        if (front_en) next_target <= entry[PC_WIDTH - 1:0];
    end

    initial for (int i = 0; i < 16; i++) next_table[i] = '0;

    // A counter written as it is read reads as anything, as block RAM does, so such a write is left
    // out: the counter is a guess, and an update lost now and then cost mandel.b 0.1% where the reads
    // gone wrong cost it 6%. The comparison is on the write's side, which has time; the read's has
    // none to spare on its way to fetch_pc.
    logic in_writes;
    assign in_writes = dir_we && !(front_en && dir_pc[DIR_BITS-1:0] == i1_pc[DIR_BITS-1:0]);

    ram_bank #(.WIDTH(2), .ADDR_WIDTH(DIR_BITS)) direction (
        .clk(clk),
        .en(front_en),
        .raddr(i1_pc[DIR_BITS-1:0]),
        .rdata(d_counter),
        .we(in_writes),
        .waddr(dir_pc[DIR_BITS-1:0]),
        .wdata(dir_counter)
    );
endmodule
