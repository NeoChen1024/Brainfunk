// The core as a computer: the image loader and its commands over the UART, boot from SPI flash, and
// the program's I/O, in front of a line memory. Nothing in it is particular to a board: a board's top
// gives it a clock, a reset, the pins, and the memory, and shows what it is doing.
//
// At power-up, once the memory is ready, the program is loaded from the flash. A long press of the
// button loads it again; a short one restarts it from Code RAM, with the tape cleared, after
// STARTUP_CYCLES, so that a host opening its port does not lose the first output.
//
// A UART Break says something by how long it is, since a Break from the host is the one thing on
// the line that no byte can be. A command Break starts a command at LOAD_BAUD, whatever is going
// on: load into Code RAM and run, program the flash and boot from it, or report the flash's ID. A
// shorter one turns framing on, and the shortest ends the program's input.
//
// Program I/O is BAUD 8N1, through FIFOs. Input waits for a byte until the end of input, if the
// image says what that stores; otherwise it has no end. Framing is for a host that knows it,
// bfconsole: it escapes 0xFF in what goes out, so that FF and another byte can carry flow control
// for the receive FIFO, and say when the program has stopped. See doc/rtl.md.
module brainfunk_system #(
    parameter int CLOCK_HZ          = 168_000_000,
    parameter int BAUD              = 115_200,
    parameter int LOAD_BAUD         = 2_000_000,
    parameter int STARTUP_CYCLES    = CLOCK_HZ / 5,
    parameter int EOF_BREAK_CYCLES     = CLOCK_HZ / 1000, // 1 ms low: a Break, the end of input
    parameter int FRAME_BREAK_CYCLES   = CLOCK_HZ / 100,  // 10 ms: framing on
    parameter int COMMAND_BREAK_CYCLES = CLOCK_HZ / 10,   // 100 ms: a command
    parameter int LONG_PRESS_CYCLES = CLOCK_HZ / 200, // 5 ms
    parameter int BOUNCE_CYCLES     = CLOCK_HZ / 25,  // ignore the button for 40 ms after a press
    parameter int IMEM_ADDR_WIDTH   = 20,
    parameter int DMEM_ADDR_WIDTH   = 16
) (
    input  logic        clk,
    input  logic        por,
    input  logic        button_n,    // asynchronous
    input  logic        uart_rx,     // asynchronous
    output logic        uart_tx,

    // SPI flash, a register at each pin: they are a long way from this logic.
    output logic        flash_cs_n,
    output logic        flash_clk,
    output logic        flash_mosi,
    input  logic        flash_miso,

    output logic        running,     // a program is running or has stopped; otherwise loading or waiting
    output logic        halted,
    output logic        trapped,

    // The line memory: see brainfunk_soc.
    input  logic        mem_ready,
    output logic        mem_req_valid,
    output logic        mem_req_write,
    output logic [17:0] mem_req_line,
    input  logic        mem_req_ready,
    input  logic        mem_rd_valid,
    input  logic [31:0] mem_rd_data,
    input  logic        mem_wr_fetch,
    output logic [31:0] mem_wr_data
);
    localparam int PROGRAM_BIT = CLOCK_HZ / BAUD;
    localparam int LOAD_BIT    = CLOCK_HZ / LOAD_BAUD;

    // ---- pins, synchronized ----
    logic [1:0] button_sync = 2'b11, rx_sync = 2'b11;
    always_ff @(posedge clk) begin
        button_sync <= {button_sync[0], button_n};
        rx_sync     <= {rx_sync[0], uart_rx};
    end

    // ---- a UART Break: the line low for longer than any byte ----
    // A command is taken as soon as the line has been low that long; the others when it goes high,
    // by how long it was. The receiver is held from the shortest on, so it starts clean after.
    localparam int BREAK_BITS = $clog2(COMMAND_BREAK_CYCLES + 1);
    logic [BREAK_BITS-1:0] low_count = '0;
    logic                  in_break = 1'b0, past_frame = 1'b0, command_break = 1'b0;
    logic                  break_seen = 1'b0, eof_break = 1'b0, frame_break = 1'b0;

    always_ff @(posedge clk) begin
        break_seen  <= 1'b0;
        eof_break   <= 1'b0;
        frame_break <= 1'b0;
        if (rx_sync[1]) begin
            eof_break     <= in_break && !past_frame;
            frame_break   <= past_frame && !command_break;
            low_count     <= '0;
            in_break      <= 1'b0;
            past_frame    <= 1'b0;
            command_break <= 1'b0;
        end else begin
            if (!command_break) low_count <= low_count + 1'b1;
            if (low_count == BREAK_BITS'(EOF_BREAK_CYCLES)) in_break <= 1'b1;
            if (low_count == BREAK_BITS'(FRAME_BREAK_CYCLES)) past_frame <= 1'b1;
            if (low_count == BREAK_BITS'(COMMAND_BREAK_CYCLES)) begin
                break_seen    <= !command_break;
                command_break <= 1'b1;
            end
        end
    end

    // ---- the button: a short low restarts the program, a long one reloads it ----
    localparam int PRESS_MAX  = LONG_PRESS_CYCLES > BOUNCE_CYCLES ? LONG_PRESS_CYCLES : BOUNCE_CYCLES;
    localparam int PRESS_BITS = $clog2(PRESS_MAX + 1);
    logic [PRESS_BITS-1:0] press_count = '0, bounce_count = '0;
    logic                  button_low_q = 1'b0, press_was_long = 1'b0, restart = 1'b0, reload = 1'b0;
    logic                  press_is_long = 1'b0, bouncing = 1'b0;

    always_ff @(posedge clk) begin
        restart      <= 1'b0;
        reload       <= 1'b0;
        button_low_q <= !button_sync[1];
        press_is_long <= press_count == PRESS_BITS'(LONG_PRESS_CYCLES);
        bouncing      <= bounce_count != '0;
        if (bouncing) bounce_count <= bounce_count - 1'b1;

        if (!button_sync[1]) begin
            if (press_is_long) begin
                reload         <= !press_was_long;
                press_was_long <= 1'b1;
                bounce_count   <= PRESS_BITS'(BOUNCE_CYCLES);
            end else if (press_count != PRESS_BITS'(LONG_PRESS_CYCLES)) begin
                press_count <= press_count + 1'b1;
            end
        end else begin
            press_count    <= '0;
            press_was_long <= 1'b0;
            // A short pulse, unless it is the contacts settling around a long one.
            if (button_low_q && !press_was_long && !bouncing) restart <= 1'b1;
        end
    end

    // ---- what the board is doing ----
    // One-hot, and read one bit at a time: this state fans out widely and is decoded everywhere.
    typedef enum logic [13:0] {
        ST_POWERUP    = 14'b00_0000_0000_0001, // waiting for the memory
        ST_FLASH_LOAD = 14'b00_0000_0000_0010, // Code RAM from the flash
        ST_WAIT_IMAGE = 14'b00_0000_0000_0100, // nothing to run
        ST_COMMAND    = 14'b00_0000_0000_1000, // after a Break: which command
        ST_LOADING    = 14'b00_0000_0001_0000, // Code RAM from the UART
        ST_ROM_LENGTH = 14'b00_0000_0010_0000, // flash: how many bytes
        ST_ROM_ERASE_START = 14'b00_0000_0100_0000,
        ST_ROM_ERASE  = 14'b00_0000_1000_0000,
        ST_ROM_READY  = 14'b00_0001_0000_0000, // flash: acknowledged a step, on to the next
        ST_ROM_PAGE   = 14'b00_0010_0000_0000,
        ST_ID         = 14'b00_0100_0000_0000,
        ST_ACK        = 14'b00_1000_0000_0000, // an answer going out at the loader's speed
        ST_STARTUP    = 14'b01_0000_0000_0000, // holding the core in reset before it runs
        ST_RUN        = 14'b10_0000_0000_0000
    } state_t;
    state_t state = ST_POWERUP, after_ack = ST_WAIT_IMAGE;

    localparam int STARTUP_BITS = $clog2(STARTUP_CYCLES + 1);
    logic [STARTUP_BITS-1:0] startup = '0;
    logic                    startup_done = 1'b0;
    always_ff @(posedge clk) startup_done <= startup == STARTUP_BITS'(STARTUP_CYCLES);

    logic        load_start = 1'b0, load_done, load_failed, image_valid = 1'b0, load_in_ready;
    logic        ack_now = 1'b0, ack_queued = 1'b0, tx_idle;
    logic [7:0]  ack_byte = 8'd0;
    logic [23:0] rom_left = '0;
    logic [1:0]  rom_length_pos = '0, id_pos = '0;
    logic [12:0] rom_page = '0;
    logic        rom_finished = 1'b0, rom_last_page = 1'b0;
    logic [8:0]  rom_sectors = '0;
    always_ff @(posedge clk) begin
        rom_finished  <= rom_left == 24'd0;
        rom_last_page <= rom_left <= 24'd256;
        rom_sectors   <= 9'(rom_left[23:12]) + 9'd1;
    end
    logic        rxf_valid, rxf_ready;
    logic [7:0]  rxf_data;
    logic        flash_read_start = 1'b0, flash_read_stop = 1'b0, flash_id_start = 1'b0;
    logic        flash_erase_start = 1'b0, flash_page_start = 1'b0;
    logic        flash_done, flash_out_valid, flash_in_ready;
    logic [7:0]  flash_out_data;
    logic [23:0] flash_id;

    // Where the loader reads from, settled before it starts.
    // The command byte, decoded a cycle ahead of being taken.
    logic command_take = 1'b0, command_ram = 1'b0, command_rom = 1'b0, command_id = 1'b0;
    always_ff @(posedge clk) begin
        command_take <= |(state & ST_COMMAND) && rxf_valid && !command_take;
        command_ram  <= |(state & ST_COMMAND) && rxf_valid && !command_take && rxf_data == "R";
        command_rom  <= |(state & ST_COMMAND) && rxf_valid && !command_take && rxf_data == "F";
        command_id   <= |(state & ST_COMMAND) && rxf_valid && !command_take && rxf_data == "I";
    end

    logic load_from_flash = 1'b0, load_from_uart = 1'b0, rst = 1'b1;
    always_ff @(posedge clk) begin
        load_from_flash <= |(state & ST_FLASH_LOAD);
        load_from_uart  <= |(state & ST_LOADING);
        rst             <= !(|(state & ST_RUN));
    end

    always_ff @(posedge clk) begin
        load_start        <= 1'b0;
        ack_now           <= 1'b0;
        flash_read_start  <= 1'b0;
        flash_read_stop   <= load_done && load_from_flash;
        flash_id_start    <= 1'b0;
        flash_erase_start <= 1'b0;
        flash_page_start  <= 1'b0;

        if (por) begin
            state       <= ST_POWERUP;
            image_valid <= 1'b0;
        end else if (break_seen) begin
            flash_read_stop <= load_from_flash;
            state           <= ST_COMMAND;
        end else if (reload && !(|(state & ST_POWERUP)) && !(|(state & ST_FLASH_LOAD))) begin
            load_start       <= 1'b1;
            flash_read_start <= 1'b1;
            state            <= ST_FLASH_LOAD;
        end else begin
            unique case (1'b1)
                |(state & ST_POWERUP): if (mem_ready) begin
                    load_start       <= 1'b1;
                    flash_read_start <= 1'b1;
                    state            <= ST_FLASH_LOAD;
                end

                |(state & ST_FLASH_LOAD): if (load_done) begin
                    image_valid     <= !load_failed;
                    startup         <= '0;
                    state           <= load_failed ? ST_WAIT_IMAGE : ST_STARTUP;
                end

                |(state & ST_COMMAND): begin // anything else is junk from around the Break
                    if (command_ram) begin
                        load_start <= 1'b1;
                        state      <= ST_LOADING;
                    end else if (command_rom) begin
                        rom_length_pos <= '0;
                        state          <= ST_ROM_LENGTH;
                    end else if (command_id) begin
                        flash_id_start <= 1'b1;
                        id_pos         <= '0;
                        state          <= ST_ID;
                    end
                end

                |(state & ST_LOADING): if (load_done) begin
                    image_valid <= !load_failed;
                    ack_now     <= 1'b1;
                    ack_byte    <= load_failed ? "E" : "K";
                    startup     <= '0;
                    after_ack   <= load_failed ? ST_WAIT_IMAGE : ST_STARTUP;
                    state       <= ST_ACK;
                end

                // Length, little endian; then erase what it needs and say so.
                |(state & ST_ROM_LENGTH): if (rxf_valid) begin
                    rom_left       <= {rxf_data, rom_left[23:8]};
                    rom_length_pos <= rom_length_pos + 2'd1;
                    if (rom_length_pos == 2'd2) state <= ST_ROM_ERASE_START;
                end

                |(state & ST_ROM_ERASE_START): begin // rom_sectors has settled by the time this has passed
                    rom_length_pos <= rom_length_pos + 2'd1;
                    if (rom_length_pos == 2'd0) begin
                        flash_erase_start <= 1'b1;
                        state             <= ST_ROM_ERASE;
                    end
                end

                |(state & ST_ROM_ERASE): if (flash_done) begin
                    rom_page  <= '0;
                    ack_now   <= 1'b1;
                    ack_byte  <= "R";
                    after_ack <= ST_ROM_READY;
                    state     <= ST_ACK;
                end

                // The host sends a page and waits for an answer, so the FIFO cannot overflow.
                |(state & ST_ROM_READY): begin
                    if (rom_finished) begin
                        // Reading it back through the loader checks the CRC, and boots it.
                        load_start       <= 1'b1;
                        flash_read_start <= 1'b1;
                        state            <= ST_FLASH_LOAD;
                    end else begin
                        flash_page_start <= 1'b1;
                        state            <= ST_ROM_PAGE;
                    end
                end

                |(state & ST_ROM_PAGE): if (flash_done) begin
                    rom_page  <= rom_page + 13'd1;
                    rom_left  <= rom_last_page ? 24'd0 : rom_left - 24'd256;
                    ack_now   <= 1'b1;
                    ack_byte  <= ".";
                    after_ack <= ST_ROM_READY;
                    state     <= ST_ACK;
                end

                |(state & ST_ID): if (flash_done || id_pos != 2'd0) begin // three bytes, one per trip through ST_ACK
                    id_pos    <= id_pos + 2'd1;
                    ack_now   <= 1'b1;
                    ack_byte  <= id_pos == 2'd0 ? flash_id[23:16] : (id_pos == 2'd1 ? flash_id[15:8] : flash_id[7:0]);
                    startup   <= '0;
                    after_ack <= id_pos != 2'd2 ? ST_ID : (image_valid ? ST_STARTUP : ST_WAIT_IMAGE);
                    state     <= ST_ACK;
                end

                |(state & ST_ACK): if (tx_idle) state <= after_ack;

                |(state & ST_STARTUP): begin
                    startup <= startup + 1'b1;
                    if (startup_done) state <= ST_RUN;
                end

                default: ; // ST_WAIT_IMAGE, ST_RUN
            endcase

            if (restart && image_valid && (|(state & ST_RUN) || |(state & ST_STARTUP) || |(state & ST_WAIT_IMAGE))) begin
                startup <= '0;
                state   <= ST_STARTUP;
            end
        end
    end

    // A register at each pin: they are a long way from this logic.
    logic flash_cs_n_d, flash_clk_d, flash_mosi_d, flash_miso_q;
    always_ff @(posedge clk) begin
        flash_cs_n   <= flash_cs_n_d;
        flash_clk    <= flash_clk_d;
        flash_mosi   <= flash_mosi_d;
        flash_miso_q <= flash_miso;
    end

    flash_ctrl flash (
        .clk(clk), .rst(por),
        .read_start(flash_read_start), .read_stop(flash_read_stop),
        .out_valid(flash_out_valid), .out_data(flash_out_data), .out_ready(load_from_flash && load_in_ready),
        .id_start(flash_id_start), .id(flash_id),
        .erase_start(flash_erase_start), .erase_sectors(rom_sectors),
        .page_start(flash_page_start), .page(rom_page),
        .in_valid(rxf_valid && |(state & ST_ROM_PAGE)), .in_data(rxf_data), .in_ready(flash_in_ready),
        .done(flash_done),
        .flash_cs_n(flash_cs_n_d), .flash_clk(flash_clk_d), .flash_mosi(flash_mosi_d), .flash_miso(flash_miso_q)
    );

    // ---- UART, at the loader's speed from a Break until the command has been answered ----
    logic [13:0] cycles_per_bit = 14'(PROGRAM_BIT);
    always_ff @(posedge clk) begin
        cycles_per_bit <= |(state & ST_RUN) || |(state & ST_STARTUP) || |(state & ST_WAIT_IMAGE) ||
                          |(state & ST_FLASH_LOAD) || |(state & ST_POWERUP) ? 14'(PROGRAM_BIT) : 14'(LOAD_BIT);
    end

    logic       rx_valid, txf_valid, txf_in_valid, txf_in_ready, tx_ready, txf_empty, txf_ready, rxf_empty;
    logic [7:0] rx_data, txf_data, txf_in_data;
    logic [9:0] rxf_used;
    logic       fifo_rst;
    assign fifo_rst = por || break_seen || restart || reload;

    uart_rx receiver (
        .clk(clk), .rst(por || in_break), .cycles_per_bit(cycles_per_bit), .rx(rx_sync[1]),
        .valid(rx_valid), .data(rx_data)
    );

    uart_fifo rx_fifo (
        .clk(clk), .rst(fifo_rst),
        .in_valid(rx_valid), .in_data(rx_data), .in_ready(),
        .out_valid(rxf_valid), .out_data(rxf_data), .out_ready(rxf_ready), .overflowed(), .empty(rxf_empty),
        .used(rxf_used)
    );

    uart_fifo tx_fifo (
        .clk(clk), .rst(fifo_rst),
        .in_valid(txf_in_valid), .in_data(txf_in_data), .in_ready(txf_in_ready),
        .out_valid(txf_valid), .out_data(txf_data), .out_ready(txf_ready), .overflowed(), .empty(txf_empty),
        .used()
    );

    // ---- the host's side of the program's I/O ----
    // The image's end-of-input field: 0 none, so input waits for ever; else what input stores, 1 for 0,
    // 2 for 255, 3 for the cell as it was. The end comes after every byte sent before it.
    logic [1:0] loader_eof, image_eof = 2'd0;
    logic       eof_seen = 1'b0, in_eof;
    always_ff @(posedge clk) begin
        if (load_done && !load_failed) image_eof <= loader_eof;
        if (fifo_rst) eof_seen <= 1'b0;
        else if (eof_break) eof_seen <= 1'b1;
    end
    assign in_eof = eof_seen && image_eof != 2'd0 && rxf_empty && !rxf_valid;

    // Framing lasts until the program restarts or reloads: a host that opens the port restarts it,
    // and must ask again. Only the program's I/O is framed; a command's answers are not.
    logic framed = 1'b0, framed_ack = 1'b0, program_io = 1'b1;
    logic framing, ack_due, flow_due, end_due, tx_go;
    always_ff @(posedge clk) begin
        program_io <= |(state & ST_RUN) || |(state & ST_STARTUP) || |(state & ST_WAIT_IMAGE) ||
                      |(state & ST_FLASH_LOAD) || |(state & ST_POWERUP);
        if (por || restart || reload) begin
            framed     <= 1'b0;
            framed_ack <= 1'b0;
        end else if (frame_break) begin
            framed     <= 1'b1;
            framed_ack <= 1'b1;
        end else if (tx_go && ack_due) begin
            framed_ack <= 1'b0;
        end
    end
    assign framing = framed && program_io;

    // Flow control: stop the host at half full, and let it go again at a quarter. bfconsole sends
    // a little at a time and checks between, so that what it has sent when it sees a stop fits in the rest.
    logic want_stop = 1'b0, told_stop = 1'b0, end_sent = 1'b0;
    always_ff @(posedge clk) begin
        if (fifo_rst) want_stop <= 1'b0;
        else if (rxf_used >= 10'd512) want_stop <= 1'b1;
        else if (rxf_used <= 10'd256) want_stop <= 1'b0;
    end

    // What goes out after the byte going now: the second of an escape pair, and the flags after F.
    logic [1:0] tail_count = '0;
    logic [7:0] tail_0 = '0, tail_1 = '0;
    logic [7:0] tx_byte;
    logic       tx_send;
    assign flow_due = framing && want_stop != told_stop;
    assign ack_due  = framing && framed_ack && !flow_due;
    assign end_due  = framing && !end_sent && (halted || trapped) && txf_empty && !txf_valid && !flow_due && !framed_ack;
    always_comb begin
        tx_send = 1'b1;
        tx_byte = 8'hFF;
        if (tail_count != 2'd0) tx_byte = tail_0;
        else if (flow_due || ack_due) tx_byte = 8'hFF;
        else if (txf_valid) tx_byte = txf_data;
        else if (!end_due) tx_send = 1'b0;
    end
    assign tx_go     = tx_send && tx_ready;
    assign txf_ready = tx_ready && tail_count == 2'd0 && !flow_due && !ack_due;

    always_ff @(posedge clk) begin
        if (rst) end_sent <= 1'b0;
        if (por || !framing) begin
            told_stop  <= 1'b0;
            tail_count <= '0;
        end else if (tx_go) begin
            if (tail_count != 2'd0) begin
                tail_count <= tail_count - 2'd1;
                tail_0     <= tail_1;
            end else if (flow_due) begin
                told_stop  <= want_stop;
                tail_count <= 2'd1;
                tail_0     <= want_stop ? 8'h13 : 8'h11;
            end else if (ack_due) begin
                tail_count <= 2'd2;
                tail_0     <= "F";
                tail_1     <= {image_valid, 5'd0, image_eof};
            end else if (txf_valid) begin
                tail_count <= txf_data == 8'hFF ? 2'd1 : 2'd0;
                tail_0     <= 8'hFF;
            end else begin
                end_sent   <= 1'b1;
                tail_count <= 2'd1;
                tail_0     <= trapped ? "T" : "H";
            end
        end
    end

    uart_tx transmitter (
        .clk(clk), .rst(por), .cycles_per_bit(cycles_per_bit),
        .valid(tx_send), .data(tx_byte), .ready(tx_ready), .tx(uart_tx)
    );

    // An answer must be on the wire before the speed changes back.
    assign tx_idle = ack_queued && txf_empty && !txf_valid && tx_ready && tail_count == 2'd0;
    always_ff @(posedge clk) begin
        if (ack_now) ack_queued <= 1'b1;
        else if (!(|(state & ST_ACK))) ack_queued <= 1'b0;
    end

    // ---- the core and the loader ----
    logic        in_ready, out_valid;
    logic [7:0]  out_data;
    logic        load_req_valid, load_req_write, load_req_ready, load_rd_valid, load_wr_fetch;
    logic [17:0] load_req_line;
    logic [31:0] load_wr_data, load_rd_data;

    assign rxf_ready = (|(state & ST_LOADING)    && load_from_uart && load_in_ready) ||
                       (|(state & ST_COMMAND)    && command_take) ||
                        |(state & ST_ROM_LENGTH) ||
                       (|(state & ST_ROM_PAGE)   && flash_in_ready) ||
                       (|(state & ST_RUN)        && !rst && in_ready);
    assign txf_in_valid = ack_now || (!rst && out_valid);
    assign txf_in_data  = ack_now ? ack_byte : out_data;

    // A Break or a reload resets the loader, so it starts the cycle after.
    logic load_start_late = 1'b0, load_rst;
    always_ff @(posedge clk) load_start_late <= load_start;
    assign load_rst = por || break_seen || reload;

    image_loader loader (
        .clk(clk), .rst(load_rst), .start(load_start_late), .strict(load_from_flash),
        .busy(), .done(load_done), .failed(load_failed), .eof(loader_eof),
        .in_valid((load_from_flash && flash_out_valid) || (load_from_uart && rxf_valid)),
        .in_data(load_from_flash ? flash_out_data : rxf_data), .in_ready(load_in_ready),
        .req_valid(load_req_valid), .req_write(load_req_write), .req_line(load_req_line), .req_ready(load_req_ready),
        .rd_valid(load_rd_valid), .rd_data(load_rd_data), .wr_fetch(load_wr_fetch), .wr_data(load_wr_data)
    );

    brainfunk_soc #(
        .IMEM_ADDR_WIDTH(IMEM_ADDR_WIDTH),
        .DMEM_ADDR_WIDTH(DMEM_ADDR_WIDTH),
        .CLEAR_TAPE_ON_RESET(1'b1)
    ) soc (
        .clk(clk),
        .rst(rst),
        .io_in_valid((rxf_valid || in_eof) && !rst),
        .io_in_data(rxf_valid ? rxf_data : {8{image_eof == 2'd2}}),
        .io_in_keep(!rxf_valid && image_eof == 2'd3),
        .io_in_ready(in_ready),
        .io_out_valid(out_valid),
        .io_out_data(out_data),
        .io_out_ready(txf_in_ready && !rst),
        .halted(halted),
        .trapped(trapped),
        .load_req_valid(load_req_valid),
        .load_req_write(load_req_write),
        .load_req_line(load_req_line),
        .load_req_ready(load_req_ready),
        .load_rd_valid(load_rd_valid),
        .load_rd_data(load_rd_data),
        .load_wr_fetch(load_wr_fetch),
        .load_wr_data(load_wr_data),
        .mem_ready(mem_ready),
        .mem_req_valid(mem_req_valid),
        .mem_req_write(mem_req_write),
        .mem_req_line(mem_req_line),
        .mem_req_ready(mem_req_ready),
        .mem_rd_valid(mem_rd_valid),
        .mem_rd_data(mem_rd_data),
        .mem_wr_fetch(mem_wr_fetch),
        .mem_wr_data(mem_wr_data)
    );

    always_ff @(posedge clk) running <= |(state & ST_RUN);
endmodule
