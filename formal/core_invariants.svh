// What makes the properties in core_props.svh inductive: how the pipeline's stages relate, in any
// state reachable or not, so that the solver need not know how it got there.

function automatic logic word_legal(input logic [31:0] instr);
    case (instr[31:28])
        OP_A, OP_S, OP_M: word_legal = instr[27:20] == 8'd0;
        OP_MUL, OP_F, OP_JE, OP_JN: word_legal = 1'b1;
        OP_IO: word_legal = instr[27:20] == 8'd0 && instr[7:1] == 7'd0;
        OP_H: word_legal = instr[27:0] == 28'd0;
        default: word_legal = 1'b0;
    endcase
endfunction

function automatic logic word_in(input logic [31:0] instr);
    word_in = instr[31:28] == OP_IO && word_legal(instr) && !instr[0];
endfunction

function automatic logic word_out(input logic [31:0] instr);
    word_out = instr[31:28] == OP_IO && word_legal(instr) && instr[0];
endfunction

function automatic logic [PC_WIDTH-1:0] branch_target(input logic [PC_WIDTH-1:0] pc, input logic [31:0] instr);
    branch_target = pc + PC_WIDTH'(1) + PC_WIDTH'($signed(instr[19:0]));
endfunction

function automatic logic [ADDR_WIDTH-1:0] short_ea_of(input logic [ADDR_WIDTH-1:0] base, input logic [31:0] instr);
    short_ea_of = base + ADDR_WIDTH'($signed(instr[27:20]));
endfunction

function automatic logic [ADDR_WIDTH-1:0] addr_ea_of(input logic [ADDR_WIDTH-1:0] base, input logic [31:0] instr);
    addr_ea_of = base + ADDR_WIDTH'($signed(instr[19:8]));
endfunction

function automatic logic [ADDR_WIDTH-1:0] long_sum_of(input logic [ADDR_WIDTH-1:0] base, input logic [31:0] instr);
    long_sum_of = base + ADDR_WIDTH'($signed(instr[19:0]));
endfunction

// Everything a stage's decoded fields must say about its word.
`define STAGE_DECODE(s) \
    if (s``_valid) begin \
        if (s``_pc == word_addr) assert (s``_instr == word); \
        assert (s``_is_trap == !word_legal(s``_instr)); \
        assert (s``_is_halt == (s``_instr[31:28] == OP_H && word_legal(s``_instr))); \
        assert (s``_is_branch == (s``_instr[31:28] == OP_JE || s``_instr[31:28] == OP_JN)); \
        assert (s``_is_je == (s``_instr[31:28] == OP_JE)); \
        assert (s``_is_scan == (s``_instr[31:28] == OP_F)); \
        if (s``_is_mul_src || s``_is_mul_dst) assert (s``_instr[31:28] == OP_MUL); \
        if (s``_is_out_read) assert (s``_instr[31:28] == OP_IO && word_legal(s``_instr) && s``_instr[0]); \
        if (s``_target_wrong) assert (s``_is_branch); \
        if (s``_instr[31:28] == OP_MUL) assert (s``_is_mul_src != s``_is_mul_dst); \
        assert (s``_writes == ((s``_instr[31:28] == OP_A || s``_instr[31:28] == OP_S) && word_legal(s``_instr) || s``_is_mul_dst || (word_in(s``_instr) && s``_retire))); \
        assert (s``_retire == (word_legal(s``_instr) && !s``_is_mul_src && !(word_in(s``_instr) && !s``_writes) && !s``_is_out_read)); \
        assert (s``_uses_cache == (word_legal(s``_instr) && s``_instr[31:28] != OP_M && s``_instr[31:28] != OP_H && !(word_out(s``_instr) && !s``_is_out_read))); \
        if (word_in(s``_instr) && s``_writes) assert (s``_data == in_byte && in_taken); \
        else assert (s``_data == s``_instr[7:0]); \
        if (word_in(s``_instr) || word_out(s``_instr)) assert (s``_ea == addr_ea_of(s``_base, s``_instr)); \
        if (s``_is_branch) begin \
            assert (s``_next_pc == (s``_predict_taken ? branch_target(s``_pc, s``_instr) : s``_pc + PC_WIDTH'(1))); \
            assert (s``_alt_pc  == (s``_predict_taken ? s``_pc + PC_WIDTH'(1) : branch_target(s``_pc, s``_instr))); \
        end else if (s``_instr[31:28] == OP_H) begin \
            assert (s``_next_pc == s``_pc); \
        end else begin \
            assert (s``_next_pc == s``_pc + PC_WIDTH'(1)); \
        end \
        if (s``_is_branch) assert (s``_ea == short_ea_of(s``_base, s``_instr)); \
        if (s``_is_mul_src) assert (s``_ea == short_ea_of(s``_base, s``_instr)); \
        if (s``_is_mul_dst || (s``_instr[31:28] == OP_A || s``_instr[31:28] == OP_S) && word_legal(s``_instr)) assert (s``_ea == addr_ea_of(s``_base, s``_instr)); \
        if (!s``_is_scan) assert (s``_base_after == (s``_instr[31:28] == OP_M && word_legal(s``_instr) ? long_sum_of(s``_base, s``_instr) : s``_base)); \
    end

`define NOTHING_AFTER(s) \
    if (s``_valid && (s``_is_halt || s``_is_trap) && !redirect) begin \
        assert (a_valid && a_pc == s``_pc && a_instr == s``_instr && !a_go && (a_is_halt || !a_legal)); \
    end

