// Decodes a program image from a byte stream into SDRAM. See doc/bincode.txt for the format.
//
// The words go straight to SDRAM a line at a time, and a copy reads its source back from there, so
// a copy can reach as far back as its 16-bit distance says. Two lines of buffer: the one being
// filled, which a copy can read from too, and the last line read back for a copy.
// One word at a time: this only has to keep up with a serial port or SPI flash.
module image_loader (
    input  logic        clk,
    input  logic        rst,
    input  logic        start,      // one cycle
    input  logic        strict,     // the image must be right at the start: flash, not a UART
    output logic        busy,
    output logic        done,       // one cycle, with failed if the image was bad
    output logic        failed,
    output logic [1:0]  eof,        // the header's end-of-input field, for an image that loaded

    input  logic        in_valid,
    input  logic [7:0]  in_data,
    output logic        in_ready,

    output logic        req_valid,
    output logic        req_write,
    output logic [17:0] req_line,
    input  logic        req_ready,
    input  logic        rd_valid,
    input  logic [31:0] rd_data,
    input  logic        wr_fetch,
    output logic [31:0] wr_data
);
    typedef enum logic [3:0] {
        ST_IDLE, ST_MAGIC, ST_HEADER, ST_TOKEN, ST_LITERAL, ST_LITERAL_PUT, ST_DISTANCE,
        ST_COPY_READ, ST_COPY_PUT, ST_FETCH_REQ, ST_FETCH, ST_FLUSH_REQ, ST_FLUSH, ST_FINISH, ST_VERDICT
    } state_t;
    state_t state;

    logic [2:0]  magic_pos;
    logic [3:0]  header_pos;
    logic [23:0] payload_left, words_total;
    logic [31:0] crc_expected;
    logic        header_bad, raw;   // raw: the payload is the words themselves, not LZ
    logic [8:0]  run_left;
    logic [1:0]  byte_pos;
    logic [31:0] word;
    logic [15:0] distance;
    logic [23:0] dest;
    logic        is_copy, last_flush;
    logic [2:0]  copy_wait;
    logic [3:0]  flush_word, fetch_word;

    // CRC-32, one bit a cycle.
    logic [31:0] crc;
    logic [7:0]  crc_byte;
    logic [3:0]  crc_bits;
    logic        crc_feed;
    logic        crc_bit;
    assign crc_bit = crc[0] ^ crc_byte[0];

    // Comparisons a cycle late, which is fine: a byte is never taken within 8 cycles of another.
    logic        payload_empty, image_bad, flushing;

    // Where a copied word comes from: the line being filled, the line read back last, or neither,
    // and then that line is read first. Lines below the one being filled are all written already.
    logic [23:0] src;
    logic [20:0] held_line;
    logic        held_valid, src_here, src_held;

    // A byte waits in a register, so that where it came from is not part of what it decides.
    logic       byte_valid = 1'b0, byte_ready = 1'b0, take;
    logic [7:0] byte_data;
    assign in_ready = !byte_valid;
    assign take     = byte_valid && byte_ready;
    always_ff @(posedge clk) begin
        if (rst || take) byte_valid <= 1'b0;
        else if (in_valid && in_ready) begin
            byte_valid <= 1'b1;
            byte_data  <= in_data;
        end
    end

    logic dest_right, payload_right, crc_right;

    logic in_payload;
    assign in_payload = state == ST_TOKEN || state == ST_LITERAL || state == ST_DISTANCE;
    // A cycle late, which is safe: after a take, the next byte is two cycles away.
    always_ff @(posedge clk) begin
        byte_ready <= !rst && crc_bits == 4'd0 && (state == ST_MAGIC || state == ST_HEADER ||
                      (in_payload && !payload_empty && !(raw && state == ST_TOKEN)));
    end
    // The CRC covers the header after the magic, up to the CRC itself, then the payload.
    assign crc_feed   = take && (in_payload || (state == ST_HEADER && header_pos < 4'd12));

    // Line 0 is the one being filled, line 1 the one read back.
    logic [3:0]  line_raddr, line_waddr;
    logic [31:0] line_rdata, line_q, line_wdata;
    logic        line_we;

    ram_bank #(.WIDTH(32), .ADDR_WIDTH(4)) lines (
        .clk(clk),
        .en(1'b1),
        .raddr(line_raddr),
        .rdata(line_q),
        .we(line_we),
        .waddr(line_waddr),
        .wdata(line_wdata)
    );
    always_ff @(posedge clk) line_rdata <= line_q; // three edges in all, as wr_fetch wants

    // Read data is registered before it is written into the buffer.
    logic        fetch_we = 1'b0;
    logic [31:0] fetch_data;
    always_ff @(posedge clk) begin
        fetch_we   <= rd_valid; // only ever for a read of the loader's
        fetch_data <= rd_data;
    end

    logic [20:0] flush_line;
    logic        putting;
    assign putting      = state == ST_LITERAL_PUT || state == ST_COPY_PUT;
    assign line_raddr   = flushing ? {1'b0, flush_word[2:0]} : {!src_here, src[2:0]};
    assign wr_data      = line_rdata;
    assign line_we      = putting || fetch_we;
    assign line_waddr   = putting ? {1'b0, dest[2:0]} : {1'b1, fetch_word[2:0]};
    assign line_wdata   = !putting ? fetch_data : is_copy ? line_rdata : word;
    assign req_valid    = state == ST_FLUSH_REQ || state == ST_FETCH_REQ;
    assign req_write    = state == ST_FLUSH_REQ;
    // Code is the bottom half of SDRAM.
    assign req_line     = 18'(state == ST_FLUSH_REQ ? flush_line : src[23:3]);
    assign busy         = state != ST_IDLE;

    localparam logic [31:0] MAGIC = "BFIM";

    // After a word goes in: is its line complete, is the image complete?
    logic [23:0] dest_next;
    assign dest_next = dest + 24'd1;

    always_ff @(posedge clk) begin
        done          <= 1'b0;
        payload_empty <= payload_left == 24'd0;
        dest_right    <= dest == words_total;
        payload_right <= payload_left == 24'd0;
        crc_right     <= ~crc == crc_expected;
        image_bad     <= !(dest_right && payload_right && crc_right);
        src           <= dest - 24'(distance);
        src_here      <= src[23:3] == dest[23:3];
        src_held      <= held_valid && src[23:3] == held_line;
        flushing      <= state == ST_FLUSH_REQ || state == ST_FLUSH;

        if (crc_feed) begin
            crc_byte <= byte_data;
            crc_bits <= 4'd8;
        end else if (crc_bits != 4'd0) begin
            crc_byte <= {1'b0, crc_byte[7:1]};
            crc_bits <= crc_bits - 4'd1;
            crc      <= (crc >> 1) ^ (crc_bit ? 32'hEDB88320 : 32'd0);
        end
        if (take && in_payload) payload_left <= payload_left - 24'd1;

        if (rst) begin
            state    <= ST_IDLE;
            crc_bits <= '0;
            failed   <= 1'b0;
        end else begin
            unique case (state)
                ST_IDLE: if (start) begin
                    state      <= ST_MAGIC;
                    magic_pos  <= '0;
                    failed     <= 1'b0;
                    last_flush <= 1'b0;
                    held_valid <= 1'b0;
                end

                // Whatever was on a serial line before the image is skipped.
                ST_MAGIC: if (take) begin
                    if (byte_data == MAGIC[31 - 8*magic_pos[1:0] -: 8]) begin
                        magic_pos <= magic_pos + 3'd1;
                        if (magic_pos == 3'd3) begin
                            header_pos <= '0;
                            header_bad <= 1'b0;
                            crc        <= '1;
                            state      <= ST_HEADER;
                        end
                    end else if (strict) begin
                        failed <= 1'b1;
                        done   <= 1'b1;
                        state  <= ST_IDLE;
                    end else begin
                        magic_pos <= byte_data == MAGIC[31:24] ? 3'd1 : 3'd0;
                    end
                end

                // Version, flags, 2 reserved bytes, then little endian: payload bytes (4), words (4),
                // CRC (4). Anything this loader cannot take fails at the end of the header: another
                // version, a reserved bit, a payload of 16 MiB or more, more words than Code RAM,
                // or a plain payload that is not whole words.
                ST_HEADER: if (take) begin
                    unique case (header_pos)
                        4'd0: header_bad <= byte_data != 8'd1;
                        4'd1: begin
                            raw <= !byte_data[0];
                            eof <= byte_data[3:2];
                            if (byte_data[7:4] != 4'd0 || byte_data[1]) header_bad <= 1'b1;
                        end
                        4'd2, 4'd3: if (byte_data != 8'd0) header_bad <= 1'b1;
                        4'd4, 4'd5, 4'd6: begin
                            payload_left <= {byte_data, payload_left[23:8]};
                            if (header_pos == 4'd4 && raw && byte_data[1:0] != 2'd0) header_bad <= 1'b1;
                        end
                        4'd8, 4'd9, 4'd10: begin
                            words_total <= {byte_data, words_total[23:8]};
                            if (header_pos == 4'd10 && byte_data[7:4] != 4'd0) header_bad <= 1'b1;
                        end
                        4'd7, 4'd11: if (byte_data != 8'd0) header_bad <= 1'b1;
                        default: crc_expected <= {byte_data, crc_expected[31:8]};
                    endcase
                    header_pos <= header_pos + 4'd1;
                    if (header_pos == 4'd15) begin
                        dest  <= '0;
                        if (header_bad) begin
                            failed <= 1'b1;
                            done   <= 1'b1;
                            state  <= ST_IDLE;
                        end else begin
                            state  <= ST_TOKEN;
                        end
                    end
                end

                ST_TOKEN: begin
                    if (raw) begin // one word at a time, as if each were a literal of its own
                        if (!payload_empty) begin
                            byte_pos <= '0;
                            is_copy  <= 1'b0;
                            run_left <= 9'd1;
                            state    <= ST_LITERAL;
                        end else if (crc_bits == 4'd0) begin
                            state <= ST_FINISH;
                        end
                    end else if (take) begin
                        byte_pos <= '0;
                        if (byte_data[7]) begin
                            is_copy  <= 1'b1;
                            run_left <= {2'b00, byte_data[6:0]} + 9'd2;
                            state    <= ST_DISTANCE;
                        end else begin
                            is_copy  <= 1'b0;
                            run_left <= {2'b00, byte_data[6:0]} + 9'd1;
                            state    <= ST_LITERAL;
                        end
                    end else if (payload_empty && crc_bits == 4'd0) begin
                        state <= ST_FINISH;
                    end
                end

                ST_LITERAL: if (take) begin
                    word     <= {byte_data, word[31:8]};
                    byte_pos <= byte_pos + 2'd1;
                    if (byte_pos == 2'd3) state <= ST_LITERAL_PUT;
                end

                ST_LITERAL_PUT: begin
                    dest     <= dest_next;
                    run_left <= run_left - 9'd1;
                    state    <= dest_next[2:0] == 3'd0 ? ST_FLUSH_REQ : (run_left == 9'd1 ? ST_TOKEN : ST_LITERAL);
                    flush_line <= 21'(dest[23:3]);
                end

                ST_DISTANCE: if (take) begin
                    distance <= {byte_data, distance[15:8]};
                    byte_pos <= byte_pos + 2'd1;
                    if (byte_pos == 2'd1) begin
                        state     <= ST_COPY_READ;
                        copy_wait <= '0;
                    end
                end

                // src takes a cycle, and where it lies another; then the buffer takes three edges.
                ST_COPY_READ: begin
                    copy_wait <= copy_wait + 3'd1;
                    if (copy_wait == 3'd2 && !src_here && !src_held) state <= ST_FETCH_REQ;
                    if (copy_wait == 3'd4) state <= ST_COPY_PUT;
                end

                ST_COPY_PUT: begin
                    dest     <= dest_next;
                    run_left <= run_left - 9'd1;
                    copy_wait <= '0;
                    state    <= dest_next[2:0] == 3'd0 ? ST_FLUSH_REQ : (run_left == 9'd1 ? ST_TOKEN : ST_COPY_READ);
                    flush_line <= 21'(dest[23:3]);
                end

                ST_FETCH_REQ: begin
                    fetch_word <= '0;
                    if (req_ready) state <= ST_FETCH;
                end

                ST_FETCH: begin
                    if (fetch_we) fetch_word <= fetch_word + 4'd1;
                    if (fetch_word == 4'd8) begin
                        held_line  <= src[23:3];
                        held_valid <= 1'b1;
                        copy_wait  <= '0;
                        state      <= ST_COPY_READ;
                    end
                end

                ST_FLUSH_REQ: begin
                    flush_word <= '0;
                    if (req_ready) state <= ST_FLUSH;
                end

                ST_FLUSH: begin
                    if (wr_fetch) flush_word <= flush_word + 4'd1;
                    if (flush_word == 4'd8) begin
                        copy_wait <= '0;
                        // Back to whichever run this line ended in.
                        if (last_flush)         state <= ST_FINISH;
                        else if (run_left == 9'd0) state <= ST_TOKEN;
                        else                    state <= is_copy ? ST_COPY_READ : ST_LITERAL;
                    end
                end

                ST_FINISH: begin // the last, partial line
                    copy_wait <= '0;
                    if (!last_flush && dest[2:0] != 3'd0) begin
                        flush_line <= 21'(dest[23:3]);
                        last_flush <= 1'b1;
                        state      <= ST_FLUSH_REQ;
                    end else begin
                        state      <= ST_VERDICT;
                    end
                end

                default: begin // ST_VERDICT: image_bad settles first
                    copy_wait <= copy_wait + 3'd1;
                    if (copy_wait == 3'd3) begin
                        last_flush <= 1'b0;
                        failed     <= image_bad;
                        done       <= 1'b1;
                        state      <= ST_IDLE;
                    end
                end
            endcase
        end
    end
endmodule
