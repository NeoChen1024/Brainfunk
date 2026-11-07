// Direct-mapped caches between the core and the SDRAM: 16 KiB each, 32-byte lines, 512 lines.
//
// Both keep the three-edge read latency of the core's memory ports. Tags are read at the same
// time as data, and the hit is a registered comparison that arrives with the data.
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

// ---- data cache ----
//
// Write-back, write-allocate. The core finds a miss in X, flushes itself and restarts from the
// instruction that missed, and waits in A for dmem_busy to fall. Nothing is in the pipeline by
// the time this module uses the read port or writes a tag.
//
// A second read port serves a MUL's source. Every cell of a line has a block RAM of its own, so the
// one the second port reads takes its address, and the others the first port's; the core never asks
// for two cells in the same bank and different lines, and nothing while this module has the read port.
// The core decodes the bank, a cycle ahead, so that only a multiplexer comes before the block RAM.
// The tags have a copy for it.
//
// A retried instruction must find everything it needs in the cache at once, or two lines could
// evict each other for ever. MUL is the only one that touches two cells, whose addresses are at
// most about 2 KiB apart, so they never share an index in a cache of this size.
module dcache #(
    parameter int ADDR_WIDTH = 16,   // cells; at least 15, because the tag cannot be empty
    parameter bit CLEAR_TAPE = 1'b0  // a reset also zeroes the tape in SDRAM
) (
    input  logic                  clk,
    input  logic                  rst,
    output logic                  ready,

    input  logic [ADDR_WIDTH-1:0] dmem_raddr,
    output logic [7:0]            dmem_rdata,
    output logic                  dmem_hit,
    output logic [31:0]           dmem_zero,   // with dmem_rdata: which cells of its line are zero
    input  logic [31:0]           dmem_bank2,    // which bank the second port reads, one-hot, or none
    input  logic [ADDR_WIDTH-1:0] dmem_raddr2,
    output logic [7:0]            dmem_rdata2,
    output logic                  dmem_hit_both, // dmem_hit, and the second port hit if it was read
    input  logic                  dmem_we,
    input  logic [ADDR_WIDTH-1:0] dmem_waddr,
    input  logic [7:0]            dmem_wdata,
    input  logic                  dmem_miss,
    input  logic [ADDR_WIDTH-1:0] dmem_miss_addr,
    output logic                  dmem_busy,

    output logic                  req_valid,
    output logic                  req_write,
    output logic [17:0]           req_line,
    input  logic                  req_ready,
    input  logic                  rd_valid,
    input  logic [31:0]           rd_data,
    input  logic                  wr_fetch,
    output logic [31:0]           wr_data
);
    localparam int TAG_WIDTH = ADDR_WIDTH - 14;

    typedef enum logic [3:0] {ST_RESET, ST_CLEAR, ST_CLEAR_WAIT, ST_IDLE, ST_TAG_1, ST_TAG_2, ST_DECIDE,
                              ST_WRITEBACK, ST_WRITEBACK_WAIT, ST_REQUEST, ST_FILL, ST_TAG_WRITE} state_t;
    state_t state;

    logic [8:0]            index_q;
    logic [TAG_WIDTH-1:0]  tag_q, victim_tag;
    logic [2:0]            word_q;
    logic [ADDR_WIDTH-6:0] clear_line;
    logic [3:0]            fetched;
    logic                  clearing;   // from a reset until the last zeroes have gone out
    logic [2:0]            drain;

    logic [TAG_WIDTH:0]    tag_rdata, tag2_rdata;
    logic                  tag_we, tag_valid_in;
    logic                  dirty_rdata;

    // The read port belongs to the core except while handling a miss.
    logic                  fsm_owns_read;
    logic [ADDR_WIDTH-1:0] fsm_raddr;
    logic [ADDR_WIDTH-1:0] raddr;
    assign fsm_owns_read = dmem_busy;
    assign raddr         = fsm_owns_read ? fsm_raddr : dmem_raddr;

    ram_bank #(.WIDTH(TAG_WIDTH + 1), .ADDR_WIDTH(9)) tag (
        .clk(clk),
        .en(1'b1),
        .raddr(raddr[13:5]),
        .rdata(tag_rdata),
        .we(tag_we),
        .waddr(index_q),
        .wdata({tag_valid_in, tag_q})
    );

    ram_bank #(.WIDTH(TAG_WIDTH + 1), .ADDR_WIDTH(9)) tag2 (
        .clk(clk),
        .en(1'b1),
        .raddr(dmem_raddr2[13:5]),
        .rdata(tag2_rdata),
        .we(tag_we),
        .waddr(index_q),
        .wdata({tag_valid_in, tag_q})
    );

    // Dirty bits are apart from the tags because every write sets one, which must not disturb
    // a tag being read for a younger instruction. They are only read while handling a miss.
    ram_bank #(.WIDTH(1), .ADDR_WIDTH(9)) dirty (
        .clk(clk),
        .en(1'b1),
        .raddr(index_q),
        .rdata(dirty_rdata),
        .we(dmem_we || state == ST_TAG_WRITE),
        .waddr(dmem_we ? dmem_waddr[13:5] : index_q),
        .wdata(dmem_we)
    );

    // One block RAM per cell of a line, so that a read gets the whole line at once. The core writes
    // one, a fill writes the four of a word. F needs the width; see the core.
    logic         filling;
    logic [11:0]  fill_addr;
    logic [31:0]  fill_data;
    logic [255:0] line_rdata;
    always_ff @(posedge clk) begin
        filling   <= state == ST_FILL && rd_valid;
        fill_addr <= {index_q, word_q};
        fill_data <= rd_data;
    end

    generate
        for (genvar i = 0; i < 32; i++) begin : g_byte
            ram_bank #(.WIDTH(8), .ADDR_WIDTH(9)) ram (
                .clk(clk),
                .en(1'b1),
                .raddr(dmem_bank2[i] ? dmem_raddr2[13:5] : raddr[13:5]),
                .rdata(line_rdata[8*i +: 8]),
                .we(filling ? fill_addr[2:0] == 3'(i / 4) : (dmem_we && dmem_waddr[4:0] == 5'(i))),
                .waddr(filling ? fill_addr[11:3] : dmem_waddr[13:5]),
                .wdata(filling ? fill_data[8*(i % 4) +: 8] : dmem_wdata)
            );
        end
    endgenerate

    logic [ADDR_WIDTH-1:0] addr_1, addr_2, addr2_1, addr2_2;
    logic                  re2_1, re2_2;
    always_ff @(posedge clk) begin
        addr_1     <= raddr;
        addr_2     <= addr_1;
        addr2_1    <= dmem_raddr2;
        addr2_2    <= addr2_1;
        re2_1      <= |dmem_bank2;
        re2_2      <= re2_1;
        dmem_hit_both <= tag_rdata == {1'b1, addr_2[ADDR_WIDTH-1:14]} &&
                         (!re2_2 || tag2_rdata == {1'b1, addr2_2[ADDR_WIDTH-1:14]});
        dmem_rdata2 <= line_rdata[8*addr2_2[4:0] +: 8];
        dmem_hit   <= tag_rdata == {1'b1, addr_2[ADDR_WIDTH-1:14]};
        dmem_rdata <= line_rdata[8*addr_2[4:0] +: 8];
        for (int i = 0; i < 32; i++) dmem_zero[i] <= line_rdata[8*i +: 8] == 8'd0;
    end

    // A line being written back is already in line_rdata, so picking its word can take a register.
    always_ff @(posedge clk) wr_data <= clearing ? 32'd0 : line_rdata[32*addr_1[4:2] +: 32];

    assign tag_we       = state == ST_RESET || state == ST_TAG_WRITE;
    assign tag_valid_in = state == ST_TAG_WRITE;
    assign ready        = state != ST_RESET && state != ST_CLEAR && state != ST_CLEAR_WAIT;

    // SDRAM line address: the tape is the top half of the part.
    logic [ADDR_WIDTH-6:0] line_address;
    always_comb begin
        unique case (state)
            ST_CLEAR, ST_CLEAR_WAIT: line_address = clear_line;
            ST_WRITEBACK, ST_WRITEBACK_WAIT: line_address = {victim_tag, index_q};
            default: line_address = {tag_q, index_q};
        endcase
    end
    assign req_line  = {1'b1, 17'(line_address)};
    assign req_valid = state == ST_CLEAR || state == ST_WRITEBACK || state == ST_REQUEST;
    assign req_write = state != ST_REQUEST;

    always_ff @(posedge clk) begin
        if (rst) begin
            state      <= ST_RESET;
            index_q    <= '0;
            dmem_busy  <= 1'b1;
            clearing   <= CLEAR_TAPE;
            clear_line <= '0;
            fetched    <= '0;
        end else begin
            // Reading the victim line for a write-back: address, then two edges of latency,
            // which the controller's wr_fetch allows for.
            if (wr_fetch) begin
                fsm_raddr[4:2] <= fetched[2:0];
                fetched        <= fetched + 4'd1;
            end

            unique case (state)
                ST_RESET: begin
                    index_q <= index_q + 9'd1;
                    if (index_q == '1) state <= CLEAR_TAPE ? ST_CLEAR : ST_IDLE;
                    if (index_q == '1 && !CLEAR_TAPE) dmem_busy <= 1'b0;
                end

                ST_CLEAR: if (req_ready) begin
                    fetched <= '0;
                    drain   <= '0;
                    state   <= ST_CLEAR_WAIT;
                end

                ST_CLEAR_WAIT: if (fetched == 4'd8) begin
                    // Write data comes out a few cycles after it is fetched.
                    drain <= drain + 3'd1;
                    if (drain == 3'd4) begin
                        clear_line <= clear_line + 1'b1;
                        state      <= &clear_line ? ST_IDLE : ST_CLEAR;
                        if (&clear_line) begin
                            clearing  <= 1'b0;
                            dmem_busy <= 1'b0;
                        end
                    end
                end

                ST_IDLE: if (dmem_miss) begin
                    dmem_busy <= 1'b1;
                    index_q   <= dmem_miss_addr[13:5];
                    tag_q     <= dmem_miss_addr[ADDR_WIDTH-1:14];
                    fsm_raddr <= dmem_miss_addr;
                    state     <= ST_TAG_1;
                end

                ST_TAG_1: state <= ST_TAG_2;
                ST_TAG_2: state <= ST_DECIDE;

                ST_DECIDE: begin
                    victim_tag <= tag_rdata[TAG_WIDTH-1:0];
                    fetched    <= '0;
                    state      <= tag_rdata[TAG_WIDTH] && dirty_rdata ? ST_WRITEBACK : ST_REQUEST;
                end

                ST_WRITEBACK: if (req_ready) state <= ST_WRITEBACK_WAIT;

                ST_WRITEBACK_WAIT: if (fetched == 4'd8) state <= ST_REQUEST;

                ST_REQUEST: if (req_ready) begin
                    word_q <= '0;
                    state  <= ST_FILL;
                end

                ST_FILL: if (rd_valid) begin
                    word_q <= word_q + 3'd1;
                    if (word_q == 3'd7) state <= ST_TAG_WRITE;
                end

                default: begin // ST_TAG_WRITE
                    dmem_busy <= 1'b0;
                    state     <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
