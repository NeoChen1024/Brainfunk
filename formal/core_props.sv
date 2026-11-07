// Properties of brainfunk_core against abstract memories and arbitrary I/O.
//
// Two tape cells and one instruction word, at addresses the solver chooses, behave like real
// memory, including the read latency. Every other read returns anything. So a property about
// "the cell at a" holds for every cell, and one about "dst at a, source at b" for every pair.
// A read colliding with a write of the same cell returns anything too, as block RAM may.
//
// The caches' signals are free as well, so any hit may be a miss, and any stall can happen. Only
// a read that hits has to return real data.
//
// The reference state is what the retire port implies, instruction by instruction.
module core_props #(
    parameter int PC_WIDTH   = 5,
    parameter int ADDR_WIDTH = 3
) (
    input logic        clk,
    input logic        rst,
    input logic [31:0] imem_rdata,
    input logic        imem_hit,
    input logic        imem_hold,
    input logic        imem_refetch,
    // Free: a guess can be anything, and the core has to get everything right anyway.
    input logic                next_taken,
    input logic [PC_WIDTH-1:0] next_target,
    input logic [1:0]          d_counter,
    input logic [7:0]  dmem_rdata,
    input logic        dmem_hit,
    input logic        dmem_busy,
    input logic        io_in_valid,
    input logic [7:0]  io_in_data,
    input logic        io_out_ready
);
    logic [PC_WIDTH-1:0]   imem_addr, imem_miss_addr, retire_pc, retire_next_pc;
    logic                  imem_en, imem_miss, dmem_we, dmem_miss, io_in_ready, io_out_valid, halted, trapped;
    logic                  retire_valid, formal_w_valid;
    logic [ADDR_WIDTH-1:0] dmem_raddr, dmem_waddr, dmem_miss_addr, retire_base_ptr;
    logic [7:0]            dmem_wdata, io_out_data;
    logic [31:0]           retire_instr;
    logic                  pred_front_en, next_we, next_valid, dir_we;
    logic [PC_WIDTH-1:0]   pred_fetch_pc, pred_i1_pc, next_key, next_new_target, dir_pc;
    logic [1:0]            dir_counter;

    brainfunk_core #(.PC_WIDTH(PC_WIDTH), .ADDR_WIDTH(ADDR_WIDTH)) core (.*);

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

    // ---- data memory: two tracked cells, three edges of latency ----
    (* anyconst *) logic [ADDR_WIDTH-1:0] a, b;
    (* anyconst *) logic [7:0]            a_init, b_init;
    logic [7:0] cell_a, cell_b;

    logic       a1_hit, a2_hit, a3_hit, b1_hit, b2_hit, b3_hit;
    logic [7:0] a1_value, a2_value, a3_value, b1_value, b2_value, b3_value;

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

        // Input reads its cell before taking a byte, so that its write cannot miss.
        if (in_taken) assume (dmem_hit);
    end

    // ---- the architectural state the retire port implies ----
    logic [PC_WIDTH-1:0]   expected_pc = '0;
    logic [ADDR_WIDTH-1:0] expected_base = '0, scan_base = '0;

    logic w_is_scan_read; // a read of F that did not end it
    assign w_is_scan_read = formal_w_valid && !retire_valid && op == OP_F;

    always_ff @(posedge clk) begin
        if (rst) begin
            expected_pc   <= '0;
            expected_base <= '0;
            scan_base     <= '0;
        end else if (retire_valid) begin
            expected_pc   <= retire_next_pc;
            expected_base <= retire_base_ptr;
            scan_base     <= retire_base_ptr;
        end else if (w_is_scan_read) begin
            scan_base     <= scan_base + ADDR_WIDTH'($signed(retire_instr[19:0]));
        end
    end

    // ---- decode, independently of the core ----
    logic [3:0]            op;
    logic [7:0]            data;
    logic [ADDR_WIDTH-1:0] addr_ea, short_ea, long_sum, scan_test;
    logic [PC_WIDTH-1:0]   sequential, target;
    assign op         = retire_instr[31:28];
    assign data       = retire_instr[7:0];
    assign addr_ea    = expected_base + ADDR_WIDTH'($signed(retire_instr[19:8]));
    assign short_ea   = expected_base + ADDR_WIDTH'($signed(retire_instr[27:20]));
    assign long_sum   = expected_base + ADDR_WIDTH'($signed(retire_instr[19:0]));
    assign scan_test  = scan_base + ADDR_WIDTH'($signed(retire_instr[27:20]));
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

    logic is_input, is_output, writes;
    assign is_input  = op == OP_IO && data == 8'd0;
    assign is_output = op == OP_IO && data == 8'd1;
    assign writes    = op == OP_A || op == OP_S || op == OP_MUL || is_input;

    // ---- I/O bytes taken and not yet accounted for by a retire ----
    logic       in_taken = 1'b0, out_taken = 1'b0;
    logic [7:0] in_byte;

    always_ff @(posedge clk) begin
        if (rst) begin
            in_taken  <= 1'b0;
            out_taken <= 1'b0;
        end else begin
            if (io_in_valid && io_in_ready) begin
                in_taken <= 1'b1;
                in_byte  <= io_in_data;
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
                    OP_F: assert (retire_base_ptr == scan_base);
                    default: assert (retire_base_ptr == expected_base);
                endcase

                // Results, on the architectural values of the cells
                if (op == OP_S) assert (dmem_wdata == data);
                if (op == OP_A && addr_ea == a) assert (dmem_wdata == 8'(cell_a + data));
                if (op == OP_MUL && addr_ea == a && short_ea == b) begin
`ifdef FORMAL_ABSTRACT_MUL
                    assert (dmem_wdata == 8'(cell_a + (cell_b ^ data)));
`else
                    assert (dmem_wdata == 8'(cell_a + 8'(cell_b * data)));