// A read of this cell, in each stage, on its way to X. Where no older write is in the way, it has the
// value the cell has now.
`define CELL_VALUE(c, cell, hit1, hit2, hit3, value1, value2, value3) \
    if (h1_we && h1_ea == c) assert (w1_data == cell); \
    if (h2_we && h2_ea == c && !(h1_we && h1_ea == c)) assert (w2_data == cell); \
    if (m1_valid && ok_w && m1_ea == c && !(|m1_hit)) assert (hit1 && value1 == cell); \
    if (m2_valid && ok_w && m2_ea == c && !m2_from_w_next && !m2_from_w && !m2_from_w1 && !m2_from_w2) assert (hit2 && value2 == cell); \
    if (x_valid && ok_w && x_ea == c && !(x_instr[31:28] == OP_S && word_legal(x_instr) || word_in(x_instr) && x_writes) && !x_from_w) begin \
        if (x_from_early) assert (x_early == cell); \
        else assert (hit3 && value3 == cell); \
    end

// Writes landed one and two cycles ago, which reads ahead of them did not see.
logic                  h1_we = 1'b0, h2_we = 1'b0;
logic [ADDR_WIDTH-1:0] h1_ea, h2_ea;
always_ff @(posedge clk) begin
    h1_we <= !rst && dmem_we;
    h1_ea <= dmem_waddr;
    h2_we <= !rst && h1_we;
    h2_ea <= h1_ea;
end

`define IS_SOURCE_OF(source, dest) (source``_valid && source``_is_mul_src && source``_pc == dest``_pc && source``_instr == dest``_instr)

function automatic logic operand_ready(input logic [31:0] instr, input logic [ADDR_WIDTH-1:0] base);
    operand_ready = mul_factor == instr[7:0] && (short_ea_of(base, instr) != a || mul_operand == cell_a) &&
                                                (short_ea_of(base, instr) != b || mul_operand == cell_b);
endfunction

`define IS_READ_OF(s) (s``_is_out_read && s``_pc == a_pc && s``_instr == a_instr && s``_base == base_ptr)

// The second half of output, with its byte already taken and nothing older left.
`define OUT_LAST(s) \
    if (s``_valid && word_out(s``_instr) && !s``_is_out_read) begin \
        assert (out_taken && !out_pending); \
        if (addr_ea_of(s``_base, s``_instr) == a) assert (io_out_data == cell_a); \
        if (addr_ea_of(s``_base, s``_instr) == b) assert (io_out_data == cell_b); \
    end

// F, independently of the core's tables and shifts.
function automatic logic scan_wide_of(input logic [31:0] instr);
    scan_wide_of = instr[STRIDE_BITS-1:0] != '0 && (instr[19:STRIDE_BITS] == '0 || &instr[19:STRIDE_BITS]);
endfunction

function automatic logic [ADDR_WIDTH-1:0] scan_next_of(input logic [ADDR_WIDTH-1:0] ea, input logic [31:0] instr);
    logic [ADDR_WIDTH-1:0] at;
    logic                  left;
    at = ea + ADDR_WIDTH'($signed(instr[19:0]));
    left = at[ADDR_WIDTH-1:LINE_BITS] != ea[ADDR_WIDTH-1:LINE_BITS];
    scan_next_of = at;
    if (scan_wide_of(instr)) begin
        for (int i = 0; i < LINE_CELLS; i++) begin
            if (!left) begin
                at = at + ADDR_WIDTH'($signed(instr[19:0]));
                left = at[ADDR_WIDTH-1:LINE_BITS] != ea[ADDR_WIDTH-1:LINE_BITS];
            end
        end
        scan_next_of = at;
    end
endfunction

function automatic logic [LINE_CELLS-1:0] scan_mask_of(input logic [ADDR_WIDTH-1:0] ea, input logic [31:0] instr);
    logic [ADDR_WIDTH-1:0] at;
    logic                  left;
    at = ea;
    left = 1'b0;
    scan_mask_of = '0;
    scan_mask_of[ea[LINE_BITS-1:0]] = 1'b1;
    if (scan_wide_of(instr)) begin
        for (int i = 0; i < LINE_CELLS; i++) begin
            at = at + ADDR_WIDTH'($signed(instr[19:0]));
            if (at[ADDR_WIDTH-1:LINE_BITS] != ea[ADDR_WIDTH-1:LINE_BITS]) left = 1'b1;
            if (!left) scan_mask_of[at[LINE_BITS-1:0]] = 1'b1;
        end
    end
endfunction

// How far F is known to have got without finding a zero: every cell it lands on before the cursor.
logic                  g_active = 1'b0;
logic [ADDR_WIDTH-1:0] g_cursor;
logic [31:0]           g_word;
logic [PC_WIDTH-1:0]   g_pc;
always_ff @(posedge clk) begin
    if (rst || scan_done) begin
        g_active <= 1'b0;
    end else if (x_valid && !flush && dmem_hit && x_is_scan && !(|x_scan_zeros)) begin
        g_active <= 1'b1;
        g_cursor <= scan_next_of(x_ea, x_instr);
        g_word   <= x_instr;
        g_pc     <= x_pc;
    end
end

