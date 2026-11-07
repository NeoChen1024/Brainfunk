// Brainfunk core: scalar pipeline implementing doc/bitcode.txt, built for clock speed.
//
//   IF  I1 I2  D   A   M0 M1 M2  X   W
//
//   IF      fetch_pc addresses the instruction memory, which takes three edges
//   D       decode; check the next-fetch guess, predict JE and JN, redirect fetch
//   A       base_ptr + offset; M moves base_ptr here. MUL, F and IO stay for several cycles
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
// An instruction fetch miss holds D until the cache has the line. A data miss is found in X, and
// flushes the instruction with everything behind it, to start again from fetch. It has made no
// writes by then, because writes come after the last read of the same instruction; input reads
// its cell first for that reason, because a byte taken cannot be asked for again.
//
// Prediction, in brainfunk_predict.sv, is two guesses, neither of them trusted:
//   - next_taken says where to fetch after a word, before that word has been seen. D finds out what
//     the word really is. A word that is not a branch has what follows it fetched again; a branch
//     is taken to be taken, and its target is compared in A, for X to act on as a misprediction.
//   - with no such guess, the direction table says whether a branch is taken, and D redirects fetch
//     to the target if so, and puts it in the next-fetch table for next time.
// A next-fetch guess that turns out wrong takes its entry out of the table.
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
    input  logic [31:0]           imem_rdata,
    input  logic                  imem_hit,        // arrives with imem_rdata
    output logic                  imem_miss,       // D holds a word that was not in the cache
    output logic [PC_WIDTH-1:0]   imem_miss_addr,
    input  logic                  imem_hold,       // the cache is writing a tag: do not read
    input  logic                  imem_refetch,    // a fill finished: look up what missed again

    output logic                  pred_front_en,
    output logic [PC_WIDTH-1:0]   pred_fetch_pc,
    output logic [PC_WIDTH-1:0]   pred_i1_pc,
    input  logic                  next_taken,      // for the word imem_addr addresses
    input  logic [PC_WIDTH-1:0]   next_target,
    input  logic [1:0]            d_counter,       // for the word imem_rdata brings
    output logic                  next_we,
    output logic [PC_WIDTH-1:0]   next_key,
    output logic                  next_valid,
    output logic [PC_WIDTH-1:0]   next_new_target,
    output logic                  dir_we,
    output logic [PC_WIDTH-1:0]   dir_pc,
    output logic [1:0]            dir_counter,

    output logic [ADDR_WIDTH-1:0] dmem_raddr,
    input  logic [7:0]            dmem_rdata,
    input  logic                  dmem_hit,        // arrives with dmem_rdata
    input  logic [(1 << LINE_BITS)-1:0] dmem_zero, // arrives with dmem_rdata: which cells of the line are zero
    output logic                  dmem_miss,       // one cycle; the instruction restarts
    output logic [ADDR_WIDTH-1:0] dmem_miss_addr,
    input  logic                  dmem_busy,       // handling a miss: nothing may issue
    output logic                  dmem_we,
    output logic [ADDR_WIDTH-1:0] dmem_waddr,
    output logic [7:0]            dmem_wdata,

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
    output logic                  retire_valid,
    output logic [PC_WIDTH-1:0]   retire_pc,
    output logic [31:0]           retire_instr,
    output logic [PC_WIDTH-1:0]   retire_next_pc,
    output logic [ADDR_WIDTH-1:0] retire_base_ptr
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

    // ---- fetch ----
    logic                front_en;
    logic                d_steer;      // what follows this word is not what is being fetched
    logic [PC_WIDTH-1:0] d_steer_pc;
    logic [PC_WIDTH-1:0] fetch_pc, i1_pc, i2_pc, d_pc, d_prev_pc;
    logic                i1_valid, i2_valid, d_valid;
    logic                i1_next_taken, i2_next_taken, d_next_taken;

    assign imem_en        = front_en;
    assign imem_addr      = fetch_pc;
    assign imem_miss      = d_valid && !imem_hit;
    assign imem_miss_addr = d_pc;

    always_ff @(posedge clk) begin
        if (rst) begin
            fetch_pc <= '0;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            d_valid  <= 1'b0;
        end else if (redirect) begin
            fetch_pc <= w_alt_pc;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            d_valid  <= 1'b0;
        end else if (imem_refetch && imem_miss) begin
            fetch_pc <= d_pc;
            i1_valid <= 1'b0;
            i2_valid <= 1'b0;
            d_valid  <= 1'b0;
        end else if (front_en) begin
            fetch_pc <= d_steer ? d_steer_pc : next_taken ? next_target : fetch_pc + PC_WIDTH'(1);
            i1_valid <= !d_steer;
            i2_valid <= !d_steer && i1_valid;
            d_valid  <= !d_steer && i2_valid;
        end
        if (front_en) begin
            i1_pc         <= fetch_pc;
            i2_pc         <= i1_pc;
            d_pc          <= i2_pc;
            d_prev_pc     <= d_pc;
            i1_next_taken <= next_taken;
            i2_next_taken <= i1_next_taken;
            d_next_taken  <= i2_next_taken;
        end
    end

    // ---- decode ----
    logic [31:0] d_instr;
    logic [3:0]  d_op;
    assign d_instr = imem_rdata;
    assign d_op    = d_instr[31:28];

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

    logic d_is_branch, d_is_mul, d_is_scan, d_is_io, d_is_halt, d_multi;
    assign d_is_branch = d_op == OP_JE || d_op == OP_JN;
    assign d_is_mul    = d_op == OP_MUL;
    assign d_is_scan   = d_op == OP_F;
    assign d_is_io     = d_op == OP_IO;
    assign d_is_halt   = d_op == OP_H;
    assign d_multi     = d_is_mul || d_is_scan || d_is_io || d_is_halt || !d_legal;

    // The direction table counts disagreement with backward taken, forward not taken.
    logic d_predict_taken, d_steer_taken, d_steer_seq;
    assign d_predict_taken = d_is_branch && (d_next_taken || (d_counter[1] != d_instr[19]));
    assign d_steer_taken   = d_valid && imem_hit && d_is_branch && !d_next_taken && d_predict_taken;
    assign d_steer_seq     = d_valid && imem_hit && !d_is_branch && d_next_taken;
    assign d_steer         = d_steer_taken || d_steer_seq;

    logic [PC_WIDTH-1:0] d_target, d_seq;
    assign d_target   = d_pc + d_instr[PC_WIDTH-1:0] + PC_WIDTH'(1);
    assign d_seq      = d_pc + PC_WIDTH'(1);
    assign d_steer_pc = d_is_branch ? d_target : d_seq;

    // F: a stride short enough to take a line at a time, and how long it is.
    logic [STRIDE_BITS-1:0] d_stride_low, d_stride;
    logic                   d_scan_wide;
    assign d_stride_low = d_instr[STRIDE_BITS-1:0];
    assign d_stride     = d_instr[19] ? -d_stride_low : d_stride_low;
    assign d_scan_wide  = d_stride_low != '0 && (d_instr[19:STRIDE_BITS] == '0 || &d_instr[19:STRIDE_BITS]);

    // Which offset goes where: a_offset is added to form the address, and a_offset_2 to move
    // base_ptr, or for MUL to form the second address.
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [31:0] d_short, d_addr, d_long; // only their low bits matter: addresses wrap
    /* verilator lint_on UNUSEDSIGNAL */
    assign d_short = 32'($signed(d_instr[27:20]));
    assign d_addr  = 32'($signed(d_instr[19:8]));
    assign d_long  = 32'($signed(d_instr[19:0]));

    // ---- A: address generation ----
    logic                  a_valid, a_legal;
    logic                  a_is_add, a_is_set, a_is_mul, a_is_scan, a_is_move, a_is_branch, a_is_je;
    logic                  a_is_in, a_is_out, a_is_halt, a_multi;
    logic                  a_predict_taken;
    logic [ADDR_WIDTH-1:0] a_offset, a_offset_2;
    logic [7:0]            a_data;
    logic [PC_WIDTH-1:0]   a_pc, a_next_pc, a_alt_pc, a_fetched_next;
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

    logic [ADDR_WIDTH-1:0] base_ptr, ea, moved;
    assign ea    = base_ptr + a_offset;
    assign moved = base_ptr + a_offset_2;

    // The next line of a scan, from the line now in M0.
    logic [ADDR_WIDTH-1:0] m0_ea;
    logic [LINE_BITS-1:0]  m0_scan_cell, scan_next_cell;
    logic [ADDR_WIDTH-1:0] scan_next_ea;
    assign scan_next_cell = NEXT_CELL[{a_scan_stride, m0_scan_cell}];
    assign scan_next_ea   = {m0_ea[ADDR_WIDTH-1:LINE_BITS] + (a_scan_back ? '1 : (ADDR_WIDTH-LINE_BITS)'(1)),
                             scan_next_cell ^ {LINE_BITS{a_scan_back}}};

    logic a_issue, a_flag_mul_dst;
    assign a_issue        = a_valid && !dmem_busy && (!a_multi || a_go);
    assign a_flag_mul_dst = a_is_mul && a_last;

    // A can take a new instruction; the front end moves if it has one to give.
    logic a_ready;
    assign a_ready  = !a_valid || (!dmem_busy && (!a_multi || a_last)) || scan_done;
    assign front_en = a_ready && !imem_miss && !imem_hold;

    logic drained;
    logic m0_valid, m1_valid, m2_valid, x_valid, w_valid;

    // Writes that would reach the cache after a read issued in the next cycle.
    logic m0_writes, m1_writes, m2_writes, older_writes, a_writes;
    assign older_writes = (m0_valid && m0_writes) || (m1_valid && m1_writes) || (m2_valid && m2_writes);
    assign a_writes     = a_valid && (a_is_add || a_is_set || a_is_mul || a_is_in);
    assign drained = !(m0_valid || m1_valid || m2_valid || x_valid || w_valid || redirect);

    logic out_pending;
    assign io_in_ready = a_valid && a_is_in && a_phase == 2'd1 && drained;

    always_ff @(posedge clk) begin
        if (rst || redirect) begin
            a_valid <= 1'b0;
        end else if (a_ready) begin
            a_valid <= front_en && d_valid;
        end

        if (rst) begin
            base_ptr <= '0;
        end else if (redirect) begin
            base_ptr <= w_base;
        end else if (scan_done) begin
            base_ptr <= scan_base;
        end else if (a_issue && (a_is_move || (a_is_scan && !a_scan_wide))) begin
            base_ptr <= moved;
        end

        if (front_en) begin
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
            a_next_taken    <= d_next_taken;
            a_fetched_next  <= i2_pc;
            a_slot          <= d_prev_pc[3:0];
            a_counter       <= d_counter;
            a_data          <= d_instr[7:0];
            a_pc            <= d_pc;
            a_instr         <= d_instr;
            a_next_pc       <= d_is_halt ? d_pc : d_predict_taken ? d_target : d_seq;
            a_alt_pc        <= d_predict_taken ? d_seq : d_target;
            a_phase         <= 2'd0;
            a_last          <= 1'b0;
            a_go            <= !d_is_scan || !(a_writes || older_writes);
            a_scan_wide     <= d_scan_wide;
            a_scan_back     <= d_instr[19];
            a_scan_stride   <= d_scan_wide ? d_stride : '0;
            a_scan_more     <= 1'b0;
            a_offset        <= (d_is_mul || d_is_scan || d_is_branch) ? d_short[ADDR_WIDTH-1:0]
                                                                      : d_addr[ADDR_WIDTH-1:0];
            a_offset_2      <= d_is_mul ? d_addr[ADDR_WIDTH-1:0] : d_long[ADDR_WIDTH-1:0];
        end else if (a_is_mul) begin
            unique case (a_phase)
                2'd0: if (!dmem_busy) begin // source issues now, then a gap for the multiplier
                    a_go     <= 1'b0;
                    a_offset <= a_offset_2;
                    a_phase  <= 2'd1;
                end
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
    logic                  m0_next_taken, m0_target_wrong;
    logic [3:0]            m0_slot;
    logic [1:0]            m0_counter;

    logic                  m1_retire, m1_no_read, m1_is_branch, m1_is_je, m1_predict_taken;
    logic                  m1_is_scan, m1_is_out_read, m1_is_mul_src, m1_is_mul_dst, m1_is_halt, m1_is_trap;
    logic [7:0]            m1_data;
    logic [ADDR_WIDTH-1:0] m1_ea, m1_base, m1_base_after;
    logic [PC_WIDTH-1:0]   m1_pc, m1_next_pc, m1_alt_pc;
    logic [31:0]           m1_instr;
    logic                  m1_next_taken, m1_target_wrong;
    logic [3:0]            m1_slot;
    logic [1:0]            m1_counter;
    logic [4:1]            m1_hit;

    logic                  m2_retire, m2_no_read, m2_is_branch, m2_is_je, m2_predict_taken;
    logic                  m2_is_scan, m2_is_out_read, m2_is_mul_src, m2_is_mul_dst, m2_is_halt, m2_is_trap;
    logic [7:0]            m2_data;
    logic [ADDR_WIDTH-1:0] m2_ea, m2_base, m2_base_after;
    logic [PC_WIDTH-1:0]   m2_pc, m2_next_pc, m2_alt_pc;
    logic [31:0]           m2_instr;
    logic                  m2_next_taken, m2_target_wrong;
    logic [3:0]            m2_slot;
    logic [1:0]            m2_counter;
    logic                  m2_from_w, m2_from_w1, m2_from_w2, m2_from_w_next;

    logic                  x_writes, x_retire, x_is_branch, x_is_je, x_predict_taken;
    logic                  x_is_scan, x_is_out_read, x_is_mul_src, x_is_mul_dst, x_is_halt, x_is_trap;
    logic [7:0]            x_data;
    logic [ADDR_WIDTH-1:0] x_ea, x_base, x_base_after;
    logic [PC_WIDTH-1:0]   x_pc, x_next_pc, x_alt_pc;
    logic [31:0]           x_instr;
    logic                  x_next_taken, x_target_wrong;
    logic [3:0]            x_slot;
    logic [1:0]            x_counter;
    logic                  x_from_w, x_from_early;
    logic [7:0]            x_early;

    logic                  w_writes, w_retire, w_is_halt, w_is_trap;
    logic [ADDR_WIDTH-1:0] w_ea, w_base, w_base_after;
    logic [PC_WIDTH-1:0]   w_pc, w_next_pc, w_alt_pc;
    logic [31:0]           w_instr;
    logic                  w_is_branch, w_taken, w_wrong_guess;
    logic [3:0]            w_slot;
    logic [1:0]            w_counter;
    logic [7:0]            w_data, w1_data, w2_data;

    // ---- X ----
    logic [7:0] x_value, x_addend, mul_operand, mul_factor, product, x_sum;
    logic       x_zero, x_taken, x_wrong_way, x_mispredict, x_scan_found, x_miss;

    // F: which cells the read in each stage looks at. Stride and direction stay in A while it lasts.
    logic [LINE_CELLS-1:0] m0_scan_mask, m1_scan_mask, m2_scan_mask, x_scan_mask, x_scan_zeros, w_scan_zeros;
    logic [LINE_BITS-1:0]  m1_scan_cell, m2_scan_cell, s1_cell;
    logic [ADDR_WIDTH-LINE_BITS-1:0] s1_line;
    logic                  scan_found_1;
    logic [ADDR_WIDTH-1:0] scan_base;

    assign x_value = x_from_w ? w_data : x_from_early ? x_early : dmem_rdata;
    assign x_zero  = x_value == 8'd0;
    assign x_taken = x_zero == x_is_je;

    // A miss restarts the instruction, which is the same as a misprediction back to itself.
    assign x_miss       = x_valid && !flush && x_uses_cache && !dmem_hit;
    assign x_wrong_way  = x_taken != x_predict_taken;
    assign x_mispredict = x_valid && !flush && dmem_hit && x_is_branch && (x_wrong_way || x_target_wrong);
    assign x_scan_zeros = dmem_zero & x_scan_mask;
    assign x_scan_found = x_valid && !flush && dmem_hit && x_is_scan && |x_scan_zeros;

    assign x_addend = x_is_mul_dst ? product : x_data;
    assign x_sum    = x_value + x_addend;

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
        end else begin
            m0_valid  <= a_issue && !flush;
            m1_valid  <= m0_valid && !flush;
            m2_valid  <= m1_valid && !flush;
            x_valid   <= m2_valid && !flush;
            w_valid   <= x_valid && !flush && !x_miss;
            dmem_we   <= x_valid && !flush && !x_miss && x_writes;
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
        m0_base          <= base_ptr;
        m0_base_after    <= a_is_move ? moved : base_ptr;
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
        m0_target_wrong  <= a_next_taken && a_is_branch && a_next_pc != a_fetched_next;
        m0_slot           <= a_slot;
        m0_counter       <= a_counter;

        // M0 to M1: which older instructions write this cell, nearest first.
        // m1 is one ahead, m2 two, x three, w four; the older the instruction, the later its
        // result reaches a register this one can use.
        m1_hit[1] <= m1_valid && m1_writes && m0_ea == m1_ea;
        m1_hit[2] <= m2_valid && m2_writes && m0_ea == m2_ea;
        m1_hit[3] <= x_valid && x_writes && m0_ea == x_ea;
        m1_hit[4] <= w_valid && w_writes && m0_ea == w_ea;

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
        m1_predict_taken <= m0_predict_taken;
        m1_is_scan       <= m0_is_scan;
        m1_is_out_read   <= m0_is_out_read;
        m1_is_mul_src    <= m0_is_mul_src;
        m1_is_mul_dst    <= m0_is_mul_dst;
        m1_is_halt       <= m0_is_halt;
        m1_is_trap       <= m0_is_trap;
        m1_data          <= m0_data;
        m1_pc            <= m0_pc;
        m1_next_pc       <= m0_next_pc;
        m1_alt_pc        <= m0_alt_pc;
        m1_instr         <= m0_instr;
        m1_next_taken    <= m0_next_taken;
        m1_target_wrong  <= m0_target_wrong;
        m1_slot           <= m0_slot;
        m1_counter       <= m0_counter;

        // M1 to M2: only the nearest writer counts.
        m2_from_w_next <= m1_hit[1];
        m2_from_w      <= !m1_hit[1] && m1_hit[2];
        m2_from_w1     <= !m1_hit[1] && !m1_hit[2] && m1_hit[3];
        m2_from_w2     <= !m1_hit[1] && !m1_hit[2] && !m1_hit[3] && m1_hit[4];

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
        m2_target_wrong  <= m1_target_wrong;
        m2_slot           <= m1_slot;
        m2_counter       <= m1_counter;

        // M2 to X. Results that are already in registers are picked up here; only the
        // instruction right ahead has to be taken at X itself. A cell that is written
        // without being read adds its data to zero.
        x_from_w     <= !m2_no_read && m2_from_w_next;
        x_from_early <= m2_no_read || m2_from_w || m2_from_w1 || m2_from_w2;
        x_early      <= (m2_no_read || !m2_from_w) ? ((m2_no_read || !m2_from_w1) ?
                        ((m2_no_read || !m2_from_w2) ? 8'd0 : w2_data) : w1_data) : w_data;

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
        x_target_wrong  <= m2_target_wrong;
        x_slot           <= m2_slot;
        x_counter       <= m2_counter;

        // Multiplier: operand at the end of X, product a cycle later, used by the
        // destination's X one cycle after that.
        if (x_valid && x_is_mul_src) begin
            mul_operand <= x_value;
            mul_factor  <= x_data;
        end
`ifdef FORMAL_ABSTRACT_MUL
        product <= mul_operand ^ mul_factor;
`else
        product <= 8'(mul_operand * mul_factor);
`endif

        // X to W. F does not retire from here: it retires when it has its result, two cycles after.
        w_writes     <= x_writes;
        w_retire     <= !x_is_scan && x_retire;
        w_is_halt    <= x_is_halt;
        w_is_trap    <= x_is_trap;
        w_ea         <= x_ea;
        w_base       <= x_base;
        w_base_after <= x_base_after;
        w_pc         <= x_pc;
        w_next_pc    <= (x_is_branch && x_wrong_way) ? x_alt_pc : x_next_pc;
        w_alt_pc     <= x_miss ? x_pc : (x_is_branch && x_wrong_way) ? x_alt_pc : x_next_pc;
        w_instr      <= x_instr;
        w_is_branch  <= x_is_branch;
        w_taken      <= x_taken;
        w_wrong_guess <= x_mispredict && x_next_taken;
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
    assign pred_fetch_pc = fetch_pc;
    assign pred_i1_pc    = i1_pc;

    // A guess proved wrong comes out; D's redirect goes in. Both act in registers, so that neither
    // lengthens the path they start from.
    logic                d_learn, w_forget;
    logic                d_seq_forget;
    assign d_learn      = front_en && !redirect && d_steer_taken;
    assign d_seq_forget = front_en && !redirect && d_steer_seq;
    assign w_forget     = w_valid && w_wrong_guess;
    always_ff @(posedge clk) begin
        next_we         <= !rst && (w_forget || d_learn || d_seq_forget);
        next_key        <= d_prev_pc;
        if (w_forget) next_key[3:0] <= w_slot;
        next_valid      <= !w_forget && d_learn;
        next_new_target <= d_target;
    end

    // Counts how far this branch disagrees with its static guess.
    logic w_agrees;
    assign w_agrees    = w_taken == w_instr[19];
    assign dir_we      = w_valid && w_is_branch;
    assign dir_pc      = w_pc;
    assign dir_counter = w_agrees ? (w_counter == 2'd0 ? 2'd0 : w_counter - 2'd1)
                                  : (w_counter == 2'd3 ? 2'd3 : w_counter + 2'd1);

    assign dmem_raddr = m0_ea;
    assign dmem_waddr = w_ea;
    assign dmem_wdata = w_data;
    assign dmem_miss_addr = w_ea;

    assign retire_valid    = (w_valid && w_retire) || scan_done;
    assign retire_pc       = w_pc;
    assign retire_instr    = w_instr;
    assign retire_next_pc  = w_next_pc;
    assign retire_base_ptr = scan_done ? scan_base : w_base_after;

`ifdef FORMAL
`include "core_props.svh"
`endif

endmodule
