// What fetch and decode guess. They are only guesses: the core checks every one, so nothing in
// here affects a program's results, however wrong or uninitialized.
//
// Next-fetch table: 16 entries, looked up with the address fetched one slot before the block it
// predicts, and saying which of its two words is a taken branch. The result is in registers by the
// time that block is fetched, so a hit sends the following fetch to the target with no gap. The slot
// before can be anything, a block on a wrong path included, because the same path through the program
// tends to give the same slots.
//
// Direction table: a 2-bit counter per word, read as the block is fetched to arrive with it in Q, from
// a register; in two copies, one read for each word of the block. It counts how far
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
    input  logic                look_en,
    input  logic [PC_WIDTH-1:0] fetch_pc,     // the key, see the core; and the block the direction table reads for
    input  logic [PC_WIDTH-1:0] other_pc,     // the key instead, when use_other
    input  logic                use_other,
    input  logic [PC_WIDTH-1:0] steer_pc,     // or this, when use_steer: it comes last
    input  logic                use_steer,

    output logic                next_taken_o, // for the block fetch_pc addresses
    output logic                next_pos_o,   // and its word that is the branch
    output logic [PC_WIDTH-1:0] next_target_o,
    output logic [3:0]          q_counter_o,  // the second and the first word's, of the block in Q

    input  logic                next_we,
    input  logic [PC_WIDTH-1:0] next_key,
    input  logic                next_valid,
    input  logic                next_new_pos,
    input  logic [PC_WIDTH-1:0] next_new_target,

    input  logic                dir_we,
    input  logic [PC_WIDTH-1:0] dir_pc,
    input  logic [1:0]          dir_counter
);
    localparam int ENTRY_WIDTH = 2 + NEXT_TAG + PC_WIDTH;

    logic                next_taken, next_pos;
    logic [PC_WIDTH-1:0] next_target;
    logic [3:0]          q_counter, q_counter_ram;
    logic [1:0][DIR_BITS-1:0] dir_raddr;
    logic [1:0]          dir_fwd_1, dir_fwd_2;
    logic [1:0]          dir_new_1, dir_new_2;

`ifdef BRAINFUNK_RETIRE_PORT
    logic [31:0] noise = 32'h1234_ABCD;
    always_ff @(posedge clk) noise <= {noise[30:0], noise[31] ^ noise[21] ^ noise[1] ^ noise[0]};
    assign next_taken_o  = chaos ? noise[1:0] == 2'd0 : next_taken;
    assign next_pos_o    = chaos ? noise[2] : next_pos;
    assign next_target_o = chaos ? fetch_pc + PC_WIDTH'(noise[9:4]) - PC_WIDTH'(32) : next_target;
    assign q_counter_o   = chaos ? noise[13:10] : q_counter;
`else
    assign next_taken_o  = next_taken;
    assign next_pos_o    = next_pos;
    assign next_target_o = next_target;
    assign q_counter_o   = q_counter;
