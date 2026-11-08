// Brainfunk core: scalar pipeline implementing doc/bitcode.txt, built for clock speed.
//
//   IF  I1 I2  Q  D   A   M0 M1 M2  X   W
//
//   IF      fetch_pc addresses the instruction memory, which takes three edges, for a block of two
//           words in a row: one at the end of an I-cache line, or when the first is a taken branch
//   Q       check the next-fetch guess, predict JE and JN, redirect fetch; queue the block's words
//   D       decode the oldest queued word
//   A       base_ptr + offset; M moves base_ptr here. MUL, F and IO stay for several cycles
//
// MUL is a read of its source and a read and write of its destination, three cycles apart so that
// the multiplier has a cycle between X and X. A MUL right after another one with the same source,
// which that one did not write, needs no read of its own: the source's value is still in mul_operand.
// A factor of 1 or -1 needs no multiplier, and is one cycle: the destination reads on the first
// port and the source on the second, and X adds or subtracts the two. The cache has a bank per
// cell of a line, each with an address of its own, so that works unless the two cells are in the
// same bank and different lines, which D can tell from the offsets alone: those take the long way.
//   M0..M2  the data memory read, which takes three edges
//   X       result, zero test; resolves branches and the end of F
//   W       writes the data memory, retires
//
// Nothing past A ever stalls, so an instruction's distance from older ones is settled by
// M0. That is where it compares addresses, so X only has a multiplexer to work through.
//
// Speculation: anything younger than an unresolved JE or JN is speculative. It can move base_ptr,
// which is restored, and read memory, which is harmless. It cannot write, because the flush reaches
// X before anything younger gets to W. Input waits for an empty pipeline; output acts in X, by which
// time everything older has been resolved.
//
// F reads a line per cycle, and looks at every cell of it the stride lands on, all at once, assuming
// none is zero. A read is a line, so the next one needs no adder: only which cell the stride lands on
// first in the next line, from a table. A stride too long for that reads one cell per cycle, with
// base_ptr stepping, through the same path. The read that finds a zero stops the reads behind it,
// and then F works out where that left base_ptr, which nothing younger has needed yet.
//
// F has no forwarding: dmem_zero comes straight from the cache, so F waits until any write older than
// it is no longer ahead of it. Nothing younger issues until F has finished.
//
// The memories are caches; see brainfunk_cache.sv. dmem_rdata arrives three edges after dmem_raddr,
// and imem_rdata three edges after imem_addr if imem_en holds, each with a hit flag.
//
// An instruction fetch miss holds Q until the cache has the line. A data miss is found in X, and
// flushes the instruction with everything behind it, to start again from fetch. It has made no
// writes by then, because writes come after the last read of the same instruction; input reads
// its cell first for that reason, because a byte taken cannot be asked for again.
//
// Prediction, in brainfunk_predict.sv, is two guesses, neither of them trusted:
//   - next_taken says where to fetch after a block, and which of its words is a taken branch, before
//     the block has been seen. Q finds out what the word really is. A word that is not a branch has
//     what follows it fetched again; a branch is taken to be taken, and its target is compared in Q,
//     for X to act on as a misprediction.
//   - with no such guess, the direction table says whether a branch is taken, and Q redirects fetch
//     to the target if so, and puts it in the next-fetch table for next time.
// A next-fetch guess that turns out wrong takes its entry out of the table.
//
// Early resolution: a branch whose cell's value is already known needs no wait for X. Two more
// guesses, which X checks like the others, find most of them, and a branch in M0 acts on them:
//   - an early store of four cells, direct-mapped with partial tags, kept in A: a set, an input byte
//     and an add to a value it holds are worked out there; any other write, and a read of a cell it
//     lacks, leave the cell pending until that instruction's X fills in its value, unless something
//     younger has taken it over. Adds to a pending cell are summed, to go on top of that value when
//     it comes. Any redirect empties it, since the wrong path may have written it, and so does the
//     end of a scan, whose base_ptr comes too late for D to work out the next cells.
//   - a zero bit per cell of the tape, written in order from W and read with an address D works
//     out, believed unless a write of the cell is still on the way.
// A branch whose guess either finds wrong redirects fetch in the next cycle, through early_redirect,
// and has its guess turned around for X; what is younger, in A and M0, goes.
//
// The fused instructions (MULC, DJN, MJN, SRUN) are not implemented yet and trap.
module brainfunk_core #(
    parameter int PC_WIDTH    = 20,
    parameter int ADDR_WIDTH  = 16,
    parameter int LINE_BITS   = 5,  // cells in a line, as in the cache
    parameter int STRIDE_BITS = 4   // F takes a line at a time with a stride below 1 << STRIDE_BITS
) (
    input  logic                  clk,
    input  logic                  rst,

    output logic                  imem_en,
    output logic [PC_WIDTH-1:0]   imem_addr,
    input  logic [63:0]           imem_rdata,      // the word at imem_addr, and the next in its line
    input  logic                  imem_hit,        // arrives with imem_rdata
    input  logic [2*(5 + LINE_BITS + ADDR_WIDTH)-1:0] imem_pre,  // brainfunk_predecode of each word, with it
    input  logic [2*PC_WIDTH-1:0] imem_target,     // each word's address + 1 + its low bits, with it
    output logic                  imem_miss,       // Q holds a block that was not in the cache
    output logic [PC_WIDTH-1:0]   imem_miss_addr,
    input  logic                  imem_hold,       // the cache is writing a tag: do not read
    input  logic                  imem_refetch,    // a fill finished: look up what missed again

    output logic                  pred_front_en,
    output logic                  pred_look_en,    // the next-fetch table is looked up, with a key:
    output logic [PC_WIDTH-1:0]   pred_fetch_pc,   // fetch_pc, which is also the block the direction table reads for,
    output logic [PC_WIDTH-1:0]   pred_other_pc,   // or this if pred_use_other,
    output logic                  pred_use_other,
    output logic [PC_WIDTH-1:0]   pred_steer_pc,   // or this if pred_use_steer, which comes last
    output logic                  pred_use_steer,
    input  logic                  next_taken,      // for the block imem_addr addresses
    input  logic                  next_pos,        // which of its words
    input  logic [PC_WIDTH-1:0]   next_target,
    input  logic [3:0]            q_counter,       // for the second and the first word imem_rdata brings
    output logic                  next_we,
    output logic [PC_WIDTH-1:0]   next_key,
    output logic                  next_valid,
    output logic                  next_new_pos,
    output logic [PC_WIDTH-1:0]   next_new_target,
    output logic                  dir_we,
    output logic [PC_WIDTH-1:0]   dir_pc,
    output logic [1:0]            dir_counter,

    output logic [ADDR_WIDTH-1:0] dmem_raddr,
    input  logic [7:0]            dmem_rdata,
    input  logic                  dmem_hit,        // arrives with dmem_rdata
    // The second read port: a MUL's source, for a factor of 1 or -1. dmem_bank2 is which bank of the line
    // it reads, one-hot, or none; never dmem_raddr's in another line.
    output logic [(1 << LINE_BITS)-1:0] dmem_bank2,
    output logic [ADDR_WIDTH-1:0] dmem_raddr2,
    input  logic [7:0]            dmem_rdata2,
    input  logic                  dmem_hit2,       // the second port's read hit, if there was one
    input  logic [(1 << LINE_BITS)-1:0] dmem_zero, // arrives with dmem_rdata: which cells of the line are zero
    output logic                  dmem_miss,       // one cycle; the instruction restarts
    output logic [ADDR_WIDTH-1:0] dmem_miss_addr,
    input  logic                  dmem_busy,       // handling a miss: nothing may issue
    output logic                  dmem_we,
    output logic [ADDR_WIDTH-1:0] dmem_waddr,
    output logic [7:0]            dmem_wdata,
    output logic                  dmem_we2,        // the second write port, never dmem_waddr's bank
    output logic [ADDR_WIDTH-1:0] dmem_waddr2,
    output logic [(1 << LINE_BITS)-1:0] dmem_wbank2,   // dmem_waddr2's bank, one-hot, decoded in X; or none
    output logic [7:0]            dmem_wdata2,

    input  logic                  io_in_valid,
    input  logic [7:0]            io_in_data,
    output logic                  io_in_ready,
    output logic                  io_out_valid,
    output logic [7:0]            io_out_data,
    input  logic                  io_out_ready,

    output logic                  halted,
    output logic                  trapped,

    // Valid in the cycle an instruction finishes, in program order; next_pc and base_ptr are
    // the values after it, and a write it makes is on the dmem write port in the same cycle.
    // H retires once, with next_pc equal to pc. An illegal word does not retire.
    // retire2 is a second instruction, before the first in program order, which only ever retires with
    // it and is the word before it: an A, S or M, writing on the second write port.
    output logic                  retire2_valid,
    output logic [PC_WIDTH-1:0]   retire2_pc,
    output logic [31:0]           retire2_instr,
    output logic [ADDR_WIDTH-1:0] retire2_base_ptr,
    output logic                  retire_valid,
    output logic [PC_WIDTH-1:0]   retire_pc,
    output logic [31:0]           retire_instr,
    output logic [PC_WIDTH-1:0]   retire_next_pc,
    output logic [ADDR_WIDTH-1:0] retire_base_ptr
`ifdef BRAINFUNK_RETIRE_PORT
    ,
    input  logic                  predict_chaos    // early guesses at random too
`endif
);

    localparam logic [3:0] OP_A = 4'd1, OP_S = 4'd2, OP_MUL = 4'd3, OP_F = 4'd4, OP_M = 4'd5,
                           OP_JE = 4'd6, OP_JN = 4'd7, OP_IO = 4'd8, OP_H = 4'd9;

    // Flushes, registered in X, so they act in the cycle the instruction is in W. Everything
    // younger is then in A or earlier, or in M0..X and killed on its way to the next stage.
    logic redirect;  // branch mispredicted: fetch from w_alt_pc
    logic scan_stop; // F found its zero: only the reads behind it go
    logic scan_done; // and has now worked out where base_ptr ends up, and retires
    logic flush;
    assign flush = redirect || scan_stop;

    // A branch that resolved early, registered like redirect: fetch from early_pc, and base_ptr is
    // early_base. The branch is in M1: what is in A and M0 goes.
    logic                  early_redirect;
    logic [PC_WIDTH-1:0]   early_pc;
    logic [ADDR_WIDTH-1:0] early_base;

    // ---- fetch ----
    // A block is two words in a row, or one: the last word of an I-cache line, or a first word that
    // the next-fetch table says is a taken branch.
    localparam int ILINE_BITS = 3;  // words in an I-cache line

    function automatic logic two_words(input logic [PC_WIDTH-1:0] pc, input logic taken, input logic pos);
        two_words = pc[ILINE_BITS-1:0] != '1 && !(taken && !pos);
    endfunction

    logic                front_en;
    logic                q_steer;      // what follows this block is not what is being fetched
    logic [PC_WIDTH-1:0] q_steer_pc;
    logic [PC_WIDTH-1:0] fetch_pc, i1_pc, i2_pc, q_pc;
    // Worked out a stage early: the pcs after q_pc's word and the one after it, and the offsets that
    // would take each to the block fetched after q_pc's.
    logic [PC_WIDTH-1:0] q_pc1, q_pc2, q_gap0, q_gap1;
    logic                q_two;      // two_words(q_pc, q_next_taken, q_next_pos)
    logic [1:0]          q_next_at;  // q_next_taken at that word
    // The next-fetch table is looked up with the block fetched before, or, when fetch is sent
    // elsewhere, with what sent it: so that the same path gives the same keys, whatever was being
    // fetched meanwhile. Each block carries its key, for the table to learn or forget.
    logic                look_en;
    logic [PC_WIDTH-1:0] look_pc, fetch_key, i1_key, i2_key, q_key, early_key;
    logic                use_steer;
    logic                i1_valid, i2_valid, q_valid;
    logic                i1_next_taken, i2_next_taken, q_next_taken;
    logic                i1_next_pos, i2_next_pos, q_next_pos;

    assign imem_en        = front_en;
    assign imem_addr      = fetch_pc;
    assign imem_miss      = q_valid && !imem_hit;
    assign imem_miss_addr = q_pc;

    // Where fetch goes on to unless Q steers it, which is decided late, and so picked last.
    logic [PC_WIDTH-1:0] fetch_ahead;
    assign fetch_ahead = next_taken ? next_target : fetch_pc + (fetch_pc[ILINE_BITS-1:0] != '1 ? PC_WIDTH'(2) : PC_WIDTH'(1));

    always_ff @(posedge clk) begin
        if (rst) begin
            fetch_pc <= '0;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            q_valid  <= 1'b0;
        end else if (redirect) begin
            fetch_pc <= w_alt_pc;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            q_valid  <= 1'b0;
        end else if (early_redirect) begin
            fetch_pc <= early_pc;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            q_valid  <= 1'b0;
        end else if (imem_refetch && imem_miss) begin
            fetch_pc <= q_pc;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            q_valid  <= 1'b0;
        end else if (front_en) begin
            fetch_pc <= use_steer ? q_steer_pc : fetch_ahead;
            i1_valid <= !q_steer;
            i2_valid <= !q_steer && i1_valid;
            q_valid  <= !q_steer && i2_valid;
        end
        if (front_en) begin
            i1_pc         <= fetch_pc;
            i2_pc         <= i1_pc;
            q_pc          <= i2_pc;
            q_two         <= two_words(i2_pc, i2_next_taken, i2_next_pos);
            q_next_at     <= {i2_next_taken && i2_next_pos, i2_next_taken && !i2_next_pos};
            q_pc1         <= i2_pc + PC_WIDTH'(1);
            q_pc2         <= i2_pc + PC_WIDTH'(2);
            q_gap0        <= i1_pc - i2_pc - PC_WIDTH'(1);
            q_gap1        <= i1_pc - i2_pc - PC_WIDTH'(2);
            i1_key        <= fetch_key;
            i2_key        <= i1_key;
            q_key         <= i2_key;
            i1_next_taken <= next_taken;
            i2_next_taken <= i1_next_taken;
            q_next_taken  <= i2_next_taken;
            i1_next_pos   <= next_pos;
            i2_next_pos   <= i1_next_pos;
            q_next_pos    <= i2_next_pos;
        end
    end

    assign look_en = redirect || early_redirect || (imem_refetch && imem_miss) || front_en;
    // The keys the next-fetch table could be looked up with are looked up at once, and chosen after.
    logic [PC_WIDTH-1:0] other_pc;
    logic                use_other;
    assign other_pc  = redirect ? w_pc : early_redirect ? early_key : q_key;
    assign use_other = redirect || early_redirect || (imem_refetch && imem_miss);
    assign look_pc   = use_other ? other_pc : fetch_pc;
    assign use_steer = q_steer && !redirect && !early_redirect && !(imem_refetch && imem_miss);
    always_ff @(posedge clk) if (look_en) fetch_key <= use_steer ? q_pc : look_pc;

    // ---- Q: the block's words, and where fetch goes after them ----
    // The block ends at its first branch guessed taken, or at the word the next-fetch table said was
    // one, which it then forgets if that was not a branch. Fetch is sent elsewhere unless it went
    // where the block's last word goes.
    logic [1:0][31:0]         q_word;
    logic [1:0][4 + LINE_BITS + ADDR_WIDTH:0] q_pre;
    logic [1:0][1:0]          q_counter_of;
    logic [1:0][PC_WIDTH-1:0] q_target;
    logic [1:0]               q_branch, q_taken, q_ends;
    logic                     q_last;
    logic                     q_steer_taken, q_steer_seq;
    assign q_word          = imem_rdata;
    assign q_pre           = imem_pre;
    assign q_counter_of[0] = q_counter[1:0];
    assign q_counter_of[1] = q_counter[3:2];
    for (genvar i = 0; i < 2; i++) begin : g_q_word
        // The direction table counts disagreement with backward taken, forward not taken.
        assign q_branch[i]  = q_pre[i][4 + LINE_BITS + ADDR_WIDTH];
        assign q_taken[i]   = q_branch[i] && (q_next_at[i] || q_counter_of[i][1] != q_word[i][19]);
        assign q_ends[i]    = q_taken[i] || q_next_at[i];
        assign q_target[i]  = imem_target[i*PC_WIDTH +: PC_WIDTH];
    end
    assign q_last        = q_two && !q_ends[0];
    assign q_steer_taken = q_valid && imem_hit && q_taken[q_last] && !q_next_at[q_last];
    assign q_steer_seq   = q_valid && imem_hit && q_next_taken && !q_taken[q_last];
    assign q_steer       = q_steer_taken || q_steer_seq;
    assign q_steer_pc    = q_taken[q_last] ? q_target[q_last] : q_last ? q_pc2 : q_pc1;

    // ---- pairs ----
    // D issues two words at once when the first is an A, S or M and the second an A, S, M, JE or JN, and
    // the two cells, if they both have one, are in different banks of the line, and if both are written,
    // in different lanes of the zero bits. Being in different banks also makes them different cells, so
    // the second word's read never needs the first one's result. The first is the younger lane's
    // companion: it reads and writes through the second port. All of that is in the words, so Q works it
    // out as it queues them, with the offset of the second word's cell or move from base_ptr as it is
    // before the first.
    localparam int ZB_LANE_BITS = 3;
    localparam int ZB_CMP = ZB_LANE_BITS < LINE_BITS ? ZB_LANE_BITS : LINE_BITS;   // as many as a line has
    logic [1:0]                   q_first_ok, q_second_ok, q_has_cell, q_writes, q_move, q_pair;
    logic [1:0][LINE_BITS-1:0]    q_cell;
    logic [1:0][ADDR_WIDTH-1:0]   q_own, q_long, q_fold;
    // The word queued last, as a first word.
    logic                         lp_first_ok, lp_has_cell, lp_move;
    logic [LINE_BITS-1:0]         lp_cell;
    logic [ADDR_WIDTH-1:0]        lp_long;

    function automatic logic cells_clash(input logic has_a, input logic [LINE_BITS-1:0] a,
                                         input logic has_b, input logic writes_b, input logic [LINE_BITS-1:0] b);
        cells_clash = has_a && has_b && (a == b || (writes_b && a[ZB_CMP-1:0] == b[ZB_CMP-1:0]));
    endfunction

    for (genvar i = 0; i < 2; i++) begin : g_q_pair
        logic unused_branch;
        assign {unused_branch, q_first_ok[i], q_has_cell[i], q_writes[i], q_move[i], q_cell[i], q_own[i]} = q_pre[i];
        assign q_second_ok[i] = q_first_ok[i] || q_branch[i];
        assign q_long[i]      = ADDR_WIDTH'($signed(q_word[i][19:0]));
    end
    assign q_pair[0] = q_second_ok[0] && lp_first_ok && !cells_clash(lp_has_cell, lp_cell, q_has_cell[0], q_writes[0], q_cell[0]);
    assign q_pair[1] = q_second_ok[1] && q_first_ok[0] && !cells_clash(q_has_cell[0], q_cell[0], q_has_cell[1], q_writes[1], q_cell[1]);
    assign q_fold[0] = q_own[0] + (lp_move ? lp_long : '0);
    assign q_fold[1] = q_own[1] + (q_move[0] ? q_long[0] : '0);

    // ---- the fetch queue ----
    // Oldest first. Fetch goes on while there is room for a block, whatever D does this cycle.
    localparam int FQ_SIZE = 4;
    logic [FQ_SIZE-1:0][31:0]         fq_instr;
    logic [FQ_SIZE-1:0][PC_WIDTH-1:0] fq_pc;
    logic [FQ_SIZE-1:0]               fq_taken, fq_next_taken, fq_target_wrong;
    logic [FQ_SIZE-1:0][3:0]          fq_slot;
    logic [FQ_SIZE-1:0][1:0]          fq_counter;
    logic [FQ_SIZE-1:0]               fq_pair;    // goes with the word before, if that is still here
    logic [FQ_SIZE-1:0][ADDR_WIDTH-1:0] fq_fold;  // then the offset of its cell or move
    logic [2:0]                       fq_count;
    logic [1:0][PC_WIDTH-1:0]         q_word_pc;
    logic [1:0]                       q_target_wrong;
    logic                             q_push, d_take, d_pair;

    // A branch the next-fetch table sent fetch after has its target checked against where fetch went.
    assign q_word_pc[0]      = q_pc;
    assign q_word_pc[1]      = q_pc1;
    assign q_target_wrong[0] = q_word[0][PC_WIDTH-1:0] != q_gap0;
    assign q_target_wrong[1] = q_word[1][PC_WIDTH-1:0] != q_gap1;

    // With three words queued, fetch goes on if D is about to take a pair.
    logic fetch_at_three;
    assign fetch_at_three = fq_count == 3'(FQ_SIZE - 1) && fq_pair[1];
    assign front_en = (fq_count <= 3'(FQ_SIZE - 2) || (fetch_at_three && a_ready)) && !imem_miss && !imem_hold;
    assign q_push   = front_en && q_valid && !redirect && !early_redirect;

    // What each entry becomes is worked out both ways, D taking or not, from registers and the block in
    // Q; whether A is ready, which comes late, only picks. A redirect empties the queue through
    // fq_count, so what the entries hold then does not matter.
    logic [2:0]         fq_kept_take;
    logic               fq_shift;
    logic [2:0]         fq_count_next;
    assign fq_kept_take  = fq_count - (d_pair ? 3'd2 : 3'd1);
    assign fq_shift      = a_ready && d_valid;
    // The count too, both ways: front_en and d_take depend on a_ready, and with them what is pushed and
    // taken. fq_count_next is what d_take and q_push give, without a_ready coming first.
    logic       front_ok, push_ready, push_wait;
    logic [2:0] push_words, count_ready, count_wait;
    assign front_ok      = !imem_miss && !imem_hold && q_valid;
    assign push_words    = q_last ? 3'd2 : 3'd1;
    assign push_ready    = front_ok && (fq_count <= 3'(FQ_SIZE - 2) || fetch_at_three);
    assign push_wait     = front_ok && fq_count <= 3'(FQ_SIZE - 2);
    logic [2:0] count_left;
    assign count_left    = fq_count - (d_valid ? (d_pair ? 3'd2 : 3'd1) : 3'd0);
    assign count_ready   = !push_ready ? count_left : q_last ? count_left + 3'd2 : count_left + 3'd1;
    assign count_wait    = !push_wait ? fq_count : q_last ? fq_count + 3'd2 : fq_count + 3'd1;
    assign fq_count_next = (rst || redirect || early_redirect) ? 3'd0 : a_ready ? count_ready : count_wait;

    always_ff @(posedge clk) begin
        if (q_push) begin
            lp_first_ok <= q_first_ok[q_last];
            lp_has_cell <= q_has_cell[q_last];
            lp_move     <= q_move[q_last];
            lp_cell     <= q_cell[q_last];
            lp_long     <= q_long[q_last];
        end
    end

    localparam int FQ_ENTRY = 1 + ADDR_WIDTH + 32 + PC_WIDTH + 1 + 1 + 1 + 4 + 2;
    function automatic logic [FQ_ENTRY-1:0] q_entry(input logic in);
        q_entry = {q_pair[in], q_fold[in], q_word[in], q_word_pc[in], q_taken[in], q_next_at[in] && q_branch[in],
                   q_target_wrong[in], q_key[3:0], q_counter_of[in]};
    endfunction
    function automatic logic [FQ_ENTRY-1:0] fq_entry(input int k);
        fq_entry = {fq_pair[k], fq_fold[k], fq_instr[k], fq_pc[k], fq_taken[k], fq_next_taken[k],
                    fq_target_wrong[k], fq_slot[k], fq_counter[k]};
    endfunction
    for (genvar j = 0; j < FQ_SIZE; j++) begin : g_fq
        localparam int N1 = (j + 1) % FQ_SIZE, N2 = (j + 2) % FQ_SIZE;
        logic [FQ_ENTRY-1:0] hold, take, next;
        // D not taking: the entries kept stay, and the block goes after them.
        assign hold = 3'(j) >= fq_count ? q_entry(3'(j) != fq_count) : fq_entry(j);
        // D taking one word or a pair: what is left moves up, and the block goes after it.
        assign take = 3'(j) >= fq_kept_take ? q_entry(3'(j) != fq_kept_take) : d_pair ? fq_entry(N2) : fq_entry(N1);
        assign next = fq_shift ? take : hold;
        always_ff @(posedge clk)
            {fq_pair[j], fq_fold[j], fq_instr[j], fq_pc[j], fq_taken[j], fq_next_taken[j],
             fq_target_wrong[j], fq_slot[j], fq_counter[j]} <= next;
    end
    always_ff @(posedge clk) fq_count <= fq_count_next;

    // ---- decode ----
    // The main lane takes the word, or the second of a pair, whose first goes alongside in the companion.
    logic [31:0]           d_instr;
    logic [3:0]            d_op;
    logic [PC_WIDTH-1:0]   d_pc;
    logic                  d_valid, d_predict_taken, d_next_taken, d_target_wrong;
    logic [3:0]            d_slot;
    logic [1:0]            d_counter;
    logic [31:0]           d_p_instr;
    logic [ADDR_WIDTH-1:0] d_p_long, d_p_addr;
    logic                  d_p_move;
    assign d_valid         = fq_count != '0;
    assign d_pair          = fq_count >= 3'd2 && fq_pair[1];
    assign d_instr         = d_pair ? fq_instr[1] : fq_instr[0];
    assign d_pc            = d_pair ? fq_pc[1] : fq_pc[0];
    assign d_predict_taken = d_pair ? fq_taken[1] : fq_taken[0];
    assign d_next_taken    = d_pair ? fq_next_taken[1] : fq_next_taken[0];
    assign d_target_wrong  = d_pair ? fq_target_wrong[1] : fq_target_wrong[0];
    assign d_slot          = d_pair ? fq_slot[1] : fq_slot[0];
    assign d_counter       = d_pair ? fq_counter[1] : fq_counter[0];
    assign d_op            = d_instr[31:28];
    assign d_p_instr       = fq_instr[0];
    assign d_p_move        = d_p_instr[31:28] == OP_M;
    assign d_p_long        = ADDR_WIDTH'($signed(d_p_instr[19:0]));
    assign d_p_addr        = ADDR_WIDTH'($signed(d_p_instr[19:8]));

    // doc/bincode.txt: a word this does not accept traps when executed.
    logic d_top_zero, d_legal;
    assign d_top_zero = d_instr[27:20] == 8'd0;
    always_comb begin
        unique case (d_op)
            OP_A, OP_S, OP_M: d_legal = d_top_zero;
            OP_MUL, OP_F, OP_JE, OP_JN: d_legal = 1'b1;
            OP_IO: d_legal = d_top_zero && d_instr[7:1] == 7'd0;
            OP_H:  d_legal = d_instr[27:0] == 28'd0;
            default: d_legal = 1'b0;
        endcase
    end

    // What only a word D does not pair needs is decoded from fq_instr[0], which it is then, without
    // waiting for d_pair: a pair's first word is never a MUL, F or branch, so those are false for it.
    logic [31:0]        d0_instr;
    logic [3:0]         d0_op;
    logic               d0_is_mul, d0_is_scan, d0_is_branch;
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [31:0] d0_short, d0_addr, d0_long;   // only their low bits matter: addresses wrap
    /* verilator lint_on UNUSEDSIGNAL */
    assign d0_instr     = fq_instr[0];
    assign d0_op        = d0_instr[31:28];
    assign d0_is_mul    = d0_op == OP_MUL;
    assign d0_is_scan   = d0_op == OP_F;
    assign d0_is_branch = d0_op == OP_JE || d0_op == OP_JN;
    assign d0_short     = 32'($signed(d0_instr[27:20]));
    assign d0_addr      = 32'($signed(d0_instr[19:8]));
    assign d0_long      = 32'($signed(d0_instr[19:0]));

    logic d_is_branch, d_is_mul, d_is_scan, d_is_io, d_is_halt, d_multi;
    assign d_is_branch = d_op == OP_JE || d_op == OP_JN;
    assign d_is_mul    = d_op == OP_MUL;
    assign d_is_scan   = d_op == OP_F;
    assign d_is_io     = d_op == OP_IO;
    assign d_is_halt   = d_op == OP_H;
    assign d_multi     = d_is_mul || d_is_scan || d_is_io || d_is_halt || !d_legal;

    logic [PC_WIDTH-1:0] d_target, d_seq;
    assign d_target   = d_pc + d_instr[PC_WIDTH-1:0] + PC_WIDTH'(1);
    assign d_seq      = d_pc + PC_WIDTH'(1);

    // F: a stride short enough to take a line at a time, and how long it is.
    logic [STRIDE_BITS-1:0] d_stride_low, d_stride;
    logic                   d_scan_wide;
    assign d_stride_low = d0_instr[STRIDE_BITS-1:0];
    assign d_stride     = d0_instr[19] ? -d_stride_low : d_stride_low;
    assign d_scan_wide  = d_stride_low != '0 && (d0_instr[19:STRIDE_BITS] == '0 || &d0_instr[19:STRIDE_BITS]);

    // Which offset goes where: a_offset is added to form the address, and a_offset_2 to move
    // base_ptr, or for MUL to form the second address. d0_short and the like: only their low bits
    // matter, as addresses wrap.

    // ---- A: address generation ----
    logic                  a_valid, a_legal;
    logic                  a_is_add, a_is_set, a_is_mul, a_is_scan, a_is_move, a_is_branch, a_is_je;
    logic                  a_is_in, a_is_out, a_is_halt, a_multi;
    logic                  a_predict_taken, a_target_wrong;
    logic                  a_mul_unit, a_mul_negate, a_mul_chained, a_mul_pair;
    logic [ADDR_WIDTH-1:0] a_mul_source;
    logic [ADDR_WIDTH-1:0] a_offset, a_offset_2;
    logic                  a_moves;   // base_ptr moves, by a_offset_2: an M in either lane

    // The companion: an A, S or M just before the main lane's word, with its cell at base_ptr + a_mul_source.
    logic                  a_p_valid, a_p_is_add, a_p_is_set, a_p_is_move;
    logic [7:0]            a_p_data;
    logic [PC_WIDTH-1:0]   a_p_pc;
    logic [31:0]           a_p_instr;
    logic [ADDR_WIDTH-1:0] a_p_long;
    logic [7:0]            a_data;
    logic [PC_WIDTH-1:0]   a_pc, a_next_pc, a_alt_pc;
    logic [31:0]           a_instr;
    logic                  a_next_taken;
    logic [3:0]            a_slot;
    logic [1:0]            a_counter;

    // A multi-cycle instruction issues when a_go is set, and moves on after issuing with a_last.
    logic       a_go, a_last;
    logic [1:0] a_phase;

    localparam int LINE_CELLS = 1 << LINE_BITS;

    // Backward strides run through the same tables with the cells of a line numbered from its end.
    logic                   a_scan_wide, a_scan_back, a_scan_more;
    logic [STRIDE_BITS-1:0] a_scan_stride;  // 0 for a stride that goes a cell at a time

    // Where in a line the first cell a stride lands on is, given where it was in the line before; and
    // which cells a stride lands on, if the first is cell 0. Stride 0 here is a cell at a time.
    logic [(1 << (STRIDE_BITS + LINE_BITS))-1:0][LINE_BITS-1:0] NEXT_CELL;
    logic [(1 << STRIDE_BITS)-1:0][LINE_CELLS-1:0]              STRIDE_MASK;

    generate
        for (genvar stride = 0; stride < (1 << STRIDE_BITS); stride++) begin : g_stride
            for (genvar at = 0; at < LINE_CELLS; at++) begin : g_at
                if (stride == 0) begin : g_single
                    assign NEXT_CELL[at]      = '0;
                    assign STRIDE_MASK[0][at] = at == 0;
                end else begin : g_stepping
                    localparam int STEPS = (LINE_CELLS - at + stride - 1) / stride;
                    assign NEXT_CELL[(stride << LINE_BITS) + at] = LINE_BITS'(at + STEPS * stride - LINE_CELLS);
                    assign STRIDE_MASK[stride][at]               = at % stride == 0;
                end
            end
        end
    endgenerate

    function automatic logic [LINE_CELLS-1:0] reversed(input logic [LINE_CELLS-1:0] value);
        for (int i = 0; i < LINE_CELLS; i++) reversed[i] = value[LINE_CELLS - 1 - i];
    endfunction

    // The lowest of these cells, a group at a time so that it takes few logic levels.
    localparam int GROUP = 8 < LINE_CELLS ? 8 : LINE_CELLS;

    function automatic logic [LINE_BITS-1:0] lowest_cell(input logic [LINE_CELLS-1:0] cells);
        lowest_cell = '0;
        for (int group = LINE_CELLS / GROUP - 1; group >= 0; group--) begin
            if (|cells[group * GROUP +: GROUP]) begin
                lowest_cell = LINE_BITS'(group * GROUP);
                for (int i = GROUP - 1; i >= 0; i--) begin
                    if (cells[group * GROUP + i]) lowest_cell = LINE_BITS'(group * GROUP + i);
                end
            end
        end
    endfunction

    logic [ADDR_WIDTH-1:0] base_ptr, ea, ea2, moved;
    assign ea    = base_ptr + a_offset;
    assign ea2   = base_ptr + a_mul_source;
    assign moved = base_ptr + a_offset_2;

    // The next line of a scan, from the line now in M0.
    logic [ADDR_WIDTH-1:0] m0_ea;
    logic [LINE_BITS-1:0]  m0_scan_cell, scan_next_cell;
    logic [ADDR_WIDTH-1:0] scan_next_ea;
    assign scan_next_cell = NEXT_CELL[{a_scan_stride, m0_scan_cell}];
    assign scan_next_ea   = {m0_ea[ADDR_WIDTH-1:LINE_BITS] + (a_scan_back ? '1 : (ADDR_WIDTH-LINE_BITS)'(1)),
                             scan_next_cell ^ {LINE_BITS{a_scan_back}}};

    logic a_issue, a_go_on, a_flag_mul_dst;
    assign a_issue        = a_valid && !dmem_busy && (!a_multi || a_go);
    assign a_go_on        = a_issue && !flush && !early_redirect;   // and nothing older takes it away
    assign a_flag_mul_dst = a_is_mul && a_last;

    // A can take a new instruction; the front end moves if it has one to give.
    logic a_ready;
    assign a_ready  = !a_valid || (!dmem_busy && (!a_multi || a_last)) || scan_done;

    // The instruction before the one in D, if it was a MUL whose source is still in mul_operand: A's
    // when A has one, because then it leaves A as D's arrives, or else the last to leave.
    logic                  prev_mul;
    logic [ADDR_WIDTH-1:0] prev_mul_source;
    always_ff @(posedge clk) begin
        if (rst || redirect || early_redirect) begin
            prev_mul <= 1'b0;
        end else if (a_valid && a_ready) begin
            prev_mul        <= a_is_mul && !a_mul_pair && !scan_done && a_mul_source != a_offset_2;
            prev_mul_source <= a_mul_source;
        end
    end

    // A MUL with both cells read at once: a factor of 1 or -1, and the cells not in the same bank of
    // different lines. Their distance is the distance of the offsets.
    logic d_mul_chained, d_mul_unit, d_mul_pair;
    assign d_mul_unit    = d0_instr[7:0] == 8'd1 || d0_instr[7:0] == 8'hFF;
    assign d_mul_pair    = d0_is_mul && d_mul_unit && !(d0_short[LINE_BITS-1:0] == d0_addr[LINE_BITS-1:0] &&
                                                        d0_short[ADDR_WIDTH-1:0] != d0_addr[ADDR_WIDTH-1:0]);
    assign d_mul_chained = d0_is_mul && !d_mul_pair &&
                           (a_valid ? a_is_mul && !a_mul_pair && a_mul_source != a_offset_2 && d0_short[ADDR_WIDTH-1:0] == a_mul_source
                                    : prev_mul && d0_short[ADDR_WIDTH-1:0] == prev_mul_source);
    assign d_take = a_ready && d_valid && !redirect && !early_redirect;

    // The early store's size, see early resolution below.
    localparam int L0_BITS = 2;
    localparam int L0_SIZE = 1 << L0_BITS;
    localparam int L0_TAG  = 8 - L0_BITS;   // address bits above the index a cell is matched on: a guess may alias

    // The offset A adds first.
    logic [ADDR_WIDTH-1:0] d_offset;
    assign d_offset = d_pair ? fq_fold[1] :
                      ((d0_is_mul && !d_mul_pair) || d0_is_scan || d0_is_branch) ? d0_short[ADDR_WIDTH-1:0]
                                                                                : d0_addr[ADDR_WIDTH-1:0];
    // From base_ptr as it will be when the word is in A, the cells of early resolution: the low bits of
    // the one the early store looks up, the first uop's, and of a MUL's destination; and a branch's, for
    // its zero bit. A move in A is added in with the same carry chain, three operands at once. A scan
    // ending now decides late: for the zero bit, both ways are summed and it picks; the early store
    // does without, see a_l0_blind.
    localparam int ZB_SEEN = 8;   // low address bits a write is matched on, for the zero bits: a match may alias
    logic [7:0]            d_l0_offset, d_l0_move, d_l0_base, d_l0_cell, d_l0_dst;
    logic [ADDR_WIDTH-1:0] d_zb_addr;
    logic [1:0][ADDR_WIDTH-1:0] d_zb_short, d_zb_ways;
    logic [L0_TAG-1:0]     a_l0_cell, a_l0_dst;    // the tags; the slots are one-hot
    logic [L0_SIZE-1:0]    a_l0_one, a_l0_put;    // the early store's slots for a_l0_cell and a_l0_dst
    logic                  a_l0_blind;            // the cells are not known: the early store is emptied
    logic [L0_SIZE-1:0]    a_l0_pkill;            // the companion's slot, which it empties
    logic [ZB_SEEN-1:0]    a_zb_addr;

    function automatic logic [ADDR_WIDTH-1:0] sum3(input logic [ADDR_WIDTH-1:0] a, b, c);
        sum3 = (a ^ b ^ c) + (((a & b) | (a & c) | (b & c)) << 1);
    endfunction

    assign d_l0_offset = d_pair ? fq_fold[1][7:0] : (d0_is_mul || d0_is_scan || d0_is_branch) ? d0_short[7:0] : d0_addr[7:0];
    assign d_l0_base   = base_ptr[7:0];
    assign d_l0_move   = (a_valid && a_moves) ? a_offset_2[7:0] : 8'd0;
    assign d_l0_cell   = 8'(sum3(ADDR_WIDTH'(d_l0_base), ADDR_WIDTH'(d_l0_move), ADDR_WIDTH'(d_l0_offset)));
    assign d_l0_dst    = 8'(sum3(ADDR_WIDTH'(d_l0_base), ADDR_WIDTH'(d_l0_move), ADDR_WIDTH'(d0_addr[7:0])));
    // Worked out for D's word not paired and paired, before d_pair picks.
    assign d_zb_short[0] = d0_short[ADDR_WIDTH-1:0];
    assign d_zb_short[1] = fq_fold[1];
    for (genvar k = 0; k < 2; k++) begin : g_zb_way
        assign d_zb_ways[k] = scan_done ? scan_base + d_zb_short[k] :
                              (a_valid && a_moves) ? sum3(base_ptr, a_offset_2, d_zb_short[k]) : base_ptr + d_zb_short[k];
    end
    assign d_zb_addr   = d_zb_ways[d_pair];

    // The companion's cell takes its slot of the early store from whatever had it: see the store.
    logic [7:0] d_p_l0_cell;
    assign d_p_l0_cell = 8'(sum3(ADDR_WIDTH'(d_l0_base), ADDR_WIDTH'(d_l0_move), ADDR_WIDTH'(d_p_addr)));

    // Taken as A is ready, not only as D moves to it, which is later: if D does not, A is empty, and
    // what these hold does not matter.
    always_ff @(posedge clk) begin
        if (a_ready) begin
            a_l0_cell  <= d_l0_cell[L0_BITS +: L0_TAG];
            a_zb_addr  <= d_zb_addr[ZB_SEEN-1:0];
            a_l0_one   <= scan_done ? '0 : L0_SIZE'(1) << d_l0_cell[L0_BITS-1:0];
            a_l0_dst   <= d_l0_dst[L0_BITS +: L0_TAG];
            a_l0_put   <= scan_done ? '0 : L0_SIZE'(1) << d_l0_dst[L0_BITS-1:0];
            a_l0_blind <= scan_done;
            a_l0_pkill <= (scan_done || !d_pair || d_p_move) ? '0 : L0_SIZE'(1) << d_p_l0_cell[L0_BITS-1:0];
        end
    end

    logic drained;
    logic m0_valid, m1_valid, m2_valid, x_valid, w_valid;

    // Writes that would reach the cache after a read issued in the next cycle.
    logic m0_writes, m1_writes, m2_writes, older_writes, a_writes;
    assign older_writes = (m0_valid && (m0_writes || m0_p_writes)) || (m1_valid && (m1_writes || m1_p_writes)) ||
                          (m2_valid && (m2_writes || m2_p_writes));
    assign a_writes     = a_valid && (a_is_add || a_is_set || a_is_mul || a_is_in || (a_p_valid && !a_p_is_move));
    assign drained = !(m0_valid || m1_valid || m2_valid || x_valid || w_valid || redirect);

    logic out_pending;
    assign io_in_ready = a_valid && a_is_in && a_phase == 2'd1 && drained;

    always_ff @(posedge clk) begin
        if (rst || redirect || early_redirect) begin
            a_valid <= 1'b0;
        end else if (a_ready) begin
            a_valid <= d_valid;
        end

        if (rst) begin
            base_ptr <= '0;
        end else if (redirect) begin
            base_ptr <= w_redirect_base;
        end else if (early_redirect) begin
            base_ptr <= early_base;
        end else if (scan_done) begin
            base_ptr <= scan_base;
        end else if (a_issue && (a_moves || (a_is_scan && !a_scan_wide))) begin
            base_ptr <= moved;
        end

        if (a_ready) begin
            a_legal         <= d_legal;
            a_is_add        <= d_op == OP_A && d_legal;
            a_is_set        <= d_op == OP_S && d_legal;
            a_is_mul        <= d_is_mul;
            a_is_scan       <= d_is_scan;
            a_is_move       <= d_op == OP_M && d_legal;
            a_is_branch     <= d_is_branch;
            a_is_je         <= d_op == OP_JE;
            a_is_in         <= d_is_io && d_legal && !d_instr[0];
            a_is_out        <= d_is_io && d_legal && d_instr[0];
            a_is_halt       <= d_is_halt && d_legal;
            a_multi         <= d_multi;
            a_predict_taken <= d_predict_taken;
            a_mul_unit      <= d_mul_unit;
            a_mul_negate    <= d_instr[7:0] == 8'hFF;
            a_mul_chained   <= d_mul_chained;
            a_mul_pair      <= d_mul_pair;
            a_mul_source    <= d_pair ? d_p_addr : d0_short[ADDR_WIDTH-1:0];
            a_next_taken    <= d_next_taken;
            a_target_wrong  <= d_target_wrong && d_next_taken;
            a_slot          <= d_slot;
            a_counter       <= d_counter;
            a_data          <= d_instr[7:0];
            a_pc            <= d_pc;
            a_instr         <= d_instr;
            a_next_pc       <= d_is_halt ? d_pc : d_predict_taken ? d_target : d_seq;
            a_alt_pc        <= d_predict_taken ? d_seq : d_target;
            // A chained MUL starts in phase 0 like any other, but issues nothing there: its source is
            // in mul_operand already. Deciding that in D would put the compare before a_offset.
            a_phase         <= d_mul_pair ? 2'd2 : 2'd0;
            a_last          <= d_mul_pair;
            a_go            <= (!d0_is_scan || !(a_writes || older_writes)) && !d_mul_chained;
            a_scan_wide     <= d_scan_wide;
            a_scan_back     <= d_instr[19];
            a_scan_stride   <= d_scan_wide ? d_stride : '0;
            a_scan_more     <= 1'b0;
            a_offset        <= d_offset;
            a_offset_2      <= !d_pair ? (d0_is_mul ? d0_addr[ADDR_WIDTH-1:0] : d0_long[ADDR_WIDTH-1:0]) :
                               d_op == OP_M ? fq_fold[1] : d_p_move ? d_p_long : '0;
            a_moves         <= (d_op == OP_M && d_legal) || (d_pair && d_p_move);
            a_p_valid       <= d_pair;
            a_p_is_add      <= d_p_instr[31:28] == OP_A;
            a_p_is_set      <= d_p_instr[31:28] == OP_S;
            a_p_is_move     <= d_p_move;
            a_p_data        <= d_p_instr[7:0];
            a_p_pc          <= fq_pc[0];
            a_p_instr       <= d_p_instr;
            a_p_long        <= d_p_long;
        end else if (a_is_mul) begin
            unique case (a_phase)
                2'd0: if (!dmem_busy) begin // source issues now, then a gap for the multiplier, if any
                    a_go     <= a_mul_unit || a_mul_chained;
                    a_last   <= a_mul_unit || a_mul_chained;
                    a_offset <= a_offset_2;
                    a_phase  <= a_mul_unit || a_mul_chained ? 2'd2 : 2'd1;
                end
                2'd2: ;
                default: begin
                    a_go    <= 1'b1;
                    a_last  <= 1'b1;
                    a_phase <= 2'd2;
                end
            endcase
        end else if (a_is_in) begin
            // A read first, so that the line is in the cache and the write cannot miss: a byte
            // once taken cannot be asked for again.
            if (a_phase == 2'd0) begin
                if (!dmem_busy) begin
                    a_go    <= 1'b0;
                    a_phase <= 2'd1;
                end
            end else if (a_phase == 2'd1 && drained && io_in_valid) begin
                a_data  <= io_in_data;
                a_go    <= 1'b1;
                a_last  <= 1'b1;
                a_phase <= 2'd2;
            end
        end else if (a_is_out) begin
            if (!dmem_busy) a_go <= 1'b0; // the read issues now, and sets out_pending from X
            if (a_phase == 2'd0) begin
                if (!dmem_busy) a_phase <= 2'd1;
            end else if (drained && !out_pending) begin
                a_go   <= 1'b1;
                a_last <= 1'b1;
            end
        end else if (a_is_scan) begin
            if (scan_stop) begin
                a_go    <= 1'b0;
                a_phase <= 2'd1;
            end else if (a_phase == 2'd0 && !older_writes) begin
                a_go <= 1'b1;
            end
            if (a_issue && a_scan_wide) a_scan_more <= 1'b1;
        end else if (!dmem_busy) begin // H or an illegal word: issued, stays until flushed
            a_go <= 1'b0;
        end
    end

    // ---- M0: compares against everything that has not written yet ----
    // Each stage's fields, as one uop. pc, instr and base_after are only for the retire port.
    logic                  m0_uses_cache, m1_uses_cache, m2_uses_cache, x_uses_cache;
    logic                  m0_retire, m0_no_read, m0_is_branch, m0_is_je, m0_predict_taken;
    logic                  m0_is_scan, m0_is_out_read, m0_is_mul_src, m0_is_mul_dst, m0_is_halt, m0_is_trap;
    logic [7:0]            m0_data;
    logic [ADDR_WIDTH-1:0] m0_base, m0_base_after;
    logic [PC_WIDTH-1:0]   m0_pc, m0_next_pc, m0_alt_pc;
    logic [31:0]           m0_instr;
    logic                  m0_next_taken, m0_target_wrong, m0_mul_unit, m0_mul_negate, m0_pair;
    logic [ADDR_WIDTH-1:0] m0_ea2;
    logic [(1 << LINE_BITS)-1:0] m0_bank2;
    logic [3:0]            m0_slot;
    logic [1:0]            m0_counter;
    logic                  m0_l0_known, m0_l0_zero, m0_l0_fill;
    logic [3:0]            m0_l0_seq;

    logic                  m1_retire, m1_no_read, m1_is_branch, m1_is_je, m1_predict_taken;
    logic                  m1_is_scan, m1_is_out_read, m1_is_mul_src, m1_is_mul_dst, m1_is_halt, m1_is_trap;
    logic [7:0]            m1_data;
    logic [ADDR_WIDTH-1:0] m1_ea, m1_base, m1_base_after;
    logic [PC_WIDTH-1:0]   m1_pc, m1_next_pc, m1_alt_pc;
    logic [31:0]           m1_instr;
    logic                  m1_next_taken, m1_target_wrong, m1_mul_unit, m1_mul_negate, m1_pair;
    logic [ADDR_WIDTH-1:0] m1_ea2;
    logic [4:1]            m1_hit2;
    logic [3:0]            m1_slot;
    logic [1:0]            m1_counter;
    logic                  m1_early_fixed, m1_l0_fill;
    logic [3:0]            m1_l0_seq;
    logic [4:1]            m1_hit;

    logic                  m2_retire, m2_no_read, m2_is_branch, m2_is_je, m2_predict_taken;
    logic                  m2_is_scan, m2_is_out_read, m2_is_mul_src, m2_is_mul_dst, m2_is_halt, m2_is_trap;
    logic [7:0]            m2_data;
    logic [ADDR_WIDTH-1:0] m2_ea, m2_base, m2_base_after;
    logic [PC_WIDTH-1:0]   m2_pc, m2_next_pc, m2_alt_pc;
    logic [31:0]           m2_instr;
    logic                  m2_next_taken, m2_target_wrong, m2_mul_unit, m2_mul_negate;
    logic [3:0]            m2_slot;
    logic [1:0]            m2_counter;
    logic                  m2_early_fixed, m2_l0_fill;
    logic [3:0]            m2_l0_seq;
    logic                  m2_from_w, m2_from_w1, m2_from_w2, m2_from_w_next;
    logic                  m2_pair, m2_s_from_w, m2_s_from_w1, m2_s_from_w2, m2_s_from_w_next;
    logic [ADDR_WIDTH-1:0] m2_ea2;

    logic                  x_writes, x_retire, x_is_branch, x_is_je, x_predict_taken;
    logic                  x_is_scan, x_is_out_read, x_is_mul_src, x_is_mul_dst, x_is_halt, x_is_trap;
    logic [7:0]            x_data;
    logic [ADDR_WIDTH-1:0] x_ea, x_base, x_base_after;
    logic [PC_WIDTH-1:0]   x_pc, x_next_pc, x_alt_pc;
    logic [31:0]           x_instr;
    logic                  x_next_taken, x_target_wrong, x_mul_unit, x_mul_negate;
    logic [3:0]            x_slot;
    logic [1:0]            x_counter;
    logic                  x_early_fixed, x_l0_fill;
    logic [3:0]            x_l0_seq;
    logic                  x_from_w, x_from_early;
    logic [7:0]            x_early;
    logic                  x_pair, x_s_from_w, x_s_from_early;
    logic [7:0]            x_s_early;
    logic [ADDR_WIDTH-1:0] x_ea2;

    // The companion, from M0 on: the cell is ea2, p_base is base_ptr before it, and base after it is the
    // main lane's base. It goes where the main lane goes, and retires before it.
    logic                  m0_p_valid, m0_p_writes, m0_p_no_read, m1_p_valid, m1_p_writes, m1_p_no_read;
    logic                  m2_p_valid, m2_p_writes, m2_p_no_read, x_p_valid, x_p_writes, x_p_no_read, w_p_valid;
    logic [7:0]            m0_p_data, m1_p_data, m2_p_data, x_p_data;
    logic [PC_WIDTH-1:0]   m0_p_pc, m1_p_pc, m2_p_pc, x_p_pc, w_p_pc;
    logic [31:0]           m0_p_instr, m1_p_instr, m2_p_instr, x_p_instr, w_p_instr;
    logic [ADDR_WIDTH-1:0] m0_p_base, m1_p_base, m2_p_base, x_p_base, w_p_base;
    logic                  m0_uses2, m1_uses2, m2_uses2, x_uses2;   // the second port is read
    logic [ADDR_WIDTH-1:0] w_ea2, w_redirect_base;
    logic [7:0]            w_data2, w1_data2, w2_data2;
    logic                  w1_we2;
    logic [ZB_SEEN-1:0]    w1_ea2;

    // Forwarding, for each port: the older uops writing the cell, by stage and lane (p, the companion).
    logic [4:1]            m1_hitp, m1_hit2p;
    logic                  m2_from_w_next_p, m2_from_w_p, m2_from_w1_p, m2_from_w2_p;
    logic                  m2_s_from_w_next_p, m2_s_from_w_p, m2_s_from_w1_p, m2_s_from_w2_p;
    logic                  x_from_w_p, x_s_from_w_p;
    logic [7:0]            x_p_sum;

    logic                  w_writes, w_retire, w_is_halt, w_is_trap;
    logic [ADDR_WIDTH-1:0] w_ea, w_base, w_base_after, w_miss_ea;
    logic [PC_WIDTH-1:0]   w_pc, w_next_pc, w_alt_pc;
    logic [31:0]           w_instr;
    logic                  w_is_branch, w_taken, w_wrong_guess;
    logic [3:0]            w_slot;
    logic [1:0]            w_counter;
    logic [7:0]            w_data, w1_data, w2_data;

    // ---- X ----
    logic [7:0] x_value, x_source, x_addend, x_operand, mul_operand, product, x_sum;
    logic       x_zero, x_taken, x_wrong_way, x_mispredict, x_scan_found, x_miss;

    // F: which cells the read in each stage looks at. Stride and direction stay in A while it lasts.
    logic [LINE_CELLS-1:0] m0_scan_mask, m1_scan_mask, m2_scan_mask, x_scan_mask, x_scan_zeros, w_scan_zeros;
    logic [LINE_BITS-1:0]  m1_scan_cell, m2_scan_cell, s1_cell;
    logic [ADDR_WIDTH-LINE_BITS-1:0] s1_line;
    logic                  scan_found_1;
    logic [ADDR_WIDTH-1:0] scan_base;

    assign x_value  = x_from_w ? w_data : x_from_w_p ? w_data2 : x_from_early ? x_early : dmem_rdata;
    assign x_source = x_s_from_w ? w_data : x_s_from_w_p ? w_data2 : x_s_from_early ? x_s_early : dmem_rdata2;
    assign x_p_sum  = (x_p_no_read ? 8'd0 : x_source) + x_p_data;
    // The zero test is made of each source, before the choice, which then picks one bit.
    assign x_zero  = x_from_w ? w_data == 8'd0 : x_from_w_p ? w_data2 == 8'd0 : x_from_early ? x_early == 8'd0 : dmem_rdata == 8'd0;
    assign x_taken = x_zero == x_is_je;

    // A miss restarts the instruction, which is the same as a misprediction back to itself.
    assign x_miss       = x_valid && !flush && ((x_uses_cache && !dmem_hit) || (x_uses2 && !dmem_hit2));
    assign x_wrong_way  = x_taken != x_predict_taken;
    assign x_mispredict = x_valid && !flush && dmem_hit && x_is_branch && (x_wrong_way || x_target_wrong);
    assign x_scan_zeros = dmem_zero & x_scan_mask;
    assign x_scan_found = x_valid && !flush && dmem_hit && x_is_scan && |x_scan_zeros;

    // A factor of 1 or -1 takes the source's value as it is: from the second port, or else straight
    // from X a cycle before. Subtracting it is adding its complement and a carry.
    assign x_addend  = !x_is_mul_dst ? x_data : !x_mul_unit ? product : x_mul_negate ? -mul_operand : mul_operand;
    assign x_operand = !x_pair ? x_addend : x_mul_negate ? ~x_source : x_source;
    assign x_sum     = x_value + x_operand + 8'(x_pair && x_mul_negate);

    // ---- early resolution ----

    // The guesses: from A, whether the early store holds a value for the cell, and whether it is
    // zero; in M0, whether the zero bit is to be believed, and the bit. Both are acted on in M0.
    logic       a_l0_known, a_l0_zero, m0_zb_known, m0_zb_zero;
    logic       a_l0_alloc;  // A leaves its cell pending in the store, for this uop's X to fill in
    logic [3:0] l0_seq;      // numbers the uops leaving A, so that a pending cell knows whose fill it waits for

    logic m0_known, m0_zero, m0_early_wrong, m0_flip;
    assign m0_known       = m0_l0_known || m0_zb_known;
    assign m0_zero        = m0_l0_known ? m0_l0_zero : m0_zb_zero;
    assign m0_early_wrong = m0_valid && !flush && !early_redirect && m0_is_branch && m0_known &&
                            (m0_zero == m0_is_je) != m0_predict_taken;

    // A branch that resolved early has its guess turned as it goes on to M1.
    assign m0_flip = m0_early_wrong;

    always_ff @(posedge clk) begin
        early_redirect <= !rst && m0_early_wrong;
        early_pc       <= m0_alt_pc;
        early_key      <= m0_pc;
        early_base     <= m0_base_after;
    end

`ifdef FORMAL
    // Any guess at all: what the core does must not depend on them.
    (* anyseq *) logic f_l0_known, f_l0_zero, f_zb_known, f_zb_zero;
    assign a_l0_known  = f_l0_known;
    assign a_l0_zero   = f_l0_zero;
    assign m0_zb_known = f_zb_known;
    assign m0_zb_zero  = f_zb_zero;
    assign a_l0_alloc  = 1'b0;
    assign l0_seq      = '0;
`else
    // The early store. A cell is empty, pending, or has its value as of the youngest uop through A.
    // A pending cell waits for its owner's X, and its value holds the adds since, to go on top.
    // Each slot works out its own match, sums and next state, so that no lookup comes before a write;
    // and it keeps whether its value is zero, for branches.
    localparam logic [1:0] L0_EMPTY = 2'd0, L0_PENDING = 2'd1, L0_VALID = 2'd2;
    // Packed, so that they are registers: every cell is read and emptied at once.
    logic [L0_SIZE-1:0][1:0]        l0_state;
    logic [L0_SIZE-1:0][L0_TAG-1:0] l0_tag;
    logic [L0_SIZE-1:0][7:0]        l0_value;
    logic [L0_SIZE-1:0][3:0]        l0_owner;
    logic [L0_SIZE-1:0]             l0_zero;

    // A's uop looks up the cell of A's first uop, which D worked out; a MUL's destination, which D also
    // worked out, only ever goes pending.
    logic [L0_SIZE-1:0] a_l0_hit, a_l0_valid, w_l0_fills;
    logic               a_l0_match, a_l0_touch, a_l0_set;
    assign a_l0_touch = a_is_add || a_is_set || a_is_mul || a_is_branch || a_is_in || (a_is_out && !a_last);
    assign a_l0_set   = a_is_set || (a_is_in && a_last);
    for (genvar i = 0; i < L0_SIZE; i++) begin : l0_slot
        assign a_l0_hit[i]   = a_l0_one[i] && l0_state[i] != L0_EMPTY && l0_tag[i] == a_l0_cell;
        assign a_l0_valid[i] = a_l0_one[i] && l0_state[i] == L0_VALID && l0_tag[i] == a_l0_cell;
    end
    assign a_l0_match = |a_l0_hit;
    assign a_l0_alloc = a_l0_touch && (a_flag_mul_dst || (!a_l0_match && !a_l0_set));

    // X's value reaches the store from W, so that nothing follows X's adder but a register. Whether the
    // slot still waits for it, and the adds since, are read in X, and stand unless A's uop takes the
    // slot or adds to it meanwhile; an add lets the cell go.
    logic       w_l0_fill, w_l0_zero;
    logic [3:0] w_l0_seq;
    logic [7:0] w_l0_base, w_l0_since, x_l0_since, w_l0_filled;
    always_comb begin
        x_l0_since = '0;
        for (int i = 0; i < L0_SIZE; i++) if (x_ea[L0_BITS-1:0] == L0_BITS'(i)) x_l0_since = l0_value[i];
    end
    always_ff @(posedge clk) begin
        w_l0_fill  <= x_valid && !flush && !x_miss && x_l0_fill;
        w_l0_seq   <= x_l0_seq;
        w_l0_base  <= x_writes ? x_sum : x_value;
        w_l0_since <= x_l0_since;
    end
    assign w_l0_filled = w_l0_base + w_l0_since;

    // Whether a + b is zero, without the carries: bit i of a ^ b must be the carry into it, which for a
    // zero sum is a | b one bit down.
    function automatic logic sums_to_zero(input logic [7:0] a, input logic [7:0] b);
        sums_to_zero = (a ^ b) == {a[6:0] | b[6:0], 1'b0};
    endfunction
    assign w_l0_zero = sums_to_zero(w_l0_base, w_l0_since);

    // a_go_on comes late, so each slot works out its next state both ways, and it only picks.
    logic l0_kill, a_data_zero;
    assign l0_kill     = rst || flush || early_redirect || (a_valid && a_l0_blind);
    assign a_data_zero = a_data == 8'd0;
    for (genvar i = 0; i < L0_SIZE; i++) begin : l0_write
        logic       x_waits, w_waits, w_taken, w_added, sel_add, sel_set, alloc, takes, sums, lets_go, go, pkill;
        logic       z_add, held_zero, go_zero;
        logic [1:0] held_state, go_state;
        logic [7:0] added, held_value, go_value;
        assign x_waits = l0_state[i] == L0_PENDING && x_ea[L0_BITS-1:0] == L0_BITS'(i) &&
                         l0_tag[i] == x_ea[L0_BITS +: L0_TAG] && l0_owner[i] == x_l0_seq;
        always_ff @(posedge clk) begin
            w_waits <= x_valid && !flush && !x_miss && x_l0_fill && x_waits;
            w_taken <= l0_kill || (go && takes) || pkill;
            w_added <= go && sums;
        end
        assign w_l0_fills[i] = w_l0_fill && w_waits && !w_taken && !w_added;
        assign lets_go       = w_l0_fill && w_waits && !w_taken && w_added;

        // What A's uop does to this slot, if it goes on: an add or a set to its cell, a read of it, or
        // a MUL's destination, which goes pending.
        assign sel_add = a_is_add && a_l0_one[i];
        assign sel_set = a_l0_set && a_l0_one[i];
        assign alloc   = (a_flag_mul_dst && a_l0_put[i]) ||
                         (a_l0_one[i] && a_l0_touch && !a_flag_mul_dst && !a_l0_set && !a_l0_hit[i]);
        assign takes   = sel_set || (sel_add && a_l0_valid[i]) || alloc;
        assign sums    = sel_add && a_l0_hit[i] && !a_l0_valid[i];   // to a pending value
        assign added   = l0_value[i] + a_data;
        assign z_add   = sums_to_zero(l0_value[i], a_data);

        // Without A's uop: a fill, or nothing.
        // The companion's write, older than A's uop, empties the slot unless that uop uses it: the cells
        // differ, and the uop only reads the slot if it is the uop's cell.
        assign pkill      = a_issue && a_l0_pkill[i];
        assign held_state = (l0_kill || lets_go || pkill) ? L0_EMPTY : w_l0_fills[i] ? L0_VALID : l0_state[i];
        assign held_value = w_l0_fills[i] ? w_l0_filled : l0_value[i];
        assign held_zero  = w_l0_fills[i] ? w_l0_zero : l0_zero[i];
        // With it, younger than the fill: it goes over it, and an add to a pending cell filled now,
        // which would need both sums at once, lets the cell go. The zero bit matters only when valid.
        assign go_state = l0_kill ? L0_EMPTY : sel_set ? L0_VALID :
                          sel_add ? (a_l0_valid[i] ? L0_VALID : (a_l0_hit[i] && (w_l0_fills[i] || lets_go)) ? L0_EMPTY : L0_PENDING) :
                          alloc ? L0_PENDING : held_state;
        assign go_value = sel_set ? a_data : sel_add ? (a_l0_hit[i] ? added : 8'd0) : alloc ? 8'd0 : held_value;
        assign go_zero  = sel_set ? a_data_zero : held_zero;
        // Only A's uop's slots can differ both ways, which keeps a_go_on's own load small.
        assign go = a_issue && (a_l0_one[i] || a_l0_put[i]);
        always_ff @(posedge clk) begin
            l0_state[i] <= go ? go_state : held_state;
            l0_value[i] <= go ? go_value : held_value;
            l0_zero[i]  <= (go && sel_add) ? z_add : go ? go_zero : held_zero;
            if (go && takes) begin
                l0_tag[i]   <= a_flag_mul_dst ? a_l0_dst : a_l0_cell;
                l0_owner[i] <= l0_seq;
            end
        end
    end
    always_ff @(posedge clk) begin
        if (rst) l0_seq <= '0;
        else if (a_go_on) l0_seq <= l0_seq + 4'd1;
    end

    // The zero bits, read with the address of a branch's cell.
    logic a_fresh;   // A has a word D gave it last cycle, or none
    always_ff @(posedge clk) a_fresh <= a_ready;
    logic zb_zero, m0_zb_clean;

    zero_bits #(.ADDR_WIDTH(ADDR_WIDTH)) zero_bits_i (
        .clk(clk), .rd_en(a_fresh), .raddr(d_zb_addr), .zero(zb_zero),
        .we(dmem_we), .waddr(w_ea), .wzero(w_data == 8'd0),
        .we2(dmem_we2), .waddr2(w_ea2), .wzero2(w_data2 == 8'd0)
    );

    // The bit was read as the branch went from D to A: writes from then on are not in it. Those are
    // the ones in M1 to W once the branch is in M0, and the one that left W since, and one more: as
    // it is in A, M0 to W and the one that just left W, which A compares, for M0 to have the answer.
    logic                  w1_we, a_zb_clean;
    logic [ZB_SEEN-1:0]    w1_ea;
    always_ff @(posedge clk) begin
        w1_we <= !rst && dmem_we;
        w1_ea <= w_ea[ZB_SEEN-1:0];
        w1_we2 <= !rst && dmem_we2;
        w1_ea2 <= w_ea2[ZB_SEEN-1:0];
    end
    assign a_zb_clean = !(m0_valid && m0_writes && a_zb_addr == m0_ea[ZB_SEEN-1:0]) &&
                        !(m1_valid && m1_writes && a_zb_addr == m1_ea[ZB_SEEN-1:0]) &&
                        !(m2_valid && m2_writes && a_zb_addr == m2_ea[ZB_SEEN-1:0]) &&
                        !(x_valid && x_writes && a_zb_addr == x_ea[ZB_SEEN-1:0]) &&
                        !(dmem_we && a_zb_addr == w_ea[ZB_SEEN-1:0]) && !(w1_we && a_zb_addr == w1_ea[ZB_SEEN-1:0]) &&
                        !(m0_valid && m0_p_writes && a_zb_addr == m0_ea2[ZB_SEEN-1:0]) &&
                        !(m1_valid && m1_p_writes && a_zb_addr == m1_ea2[ZB_SEEN-1:0]) &&
                        !(m2_valid && m2_p_writes && a_zb_addr == m2_ea2[ZB_SEEN-1:0]) &&
                        !(x_valid && x_p_writes && a_zb_addr == x_ea2[ZB_SEEN-1:0]) &&
                        !(dmem_we2 && a_zb_addr == w_ea2[ZB_SEEN-1:0]) && !(w1_we2 && a_zb_addr == w1_ea2[ZB_SEEN-1:0]);
    always_ff @(posedge clk) m0_zb_clean <= a_zb_clean;

`ifdef BRAINFUNK_RETIRE_PORT
    logic [31:0] noise = 32'hC0DE_5EED;
    always_ff @(posedge clk) noise <= {noise[30:0], noise[31] ^ noise[21] ^ noise[1] ^ noise[0]};
    logic a_l0_found;
    assign a_l0_found  = |(a_l0_valid & l0_zero);
    assign a_l0_known  = predict_chaos ? noise[0] : |a_l0_valid;
    assign a_l0_zero   = predict_chaos ? noise[1] : a_l0_found;
    assign m0_zb_known = predict_chaos ? noise[2] : m0_zb_clean;
    assign m0_zb_zero  = predict_chaos ? noise[3] : zb_zero;
`else
    assign a_l0_known  = |a_l0_valid;
    assign a_l0_zero   = |(a_l0_valid & l0_zero);
    assign m0_zb_known = m0_zb_clean;
    assign m0_zb_zero  = zb_zero;
`endif
`endif

    always_ff @(posedge clk) begin
        if (rst) begin
            m0_valid  <= 1'b0;
            m1_valid  <= 1'b0;
            m2_valid  <= 1'b0;
            x_valid   <= 1'b0;
            w_valid   <= 1'b0;
            redirect  <= 1'b0;
            scan_stop <= 1'b0;
            scan_found_1 <= 1'b0;
            scan_done <= 1'b0;
            dmem_miss <= 1'b0;
            dmem_we   <= 1'b0;
            dmem_we2  <= 1'b0;
            dmem_wbank2 <= '0;
        end else begin
            m0_valid  <= a_go_on;
            m1_valid  <= m0_valid && !flush && !early_redirect;
            m2_valid  <= m1_valid && !flush;
            x_valid   <= m2_valid && !flush;
            w_valid   <= x_valid && !flush && !x_miss;
            dmem_we   <= x_valid && !flush && !x_miss && x_writes;
            dmem_we2  <= x_valid && !flush && !x_miss && x_p_valid && x_p_writes;
            // Without x_miss and flush, which come late: those take dmem_we away too.
            dmem_wbank2 <= (x_valid && x_p_valid && x_p_writes) ? (1 << LINE_BITS)'(1) << x_ea2[LINE_BITS-1:0] : '0;
            redirect  <= x_mispredict || x_miss;
            scan_stop <= x_scan_found;
            scan_found_1 <= scan_stop;
            scan_done <= scan_found_1;
            dmem_miss <= x_miss;
        end

        // A to M0
        // The address only moves on from a line that has been read.
        if (!a_scan_more || a_issue) begin
            m0_ea        <= a_scan_more ? scan_next_ea : ea;
            m0_scan_cell <= a_scan_more ? scan_next_cell : ea[LINE_BITS-1:0] ^ {LINE_BITS{a_scan_back}};
        end
        m0_scan_mask     <= STRIDE_MASK[a_scan_stride];
        m0_base          <= (a_p_valid && a_p_is_move) ? base_ptr + a_p_long : base_ptr;   // the main lane's
        m0_base_after    <= a_moves ? moved : base_ptr;
        m0_writes        <= a_is_add || a_is_set || a_flag_mul_dst || (a_is_in && a_last);
        m0_uses_cache    <= a_is_add || a_is_set || a_is_mul || a_is_scan || a_is_branch || a_is_in ||
                            (a_is_out && !a_last);
        m0_retire        <= a_legal && (!a_multi || a_last || a_is_scan || a_is_halt);
        m0_no_read       <= a_is_set || (a_is_in && a_last);
        m0_is_branch     <= a_is_branch;
        m0_is_je         <= a_is_je;
        m0_predict_taken <= a_predict_taken;
        m0_is_scan       <= a_is_scan;
        m0_is_out_read   <= a_is_out && !a_last;
        m0_is_mul_src    <= a_is_mul && !a_last;
        m0_is_mul_dst    <= a_flag_mul_dst;
        m0_is_halt       <= a_is_halt;
        m0_is_trap       <= !a_legal;
        m0_data          <= a_data;
        m0_pc            <= a_pc;
        m0_next_pc       <= a_next_pc;
        m0_alt_pc        <= a_alt_pc;
        m0_instr         <= a_instr;
        m0_next_taken    <= a_next_taken;
        m0_mul_unit      <= a_mul_unit;
        m0_mul_negate    <= a_mul_negate;
        m0_pair          <= a_is_mul && a_mul_pair;
        m0_ea2           <= ea2;
        // as m0_valid, which the cache would otherwise have to decode on its way to the block RAM
        m0_bank2         <= (!rst && a_go_on && ((a_is_mul && a_mul_pair) || (a_p_valid && !a_p_is_move))) ?
                            (1 << LINE_BITS)'(1) << ea2[LINE_BITS-1:0] : '0;
        m0_uses2         <= (a_is_mul && a_mul_pair) || (a_p_valid && !a_p_is_move);
        m0_p_valid       <= a_p_valid;
        m0_p_writes      <= a_p_valid && !a_p_is_move;
        m0_p_no_read     <= a_p_is_set;
        m0_p_data        <= a_p_data;
        m0_p_pc          <= a_p_pc;
        m0_p_instr       <= a_p_instr;
        m0_p_base        <= base_ptr;
        m0_target_wrong  <= a_target_wrong;
        m0_l0_known      <= a_l0_known && a_is_branch;
        m0_l0_zero       <= a_l0_zero;
        m0_l0_fill       <= a_l0_alloc;
        m0_l0_seq        <= l0_seq;
        m0_slot           <= a_slot;
        m0_counter       <= a_counter;

        // M0 to M1: which older instructions write this cell, nearest first.
        // m1 is one ahead, m2 two, x three, w four; the older the instruction, the later its
        // result reaches a register this one can use.
        m1_hit[1] <= m1_valid && m1_writes && m0_ea == m1_ea;
        m1_hit[2] <= m2_valid && m2_writes && m0_ea == m2_ea;
        m1_hit[3] <= x_valid && x_writes && m0_ea == x_ea;
        m1_hit[4] <= w_valid && w_writes && m0_ea == w_ea;
        m1_hit2[1] <= m1_valid && m1_writes && m0_ea2 == m1_ea;
        m1_hit2[2] <= m2_valid && m2_writes && m0_ea2 == m2_ea;
        m1_hit2[3] <= x_valid && x_writes && m0_ea2 == x_ea;
        m1_hit2[4] <= w_valid && w_writes && m0_ea2 == w_ea;
        m1_hitp[1] <= m1_valid && m1_p_writes && m0_ea == m1_ea2;
        m1_hitp[2] <= m2_valid && m2_p_writes && m0_ea == m2_ea2;
        m1_hitp[3] <= x_valid && x_p_writes && m0_ea == x_ea2;
        m1_hitp[4] <= dmem_we2 && m0_ea == w_ea2;
        m1_hit2p[1] <= m1_valid && m1_p_writes && m0_ea2 == m1_ea2;
        m1_hit2p[2] <= m2_valid && m2_p_writes && m0_ea2 == m2_ea2;
        m1_hit2p[3] <= x_valid && x_p_writes && m0_ea2 == x_ea2;
        m1_hit2p[4] <= dmem_we2 && m0_ea2 == w_ea2;
        m1_p_valid      <= m0_p_valid;
        m1_p_writes     <= m0_p_writes;
        m1_p_no_read    <= m0_p_no_read;
        m1_p_data       <= m0_p_data;
        m1_p_pc         <= m0_p_pc;
        m1_p_instr      <= m0_p_instr;
        m1_p_base <= m0_p_base;
        m1_uses2        <= m0_uses2;

        m1_ea            <= m0_ea;
        m1_base          <= m0_is_scan ? m0_ea - a_offset : m0_base;
        m1_scan_cell     <= m0_scan_cell;
        m1_scan_mask     <= m0_scan_mask << (8 * (m0_scan_cell >> 3));
        m1_base_after    <= m0_base_after;
        m1_writes        <= m0_writes;
        m1_uses_cache    <= m0_uses_cache;
        m1_retire        <= m0_retire;
        m1_no_read       <= m0_no_read;
        m1_is_branch     <= m0_is_branch;
        m1_is_je         <= m0_is_je;
        m1_predict_taken <= m0_flip ? !m0_predict_taken : m0_predict_taken;
        m1_is_scan       <= m0_is_scan;
        m1_is_out_read   <= m0_is_out_read;
        m1_is_mul_src    <= m0_is_mul_src;
        m1_is_mul_dst    <= m0_is_mul_dst;
        m1_is_halt       <= m0_is_halt;
        m1_is_trap       <= m0_is_trap;
        m1_data          <= m0_data;
        m1_pc            <= m0_pc;
        m1_next_pc       <= m0_flip ? m0_alt_pc : m0_next_pc;
        m1_alt_pc        <= m0_flip ? m0_next_pc : m0_alt_pc;
        m1_instr         <= m0_instr;
        m1_next_taken    <= m0_next_taken;
        m1_mul_unit      <= m0_mul_unit;
        m1_mul_negate    <= m0_mul_negate;
        m1_pair          <= m0_pair;
        m1_ea2           <= m0_ea2;
        m1_target_wrong  <= m0_target_wrong && !m0_flip;
        m1_early_fixed   <= m0_flip;
        m1_l0_fill       <= m0_l0_fill;
        m1_l0_seq        <= m0_l0_seq;
        m1_slot           <= m0_slot;
        m1_counter       <= m0_counter;

        // M1 to M2: only the nearest writer counts. The two lanes of a pair never write the same cell.
        m2_from_w_next   <= m1_hit[1];
        m2_from_w_next_p <= m1_hitp[1];
        m2_from_w        <= !(m1_hit[1] || m1_hitp[1]) && m1_hit[2];
        m2_from_w_p      <= !(m1_hit[1] || m1_hitp[1]) && m1_hitp[2];
        m2_from_w1       <= !(m1_hit[1] || m1_hitp[1]) && !(m1_hit[2] || m1_hitp[2]) && m1_hit[3];
        m2_from_w1_p     <= !(m1_hit[1] || m1_hitp[1]) && !(m1_hit[2] || m1_hitp[2]) && m1_hitp[3];
        m2_from_w2       <= !(m1_hit[1] || m1_hitp[1]) && !(m1_hit[2] || m1_hitp[2]) && !(m1_hit[3] || m1_hitp[3]) && m1_hit[4];
        m2_from_w2_p     <= !(m1_hit[1] || m1_hitp[1]) && !(m1_hit[2] || m1_hitp[2]) && !(m1_hit[3] || m1_hitp[3]) && m1_hitp[4];
        m2_s_from_w_next   <= m1_hit2[1];
        m2_s_from_w_next_p <= m1_hit2p[1];
        m2_s_from_w        <= !(m1_hit2[1] || m1_hit2p[1]) && m1_hit2[2];
        m2_s_from_w_p      <= !(m1_hit2[1] || m1_hit2p[1]) && m1_hit2p[2];
        m2_s_from_w1       <= !(m1_hit2[1] || m1_hit2p[1]) && !(m1_hit2[2] || m1_hit2p[2]) && m1_hit2[3];
        m2_s_from_w1_p     <= !(m1_hit2[1] || m1_hit2p[1]) && !(m1_hit2[2] || m1_hit2p[2]) && m1_hit2p[3];
        m2_s_from_w2       <= !(m1_hit2[1] || m1_hit2p[1]) && !(m1_hit2[2] || m1_hit2p[2]) && !(m1_hit2[3] || m1_hit2p[3]) && m1_hit2[4];
        m2_s_from_w2_p     <= !(m1_hit2[1] || m1_hit2p[1]) && !(m1_hit2[2] || m1_hit2p[2]) && !(m1_hit2[3] || m1_hit2p[3]) && m1_hit2p[4];
        m2_p_valid      <= m1_p_valid;
        m2_p_writes     <= m1_p_writes;
        m2_p_no_read    <= m1_p_no_read;
        m2_p_data       <= m1_p_data;
        m2_p_pc         <= m1_p_pc;
        m2_p_instr      <= m1_p_instr;
        m2_p_base <= m1_p_base;
        m2_uses2        <= m1_uses2;

        m2_ea            <= m1_ea;
        m2_scan_cell     <= m1_scan_cell;
        m2_scan_mask     <= m1_scan_mask << (2 * ((m1_scan_cell >> 1) & LINE_BITS'(3)));
        m2_base          <= m1_base;
        m2_base_after    <= m1_base_after;
        m2_writes        <= m1_writes;
        m2_uses_cache    <= m1_uses_cache;
        m2_retire        <= m1_retire;
        m2_no_read       <= m1_no_read;
        m2_is_branch     <= m1_is_branch;
        m2_is_je         <= m1_is_je;
        m2_predict_taken <= m1_predict_taken;
        m2_is_scan       <= m1_is_scan;
        m2_is_out_read   <= m1_is_out_read;
        m2_is_mul_src    <= m1_is_mul_src;
        m2_is_mul_dst    <= m1_is_mul_dst;
        m2_is_halt       <= m1_is_halt;
        m2_is_trap       <= m1_is_trap;
        m2_data          <= m1_data;
        m2_pc            <= m1_pc;
        m2_next_pc       <= m1_next_pc;
        m2_alt_pc        <= m1_alt_pc;
        m2_instr         <= m1_instr;
        m2_next_taken    <= m1_next_taken;
        m2_mul_unit      <= m1_mul_unit;
        m2_mul_negate    <= m1_mul_negate;
        m2_pair          <= m1_pair;
        m2_ea2           <= m1_ea2;
        m2_target_wrong  <= m1_target_wrong;
        m2_early_fixed   <= m1_early_fixed;
        m2_l0_fill       <= m1_l0_fill;
        m2_l0_seq        <= m1_l0_seq;
        m2_slot           <= m1_slot;
        m2_counter       <= m1_counter;

        // M2 to X. Results that are already in registers are picked up here; only the
        // instruction right ahead has to be taken at X itself. A cell that is written
        // without being read adds its data to zero.
        x_from_w     <= !m2_no_read && m2_from_w_next;
        x_from_w_p   <= !m2_no_read && m2_from_w_next_p;
        x_from_early <= m2_no_read || m2_from_w || m2_from_w1 || m2_from_w2 || m2_from_w_p || m2_from_w1_p || m2_from_w2_p;
        x_early      <= m2_no_read ? 8'd0 : m2_from_w ? w_data : m2_from_w_p ? w_data2 : m2_from_w1 ? w1_data :
                        m2_from_w1_p ? w1_data2 : m2_from_w2 ? w2_data : m2_from_w2_p ? w2_data2 : 8'd0;
        x_s_from_w     <= m2_s_from_w_next;
        x_s_from_w_p   <= m2_s_from_w_next_p;
        x_s_from_early <= m2_s_from_w || m2_s_from_w1 || m2_s_from_w2 || m2_s_from_w_p || m2_s_from_w1_p || m2_s_from_w2_p;
        x_s_early      <= m2_s_from_w ? w_data : m2_s_from_w_p ? w_data2 : m2_s_from_w1 ? w1_data :
                          m2_s_from_w1_p ? w1_data2 : m2_s_from_w2 ? w2_data : w2_data2;
        x_p_valid      <= m2_p_valid;
        x_p_writes     <= m2_p_writes;
        x_p_no_read    <= m2_p_no_read;
        x_p_data       <= m2_p_data;
        x_p_pc         <= m2_p_pc;
        x_p_instr      <= m2_p_instr;
        x_p_base <= m2_p_base;
        x_uses2        <= m2_uses2;

        x_ea            <= m2_ea;
        x_scan_mask     <= a_scan_back ? reversed(m2_scan_mask << m2_scan_cell[0]) : m2_scan_mask << m2_scan_cell[0];
        x_base          <= m2_base;
        x_base_after    <= m2_base_after;
        x_writes        <= m2_writes;
        x_uses_cache    <= m2_uses_cache;
        x_retire        <= m2_retire;
        x_is_branch     <= m2_is_branch;
        x_is_je         <= m2_is_je;
        x_predict_taken <= m2_predict_taken;
        x_is_scan       <= m2_is_scan;
        x_is_out_read   <= m2_is_out_read;
        x_is_mul_src    <= m2_is_mul_src;
        x_is_mul_dst    <= m2_is_mul_dst;
        x_is_halt       <= m2_is_halt;
        x_is_trap       <= m2_is_trap;
        x_data          <= m2_data;
        x_pc            <= m2_pc;
        x_next_pc       <= m2_next_pc;
        x_alt_pc        <= m2_alt_pc;
        x_instr         <= m2_instr;
        x_next_taken    <= m2_next_taken;
        x_mul_unit      <= m2_mul_unit;
        x_mul_negate    <= m2_mul_negate;
        x_pair          <= m2_pair;
        x_ea2           <= m2_ea2;
        x_target_wrong  <= m2_target_wrong;
        x_early_fixed   <= m2_early_fixed;
        x_l0_fill       <= m2_l0_fill;
        x_l0_seq        <= m2_l0_seq;
        x_slot           <= m2_slot;
        x_counter       <= m2_counter;

        // Multiplier: operand at the end of X, product a cycle later with the factor of the destination
        // then in M2, used by that destination's X.
        if (x_valid && x_is_mul_src) mul_operand <= x_value;
`ifdef FORMAL_ABSTRACT_MUL
        product <= mul_operand ^ m2_data;
`else
        product <= 8'(mul_operand * m2_data);
`endif

        // X to W. F does not retire from here: it retires when it has its result, two cycles after.
        w_writes     <= x_writes;
        w_retire     <= !x_is_scan && x_retire;
        w_is_halt    <= x_is_halt;
        w_is_trap    <= x_is_trap;
        w_ea         <= x_ea;
        w_miss_ea    <= (x_uses_cache && !dmem_hit) ? x_ea : x_ea2;
        w_redirect_base <= x_miss ? (x_p_valid ? x_p_base : x_base) : x_base_after;
        w_p_valid    <= x_p_valid;
        w_p_pc       <= x_p_pc;
        w_p_instr    <= x_p_instr;
        w_p_base <= x_p_base;
        w_ea2        <= x_ea2;
        w_data2      <= x_p_sum;
        w1_data2     <= w_data2;
        w2_data2     <= w1_data2;
        w_base       <= x_base;
        w_base_after <= x_base_after;
        w_pc         <= x_pc;
        w_next_pc    <= (x_is_branch && x_wrong_way) ? x_alt_pc : x_next_pc;
        w_alt_pc     <= x_miss ? (x_p_valid ? x_p_pc : x_pc) : (x_is_branch && x_wrong_way) ? x_alt_pc : x_next_pc;
        w_instr      <= x_instr;
        w_is_branch  <= x_is_branch;
        w_taken      <= x_taken;
        w_wrong_guess <= (x_mispredict || (x_valid && !flush && x_early_fixed)) && x_next_taken;
        w_slot        <= x_slot;
        w_counter    <= x_counter;
        w_data       <= x_sum;
        w_scan_zeros <= a_scan_back ? reversed(x_scan_zeros) : x_scan_zeros;
        s1_cell      <= lowest_cell(w_scan_zeros) ^ {LINE_BITS{a_scan_back}};
        s1_line      <= w_ea[ADDR_WIDTH-1:LINE_BITS];
        scan_base    <= {s1_line, s1_cell} - a_offset;
        w1_data      <= w_data;
        w2_data      <= w1_data;

        if (rst) begin
            out_pending <= 1'b0;
            halted      <= 1'b0;
            trapped     <= 1'b0;
        end else begin
            if (x_valid && !flush && dmem_hit && x_is_out_read) out_pending <= 1'b1;
            else if (out_pending && io_out_ready)   out_pending <= 1'b0;
            if (w_valid && w_is_halt) halted <= 1'b1;
            if (w_valid && w_is_trap) trapped <= 1'b1;
        end
        if (x_valid && x_is_out_read) io_out_data <= x_value;
    end

    assign io_out_valid = out_pending;

    // ---- what the predictor learns ----
    assign pred_front_en = front_en;
    assign pred_fetch_pc  = fetch_pc;
    assign pred_other_pc  = other_pc;
    assign pred_use_other = use_other;
    assign pred_steer_pc  = q_pc;
    assign pred_use_steer = use_steer;
    assign pred_look_en  = look_en;

    // A guess proved wrong comes out; Q's redirect goes in. Both act in registers, so that neither
    // lengthens the path they start from.
    logic                q_learn, w_forget;
    logic                q_seq_forget;
    assign q_learn      = front_en && !redirect && !early_redirect && q_steer_taken;
    assign q_seq_forget = front_en && !redirect && !early_redirect && q_steer_seq;
    assign w_forget     = w_valid && w_wrong_guess;
    always_ff @(posedge clk) begin
        next_we         <= !rst && (w_forget || q_learn || q_seq_forget);
        next_key        <= q_key;
        if (w_forget) next_key[3:0] <= w_slot;
        next_valid      <= !w_forget && q_learn;
        next_new_pos    <= q_last;
        next_new_target <= q_target[q_last];
    end

    // Counts how far this branch disagrees with its static guess.
    logic w_agrees;
    assign w_agrees    = w_taken == w_instr[19];
    assign dir_we      = w_valid && w_is_branch;
    assign dir_pc      = w_pc;
    assign dir_counter = w_agrees ? (w_counter == 2'd0 ? 2'd0 : w_counter - 2'd1)
                                  : (w_counter == 2'd3 ? 2'd3 : w_counter + 2'd1);

    assign dmem_raddr  = m0_ea;
    assign dmem_bank2  = m0_bank2;
    assign dmem_raddr2 = m0_ea2;
    assign dmem_waddr = w_ea;
    assign dmem_wdata = w_data;
    assign dmem_waddr2 = w_ea2;
    assign dmem_wdata2 = w_data2;
    assign dmem_miss_addr = w_miss_ea;

    assign retire2_valid   = w_valid && w_p_valid;
    assign retire2_pc      = w_p_pc;
    assign retire2_instr   = w_p_instr;
    assign retire2_base_ptr = w_base;
    assign retire_valid    = (w_valid && w_retire) || scan_done;
    assign retire_pc       = w_pc;
    assign retire_instr    = w_instr;
    assign retire_next_pc  = w_next_pc;
    assign retire_base_ptr = scan_done ? scan_base : w_base_after;

`ifdef FORMAL
`include "core_props.svh"
`endif

endmodule

// What Q needs of a word to pair it, worked out by the I-cache as it reads the word, so that it
// arrives in a register with it: the word's kind, the low bits of its cell, and its cell's or move's
// offset from base_ptr. A, S and M with nonzero top bits trap, so they are of no kind.
module brainfunk_predecode #(
    parameter int ADDR_WIDTH = 16,
    parameter int LINE_BITS  = 5
) (
    input  logic [31:0]                       word,
    // {branch, simple (A, S or M), has a cell (A, S or a branch), writes (A or S), M, cell, offset}
    output logic [4 + LINE_BITS + ADDR_WIDTH:0] pre
);
    localparam logic [3:0] OP_A = 4'd1, OP_S = 4'd2, OP_M = 4'd5, OP_JE = 4'd6, OP_JN = 4'd7;
    logic [3:0]            op;
    logic                  top_zero, branch, first_ok, writes, move;
    logic [LINE_BITS-1:0]  low_cell;
    logic [ADDR_WIDTH-1:0] long_offset, short_offset, addr_offset, own;
    assign op           = word[31:28];
    assign top_zero     = word[27:20] == 8'd0;
    assign branch       = op == OP_JE || op == OP_JN;
    assign first_ok     = (op == OP_A || op == OP_S || op == OP_M) && top_zero;
    assign writes       = (op == OP_A || op == OP_S) && top_zero;
    assign move         = op == OP_M && top_zero;
    assign low_cell     = branch ? word[20 +: LINE_BITS] : word[8 +: LINE_BITS];
    assign long_offset  = ADDR_WIDTH'($signed(word[19:0]));
    assign short_offset = ADDR_WIDTH'($signed(word[27:20]));
    assign addr_offset  = ADDR_WIDTH'($signed(word[19:8]));
    assign own          = op == OP_M ? long_offset : branch ? short_offset : addr_offset;
    assign pre          = {branch, first_ok, writes || branch, writes, move, low_cell, own};
endmodule
