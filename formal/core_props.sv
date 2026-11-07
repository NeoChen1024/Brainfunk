// Properties of brainfunk_core against arbitrary memories and I/O: every word the
// instruction memory and every byte the data memory returns is free.
module core_props #(
    parameter int PC_WIDTH   = 20,
    parameter int ADDR_WIDTH = 16
) (
    input logic        clk,
    input logic        rst,
    input logic [31:0] imem_rdata,
    input logic [7:0]  dmem_rdata,
    input logic        io_in_valid,
    input logic [7:0]  io_in_data,
    input logic        io_out_ready
);
    logic [PC_WIDTH-1:0]   imem_addr, retire_pc, retire_next_pc;
    logic                  dmem_en, dmem_we, io_in_ready, io_out_valid, halted, trapped, retire_valid;
    logic [ADDR_WIDTH-1:0] dmem_addr, retire_base_ptr;
    logic [7:0]            dmem_wdata, io_out_data;
    logic [31:0]           retire_instr;

    brainfunk_core #(.PC_WIDTH(PC_WIDTH), .ADDR_WIDTH(ADDR_WIDTH)) core (.*);

    initial assume (rst);

    logic past_valid = 1'b0;
    always_ff @(posedge clk) past_valid <= 1'b1;

    // ---- the architectural state the retire port implies ----
    logic [PC_WIDTH-1:0]   expected_pc = '0;
    logic [ADDR_WIDTH-1:0] expected_base = '0;

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
    logic [ADDR_WIDTH-1:0] addr_ea, narrow_ea, long_sum;
    logic [PC_WIDTH-1:0]   sequential, target;
    assign op         = retire_instr[31:28];
    assign data       = retire_instr[7:0];
    assign addr_ea    = expected_base + ADDR_WIDTH'($signed(retire_instr[19:8]));
    assign narrow_ea  = expected_base + ADDR_WIDTH'($signed(retire_instr[27:20]));
    assign long_sum   = expected_base + ADDR_WIDTH'($signed(retire_instr[19:0]));
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

    logic is_input, writes;
    assign is_input = op == OP_IO && data == 8'd0;
    assign writes   = op == OP_A || op == OP_S || op == OP_MUL || is_input;

    always_ff @(posedge clk) begin
        if (past_valid && !$past(rst) && !rst) begin
            // Fetch follows pc, and retirement is in program order.
            assert (imem_addr == expected_pc);
            assert (retire_pc == expected_pc);

            // Only M and F move base_ptr.  Not only at retire: this is the invariant
            // that lets induction relate the core's base_ptr to the retired one.
            if (op != OP_F && op != OP_M) assert (retire_base_ptr == expected_base);

            // Halt and trap are sticky, and nothing happens afterwards.
            if ($past(halted))  assert (halted);
            if ($past(trapped)) assert (trapped);
            if (halted || trapped) begin
                assert (!retire_valid && !dmem_en && !io_in_ready && !io_out_valid);
            end

            // A trap is only ever for an illegal word.
            if (trapped) assert (!legal);
            assert (!(halted && trapped));
            if ($past(retire_valid) && $past(op) == OP_H) assert (halted);

            // A write happens in the cycle its instruction retires, at the effective address.
            if (dmem_we) begin
                assert (dmem_en);
                assert (retire_valid && writes);
                assert (dmem_addr == addr_ea);
            end

            if (retire_valid) begin
                assert (legal);
                assert (dmem_we == writes);

                // pc update rule
                case (op)
                    OP_JE: assert (retire_next_pc == (dmem_rdata == 8'd0 ? target : sequential));
                    OP_JN: assert (retire_next_pc == (dmem_rdata != 8'd0 ? target : sequential));
                    OP_H:  assert (retire_next_pc == expected_pc);
                    default: assert (retire_next_pc == sequential);
                endcase

                if (op == OP_M) assert (retire_base_ptr == long_sum);

                // Results
                if (op == OP_S) assert (dmem_wdata == data);
                if (op == OP_A) assert (dmem_wdata == 8'(dmem_rdata + data));
                if (is_input) begin
                    assert (io_in_valid && io_in_ready);
                    assert (dmem_wdata == io_in_data);
                end
                if (op == OP_IO && data == 8'd1) assert (io_out_valid && io_out_ready);

                // F finishes on a zero cell, without touching pc on the way.
                if (op == OP_F) assert (dmem_rdata == 8'd0);
            end

            // The core must not lose I/O: a byte stays offered until it is taken.
            if ($past(io_out_valid) && !$past(io_out_ready)) begin
                assert (io_out_valid);
                assert (io_out_data == $past(io_out_data));
            end
        end
    end

`ifdef BOUNDED_LATENCY
    // Without F and I/O, whose duration depends on the outside world, every instruction
    // retires within a fixed number of cycles.
    logic [3:0] busy = '0;
    always_ff @(posedge clk) begin
        assume (imem_rdata[31:28] != OP_F && imem_rdata[31:28] != OP_IO);
        if (rst || retire_valid || halted || trapped) busy <= '0;
        else if (busy != 4'hF) busy <= busy + 4'd1;
        if (past_valid && !rst) assert (busy <= 4'd3);
    end
`endif

    // ---- every path is reachable, or the assertions above prove nothing ----
    always_ff @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (retire_valid && op == OP_A);
            cover (retire_valid && op == OP_S);
            cover (retire_valid && op == OP_MUL);
            cover (retire_valid && op == OP_M);
            cover (retire_valid && op == OP_F && retire_base_ptr != expected_base);
            cover (retire_valid && op == OP_JE && retire_next_pc == target && target != sequential);
            cover (retire_valid && op == OP_JN && retire_next_pc == target && target != sequential);
            cover (retire_valid && is_input);
            cover (retire_valid && op == OP_IO && data == 8'd1);
            cover (halted);
            cover (trapped);
        end
    end
endmodule
