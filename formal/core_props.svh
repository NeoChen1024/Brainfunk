`default_nettype none
// Properties of brainfunk_core against abstract memories and arbitrary I/O.
//
// Two tape cells and one instruction word, at addresses the solver chooses, behave like real
// memory, including the read latency. Every other read returns anything. So a property about
// "the cell at a" holds for every cell, and one about "dst at a, source at b" for every pair.
// A read colliding with a write of the same cell returns anything too, as block RAM may.
//
// The memories' signals are free as well, so any hit may be a miss, and any stall can happen. Only
// a read that hits has to return real data. The tape keeps two cells in each row of a bank, and a
// write to either makes the row's read undefined: F's zero bits for a cell are real only if no write
// met its bank's row, and F has no hit flag to fall back on, so the core must never let one.
//
// The reference state is what the retire port implies, instruction by instruction.
//
// Included at the end of brainfunk_core when FORMAL is set, so that properties and the invariants that
// make them inductive can see inside the pipeline. The core's inputs are free in that case: the solver
// picks them. PC_WIDTH, ADDR_WIDTH, LINE_BITS and STRIDE_BITS are set small in core.sby.

// Something is in W, retiring or not; the retire_ outputs describe it either way.
logic formal_w_valid;
assign formal_w_valid = w_valid || scan_done;

initial assume (rst);

logic past_valid = 1'b0;
always_ff @(posedge clk) past_valid <= 1'b1;

// ---- instruction memory: one tracked word, three edges of latency ----
(* anyconst *) logic [PC_WIDTH-1:0] word_addr;
(* anyconst *) logic [31:0]         word;
logic i1_hit, i2_hit, i3_hit;

// F restarted by a miss resumes from where it was, so its word must still be there.
logic [PC_WIDTH-1:0] i1_addr, i2_addr, i3_addr, scan_pc;
logic [31:0]         scan_word;
logic                scan_active = 1'b0;

always_ff @(posedge clk) begin
    if (imem_en) begin
        i1_hit <= imem_addr == word_addr;
        i2_hit <= i1_hit;
        i3_hit <= i2_hit;
        i1_addr <= imem_addr;
        i2_addr <= i1_addr;
        i3_addr <= i2_addr;
    end
    if (rst || retire_valid) begin
        scan_active <= 1'b0;
    end else if (w_is_scan_read) begin
        scan_active <= 1'b1;
        scan_pc     <= retire_pc;
        scan_word   <= retire_instr;
    end
    if (!rst && scan_active && i3_addr == scan_pc && imem_hit) assume (imem_rdata == scan_word);
    if (past_valid && !$past(rst) && !$past(imem_en)) begin
        assume (imem_rdata == $past(imem_rdata));
        assume (imem_hit == $past(imem_hit));
    end
    if (!rst && i3_hit && imem_hit) assume (imem_rdata == word);
end

// ---- data memory: two tracked cells, three edges of latency, on both read ports ----
// Two cells share a bank's row: the same line, and the same low BANK_BITS.
function automatic logic same_row(input logic [ADDR_WIDTH-1:0] x, input logic [ADDR_WIDTH-1:0] y);
    same_row = x[ADDR_WIDTH-1:LINE_BITS] == y[ADDR_WIDTH-1:LINE_BITS] && x[BANK_BITS-1:0] == y[BANK_BITS-1:0];
endfunction
(* anyconst *) logic [ADDR_WIDTH-1:0] a, b;
(* anyconst *) logic [7:0]            a_init, b_init;
logic [7:0] cell_a, cell_b;

logic       a1_hit, a2_hit, a3_hit, b1_hit, b2_hit, b3_hit;
logic       as1_hit, as2_hit, as3_hit, bs1_hit, bs2_hit, bs3_hit;  // the second port
logic       re2_1, re2_2, re2_3;
logic [7:0] a1_value, a2_value, a3_value, b1_value, b2_value, b3_value;
// F reads the line, and sees which of its cells are zero.
logic       a1_line, a2_line, a3_line, b1_line, b2_line, b3_line;

always_ff @(posedge clk) begin
    if (rst) begin
        cell_a <= a_init;
        cell_b <= a == b ? a_init : b_init;
    end else if (dmem_we) begin
        if (dmem_waddr == a) cell_a <= dmem_wdata;
        if (dmem_waddr == b) cell_b <= dmem_wdata;
    end

    a1_hit   <= !rst && dmem_raddr == a && !(dmem_we && dmem_waddr == a);
    a1_value <= cell_a;
    a2_hit   <= a1_hit;
    a2_value <= a1_value;
    a3_hit   <= a2_hit;
    a3_value <= a2_value;
    b1_hit   <= !rst && dmem_raddr == b && !(dmem_we && dmem_waddr == b);
    b1_value <= cell_b;
    b2_hit   <= b1_hit;
    b2_value <= b1_value;
    b3_hit   <= b2_hit;
    b3_value <= b2_value;

    if (a3_hit && dmem_hit) assume (dmem_rdata == a3_value);
    if (b3_hit && dmem_hit) assume (dmem_rdata == b3_value);

    as1_hit <= !rst && dmem_raddr2 == a && !(dmem_we && dmem_waddr == a);
    as2_hit <= as1_hit;
    as3_hit <= as2_hit;
    bs1_hit <= !rst && dmem_raddr2 == b && !(dmem_we && dmem_waddr == b);
    bs2_hit <= bs1_hit;
    bs3_hit <= bs2_hit;
    re2_1 <= !rst && |dmem_bank2;
    re2_2 <= re2_1;
    re2_3 <= re2_2;
    if (dmem_hit_both) assume (dmem_hit);
    if (!re2_3) assume (dmem_hit_both == dmem_hit);
    if (re2_3 && as3_hit && dmem_hit_both) assume (dmem_rdata2 == a3_value);
    if (re2_3 && bs3_hit && dmem_hit_both) assume (dmem_rdata2 == b3_value);

    a1_line <= !rst && dmem_raddr[ADDR_WIDTH-1:LINE_BITS] == a[ADDR_WIDTH-1:LINE_BITS] && !(dmem_we && same_row(dmem_waddr, a));
    a2_line <= a1_line;
    a3_line <= a2_line;
    b1_line <= !rst && dmem_raddr[ADDR_WIDTH-1:LINE_BITS] == b[ADDR_WIDTH-1:LINE_BITS] && !(dmem_we && same_row(dmem_waddr, b));
    b2_line <= b1_line;
    b3_line <= b2_line;
    if (a3_line) assume (dmem_zero[a[LINE_BITS-1:0]] == (a3_value == 8'd0));
    if (b3_line) assume (dmem_zero[b[LINE_BITS-1:0]] == (b3_value == 8'd0));

    // Input takes its byte when nothing older is left to write, so no write meets the read of its cell.
    if (in_taken) assume (dmem_hit);
end

// ---- the architectural state the retire port implies ----
logic [PC_WIDTH-1:0]   expected_pc = '0;
logic [ADDR_WIDTH-1:0] expected_base = '0;

logic w_is_scan_read; // a read of F that did not end it
assign w_is_scan_read = formal_w_valid && !retire_valid && op == OP_F;

always_ff @(posedge clk) begin
    if (rst) begin
        expected_pc   <= '0;
        expected_base <= '0;
    end else if (retire_valid) begin
        expected_pc   <= retire_next_pc;
        expected_base <= retire_base_ptr;
    end
end

// ---- decode, independently of the core ----
logic [3:0]            op;
logic [7:0]            data;
logic [ADDR_WIDTH-1:0] addr_ea, short_ea, long_sum, scan_step, scan_end;
logic [PC_WIDTH-1:0]   sequential, target;
assign op         = retire_instr[31:28];
assign data       = retire_instr[7:0];
assign addr_ea    = expected_base + ADDR_WIDTH'($signed(retire_instr[19:8]));
assign short_ea   = expected_base + ADDR_WIDTH'($signed(retire_instr[27:20]));
assign long_sum   = expected_base + ADDR_WIDTH'($signed(retire_instr[19:0]));
assign scan_step  = ADDR_WIDTH'($signed(retire_instr[19:0]));
assign scan_end   = retire_base_ptr + ADDR_WIDTH'($signed(retire_instr[27:20]));
assign sequential = expected_pc + PC_WIDTH'(1);
assign target     = sequential + PC_WIDTH'($signed(retire_instr[19:0]));

localparam logic [3:0] OP_A = 4'd1, OP_S = 4'd2, OP_MUL = 4'd3, OP_F = 4'd4, OP_M = 4'd5,
                       OP_JE = 4'd6, OP_JN = 4'd7, OP_IO = 4'd8, OP_H = 4'd9;

logic legal;
always_comb begin
    case (op)
        OP_A, OP_S, OP_M: legal = retire_instr[27:20] == 8'd0;
        OP_MUL, OP_F, OP_JE, OP_JN: legal = 1'b1;
        OP_IO: legal = retire_instr[27:20] == 8'd0 && (data == 8'd0 || data == 8'd1);
        OP_H: legal = retire_instr[27:0] == 28'd0;
        default: legal = 1'b0;
    endcase
end

// A MUL that reads both cells at once: a factor of 1 or -1, and the cells not in one bank of two lines.
function automatic logic mul_pair_of(input logic [31:0] instr);
    mul_pair_of = instr[31:28] == OP_MUL && (instr[7:0] == 8'd1 || instr[7:0] == 8'hFF) &&
                  instr[20 +: BANK_BITS] != instr[8 +: BANK_BITS];
endfunction

logic is_input, is_output, writes;
assign is_input  = op == OP_IO && data == 8'd0;
assign is_output = op == OP_IO && data == 8'd1;
assign writes    = op == OP_A || op == OP_S || op == OP_MUL || is_input;

// F's cells from where it starts, one stride at a time: does it get to the cell it ended on, and to
// a given cell before that? The tape is short enough to walk all of it.
function automatic logic scan_reaches(input logic [ADDR_WIDTH-1:0] start, step, last);
    logic [ADDR_WIDTH-1:0] at;
    at = start;
    scan_reaches = 1'b0;
    for (int i = 0; i < (1 << ADDR_WIDTH); i++) begin
        if (at == last) scan_reaches = 1'b1;
        at = at + step;
    end
endfunction

function automatic logic scan_passes(input logic [ADDR_WIDTH-1:0] start, step, last, cell_addr);
    logic [ADDR_WIDTH-1:0] at;
    logic                  ended;
    at = start;
    ended = 1'b0;
    scan_passes = 1'b0;
    for (int i = 0; i < (1 << ADDR_WIDTH); i++) begin
        if (at == last) ended = 1'b1;
        if (!ended && at == cell_addr) scan_passes = 1'b1;
        at = at + step;
    end
endfunction

// ---- I/O bytes taken and not yet accounted for by a retire ----
logic       in_taken = 1'b0, out_taken = 1'b0;
logic [7:0] in_byte;
logic       in_keep;   // the byte taken was the end of input that leaves the cell
logic [7:0] in_value;  // what the input's write adds to nothing, or with keep, to the cell
assign in_value = in_keep ? 8'd0 : in_byte;

always_ff @(posedge clk) begin
    if (rst) begin
        in_taken  <= 1'b0;
        out_taken <= 1'b0;
    end else begin
        if (io_in_valid && io_in_ready) begin
            in_taken <= 1'b1;
            in_byte  <= io_in_data;
            in_keep  <= io_in_keep;
        end else if (retire_valid && is_input) begin
            in_taken <= 1'b0;
        end
        if (io_out_valid && io_out_ready) out_taken <= 1'b1;
        else if (retire_valid && is_output) out_taken <= 1'b0;
    end
end

always_ff @(posedge clk) begin
    if (past_valid && !$past(rst) && !rst) begin
        // Whatever reaches W is on the program's path, and is the word at that pc.
        if (formal_w_valid) begin
            assert (retire_pc == expected_pc);
            if (retire_pc == word_addr) assert (retire_instr == word);
        end

        // What may pass W without retiring.
        if (formal_w_valid && !retire_valid) begin
            assert (!legal || op == OP_F || op == OP_MUL || is_output || is_input);
        end

        // Halt and trap are sticky, and nothing happens afterwards.
        if ($past(halted))  assert (halted);
        if ($past(trapped)) assert (trapped);
        if ($past(halted) || $past(trapped)) begin
            assert (!formal_w_valid && !io_in_ready && !io_out_valid);
        end
        assert (!(halted && trapped));

        // A trap is exactly an illegal word reaching W.
        if ($past(formal_w_valid) && !$past(legal)) assert (trapped);
        if (trapped && !$past(trapped)) assert ($past(formal_w_valid) && !$past(legal));
        if ($past(retire_valid) && $past(op) == OP_H) assert (halted);
        if (halted && !$past(halted)) assert ($past(retire_valid) && $past(op) == OP_H);

        // The tape can serve two reads at once only from different banks, or from one row of one.
        if (|dmem_bank2) begin
            assert (dmem_bank2 == (1 << LINE_BITS)'(1) << dmem_raddr2[LINE_BITS-1:0]);
            assert (dmem_raddr2[BANK_BITS-1:0] != dmem_raddr[BANK_BITS-1:0] ||
                    dmem_raddr2[ADDR_WIDTH-1:LINE_BITS] == dmem_raddr[ADDR_WIDTH-1:LINE_BITS]);
        end

        // A write is the retiring instruction's, at its effective address.
        if (dmem_we) begin
            assert (retire_valid && writes);
            assert (dmem_waddr == addr_ea);
        end

        if (retire_valid) begin
            assert (legal);
            assert (dmem_we == writes);

            // pc update rule, on the architectural value of the tested cell
            case (op)
                OP_JE: if (short_ea == a) assert (retire_next_pc == (cell_a == 8'd0 ? target : sequential));
                       else assert (retire_next_pc == target || retire_next_pc == sequential);
                OP_JN: if (short_ea == a) assert (retire_next_pc == (cell_a != 8'd0 ? target : sequential));
                       else assert (retire_next_pc == target || retire_next_pc == sequential);
                OP_H:  assert (retire_next_pc == expected_pc);
                default: assert (retire_next_pc == sequential);
            endcase

            // base_ptr update rule
            case (op)
                OP_M: assert (retire_base_ptr == long_sum);
                OP_F: begin
                    assert (scan_reaches(short_ea, scan_step, scan_end));
                    if (scan_passes(short_ea, scan_step, scan_end, a)) assert (cell_a != 8'd0);
                    if (scan_passes(short_ea, scan_step, scan_end, b)) assert (cell_b != 8'd0);
                end
                default: assert (retire_base_ptr == expected_base);
            endcase

            // Results, on the architectural values of the cells
            if (op == OP_S) assert (dmem_wdata == data);
            if (op == OP_A && addr_ea == a) assert (dmem_wdata == 8'(cell_a + data));
            if (op == OP_MUL && addr_ea == a && short_ea == b) begin
`ifdef FORMAL_ABSTRACT_MUL
                // Factors of 1 and -1 do not go through the multiplier, so they are checked as they are.
                if (data == 8'd1) assert (dmem_wdata == 8'(cell_a + cell_b));
                else if (data == 8'hFF) assert (dmem_wdata == 8'(cell_a - cell_b));
                else assert (dmem_wdata == 8'(cell_a + (cell_b ^ data)));
`else
                assert (dmem_wdata == 8'(cell_a + 8'(cell_b * data)));
`endif
            end
            if (op == OP_F && scan_end == a) assert (cell_a == 8'd0);
            if (is_input) begin
                assert (in_taken);
                if (!in_keep) assert (dmem_wdata == in_byte);
                else if (addr_ea == a) assert (dmem_wdata == cell_a);
            end
            if (is_output) begin
                assert (out_taken);
                if (addr_ea == a) assert (io_out_data == cell_a);
            end
        end

        // One byte per IO instruction, and not on a speculative path.
        if (trapped || halted) assert (!in_taken && !out_taken);
        if (in_taken)  assert (!io_in_ready);
        if (out_taken) assert (!io_out_valid);

        // The core must not lose I/O: a byte stays offered until it is taken.
        if ($past(io_out_valid) && !$past(io_out_ready)) begin
            assert (io_out_valid);
            assert (io_out_data == $past(io_out_data));
        end
    end
end

`ifdef FORMAL_INVARIANTS
`include "core_invariants.svh"
`endif

`ifdef NO_MISSES
// The pipeline alone, without the solver also having to consider every miss: it gets further.
always_ff @(posedge clk) assume (imem_hit && dmem_hit && dmem_hit_both && !imem_hold && !imem_refetch && !dmem_busy);
`endif

`ifdef BOUNDED_LATENCY
// Without F and I/O, whose duration depends on the outside world, an instruction retires
// within a fixed number of cycles of the one before, including a misprediction.
logic [4:0] busy = '0;
always_ff @(posedge clk) begin
    assume (imem_rdata[31:28] != OP_F && imem_rdata[31:28] != OP_IO);
    assume (imem_hit && dmem_hit && dmem_hit_both && !imem_hold && !imem_refetch && !dmem_busy);
    if (rst || retire_valid || halted || trapped) busy <= '0;
    else if (busy != 5'h1F) busy <= busy + 5'd1;
    if (past_valid && !rst) assert (busy <= 5'd14);
end
`endif

// ---- every path is reachable, or the assertions above prove nothing ----
logic [7:0] retired = '0;
always_ff @(posedge clk) begin
    if (rst) retired <= '0;
    else if (retire_valid && retired != 8'hFF) retired <= retired + 8'd1;
end

always_ff @(posedge clk) begin
    if (past_valid && !rst) begin
        cover (retire_valid && op == OP_A);
        cover (retire_valid && op == OP_S);
        cover (retire_valid && op == OP_MUL);
        cover (retire_valid && op == OP_MUL && addr_ea == a && short_ea == b && a != b);
        cover (retire_valid && op == OP_MUL && addr_ea == a && short_ea == b && a != b && mul_pair_of(retire_instr));
        cover (retire_valid && op == OP_MUL && data == 8'hFF && !mul_pair_of(retire_instr));
        cover (retired >= 8'd2 && retire_valid && mul_pair_of(retire_instr) && short_ea == a && $past(retire_valid) &&
               $past(dmem_we) && $past(dmem_waddr) == a);
        cover (retire_valid && op == OP_M);
        cover (retire_valid && op == OP_F && retire_base_ptr != expected_base);
        cover (retire_valid && op == OP_JE && retire_next_pc == target && target != sequential);
        cover (retire_valid && op == OP_JE && retire_next_pc == sequential && target != sequential);
        cover (retire_valid && op == OP_JN && retire_next_pc == target && target != sequential);
        cover (retire_valid && op == OP_JN && retire_next_pc == sequential && target != sequential);
        cover (retire_valid && is_input);
        cover (retire_valid && is_input && in_keep);
        cover (retire_valid && is_output);
        cover (halted);
        cover (trapped);
        cover (x_miss);
        cover (retired >= 8'd1 && x_miss && retire_valid);

        // Forwarding: two neighbouring instructions on the tracked cell.
        cover (retired >= 8'd2 && retire_valid && op == OP_A && addr_ea == a && $past(retire_valid) &&
               $past(dmem_we) && $past(dmem_waddr) == a);
    end
end

`default_nettype wire
