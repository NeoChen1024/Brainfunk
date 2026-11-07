// Brainfunk core: multi-cycle FSM implementing doc/bitcode.txt.
//
// One instruction at a time. A single effective-address adder serves every memory
// access and every base_ptr update. All writes happen in the cycle an instruction
// retires, so a retiring instruction's whole effect is visible at the retire port.
//
// Instruction memory and data memory are synchronous: data arrives the cycle after
// the address, and dmem_rdata must hold until the next read.
//
// The fused instructions (MULC, DJN, MJN, SRUN) are not implemented yet and trap.
module brainfunk_core #(
    parameter int PC_WIDTH   = 20,
    parameter int ADDR_WIDTH = 16
) (
    input  logic                  clk,
    input  logic                  rst,

    output logic [PC_WIDTH-1:0]   imem_addr,
    input  logic [31:0]           imem_rdata,

    output logic                  dmem_en,
    output logic                  dmem_we,
    output logic [ADDR_WIDTH-1:0] dmem_addr,
    output logic [7:0]            dmem_wdata,
    input  logic [7:0]            dmem_rdata,

    input  logic                  io_in_valid,
    input  logic [7:0]            io_in_data,
    output logic                  io_in_ready,
    output logic                  io_out_valid,
    output logic [7:0]            io_out_data,
    input  logic                  io_out_ready,

    output logic                  halted,
    output logic                  trapped,

    // Valid in the cycle an instruction finishes; next_pc and base_ptr are the values
    // after it. H retires once, with next_pc equal to pc. An illegal word does not retire.
    output logic                  retire_valid,
    output logic [PC_WIDTH-1:0]   retire_pc,
    output logic [31:0]           retire_instr,
    output logic [PC_WIDTH-1:0]   retire_next_pc,
    output logic [ADDR_WIDTH-1:0] retire_base_ptr
);

    localparam logic [3:0] OP_A = 4'd1, OP_S = 4'd2, OP_MUL = 4'd3, OP_F = 4'd4, OP_M = 4'd5,
                           OP_JE = 4'd6, OP_JN = 4'd7, OP_IO = 4'd8, OP_H = 4'd9;

    typedef enum logic [3:0] {
        ST_FETCH,        // imem_addr = pc
        ST_DECODE,       // imem_rdata holds the instruction; first memory access
        ST_ADD_WRITE,    // A: dmem_rdata holds the old cell
        ST_MUL_READ_DST, // MUL: dmem_rdata holds the source
        ST_MUL_WRITE,    // MUL: dmem_rdata holds the old destination
        ST_SCAN_TEST,    // F: dmem_rdata holds the tested cell
        ST_SCAN_READ,    // F: base_ptr has moved, read the next cell
        ST_BRANCH_TEST,  // JE, JN: dmem_rdata holds the tested cell
        ST_INPUT,        // IO: wait for an input byte
        ST_OUTPUT_LATCH, // IO: dmem_rdata holds the output byte
        ST_OUTPUT,       // IO: wait for the output to be taken
        ST_HALTED,
        ST_TRAPPED
    } state_t;

    state_t state, state_d;
    logic [PC_WIDTH-1:0]   pc, pc_d;
    logic [ADDR_WIDTH-1:0] base_ptr, base_ptr_d;
    logic [31:0]           instr_q;
    logic [7:0]            operand_q; // MUL source, output byte

    // Valid from ST_DECODE on: the memory holds the word only in that cycle.
    logic [31:0] instr;
    assign instr = (state == ST_DECODE) ? imem_rdata : instr_q;

    logic [3:0]         opcode;
    logic [7:0]         low_byte;
    logic [7:0]         top_byte;
    logic signed [31:0] narrow_offset, addr_offset, long_offset;
    assign opcode        = instr[31:28];
    assign low_byte      = instr[7:0];
    assign top_byte      = instr[27:20];
    assign narrow_offset = 32'($signed(instr[27:20]));
    assign addr_offset   = 32'($signed(instr[19:8]));
    assign long_offset   = 32'($signed(instr[19:0]));

    // doc/bincode.txt: a word this does not accept traps when executed.
    logic legal;
    always_comb begin
        unique case (opcode)
            OP_A, OP_S: legal = top_byte == 8'd0;
            OP_MUL, OP_F, OP_JE, OP_JN: legal = 1'b1;
            OP_M:  legal = top_byte == 8'd0;
            OP_IO: legal = top_byte == 8'd0 && low_byte[7:1] == 7'd0;
            OP_H:  legal = instr[27:0] == 28'd0;
            default: legal = 1'b0;
        endcase
    end

    // The effective-address adder.
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [31:0]    ea_offset;   // only its low bits matter: addresses wrap
    /* verilator lint_on UNUSEDSIGNAL */
    logic [ADDR_WIDTH-1:0] ea;
    assign ea = base_ptr + ea_offset[ADDR_WIDTH-1:0];

    logic [PC_WIDTH-1:0] pc_next, pc_target;
    assign pc_next   = pc + PC_WIDTH'(1);
    assign pc_target = pc_next + long_offset[PC_WIDTH-1:0];

    logic [7:0] product;
    assign product = 8'(operand_q * low_byte);

    always_comb begin
        state_d      = state;
        pc_d         = pc;
        base_ptr_d   = base_ptr;
        ea_offset    = addr_offset;
        dmem_en      = 1'b0;
        dmem_we      = 1'b0;
        dmem_wdata   = 8'd0;
        io_in_ready  = 1'b0;
        io_out_valid = 1'b0;
        retire_valid = 1'b0;

        unique case (state)
            ST_FETCH: state_d = ST_DECODE;

            ST_DECODE: begin
                if (!legal) begin
                    state_d = ST_TRAPPED;
                end else begin
                    unique case (opcode)
                        OP_A: begin
                            dmem_en = 1'b1;
                            state_d = ST_ADD_WRITE;
                        end
                        OP_S: begin
                            dmem_en      = 1'b1;
                            dmem_we      = 1'b1;
                            dmem_wdata   = low_byte;
                            pc_d         = pc_next;
                            retire_valid = 1'b1;
                            state_d      = ST_FETCH;
                        end
                        OP_MUL: begin
                            ea_offset = narrow_offset;
                            dmem_en   = 1'b1;
                            state_d   = ST_MUL_READ_DST;
                        end
                        OP_F: begin
                            ea_offset = narrow_offset;
                            dmem_en   = 1'b1;
                            state_d   = ST_SCAN_TEST;
                        end
                        OP_M: begin
                            ea_offset    = long_offset;
                            base_ptr_d   = ea;
                            pc_d         = pc_next;
                            retire_valid = 1'b1;
                            state_d      = ST_FETCH;
                        end
                        OP_JE, OP_JN: begin
                            ea_offset = narrow_offset;
                            dmem_en   = 1'b1;
                            state_d   = ST_BRANCH_TEST;
                        end
                        OP_IO: begin
                            if (low_byte[0]) begin
                                dmem_en = 1'b1;
                                state_d = ST_OUTPUT_LATCH;
                            end else begin
                                state_d = ST_INPUT;
                            end
                        end
                        default: begin // OP_H: legal guarantees nothing else reaches here
                            retire_valid = 1'b1;
                            state_d      = ST_HALTED;
                        end
                    endcase
                end
            end

            ST_ADD_WRITE: begin
                dmem_en      = 1'b1;
                dmem_we      = 1'b1;
                dmem_wdata   = dmem_rdata + low_byte;
                pc_d         = pc_next;
                retire_valid = 1'b1;
                state_d      = ST_FETCH;
            end

            ST_MUL_READ_DST: begin
                dmem_en = 1'b1;
                state_d = ST_MUL_WRITE;
            end

            ST_MUL_WRITE: begin
                dmem_en      = 1'b1;
                dmem_we      = 1'b1;
                dmem_wdata   = dmem_rdata + product;
                pc_d         = pc_next;
                retire_valid = 1'b1;
                state_d      = ST_FETCH;
            end

            ST_SCAN_TEST: begin
                if (dmem_rdata == 8'd0) begin
                    pc_d         = pc_next;
                    retire_valid = 1'b1;
                    state_d      = ST_FETCH;
                end else begin
                    ea_offset  = long_offset;
                    base_ptr_d = ea;
                    state_d    = ST_SCAN_READ;
                end
            end

            ST_SCAN_READ: begin
                ea_offset = narrow_offset;
                dmem_en   = 1'b1;
                state_d   = ST_SCAN_TEST;
            end

            ST_BRANCH_TEST: begin
                pc_d         = ((dmem_rdata == 8'd0) == (opcode == OP_JE)) ? pc_target : pc_next;
                retire_valid = 1'b1;
                state_d      = ST_FETCH;
            end

            ST_INPUT: begin
                io_in_ready = 1'b1;
                if (io_in_valid) begin
                    dmem_en      = 1'b1;
                    dmem_we      = 1'b1;
                    dmem_wdata   = io_in_data;
                    pc_d         = pc_next;
                    retire_valid = 1'b1;
                    state_d      = ST_FETCH;
                end
            end

            ST_OUTPUT_LATCH: state_d = ST_OUTPUT;

            ST_OUTPUT: begin
                io_out_valid = 1'b1;
                if (io_out_ready) begin
                    pc_d         = pc_next;
                    retire_valid = 1'b1;
                    state_d      = ST_FETCH;
                end
            end

            default: ; // ST_HALTED, ST_TRAPPED: sticky
        endcase
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            state    <= ST_FETCH;
            pc       <= '0;
            base_ptr <= '0;
        end else begin
            state    <= state_d;
            pc       <= pc_d;
            base_ptr <= base_ptr_d;
            if (state == ST_DECODE) instr_q <= imem_rdata;
            if (state == ST_MUL_READ_DST || state == ST_OUTPUT_LATCH) operand_q <= dmem_rdata;
        end
    end

`ifdef FORMAL
    // Which instruction each state can be executing.  Implied by reset and the state
    // machine; stating it lets induction reject states that are not reachable.
    always_ff @(posedge clk) begin
        if (!rst) begin
            if (state == ST_ADD_WRITE) assert (opcode == OP_A && legal);
            if (state == ST_MUL_READ_DST || state == ST_MUL_WRITE) assert (opcode == OP_MUL);
            if (state == ST_SCAN_TEST || state == ST_SCAN_READ) assert (opcode == OP_F);
            if (state == ST_BRANCH_TEST) assert (opcode == OP_JE || opcode == OP_JN);
            if (state == ST_INPUT) assert (opcode == OP_IO && low_byte == 8'd0 && legal);
            if (state == ST_OUTPUT_LATCH || state == ST_OUTPUT)
                assert (opcode == OP_IO && low_byte == 8'd1 && legal);
            if (state == ST_HALTED) assert (opcode == OP_H && legal);
            if (state == ST_TRAPPED) assert (!legal);
        end
    end
`endif

    assign imem_addr       = pc;
    assign dmem_addr       = ea;
    assign io_out_data     = operand_q;
    assign halted          = state == ST_HALTED;
    assign trapped         = state == ST_TRAPPED;
    assign retire_pc       = pc;
    assign retire_instr    = instr;
    assign retire_next_pc  = pc_d;
    assign retire_base_ptr = base_ptr_d;

endmodule