logic [ADDR_WIDTH-1:0] stop_end, found_end, done_end;
assign stop_end  = {w_ea[ADDR_WIDTH-1:LINE_BITS], LINE_BITS'(lowest_cell(w_scan_zeros) ^ {LINE_BITS{a_scan_back}})};
assign found_end = {s1_line, s1_cell};
assign done_end  = short_ea_of(scan_base, a_instr);

logic any_scan_read, scan_ending;
assign any_scan_read = m0_valid && m0_is_scan || m1_valid && m1_is_scan || m2_valid && m2_is_scan || x_valid && x_is_scan || w_valid && w_instr[31:28] == OP_F;
assign scan_ending   = scan_stop || scan_found_1 || scan_done;

// A read of F: it belongs to the F in A, with nothing writing anywhere ahead of it, and everything behind
// it in the pipeline is a copy of the same word.
`define SCAN_READ(s, ok_older, older_base) \
    if (ok_older && s``_valid && s``_is_scan) begin \
        assert (a_valid && a_is_scan && a_pc == s``_pc && a_instr == s``_instr); \
        assert (!(m0_valid && m0_writes) && !(m1_valid && m1_writes) && !(m2_valid && m2_writes) && !(x_valid && x_writes) && !(w_valid && w_writes)); \
    end

// The end of F: the cell it stops on is reached, it is zero, and nothing before it is.
`define SCAN_END(last) \
    begin \
        assert (scan_reaches(short_ea_of(expected_base, a_instr), ADDR_WIDTH'($signed(a_instr[19:0])), last)); \
        if (scan_passes(short_ea_of(expected_base, a_instr), ADDR_WIDTH'($signed(a_instr[19:0])), last, a)) assert (cell_a != 8'd0); \
        if (scan_passes(short_ea_of(expected_base, a_instr), ADDR_WIDTH'($signed(a_instr[19:0])), last, b)) assert (cell_b != 8'd0); \
        if (last == a) assert (cell_a == 8'd0); \
        if (last == b) assert (cell_b == 8'd0); \
    end

// The path the pipeline is on, from what has retired, oldest stage first: a stage is on it, and says
// where it goes next, unless something older is already certain to flush it.
logic [PC_WIDTH-1:0]   path_w_pc, path_x_pc, path_m2_pc, path_m1_pc, path_m0_pc, path_a_pc;
logic [ADDR_WIDTH-1:0] path_w_base, path_x_base, path_m2_base, path_m1_base, path_m0_base;
logic                  ok_w, ok_x, ok_m2, ok_m1, ok_m0, ok_a;

assign path_w_pc   = w_valid && w_retire ? w_next_pc : expected_pc;
assign path_w_base = w_valid && w_retire ? w_base_after : expected_base;
assign ok_w        = !redirect;

assign path_x_pc   = x_valid ? (x_retire && !x_is_scan ? x_next_pc : x_pc) : path_w_pc;
assign path_x_base = x_valid ? x_base_after : path_w_base;
assign ok_x        = ok_w && !(x_valid && x_target_wrong);

assign path_m2_pc   = m2_valid ? (m2_retire && !m2_is_scan ? m2_next_pc : m2_pc) : path_x_pc;
assign path_m2_base = m2_valid ? m2_base_after : path_x_base;
assign ok_m2        = ok_x && !(m2_valid && m2_target_wrong);

assign path_m1_pc   = m1_valid ? (m1_retire && !m1_is_scan ? m1_next_pc : m1_pc) : path_m2_pc;
assign path_m1_base = m1_valid ? m1_base_after : path_m2_base;
assign ok_m1        = ok_m2 && !(m1_valid && m1_target_wrong);

assign path_m0_pc   = m0_valid ? (m0_retire && !m0_is_scan ? m0_next_pc : m0_pc) : path_m1_pc;
assign path_m0_base = m0_valid ? m0_base_after : path_m1_base;
assign ok_m0        = ok_m1 && !(m0_valid && m0_target_wrong);

// A is on the path; what follows it only is if A can ever finish and its guess was right.
logic a_finishes;
assign a_finishes = !a_is_halt && a_legal;
assign path_a_pc  = a_valid ? a_next_pc : path_m0_pc;
assign ok_a       = ok_m0 && (!a_valid || (a_finishes && !(a_next_taken && a_is_branch && a_next_pc != a_fetched_next)));

// The second half of an earlier output, still ahead of this one.
logic older_out_last;
assign older_out_last = m0_valid && word_out(m0_instr) && !m0_is_out_read || m1_valid && word_out(m1_instr) && !m1_is_out_read ||
                        m2_valid && word_out(m2_instr) && !m2_is_out_read || x_valid && word_out(x_instr) && !x_is_out_read ||
                        w_valid && word_out(w_instr) && w_retire;

always_ff @(posedge clk) if (!rst) begin
    // Fields of each stage.
    `STAGE_DECODE(m0)
    `STAGE_DECODE(m1)
    `STAGE_DECODE(m2)

    if (x_valid) begin
        if (x_pc == word_addr) assert (x_instr == word);
        assert (x_is_trap == !word_legal(x_instr));
        assert (x_is_halt == (x_instr[31:28] == OP_H && word_legal(x_instr)));
        assert (x_is_branch == (x_instr[31:28] == OP_JE || x_instr[31:28] == OP_JN));
        assert (x_is_je == (x_instr[31:28] == OP_JE));
        assert (x_is_scan == (x_instr[31:28] == OP_F));
        if (x_is_mul_src || x_is_mul_dst) assert (x_instr[31:28] == OP_MUL);
        if (x_is_out_read) assert (x_instr[31:28] == OP_IO && word_legal(x_instr) && x_instr[0]);
        if (x_target_wrong) assert (x_is_branch);
        if (x_instr[31:28] == OP_MUL) assert (x_is_mul_src != x_is_mul_dst);
        assert (x_writes == ((x_instr[31:28] == OP_A || x_instr[31:28] == OP_S) && word_legal(x_instr) || x_is_mul_dst || (word_in(x_instr) && x_retire)));
        assert (x_retire == (word_legal(x_instr) && !x_is_mul_src && !(word_in(x_instr) && !x_writes) && !x_is_out_read));
        assert (x_uses_cache == (word_legal(x_instr) && x_instr[31:28] != OP_M && x_instr[31:28] != OP_H && !(word_out(x_instr) && !x_is_out_read)));
        if (word_in(x_instr) && x_writes) assert (x_data == in_byte && in_taken);
        else assert (x_data == x_instr[7:0]);
        if (word_in(x_instr) || word_out(x_instr)) assert (x_ea == addr_ea_of(x_base, x_instr));
        if (x_is_branch) begin
            assert (x_next_pc == (x_predict_taken ? branch_target(x_pc, x_instr) : x_pc + PC_WIDTH'(1)));
            assert (x_alt_pc  == (x_predict_taken ? x_pc + PC_WIDTH'(1) : branch_target(x_pc, x_instr)));
        end else if (x_instr[31:28] == OP_H) begin
            assert (x_next_pc == x_pc);
        end else begin
            assert (x_next_pc == x_pc + PC_WIDTH'(1));
        end
        if (x_is_branch || x_is_mul_src) assert (x_ea == short_ea_of(x_base, x_instr));
        if (x_is_mul_dst || (x_instr[31:28] == OP_A || x_instr[31:28] == OP_S) && word_legal(x_instr)) assert (x_ea == addr_ea_of(x_base, x_instr));
        if (!x_is_scan) assert (x_base_after == (x_instr[31:28] == OP_M && word_legal(x_instr) ? long_sum_of(x_base, x_instr) : x_base));
    end

    if (w_valid) begin
        if (w_pc == word_addr) assert (w_instr == word);
        assert (w_is_trap == !word_legal(w_instr));
        assert (w_is_halt == (w_instr[31:28] == OP_H && word_legal(w_instr)));
        assert (w_is_branch == (w_instr[31:28] == OP_JE || w_instr[31:28] == OP_JN));
        assert (w_writes == ((w_instr[31:28] == OP_A || w_instr[31:28] == OP_S) && word_legal(w_instr) || ((w_instr[31:28] == OP_MUL || word_in(w_instr)) && w_retire)));
        if (!word_legal(w_instr)) assert (!w_retire);
        else if (w_instr[31:28] == OP_MUL || word_in(w_instr)) assert (w_retire == w_writes);
        else if (w_instr[31:28] == OP_F) assert (!w_retire);
        else if (!word_out(w_instr)) assert (w_retire);
        if (word_in(w_instr) && w_writes) assert (in_taken && w_data == in_byte);
        if (w_is_branch) assert (w_next_pc == branch_target(w_pc, w_instr) || w_next_pc == w_pc + PC_WIDTH'(1));
        else if (w_instr[31:28] == OP_H) assert (w_next_pc == w_pc);
        else assert (w_next_pc == w_pc + PC_WIDTH'(1));
        if (w_writes) assert (w_ea == addr_ea_of(w_base, w_instr));
        if (w_instr[31:28] != OP_F) assert (w_base_after == (w_instr[31:28] == OP_M && word_legal(w_instr) ? long_sum_of(w_base, w_instr) : w_base));
    end

    // Writes follow W.
    assert (dmem_we == (w_valid && w_writes));

    // A's fields.
    if (a_valid) begin
        if (a_pc == word_addr) assert (a_instr == word);
        assert (a_legal == word_legal(a_instr));
        assert (a_is_add == (a_instr[31:28] == OP_A && a_legal));
        assert (a_is_set == (a_instr[31:28] == OP_S && a_legal));
        assert (a_is_mul == (a_instr[31:28] == OP_MUL));
        assert (a_is_scan == (a_instr[31:28] == OP_F));
        assert (a_is_move == (a_instr[31:28] == OP_M && a_legal));
        assert (a_is_branch == (a_instr[31:28] == OP_JE || a_instr[31:28] == OP_JN));
        assert (a_is_je == (a_instr[31:28] == OP_JE));
        assert (a_is_halt == (a_instr[31:28] == OP_H && a_legal));
        assert (a_is_in == (a_instr[31:28] == OP_IO && a_legal && !a_instr[0]));
        assert (a_is_out == (a_instr[31:28] == OP_IO && a_legal && a_instr[0]));
        assert (a_multi == (a_is_mul || a_is_scan || a_instr[31:28] == OP_IO || a_instr[31:28] == OP_H || !a_legal));
        if (a_is_branch) begin
            assert (a_next_pc == (a_predict_taken ? branch_target(a_pc, a_instr) : a_pc + PC_WIDTH'(1)));
            assert (a_alt_pc  == (a_predict_taken ? a_pc + PC_WIDTH'(1) : branch_target(a_pc, a_instr)));
        end else if (a_instr[31:28] == OP_H) begin
            assert (a_next_pc == a_pc);
        end else begin
            assert (a_next_pc == a_pc + PC_WIDTH'(1));
        end
        if (!a_is_in) assert (a_data == a_instr[7:0]);
        if (a_is_in || a_is_out) assert (a_offset == ADDR_WIDTH'($signed(a_instr[19:8])));
        if (a_is_in) begin
            case (a_phase)
                2'd0: assert (a_go && !a_last && a_data == a_instr[7:0]);
                2'd1: assert (!a_go && !a_last && a_data == a_instr[7:0]);
                2'd2: assert (a_go && a_last && a_data == in_byte && in_taken);
                default: assert (1'b0);
            endcase
        end
        if (a_is_out) begin
            case (a_phase)
                2'd0: assert (a_go && !a_last);
                2'd1: assert (a_go == a_last);
                default: assert (1'b0);
            endcase
            if (a_last) assert (out_taken && !out_pending);
        end
        if (a_is_branch) assert (a_offset == ADDR_WIDTH'($signed(a_instr[27:20])));
        if (a_is_add || a_is_set || a_is_move) assert (a_offset == ADDR_WIDTH'($signed(a_instr[19:8])));
        if (a_is_move) assert (a_offset_2 == ADDR_WIDTH'($signed(a_instr[19:0])));
        if (a_is_mul) begin
            assert (a_offset_2 == ADDR_WIDTH'($signed(a_instr[19:8])));
            case (a_phase)
                2'd0: assert (a_go && !a_last && a_offset == ADDR_WIDTH'($signed(a_instr[27:20])));
                2'd1: assert (!a_go && !a_last && a_offset == a_offset_2);
                2'd2: assert (a_go && a_last && a_offset == a_offset_2);
                default: assert (1'b0);
            endcase
        end
        if (!a_is_scan) assert (!a_scan_more);
        if (!a_multi) begin
            assert (a_go && !a_last && a_phase == 2'd0);
        end
        if (a_is_halt || !a_legal) assert (!a_last);
    end

    // The pipeline on the path, oldest first.
    if (w_valid) assert (w_pc == expected_pc);
    if (w_valid && w_instr[31:28] != OP_F) assert (w_base == expected_base);
    if (redirect && w_valid) assert (w_alt_pc == w_next_pc && w_retire && w_is_branch);
    if (redirect) assert (w_alt_pc == (w_valid ? w_next_pc : expected_pc));
    if (redirect && !g_active) assert (w_base == expected_base);
    if (redirect && g_active) assert (short_ea_of(w_base, g_word) == g_cursor);
    if (ok_w && x_valid)  assert (x_pc  == path_w_pc);
    if (ok_x && m2_valid) assert (m2_pc == path_x_pc);
    if (ok_m2 && m1_valid) assert (m1_pc == path_m2_pc);
    if (ok_m1 && m0_valid) assert (m0_pc == path_m1_pc);
    if (ok_w && x_valid && !x_is_scan) assert (x_base == path_w_base);
    if (ok_x && m2_valid && !m2_is_scan) assert (m2_base == path_x_base);
    if (ok_m2 && m1_valid && !m1_is_scan) assert (m1_base == path_m2_base);
    if (ok_m1 && m0_valid && !m0_is_scan) assert (m0_base == path_m1_base);
    if (ok_m0 && a_valid) assert (a_pc == path_m0_pc);
    if (ok_m0 && !any_scan_read && !g_active && !scan_ending) assert (base_ptr == path_m0_base);

    // What has been fetched: each word follows the one before unless it was sent elsewhere.
    if (d_valid && i2_valid && !d_next_taken) assert (i2_pc == d_pc + PC_WIDTH'(1));
    if (i2_valid && i1_valid && !i2_next_taken) assert (i1_pc == i2_pc + PC_WIDTH'(1));
    if (i1_valid && !i1_next_taken) assert (fetch_pc == i1_pc + PC_WIDTH'(1));
    if (a_valid && d_valid) assert (d_pc == a_fetched_next);
    if (d_valid) assert (i2_valid);
    if (i2_valid) assert (i1_valid);

    // ...and the first of it is where the path goes.
    if (ok_a) begin
        if (d_valid) assert (d_pc == path_a_pc);
        else if (i2_valid) assert (i2_pc == path_a_pc);
        else if (i1_valid) assert (i1_pc == path_a_pc);
        else assert (fetch_pc == path_a_pc);
    end

    // Nothing follows a word that never finishes.
    `NOTHING_AFTER(w)
    `NOTHING_AFTER(x)
    `NOTHING_AFTER(m2)
    `NOTHING_AFTER(m1)
    `NOTHING_AFTER(m0)
    if (w_valid && (w_is_halt || w_is_trap) && !redirect) assert (!x_valid && !m2_valid && !m1_valid && !m0_valid);
    if (x_valid && (x_is_halt || x_is_trap) && !redirect) assert (!m2_valid && !m1_valid && !m0_valid);
    if (m2_valid && (m2_is_halt || m2_is_trap) && !redirect) assert (!m1_valid && !m0_valid);
    if (m1_valid && (m1_is_halt || m1_is_trap) && !redirect) assert (!m0_valid);
    if (halted) assert (a_valid && a_is_halt && !a_go);
    if (trapped) assert (a_valid && !a_legal && !a_go);
    if (halted || trapped) assert (!m0_valid && !m1_valid && !m2_valid && !x_valid && !w_valid && !redirect && !scan_stop && !scan_found_1 && !scan_done);
    if (a_valid && (a_is_halt || !a_legal) && !a_go && !redirect) begin
        assert (m0_valid && (m0_is_halt || m0_is_trap) && m0_pc == a_pc || !m0_valid && (m1_valid && (m1_is_halt || m1_is_trap) && m1_pc == a_pc || !m1_valid && (m2_valid && (m2_is_halt || m2_is_trap) && m2_pc == a_pc || !m2_valid && (x_valid && (x_is_halt || x_is_trap) && x_pc == a_pc || !x_valid && (w_valid && (w_is_halt || w_is_trap) && w_pc == a_pc || !w_valid && (halted || trapped))))));
    end
    if (a_valid && (a_is_halt || !a_legal) && a_go) assert (!halted && !trapped);


    // Where a read's value comes from, for every older write.
    if (a == b) assert (cell_a == cell_b);
    if (m1_valid && ok_w) begin
        assert (m1_hit[1] == (m2_valid && m2_writes && m2_ea == m1_ea));
        assert (m1_hit[2] == (x_valid && x_writes && x_ea == m1_ea));
        assert (m1_hit[3] == (w_valid && w_writes && w_ea == m1_ea));
        assert (m1_hit[4] == (h1_we && h1_ea == m1_ea));
    end
    if (m2_valid && ok_w) begin
        assert (m2_from_w_next == (x_valid && x_writes && x_ea == m2_ea));
        assert (m2_from_w == (!m2_from_w_next && w_valid && w_writes && w_ea == m2_ea));
        assert (m2_from_w1 == (!m2_from_w_next && !m2_from_w && h1_we && h1_ea == m2_ea));
        assert (m2_from_w2 == (!m2_from_w_next && !m2_from_w && !m2_from_w1 && h2_we && h2_ea == m2_ea));
        assert (m2_no_read == (m2_instr[31:28] == OP_S && word_legal(m2_instr) || word_in(m2_instr) && m2_writes));
    end
    if (m1_valid) assert (m1_no_read == (m1_instr[31:28] == OP_S && word_legal(m1_instr) || word_in(m1_instr) && m1_writes));
    if (m0_valid) assert (m0_no_read == (m0_instr[31:28] == OP_S && word_legal(m0_instr) || word_in(m0_instr) && m0_writes));
    if (x_valid && ok_w) begin
        if (x_instr[31:28] == OP_S && word_legal(x_instr) || word_in(x_instr) && x_writes) begin
            assert (!x_from_w && x_from_early && x_early == 8'd0);
        end else begin
            assert (x_from_w == (w_valid && w_writes && w_ea == x_ea));
        end
    end
    `CELL_VALUE(a, cell_a, a1_hit, a2_hit, a3_hit, a1_value, a2_value, a3_value)
    `CELL_VALUE(b, cell_b, b1_hit, b2_hit, b3_hit, b1_value, b2_value, b3_value)

    // The multiplier: its source is taken in X, and the destination is at least two stages behind it.
    // The cache being busy can hold the destination back further, but nothing else can touch the operand.
    if (ok_w && a_valid && a_is_mul && a_phase == 2'd1) assert (`IS_SOURCE_OF(m0, a) && m0_base == base_ptr);
    if (ok_w && a_valid && a_is_mul && a_phase == 2'd2) begin
        assert (!m0_valid);
        if (m1_valid) assert (`IS_SOURCE_OF(m1, a) && m1_base == base_ptr);
        else if (m2_valid) assert (`IS_SOURCE_OF(m2, a) && m2_base == base_ptr);
        else if (x_valid) assert (`IS_SOURCE_OF(x, a) && x_base == base_ptr);
        else assert (operand_ready(a_instr, base_ptr));
    end
    if (ok_w && m0_valid && m0_is_mul_dst) begin
        assert (!m1_valid);
        if (m2_valid) assert (`IS_SOURCE_OF(m2, m0) && m2_base == m0_base);
        else if (x_valid) assert (`IS_SOURCE_OF(x, m0) && x_base == m0_base);
        else assert (operand_ready(m0_instr, m0_base));
    end
    if (ok_w && m1_valid && m1_is_mul_dst) begin
        assert (!m2_valid);
        if (x_valid) assert (`IS_SOURCE_OF(x, m1) && x_base == m1_base);
        else assert (operand_ready(m1_instr, m1_base));
    end
    if (ok_w && m2_valid && m2_is_mul_dst) begin
        assert (!x_valid);
        assert (operand_ready(m2_instr, m2_base));
    end
    if (ok_w && x_valid && x_is_mul_dst) begin
`ifdef FORMAL_ABSTRACT_MUL
        if (short_ea_of(x_base, x_instr) == a) assert (product == (cell_a ^ x_data));
        if (short_ea_of(x_base, x_instr) == b) assert (product == (cell_b ^ x_data));
`else
        if (short_ea_of(x_base, x_instr) == a) assert (product == 8'(cell_a * x_data));
        if (short_ea_of(x_base, x_instr) == b) assert (product == 8'(cell_b * x_data));
`endif
    end

    // The tracked word reads as itself.
    if (i1_hit == 1'b0) assert (1'b1);
    if (d_valid && d_pc == word_addr) assert (i3_hit);
    if (i2_valid && i2_pc == word_addr) assert (i2_hit);
    if (i1_valid && i1_pc == word_addr) assert (i1_hit);

    // Input: a byte taken is on its way to W with nothing older left, so nothing can flush it.
    if (a_valid && a_is_in && a_phase == 2'd2) assert (!m0_valid && !m1_valid && !m2_valid && !x_valid && !w_valid && !redirect);
    if (m0_valid && word_in(m0_instr) && m0_writes) assert (!m1_valid && !m2_valid && !x_valid && !w_valid && !redirect);
    if (m1_valid && word_in(m1_instr) && m1_writes) assert (!m2_valid && !x_valid && !w_valid && !redirect);
    if (m2_valid && word_in(m2_instr) && m2_writes) assert (!x_valid && !w_valid && !redirect);
    if (x_valid && word_in(x_instr) && x_writes) assert (!w_valid && !redirect);
    if (w_valid && word_in(w_instr) && w_writes) assert (!redirect);
    if (in_taken) begin
        assert (a_valid && a_is_in && a_phase == 2'd2 ||
                m0_valid && word_in(m0_instr) && m0_writes || m1_valid && word_in(m1_instr) && m1_writes ||
                m2_valid && word_in(m2_instr) && m2_writes || x_valid && word_in(x_instr) && x_writes ||
                w_valid && word_in(w_instr) && w_writes);
    end
    if (a_valid && a_is_in && a_phase != 2'd2 && ok_w && !(m0_valid && word_in(m0_instr) && m0_writes || m1_valid && word_in(m1_instr) && m1_writes ||
                                                            m2_valid && word_in(m2_instr) && m2_writes || x_valid && word_in(x_instr) && x_writes ||
                                                            w_valid && word_in(w_instr) && w_writes)) assert (!in_taken);

    // Output: its byte stays offered from X until taken, and then nothing can flush the rest of it.
    if (a_valid && a_is_out && a_last) assert (!m0_valid && !m1_valid && !m2_valid && !x_valid && !w_valid && !redirect);
    if (m0_valid && word_out(m0_instr) && !m0_is_out_read) assert (!m1_valid && !m2_valid && !x_valid && !w_valid && !redirect);
    if (m1_valid && word_out(m1_instr) && !m1_is_out_read) assert (!m2_valid && !x_valid && !w_valid && !redirect);
    if (m2_valid && word_out(m2_instr) && !m2_is_out_read) assert (!x_valid && !w_valid && !redirect);
    if (x_valid && word_out(x_instr) && !x_is_out_read) assert (!w_valid && !redirect);
    `OUT_LAST(m0)
    `OUT_LAST(m1)
    `OUT_LAST(m2)
    `OUT_LAST(x)
    if (out_taken || out_pending) assert (!(out_taken && out_pending) && !redirect);
    if (w_valid && word_out(w_instr) && w_retire) assert (out_taken && !out_pending && !redirect);
    if (w_valid && word_out(w_instr) && w_retire && addr_ea_of(w_base, w_instr) == a) assert (io_out_data == cell_a);
    if (w_valid && word_out(w_instr) && w_retire && addr_ea_of(w_base, w_instr) == b) assert (io_out_data == cell_b);
    if (a_valid && a_is_out && a_phase == 2'd1 && ok_w) begin
        // The read is in flight, or has gone by and left its byte.
        if (m0_valid) assert (`IS_READ_OF(m0) && (older_out_last || !out_taken && !out_pending));
        else if (m1_valid) assert (`IS_READ_OF(m1) && (older_out_last || !out_taken && !out_pending));
        else if (m2_valid) assert (`IS_READ_OF(m2) && (older_out_last || !out_taken && !out_pending));
        else if (x_valid) assert (`IS_READ_OF(x) && (older_out_last || !out_taken && !out_pending));
        else begin
            assert (out_taken || out_pending);
            if (addr_ea_of(base_ptr, a_instr) == a) assert (io_out_data == cell_a);
            if (addr_ea_of(base_ptr, a_instr) == b) assert (io_out_data == cell_b);
        end
    end
    if (out_taken || out_pending) assert (a_valid && a_is_out && a_phase == 2'd1 ||
                                          m0_valid && word_out(m0_instr) && !m0_is_out_read ||
                                          m1_valid && word_out(m1_instr) && !m1_is_out_read ||
                                          m2_valid && word_out(m2_instr) && !m2_is_out_read ||
                                          x_valid && word_out(x_instr) && !x_is_out_read ||
                                          w_valid && word_out(w_instr) && w_retire);
    if (ok_w && m0_valid && m0_is_out_read) assert (!(m1_valid && m1_is_out_read) && !(m2_valid && m2_is_out_read) && !(x_valid && x_is_out_read));
    if (ok_w && m1_valid && m1_is_out_read) assert (!(m2_valid && m2_is_out_read) && !(x_valid && x_is_out_read));
    if (ok_w && m2_valid && m2_is_out_read) assert (!(x_valid && x_is_out_read));
    if (ok_w && x_valid && x_is_out_read) assert (!(out_pending || out_taken) || older_out_last);
    if (a_valid && a_is_out && a_phase == 2'd0 && !older_out_last) assert (!out_taken && !out_pending);
    if (ok_w && m0_valid && m0_is_out_read) assert (a_valid && a_is_out && a_phase == 2'd1 && `IS_READ_OF(m0));
    if (ok_w && m1_valid && m1_is_out_read) assert (a_valid && a_is_out && a_phase == 2'd1 && `IS_READ_OF(m1));
    if (ok_w && m2_valid && m2_is_out_read) assert (a_valid && a_is_out && a_phase == 2'd1 && `IS_READ_OF(m2));
    if (ok_w && x_valid && x_is_out_read) assert (a_valid && a_is_out && a_phase == 2'd1 && `IS_READ_OF(x));

    // ---- F ----
    if (a_valid && a_is_scan) begin
        assert (a_scan_wide == scan_wide_of(a_instr));
        assert (a_scan_back == a_instr[19]);
        assert (a_offset == ADDR_WIDTH'($signed(a_instr[27:20])));
        assert (a_offset_2 == ADDR_WIDTH'($signed(a_instr[19:0])));
        assert (!a_last);
        if (a_scan_wide) assert (a_scan_stride == (a_instr[19] ? -a_instr[STRIDE_BITS-1:0] : a_instr[STRIDE_BITS-1:0]));
        else assert (a_scan_stride == '0 && !a_scan_more);
        assert (a_phase <= 2'd1);
        assert ((a_phase == 2'd1) == (scan_found_1 || scan_done));
        if (a_phase == 2'd1) assert (!a_go);
        if (a_go) assert (!(m0_valid && m0_writes) && !(m1_valid && m1_writes) && !(m2_valid && m2_writes) && !(x_valid && x_writes));
        if (a_scan_more) assert (m0_scan_cell == (m0_ea[LINE_BITS-1:0] ^ {LINE_BITS{a_scan_back}}));
        if (a_scan_more && a_phase == 2'd0) assert (a_go);
    end
    if (ok_m1 && m0_valid && m0_is_scan) assert (m0_scan_cell == (m0_ea[LINE_BITS-1:0] ^ {LINE_BITS{a_scan_back}}));
    if (ok_m2 && m1_valid && m1_is_scan) assert (m1_scan_cell == (m1_ea[LINE_BITS-1:0] ^ {LINE_BITS{a_scan_back}}));
    if (ok_x && m2_valid && m2_is_scan) assert (m2_scan_cell == (m2_ea[LINE_BITS-1:0] ^ {LINE_BITS{a_scan_back}}));
    `SCAN_READ(m0, ok_m1, path_m1_base)
    `SCAN_READ(m1, ok_m2, path_m2_base)
    `SCAN_READ(m2, ok_x, path_x_base)
    `SCAN_READ(x, ok_w, path_w_base)

    // Copies of F behind a read, whether or not they are valid: they will be in W when it retires.
    if (ok_w && x_valid && x_is_scan) assert (m2_pc == a_pc && m2_instr == a_instr && m2_next_pc == a_next_pc && !m2_is_branch && m1_pc == a_pc && m1_instr == a_instr && m1_next_pc == a_next_pc && !m1_is_branch && m0_pc == a_pc && m0_instr == a_instr && m0_next_pc == a_next_pc && !m0_is_branch);
    if (ok_x && m2_valid && m2_is_scan) assert (m1_pc == a_pc && m1_instr == a_instr && m1_next_pc == a_next_pc && !m1_is_branch && m0_pc == a_pc && m0_instr == a_instr && m0_next_pc == a_next_pc && !m0_is_branch);
    if (ok_m2 && m1_valid && m1_is_scan) assert (m0_pc == a_pc && m0_instr == a_instr && m0_next_pc == a_next_pc && !m0_is_branch);

    // What each read looks at.
    if (ok_w && x_valid && x_is_scan) begin
        assert (x_scan_mask == scan_mask_of(x_ea, x_instr));
        assert (x_base == x_ea - a_offset);
    end
    if (ok_x && m2_valid && m2_is_scan) begin
        assert (m2_scan_mask == (STRIDE_MASK[a_scan_stride] << (m2_scan_cell & ~LINE_BITS'(1))));
        assert (m2_base == m2_ea - a_offset);
    end
    if (ok_m2 && m1_valid && m1_is_scan) begin
        assert (m1_scan_mask == (STRIDE_MASK[a_scan_stride] << (8 * (m1_scan_cell >> 3))));
        assert (m1_base == m1_ea - a_offset);
    end
    if (ok_m1 && m0_valid && m0_is_scan) assert (m0_scan_mask == STRIDE_MASK[a_scan_stride]);

    // Each read follows the one before, and the oldest is where the cursor says.
    if (ok_m1 && m0_valid && m0_is_scan && m1_valid && m1_is_scan) assert (m0_ea == scan_next_of(m1_ea, a_instr));
    if (ok_m1 && m0_valid && m0_is_scan && !m1_valid && m2_valid && m2_is_scan) assert (m0_ea == scan_next_of(m2_ea, a_instr));
    if (ok_m1 && m0_valid && m0_is_scan && !m1_valid && !m2_valid && x_valid && x_is_scan) assert (m0_ea == scan_next_of(x_ea, a_instr));
    if (ok_m2 && m1_valid && m1_is_scan && m2_valid && m2_is_scan) assert (m1_ea == scan_next_of(m2_ea, a_instr));
    if (ok_m2 && m1_valid && m1_is_scan && !m2_valid && x_valid && x_is_scan) assert (m1_ea == scan_next_of(x_ea, a_instr));
    if (ok_x && m2_valid && m2_is_scan && x_valid && x_is_scan) assert (m2_ea == scan_next_of(x_ea, a_instr));
    if (!scan_stop && ok_w && x_valid && x_is_scan) assert (x_ea == (g_active ? g_cursor : short_ea_of(path_w_base, x_instr)));
    if (!scan_stop && ok_x && m2_valid && m2_is_scan && !(x_valid && x_is_scan)) assert (m2_ea == (g_active ? g_cursor : short_ea_of(path_x_base, m2_instr)));
    if (!scan_stop && ok_m2 && m1_valid && m1_is_scan && !(m2_valid && m2_is_scan) && !(x_valid && x_is_scan)) assert (m1_ea == (g_active ? g_cursor : short_ea_of(path_m2_base, m1_instr)));
    if (!scan_stop && ok_m1 && m0_valid && m0_is_scan && !(m1_valid && m1_is_scan) && !(m2_valid && m2_is_scan) && !(x_valid && x_is_scan)) assert (m0_ea == (g_active ? g_cursor : short_ea_of(path_m1_base, m0_instr)));

    // ...and what A issues next follows the youngest read.
    if (ok_m0 && a_valid && a_is_scan && !scan_ending) begin
        if (m0_valid && m0_is_scan) assert ((a_scan_more ? scan_next_ea : ea) == scan_next_of(m0_ea, a_instr));
        else if (m1_valid && m1_is_scan) assert ((a_scan_more ? scan_next_ea : ea) == scan_next_of(m1_ea, a_instr));
        else if (m2_valid && m2_is_scan) assert ((a_scan_more ? scan_next_ea : ea) == scan_next_of(m2_ea, a_instr));
        else if (x_valid && x_is_scan) assert ((a_scan_more ? scan_next_ea : ea) == scan_next_of(x_ea, a_instr));
        else if (g_active) assert ((a_scan_more ? scan_next_ea : ea) == g_cursor);
        else assert (!a_scan_more);
    end

    if (ok_m0 && a_valid && a_is_scan && a_scan_more && !m0_valid) begin
        if (m1_valid && m1_is_scan) assert (m0_ea == m1_ea);
        else if (m2_valid && m2_is_scan) assert (m0_ea == m2_ea);
        else if (x_valid && x_is_scan) assert (m0_ea == x_ea);
    end

    // The cursor: only F has one, it has retired nothing since it began, and it is on F's path.
    if (g_active) begin
        assert (expected_pc == g_pc);
        assert (g_word[31:28] == OP_F);
        if (g_pc == word_addr) assert (g_word == word);
        if (a_valid && a_pc == g_pc && ok_w) assert (a_instr == g_word);
        if (w_valid) assert (w_instr == g_word && !w_writes);
        if (ok_w && x_valid) assert (x_is_scan && x_instr == g_word);
        if (ok_w && m2_valid) assert (m2_is_scan && m2_instr == g_word);
        if (ok_w && m1_valid) assert (m1_is_scan && m1_instr == g_word);
        if (ok_w && m0_valid) assert (m0_is_scan && m0_instr == g_word);
        if (!scan_ending && (!a_valid || a_pc != g_pc || !a_is_scan)) assert (short_ea_of(base_ptr, g_word) == g_cursor);
        assert (scan_reaches(short_ea_of(expected_base, g_word), ADDR_WIDTH'($signed(g_word[19:0])), g_cursor));
        if (scan_passes(short_ea_of(expected_base, g_word), ADDR_WIDTH'($signed(g_word[19:0])), g_cursor, a)) assert (cell_a != 8'd0);
        if (scan_passes(short_ea_of(expected_base, g_word), ADDR_WIDTH'($signed(g_word[19:0])), g_cursor, b)) assert (cell_b != 8'd0);
    end
    if (scan_active) assert (g_active ? (scan_pc == g_pc && scan_word == g_word) : scan_ending);
    if (g_active && !scan_active) assert (w_valid && w_instr[31:28] == OP_F);
    if (i1_valid) assert (i1_addr == i1_pc);
    if (i2_valid) assert (i2_addr == i2_pc);
    if (d_valid) assert (i3_addr == d_pc);

    // A read in the line of a tracked cell sees it.
    if (x_valid && x_is_scan && ok_w && x_ea[ADDR_WIDTH-1:LINE_BITS] == a[ADDR_WIDTH-1:LINE_BITS]) assert (a3_line && a3_value == cell_a);
    if (x_valid && x_is_scan && ok_w && x_ea[ADDR_WIDTH-1:LINE_BITS] == b[ADDR_WIDTH-1:LINE_BITS]) assert (b3_line && b3_value == cell_b);
    if (ok_x && m2_valid && m2_is_scan && m2_ea[ADDR_WIDTH-1:LINE_BITS] == a[ADDR_WIDTH-1:LINE_BITS]) assert (a2_line && a2_value == cell_a);
    if (ok_x && m2_valid && m2_is_scan && m2_ea[ADDR_WIDTH-1:LINE_BITS] == b[ADDR_WIDTH-1:LINE_BITS]) assert (b2_line && b2_value == cell_b);
    if (ok_m2 && m1_valid && m1_is_scan && m1_ea[ADDR_WIDTH-1:LINE_BITS] == a[ADDR_WIDTH-1:LINE_BITS]) assert (a1_line && a1_value == cell_a);
    if (ok_m2 && m1_valid && m1_is_scan && m1_ea[ADDR_WIDTH-1:LINE_BITS] == b[ADDR_WIDTH-1:LINE_BITS]) assert (b1_line && b1_value == cell_b);

    // F ending: nothing is left but F in A, and it works out where it stopped.
    if (scan_stop || scan_found_1 || scan_done) begin
        assert (a_valid && a_is_scan && !a_last);
        assert (!redirect && !x_mispredict);
        assert (expected_pc == a_pc);
    end
    if (scan_stop) begin
        assert (w_valid && w_pc == a_pc && w_instr == a_instr && w_next_pc == a_next_pc && !w_writes);
        assert (x_pc == a_pc && x_instr == a_instr && x_next_pc == a_next_pc && !x_is_branch);
        assert (m2_pc == a_pc && m2_instr == a_instr && m2_next_pc == a_next_pc && !m2_is_branch);
        assert (w_scan_zeros != '0);
        `SCAN_END(stop_end)
        assert (!scan_found_1 && !scan_done);
    end
    if (scan_found_1) begin
        assert (!w_valid && !x_valid && !m2_valid && !m1_valid && !m0_valid);
        assert (w_pc == a_pc && w_instr == a_instr && w_next_pc == a_next_pc);
        assert (x_pc == a_pc && x_instr == a_instr && x_next_pc == a_next_pc && !x_is_branch);
        `SCAN_END(found_end)
        assert (!scan_done);
    end
    if (scan_done) begin
        assert (!w_valid && !x_valid && !m2_valid && !m1_valid && !m0_valid);
        assert (w_pc == a_pc && w_instr == a_instr && w_next_pc == a_next_pc);
        `SCAN_END(done_end)
    end
end
