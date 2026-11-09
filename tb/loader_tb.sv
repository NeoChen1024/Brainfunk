// The image loader with the SDRAM controller and model: loads a packed image as fast as the loader
// takes bytes, or with GAP idle cycles between them, then checks every word in SDRAM against the
// unpacked image. With REFUSE, the loader must say the image is bad: one CORRUPT_AT flips a byte of,
// or one made wrong some other way.
// With PRE_FILE, another image is loaded first, so nothing may carry over from one load to the next.
// EOF is what the loader must report of the header's end-of-input field.
`timescale 1ns/1ps
module loader_tb #(
    parameter int IMAGE_BYTES = 1,
    parameter     IMAGE_FILE  = "build/loader_tb.bytes",
    parameter int WORDS       = 1,
    parameter     WORDS_FILE  = "build/loader_tb.hex",
    parameter int GAP         = 0,
    parameter int CORRUPT_AT  = -1,
    parameter bit REFUSE      = 1'b0,
    parameter int PRE_BYTES   = 1,
    parameter     PRE_FILE    = "",
    parameter int EOF         = 0
);
    logic clk = 0;
    always #5 clk = ~clk;

    logic [7:0]  image[0:IMAGE_BYTES - 1];
    logic [7:0]  pre[0:PRE_BYTES - 1];
    logic [31:0] expected[0:WORDS - 1];

    logic        por = 1'b1, rst = 1'b1, start = 1'b0, done, failed, sdram_ready;
    logic [1:0]  eof;
    logic        in_valid = 1'b0, in_ready;
    logic [7:0]  in_data;
    logic        req_valid, req_write, req_ready, rd_valid, wr_fetch;
    logic [17:0] req_line;
    logic [31:0] rd_data, wr_data;

    image_loader loader (
        .clk(clk), .rst(rst), .start(start), .strict(1'b1), .busy(), .done(done), .failed(failed), .eof(eof),
        .in_valid(in_valid), .in_data(in_data), .in_ready(in_ready),
        .req_valid(req_valid), .req_write(req_write), .req_line(req_line), .req_ready(req_ready),
        .rd_valid(rd_valid), .rd_data(rd_data), .wr_fetch(wr_fetch), .wr_data(wr_data)
    );

    logic        sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    logic [10:0] sd_addr;
    logic [1:0]  sd_ba;
    logic [31:0] sd_dq_out, sd_dq_in;

    sdram_ctrl #(.BOOT_CYCLES(16)) sdram (
        .clk(clk), .por(por), .init_done(sdram_ready),
        .req_valid(req_valid), .req_write(req_write), .req_line(req_line), .req_ready(req_ready),
        .rd_valid(rd_valid), .rd_data(rd_data), .wr_fetch(wr_fetch), .wr_data(wr_data),
        .sd_ras_n(sd_ras_n), .sd_cas_n(sd_cas_n), .sd_we_n(sd_we_n), .sd_addr(sd_addr), .sd_ba(sd_ba),
        .sd_dq_out(sd_dq_out), .sd_dq_oe(sd_dq_oe), .sd_dq_in(sd_dq_in)
    );

    sdram_phy phy (
        .clk(clk), .sdclk(clk), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .addr(sd_addr), .ba(sd_ba), .dq_out(sd_dq_out), .dq_oe(sd_dq_oe), .dq_in(sd_dq_in)
    );

    int reads = 0;
    always @(posedge clk) if (req_valid && req_ready && !req_write) reads++;

    // The loader may give up on an image before it has all of it.
    bit finished = 1'b0;
    always @(posedge clk) begin
        if (start) finished <= 1'b0;
        else if (done) finished <= 1'b1;
    end

    // Driven between edges, so what the loader takes at an edge is plain.
    task automatic send(input logic [7:0] b);
        if (finished) return;
        in_valid = 1'b1;
        in_data  = b;
        while (!in_ready && !finished) @(negedge clk);
        @(negedge clk);
        in_valid = 1'b0;
        repeat (GAP) @(negedge clk);
    endtask

    task automatic begin_load;
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;
    endtask

    initial begin
        $readmemh(IMAGE_FILE, image);
        $readmemh(WORDS_FILE, expected);
        if (PRE_FILE != "") $readmemh(PRE_FILE, pre);
        repeat (4) @(negedge clk);
        por = 1'b0;
        rst = 1'b0;
        while (!sdram_ready) @(negedge clk);
        if (PRE_FILE != "") begin
            begin_load();
            for (int i = 0; i < PRE_BYTES; i++) send(pre[i]);
            while (!finished) @(negedge clk);
            if (failed) begin $display("FAIL: the loader refused the first image"); $fatal; end
        end
        begin_load();
        for (int i = 0; i < IMAGE_BYTES; i++) send(i == CORRUPT_AT ? image[i] ^ 8'h80 : image[i]);
        if (REFUSE) begin
            // A payload size made larger leaves it waiting for bytes that never come, which a UART
            // Break ends; from the flash, more bytes always come, and the CRC is wrong.
            for (int i = 0; i < 100000 && !finished; i++) @(negedge clk);
            if (finished && !failed) begin $display("FAIL: a corrupt image was taken"); $fatal; end
            if (finished) $display("loader_tb passed: refused");
            else $display("loader_tb passed: still waiting");
            $finish;
        end
        while (!finished) @(negedge clk);
        if (failed) begin $display("FAIL: the loader refused the image"); $fatal; end
        if (eof != 2'(EOF)) begin $display("FAIL: end of input %0d, wanted %0d", eof, EOF); $fatal; end
        for (int i = 0; i < WORDS; i++) begin
            if (phy.sdram_peek(i) != expected[i]) begin
                $display("FAIL: word %0d is %08x, wanted %08x", i, phy.sdram_peek(i), expected[i]);
                $fatal;
            end
        end
        $display("loader_tb passed: %0d words, %0d bytes, %0d line reads, %0t", WORDS, IMAGE_BYTES, reads, $time);
        $finish;
    end
    initial begin
        repeat (200000000) @(posedge clk);
        $display("FAIL: timeout");
        $fatal;
    end
endmodule