`endif

    logic [2:0][ENTRY_WIDTH-1:0] entries;
    logic [2:0]                  hits;
    logic [ENTRY_WIDTH-1:0]      entry;
    logic                        hit;

    // Every key is looked up, each in a copy of the table of its own, and the choice made after: the
    // reasons for it come late.
    for (genvar k = 0; k < 3; k++) begin : g_look
        logic [ENTRY_WIDTH-1:0] next_table[0:15];
        logic [PC_WIDTH-1:0]    key;
        initial for (int i = 0; i < 16; i++) next_table[i] = '0;
        always_ff @(posedge clk)
            if (next_we) next_table[next_key[3:0]] <= {next_valid, next_key[4 + NEXT_TAG - 1:4], next_new_pos, next_new_target};
        assign key        = k == 0 ? fetch_pc : k == 1 ? other_pc : steer_pc;
        assign entries[k] = next_table[key[3:0]];
        assign hits[k]    = entries[k][ENTRY_WIDTH - 1] && entries[k][PC_WIDTH + NEXT_TAG:PC_WIDTH + 1] == key[4 + NEXT_TAG - 1:4];
    end
    assign entry = use_steer ? entries[2] : use_other ? entries[1] : entries[0];
    assign hit   = use_steer ? hits[2] : use_other ? hits[1] : hits[0];

    always_ff @(posedge clk) begin
        if (rst) begin
            next_taken <= 1'b0;
        end else if (look_en) begin
            next_taken <= hit;
        end
        if (look_en) begin
            next_pos    <= entry[PC_WIDTH];
            next_target <= entry[PC_WIDTH - 1:0];
        end
        // A counter written as it is read reads as anything, so the write is passed along instead.
        if (front_en) begin
            dir_fwd_1 <= {dir_we && dir_pc[DIR_BITS-1:0] == dir_raddr[1], dir_we && dir_pc[DIR_BITS-1:0] == dir_raddr[0]};
            dir_fwd_2 <= dir_fwd_1;
            dir_new_1 <= dir_counter;
            dir_new_2 <= dir_new_1;
            q_counter <= {dir_fwd_2[1] ? dir_new_2 : q_counter_ram[3:2], dir_fwd_2[0] ? dir_new_2 : q_counter_ram[1:0]};
        end
    end

    // The second copy reads the word after, within an I-cache line of eight: the second word of a
    // block never crosses one.
    assign dir_raddr[0] = fetch_pc[DIR_BITS-1:0];
    assign dir_raddr[1] = {fetch_pc[DIR_BITS-1:3], fetch_pc[2:0] + 3'd1};
    ram_bank #(.WIDTH(2), .ADDR_WIDTH(DIR_BITS)) direction (
        .clk(clk),
        .en(front_en),
        .raddr(dir_raddr[0]),
        .rdata(q_counter_ram[1:0]),
        .we(dir_we),
        .waddr(dir_pc[DIR_BITS-1:0]),
        .wdata(dir_counter)
    );
    ram_bank #(.WIDTH(2), .ADDR_WIDTH(DIR_BITS)) direction_next (
        .clk(clk),
        .en(front_en),
        .raddr(dir_raddr[1]),
        .rdata(q_counter_ram[3:2]),
        .we(dir_we),
        .waddr(dir_pc[DIR_BITS-1:0]),
        .wdata(dir_counter)
    );
endmodule

// A bit per tape cell, set while the cell is not zero: a guess for branches, which X still checks.
// Written in order as W writes the cell; read with an address D works out, so that the bit is in a
// register by the time the branch is in M0. Reading a cell in the same cycle it is written gives
// either value, as block RAM may. The tape starts out zero, as block RAM does; a reset without a new
// configuration leaves the bits as they were, which costs only time.
//
// Eight lanes of block RAM, a cell's low address bits choosing the lane: every lane's bit goes
// straight to a register, and the choice is made after it, in M0. Two writes at once are to different
// lanes, which the core makes sure of.
module zero_bits #(
    parameter int ADDR_WIDTH = 16
) (
    input  logic                  clk,
    input  logic                  rd_en,   // raddr was for the word that moved from D to A last cycle
    input  logic [ADDR_WIDTH-1:0] raddr,
    output logic                  zero,    // for that word, once it is in M0
    input  logic                  we,
    input  logic [ADDR_WIDTH-1:0] waddr,
    input  logic                  wzero,
    input  logic                  we2,
    input  logic [ADDR_WIDTH-1:0] waddr2,
    input  logic                  wzero2
);
    localparam int ROW_WIDTH = ADDR_WIDTH - 3;
    logic [7:0] ram_q, nonzero;
    logic [2:0] lane, lane_q;

    // The block RAM reads every cycle, and its output is kept when it was for the word now in A: so
    // that what decides it, which comes late, drives one register and no block RAM.
    for (genvar k = 0; k < 8; k++) begin : lanes
        logic second;
        assign second = we2 && waddr2[2:0] == 3'(k);
        zero_lane #(.ADDR_WIDTH(ROW_WIDTH)) lane_ram (
            .clk(clk), .raddr(raddr[ADDR_WIDTH-1:3]), .nonzero(ram_q[k]),
            .we((we && waddr[2:0] == 3'(k)) || second), .waddr(second ? waddr2[ADDR_WIDTH-1:3] : waddr[ADDR_WIDTH-1:3]),
            .wnonzero(second ? !wzero2 : !wzero)
        );
    end

    always_ff @(posedge clk) begin
        lane <= raddr[2:0];
        if (rd_en) begin
            nonzero <= ram_q;   // the block RAM's outputs go straight to registers
            lane_q  <= lane;
        end
    end
    assign zero = !nonzero[lane_q];
endmodule

// One lane of the zero bits: a block RAM of its own.
module zero_lane #(
    parameter int ADDR_WIDTH = 13
) (
    input  logic                  clk,
    input  logic [ADDR_WIDTH-1:0] raddr,
    output logic                  nonzero,
    input  logic                  we,
    input  logic [ADDR_WIDTH-1:0] waddr,
    input  logic                  wnonzero
);
    logic mem[0:(1 << ADDR_WIDTH) - 1];
`ifdef VERILATOR
    initial for (int i = 0; i < (1 << ADDR_WIDTH); i++) mem[i] = 1'b0;
`endif
    always_ff @(posedge clk) begin
        if (we) mem[waddr] <= wnonzero;
        nonzero <= mem[raddr];
    end
endmodule
