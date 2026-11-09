// The user SPI flash, a W25Q16 (2 MiB), in SPI mode 0. Reading runs the part well inside its limit
// for the plain read command. A byte stream goes in or out with valid/ready, and the clock simply
// stops while the other side is not ready, which SPI allows.
module flash_ctrl #(
    parameter int HALF_PERIOD = 4   // clk cycles per half SPI clock: 168 / 8 = 21 MHz
) (
    input  logic        clk,
    input  logic        rst,

    input  logic        read_start,    // from address 0, until read_stop
    input  logic        read_stop,
    output logic        out_valid,
    output logic [7:0]  out_data,
    input  logic        out_ready,

    input  logic        id_start,      // the JEDEC ID, in id
    output logic [23:0] id,

    input  logic        erase_start,   // erase_sectors 4 KiB sectors from address 0
    input  logic [8:0]  erase_sectors,

    input  logic        page_start,    // write 256 bytes from the stream below at this page
    input  logic [12:0] page,
    input  logic        in_valid,
    input  logic [7:0]  in_data,
    output logic        in_ready,

    output logic        done,          // one cycle, after anything but a read

    output logic        flash_cs_n,
    output logic        flash_clk,
    output logic        flash_mosi,
    input  logic        flash_miso
);
    localparam logic [7:0] CMD_READ = 8'h03, CMD_PROGRAM = 8'h02, CMD_ERASE = 8'h20, CMD_ENABLE = 8'h06,
                           CMD_STATUS = 8'h05, CMD_ID = 8'h9F;

    // ---- one byte at a time ----
    logic       byte_start, byte_busy, byte_done;
    logic [7:0] byte_out, byte_in;
    logic [3:0] bit_count;
    logic [3:0] divider;
    logic [7:0] shift;

    assign byte_busy = bit_count != 4'd0;

    always_ff @(posedge clk) begin
        byte_done <= 1'b0;
        if (rst) begin
            bit_count <= '0;
            flash_clk <= 1'b0;
        end else if (byte_start && !byte_busy) begin
            shift      <= byte_out;
            flash_mosi <= byte_out[7];
            bit_count  <= 4'd8;
            divider    <= '0;
            flash_clk  <= 1'b0;
        end else if (byte_busy) begin
            divider <= divider + 4'd1;
            if (divider == 4'(HALF_PERIOD - 1)) begin
                divider   <= '0;
                flash_clk <= !flash_clk;
                if (!flash_clk) begin
                    byte_in <= {byte_in[6:0], flash_miso}; // sampled as the clock rises
                end else begin
                    bit_count <= bit_count - 4'd1;
                    if (bit_count == 4'd1) byte_done <= 1'b1;
                    shift      <= {shift[6:0], 1'b0};
                    flash_mosi <= shift[6];
                end
            end
        end
    end

    // ---- commands ----
    typedef enum logic [3:0] {
        ST_IDLE, ST_SEND, ST_READING, ST_ID, ST_ENABLE, ST_ERASE, ST_PROGRAM, ST_DATA, ST_POLL, ST_POLL_READ,
        ST_GAP, ST_END
    } state_t;
    state_t state, after_send, after_gap;

    logic [31:0] send_bytes;
    logic [2:0]  send_left;
    logic [8:0]  sectors_left;
    logic [12:0] sector;
    logic [8:0]  data_left;
    logic [1:0]  id_left;
    logic [3:0]  gap;
    logic        erasing, have_output;

    assign in_ready = state == ST_DATA && !byte_busy && !byte_start;

    always_ff @(posedge clk) begin
        done       <= 1'b0;
        byte_start <= 1'b0;
        if (rst) begin
            state      <= ST_IDLE;
            flash_cs_n <= 1'b1;
            out_valid  <= 1'b0;
            have_output <= 1'b0;
            gap        <= '0;
        end else begin
            if (out_valid && out_ready) out_valid <= 1'b0;

            unique case (state)
                ST_IDLE: begin
                    flash_cs_n <= 1'b1;
                    erasing    <= 1'b0;
                    if (read_start) begin
                        flash_cs_n <= 1'b0;
                        send_bytes <= {CMD_READ, 24'd0};
                        send_left  <= 3'd4;
                        after_send <= ST_READING;
                        have_output <= 1'b0;
                        state      <= ST_SEND;
                    end else if (id_start) begin
                        flash_cs_n <= 1'b0;
                        send_bytes <= {CMD_ID, 24'd0};
                        send_left  <= 3'd1;
                        id_left    <= 2'd3;
                        after_send <= ST_ID;
                        state      <= ST_SEND;
                    end else if (erase_start) begin
                        sectors_left <= erase_sectors;
                        sector       <= '0;
                        erasing      <= 1'b1;
                        state        <= ST_ENABLE;
                    end else if (page_start) begin
                        state <= ST_ENABLE;
                    end
                end

                // send_left bytes from the top of send_bytes.
                ST_SEND: if (!byte_busy && !byte_start) begin
                    if (send_left == 3'd0) begin
                        state <= after_send;
                    end else begin
                        byte_out   <= send_bytes[31:24];
                        byte_start <= 1'b1;
                        send_bytes <= send_bytes << 8;
                        send_left  <= send_left - 3'd1;
                    end
                end

                ST_READING: begin
                    if (read_stop) begin
                        state <= ST_END;
                    end else if (!byte_busy && !byte_start) begin
                        // A byte finished: hand it over, then clock the next when there is room.
                        if (have_output && !out_valid) begin
                            out_valid   <= 1'b1;
                            out_data    <= byte_in;
                            have_output <= 1'b0;
                        end else if (!have_output && !out_valid) begin
                            byte_out    <= 8'h00;
                            byte_start  <= 1'b1;
                            have_output <= 1'b1;
                        end
                    end
                end

                ST_ID: if (!byte_busy && !byte_start) begin
                    if (id_left == 2'd0) begin
                        id    <= {id[15:0], byte_in};
                        state <= ST_END;
                    end else begin
                        if (id_left != 2'd3) id <= {id[15:0], byte_in};
                        byte_out   <= 8'h00;
                        byte_start <= 1'b1;
                        id_left    <= id_left - 2'd1;
                    end
                end

                // Write enable, then whichever command needed it.
                ST_ENABLE: begin
                    flash_cs_n <= 1'b0;
                    send_bytes <= {CMD_ENABLE, 24'd0};
                    send_left  <= 3'd1;
                    after_send <= ST_GAP;
                    after_gap  <= erasing ? ST_ERASE : ST_PROGRAM;
                    state      <= ST_SEND;
                end

                ST_GAP: if (!byte_busy && !byte_start) begin // CS high for a moment between commands
                    flash_cs_n <= 1'b1;
                    gap        <= gap + 4'd1;
                    if (gap == 4'd15) begin
                        gap   <= '0;
                        state <= after_gap;
                    end
                end

                ST_ERASE: begin
                    flash_cs_n <= 1'b0;
                    send_bytes <= {CMD_ERASE, 3'd0, sector[8:0], 12'd0};
                    send_left  <= 3'd4;
                    after_send <= ST_GAP;
                    after_gap  <= ST_POLL;
                    state      <= ST_SEND;
                end

                ST_PROGRAM: begin
                    flash_cs_n <= 1'b0;
                    send_bytes <= {CMD_PROGRAM, 3'd0, page, 8'd0};
                    send_left  <= 3'd4;
                    data_left  <= 9'd256;
                    after_send <= ST_DATA;
                    state      <= ST_SEND;
                end

                ST_DATA: begin
                    if (data_left == 9'd0) begin
                        if (!byte_busy && !byte_start) begin
                            after_gap <= ST_POLL;
                            state     <= ST_GAP;
                        end
                    end else if (in_valid && in_ready) begin
                        byte_out   <= in_data;
                        byte_start <= 1'b1;
                        data_left  <= data_left - 9'd1;
                    end
                end

                // Wait for the write in progress bit to clear.
                ST_POLL: begin
                    flash_cs_n <= 1'b0;
                    send_bytes <= {CMD_STATUS, 8'h00, 16'd0};
                    send_left  <= 3'd2;
                    after_send <= ST_POLL_READ;
                    state      <= ST_SEND;
                end

                ST_POLL_READ: if (!byte_busy && !byte_start) begin
                    flash_cs_n <= 1'b1;
                    gap        <= gap + 4'd1;
                    if (gap == 4'd15) begin
                        gap <= '0;
                        if (byte_in[0]) begin
                            state <= ST_POLL;
                        end else if (erasing && sectors_left != 9'd1) begin
                            sectors_left <= sectors_left - 9'd1;
                            sector       <= sector + 13'd1;
                            state        <= ST_ENABLE;
                        end else begin
                            state <= ST_END;
                        end
                    end
                end

                default: begin // ST_END
                    flash_cs_n <= 1'b1;
                    done       <= 1'b1;
                    state      <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
