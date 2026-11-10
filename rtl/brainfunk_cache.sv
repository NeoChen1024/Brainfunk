// The instruction cache between the core and the code in the line memory: 16 KiB, direct-mapped,
// 32-byte lines, 512 lines. The tape needs none; see brainfunk_tape.sv.
//
// It keeps the three-edge read latency of the core's memory ports. Tags are read at the same time as
// data, and the hit is a registered comparison that arrives with the data.
//
// A write of a tag or data word during a read of the same place would read undefined data
// (see brainfunk_mem.sv), so tags are only written while nothing is looking at them.

// ---- instruction cache ----
//
// A miss holds the front end. If a redirect abandons the instruction that missed, the line is
// still filled, and fetching goes on meanwhile. Tag writes hold the front end for their cycle.
module icache #(
    parameter int PC_WIDTH = 20
) (
    input  logic                clk,
    input  logic                rst,
    output logic                ready,

    input  logic                imem_en,
    input  logic [PC_WIDTH-1:0] imem_addr,
    output logic [31:0]         imem_rdata,
    output logic                imem_hit,
    input  logic                imem_miss,
    input  logic [PC_WIDTH-1:0] imem_miss_addr,
    output logic                imem_hold,
    output logic                imem_refetch,

    output logic                req_valid,
    output logic [17:0]         req_line,
    input  logic                req_ready,
    input  logic                rd_valid,
    input  logic [31:0]         rd_data
);
    localparam int TAG_WIDTH = PC_WIDTH - 12;

    typedef enum logic [2:0] {ST_RESET, ST_IDLE, ST_INVALIDATE, ST_REQUEST, ST_FILL, ST_TAG, ST_DONE} state_t;
    state_t state;

    logic [8:0]          index_q;    // the line being invalidated or filled
    logic [TAG_WIDTH-1:0] tag_q;
    logic [2:0]          word_q;
    logic                tag_we;
    logic                tag_valid_in;
    logic [TAG_WIDTH:0]  tag_rdata;
    logic                fill_we;
    logic [11:0]         fill_addr;
    logic [31:0]         fill_data;

    ram_bank #(.WIDTH(TAG_WIDTH + 1), .ADDR_WIDTH(9)) tag (
        .clk(clk),
        .en(imem_en),
        .raddr(imem_addr[11:3]),
        .rdata(tag_rdata),
        .we(tag_we),
        .waddr(index_q),
        .wdata({tag_valid_in, tag_q})
    );

    banked_ram #(.WIDTH(32), .ADDR_WIDTH(12), .BANK_WIDTH(11)) data (
        .clk(clk),
        .en(imem_en),
        .raddr(imem_addr[11:0]),
        .rdata(imem_rdata),
        .we(fill_we),
        .waddr(fill_addr),
        .wdata(fill_data)
    );

    logic [PC_WIDTH-1:0] addr_1, addr_2;
    always_ff @(posedge clk) begin
        fill_we   <= state == ST_FILL && rd_valid;
        fill_addr <= {index_q, word_q};
        fill_data <= rd_data;
        if (imem_en) begin
            addr_1   <= imem_addr;
            addr_2   <= addr_1;
            imem_hit <= tag_rdata == {1'b1, addr_2[PC_WIDTH-1:12]};
        end
    end

    assign tag_we       = state == ST_RESET || state == ST_INVALIDATE || state == ST_TAG;
    assign tag_valid_in = state == ST_TAG;
    assign imem_hold    = state == ST_INVALIDATE || state == ST_TAG;
    assign imem_refetch = state == ST_DONE;
    assign ready        = state != ST_RESET;
    assign req_valid    = state == ST_REQUEST;
    assign req_line     = 18'({tag_q, index_q}); // code is the bottom half of the part

    always_ff @(posedge clk) begin
        if (rst) begin
            state   <= ST_RESET;
            index_q <= '0;
        end else begin
            unique case (state)
                ST_RESET: begin
                    index_q <= index_q + 9'd1;
                    if (index_q == '1) state <= ST_IDLE;
                end
                ST_IDLE: if (imem_miss) begin
                    index_q <= imem_miss_addr[11:3];
                    tag_q   <= imem_miss_addr[PC_WIDTH-1:12];
                    state   <= ST_INVALIDATE;
                end
                ST_INVALIDATE: state <= ST_REQUEST;
                ST_REQUEST: if (req_ready) begin
                    word_q <= '0;
                    state  <= ST_FILL;
                end
                ST_FILL: if (rd_valid) begin
                    word_q <= word_q + 3'd1;
                    if (word_q == 3'd7) state <= ST_TAG;
                end
                ST_TAG: state <= ST_DONE;
                default: state <= ST_IDLE; // ST_DONE
            endcase
        end
    end
endmodule
