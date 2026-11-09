// The board top over its UART pins, as bfconsole sees it: framing, flow control, the end of input,
// and the end of the program. The program is
//
//   -.  >+++++++++++++++++++.  >,[>--[--]<.,]  +++++,.
//
// at -O0, where [--] stays a loop to slow the echo down ([-] would become a set). It prints FF and
// 13, echoes what comes, slowly enough that the receive FIFO fills, until a 0, then reads once more
// and prints what that read stored: by +eof, the image's end-of-input field, 0, 5 (keep), or 255.
// With no field the read waits for ever. +trap is -. and an illegal word. The images are files of
// hexadecimal bytes, +image and +trap, of +image_bytes and +trap_bytes, so that one build runs
// every case.
//
// A host that sees a stop goes on for LAG bytes, as one would with bytes already on their way, and
// the FIFO must hold them; a host that ignored the stop would overflow it. The end of input is sent
// while the FIFO still holds the echo's input, and must come after all of it.
`timescale 1ns/1ps
module console_tb #(
    parameter int COUNT = 1500,
    parameter int LAG   = 150
);
    logic clk = 0, rx = 1, button_n = 1;
    wire tx, led, flash_cs_n, flash_clk, flash_mosi, flash_miso, flash_wp_n, flash_hold_n;
    localparam int BIT_CYCLES = 8;

    logic [7:0] image[0:4095];
    logic [7:0] trap[0:4095];
    int         IMAGE_BYTES = 0, TRAP_BYTES = 0, EOF = 0; // EOF: the image's field, 0 none, 1 zero, 2 255, 3 keep
    string      image_file, trap_file;

    yadan_top #(.CLOCK_HZ(8), .BAUD(1), .LOAD_BAUD(1), .STARTUP_CYCLES(32), .EOF_BREAK_CYCLES(100),
                .FRAME_BREAK_CYCLES(200), .COMMAND_BREAK_CYCLES(400),
                .LONG_PRESS_CYCLES(100), .BOUNCE_CYCLES(50), .SDRAM_BOOT_CYCLES(16)) dut (
        .clk_24mhz(clk), .reset_button_n(button_n), .uart_rx_pin(rx), .uart_tx_pin(tx), .led(led),
        .flash_cs_n(flash_cs_n), .flash_clk(flash_clk), .flash_mosi(flash_mosi), .flash_miso(flash_miso),
        .flash_wp_n(flash_wp_n), .flash_hold_n(flash_hold_n));

    spi_flash_model flash (.cs_n(flash_cs_n), .sck(flash_clk), .mosi(flash_mosi), .miso(flash_miso),
                           .clk_for_timing(clk));

    always #5 clk = ~clk;

    task automatic send(input logic [7:0] b);
        rx = 0;
        repeat (BIT_CYCLES) @(posedge clk);
        for (int i = 0; i < 8; i++) begin
            rx = b[i];
            repeat (BIT_CYCLES) @(posedge clk);
        end
        rx = 1;
        repeat (BIT_CYCLES) @(posedge clk);
    endtask

    task automatic hold_low(input int cycles);
        rx = 0;
        repeat (cycles) @(posedge clk);
        rx = 1;
        repeat (40) @(posedge clk);
    endtask

    task automatic receive(output logic [7:0] b);
        while (tx) @(posedge clk);
        repeat (BIT_CYCLES + BIT_CYCLES / 2) @(posedge clk);
        for (int i = 0; i < 8; i++) begin
            b[i] = tx;
            repeat (BIT_CYCLES) @(posedge clk);
        end
        if (tx !== 1'b1) begin $display("FAIL: bad stop bit"); $fatal; end
    endtask

    // ---- the host's side, as bfconsole decodes it ----
    logic [7:0] out_q[$];
    bit         framed = 1'b0, stopped = 1'b0, ack_seen = 1'b0;
    int         stops = 0, goes = 0;
    logic [7:0] ack_flags = '0, end_code = '0;

    initial forever begin
        logic [7:0] b, c;
        receive(b);
        if (framed && b == 8'hFF) begin
            receive(c);
            unique case (c)
                8'hFF: out_q.push_back(8'hFF);
                8'h13: begin stopped = 1'b1; stops++; end
                8'h11: begin stopped = 1'b0; goes++; end
                "F": begin receive(ack_flags); ack_seen = 1'b1; stopped = 1'b0; end
                "H", "T": end_code = c;
                default: begin $display("FAIL: FF then %02x", c); $fatal; end
            endcase
        end else begin
            // The end is said once every byte of output is out.
            if (end_code != 8'd0) begin $display("FAIL: output %02x after the end", b); $fatal; end
            out_q.push_back(b);
        end
    end

    task automatic expect_out(input logic [7:0] want, input string what);
        int waited = 0;
        while (out_q.size() == 0) begin
            @(posedge clk);
            if (++waited > 200000) begin $display("FAIL: nothing for %s", what); $fatal; end
        end
        if (out_q[0] != want) begin $display("FAIL: %s: %02x, wanted %02x", what, out_q[0], want); $fatal; end
        void'(out_q.pop_front());
    endtask

    task automatic turn_framing_on(input logic [7:0] want_flags);
        framed   = 1'b1;
        ack_seen = 1'b0;
        hold_low(300);
        for (int i = 0; i < 2000 && !ack_seen; i++) @(posedge clk);
        if (!ack_seen) begin $display("FAIL: no answer to framing"); $fatal; end
        if (ack_flags != want_flags) begin $display("FAIL: flags %02x, wanted %02x", ack_flags, want_flags); $fatal; end
    endtask

    task automatic load(input bit trap_image);
        hold_low(600);
        send("R");
        if (trap_image) for (int i = 0; i < TRAP_BYTES; i++) send(trap[i]);
        else for (int i = 0; i < IMAGE_BYTES; i++) send(image[i]);
        expect_out("K", "the loader's answer");
    endtask

    function automatic logic [7:0] data(input int i);
        return 8'("a") + 8'(i % 26);
    endfunction

    task automatic wait_for_end(input logic [7:0] want);
        for (int i = 0; i < 200000 && end_code == 8'd0; i++) @(posedge clk);
        if (end_code != want) begin $display("FAIL: end %02x, wanted %02x", end_code, want); $fatal; end
    endtask

    initial begin
        automatic int lag = 0;
        if (!$value$plusargs("image=%s", image_file) || !$value$plusargs("image_bytes=%d", IMAGE_BYTES) ||
            !$value$plusargs("trap=%s", trap_file) || !$value$plusargs("trap_bytes=%d", TRAP_BYTES) ||
            !$value$plusargs("eof=%d", EOF)) begin
            $display("FAIL: needs +image, +image_bytes, +trap, +trap_bytes and +eof");
            $fatal;
        end
        $readmemh(image_file, image, 0, IMAGE_BYTES - 1);
        $readmemh(trap_file, trap, 0, TRAP_BYTES - 1);
        repeat (2000) @(posedge clk);           // the empty flash has been read and refused by now
        turn_framing_on(8'h00);                 // no program yet

        load(1'b0);
        expect_out(8'hFF, "an FF in the output");
        expect_out(8'h13, "an XOFF's value in the output");

        for (int i = 0; i < COUNT; i++) begin
            if (stopped && lag == LAG) begin
                while (stopped) @(posedge clk);
                lag = 0;
            end else if (stopped) begin
                lag++;
            end
            send(data(i));
        end
        send(8'h00);
        if (EOF != 0) hold_low(150);           // the end of input, with the FIFO not yet empty
        for (int i = 0; i < COUNT; i++) expect_out(data(i), $sformatf("echo %0d", i));
        if (stops == 0 || goes == 0) begin $display("FAIL: %0d stops, %0d goes", stops, goes); $fatal; end

        if (EOF == 0) begin
            // No end of input: a Break that short is nothing, and the read still waits.
            hold_low(150);
            repeat (20000) @(posedge clk);
            if (out_q.size() != 0 || end_code != 8'd0) begin $display("FAIL: input ended with no field"); $fatal; end
        end else begin
            expect_out(EOF == 1 ? 8'd0 : EOF == 2 ? 8'd255 : 8'd5, "what the end of input stored");
            wait_for_end("H");
            if (out_q.size() != 0) begin $display("FAIL: output after the end"); $fatal; end
        end

        // A restart turns framing off: the same output comes as it is.
        framed   = 1'b0;
        end_code = '0;
        button_n = 0;
        repeat (8) @(posedge clk);
        button_n = 1;
        expect_out(8'hFF, "unframed FF");
        expect_out(8'h13, "unframed 13");

        // Framing again says what the image is. A Break that turns it on is no end of input: the read
        // after the echo waits until one comes.
        turn_framing_on({1'b1, 5'd0, 2'(EOF)});
        send("x");
        expect_out("x", "an echo after framing");
        send(8'h00);
        repeat (20000) @(posedge clk);
        if (out_q.size() != 0 || end_code != 8'd0) begin $display("FAIL: input ended at framing"); $fatal; end
        if (EOF != 0) begin
            hold_low(150);
            expect_out(EOF == 1 ? 8'd0 : EOF == 2 ? 8'd255 : 8'd5, "the end of input after a restart");
            wait_for_end("H");
        end

        // Framing stays on through a command; a trap ends the program too.
        end_code = '0;
        load(1'b1);
        expect_out(8'hFF, "the trapping program's output");
        wait_for_end("T");

        $display("console_tb passed: %0d stops, %0d goes", stops, goes);
        $finish;
    end

    initial begin
        repeat (40000000) @(posedge clk);
        $display("FAIL: timeout");
        $fatal;
    end
endmodule