`endif
                end
                if (op == OP_F && scan_test == a) assert (cell_a == 8'd0);
                if (is_input) begin
                    assert (in_taken);
                    assert (dmem_wdata == in_byte);
                end
                if (is_output) begin
                    assert (out_taken);
                    if (addr_ea == a) assert (io_out_data == cell_a);
                end
            end

            // A read of F that does not end it saw a non-zero cell, at the right place.
            if (w_is_scan_read) begin
                assert (retire_base_ptr == scan_base);
                if (scan_test == a) assert (cell_a != 8'd0);
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

`ifdef NO_MISSES
    // The pipeline alone, without the solver also having to consider every miss: it gets further.
    always_ff @(posedge clk) assume (imem_hit && dmem_hit && !imem_hold && !imem_refetch && !dmem_busy);
`endif

`ifdef BOUNDED_LATENCY
    // Without F and I/O, whose duration depends on the outside world, an instruction retires
    // within a fixed number of cycles of the one before, including a misprediction.
    logic [4:0] busy = '0;
    always_ff @(posedge clk) begin
        assume (imem_rdata[31:28] != OP_F && imem_rdata[31:28] != OP_IO);
        assume (imem_hit && dmem_hit && !imem_hold && !imem_refetch && !dmem_busy);
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
            cover (retire_valid && op == OP_M);
            cover (retire_valid && op == OP_F && retire_base_ptr != expected_base);
            cover (retire_valid && op == OP_JE && retire_next_pc == target && target != sequential);
            cover (retire_valid && op == OP_JE && retire_next_pc == sequential && target != sequential);
            cover (retire_valid && op == OP_JN && retire_next_pc == target && target != sequential);
            cover (retire_valid && op == OP_JN && retire_next_pc == sequential && target != sequential);
            cover (retire_valid && is_input);
            cover (retire_valid && is_output);
            cover (halted);
            cover (trapped);
            cover (dmem_miss);
            cover (retired >= 8'd1 && dmem_miss && $past(retire_valid));

            // Forwarding: two neighbouring instructions on the tracked cell.
            cover (retired >= 8'd2 && retire_valid && op == OP_A && addr_ea == a && $past(retire_valid) &&
                   $past(dmem_we) && $past(dmem_waddr) == a);
        end
    end
endmodule
