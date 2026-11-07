// The board top over its UART pins, with an SDRAM and a W25Q16 model. The program is +.,+.,+. and
// so prints 1, then reads 2 bytes, echoes each incremented, and halts. A loop that is skipped comes
// first, so that loading it copies from lines already in SDRAM.
//
// An empty flash boots to nothing. A Break and a packed image load the program into Code RAM, and a
// corrupted one is refused. A short reset pulse restarts it on a clear tape. A Break can then
// program the flash, which boots; a long press reloads from it, and power-up would too.
`timescale 1ns/1ps
module uart_tb #(
    parameter int IMAGE_BYTES = 1,
    parameter     IMAGE_FILE  = "build/uart_tb.bytes"
);
    logic clk = 0, rx = 1, button_n = 1;
    wire tx, led, flash_cs_n, flash_clk, flash_mosi, flash_miso, flash_wp_n, flash_hold_n;
    localparam int BIT_CYCLES = 8;

    logic [7:0] image[0:IMAGE_BYTES - 1];

    yadan_top #(.CLOCK_HZ(8), .BAUD(1), .LOAD_BAUD(1), .STARTUP_CYCLES(32), .BREAK_CYCLES(200),
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

    task automatic receive(output logic [7:0] b);
        while (tx) @(posedge clk);
        repeat (BIT_CYCLES + BIT_CYCLES / 2) @(posedge clk);
        for (int i = 0; i < 8; i++) begin
            b[i] = tx;
            repeat (BIT_CYCLES) @(posedge clk);
        end
        if (tx !== 1'b1) begin $display("FAIL: bad stop bit"); $fatal; end
    endtask

    task automatic send_break;
        rx = 0;
        repeat (400) @(posedge clk);
        rx = 1;
        repeat (40) @(posedge clk);
    endtask

    // Loads the image, with one payload byte flipped if corrupt is set; expects the answer.
    task automatic load(input bit corrupt, input logic [7:0] answer);
        logic [7:0] got;
        send_break();
        send("R");
        fork
            for (int i = 0; i < IMAGE_BYTES; i++) send(corrupt && i == IMAGE_BYTES - 2 ? ~image[i] : image[i]);
            begin
                receive(got);
                if (got != answer) begin $display("FAIL: loader answered %02x, wanted %02x", got, answer); $fatal; end
            end
        join
    endtask

    // Programs the flash a page at a time, answering each, and leaves it booting.
    task automatic program_flash;
        logic [7:0] got;
        send_break();
        send("F");
        send(8'(IMAGE_BYTES));
        send(8'(IMAGE_BYTES >> 8));
        send(8'(IMAGE_BYTES >> 16));
        receive(got);
        if (got != "R") begin $display("FAIL: erase answered %02x", got); $fatal; end
        for (int page = 0; page < (IMAGE_BYTES + 255) / 256; page++) begin
            for (int i = 0; i < 256; i++) begin
                send(page * 256 + i < IMAGE_BYTES ? image[page * 256 + i] : 8'hFF);
            end
            receive(got);
            if (got != ".") begin $display("FAIL: page %0d answered %02x", page, got); $fatal; end
        end
    endtask

    task automatic check_id;
        logic [7:0] got;
        send_break();
        send("I");
        receive(got);
        if (got != 8'hEF) begin $display("FAIL: ID byte 0 %02x", got); $fatal; end
        receive(got);
        if (got != 8'h40) begin $display("FAIL: ID byte 1 %02x", got); $fatal; end
        receive(got);
        if (got != 8'h15) begin $display("FAIL: ID byte 2 %02x", got); $fatal; end
    endtask

    task automatic press(input int cycles);
        button_n = 0;
        repeat (cycles) @(posedge clk);
        button_n = 1;
    endtask

    task automatic run_program;
        logic [7:0] got;
        receive(got);
        if (got != 8'd1) begin $display("FAIL: tape not clear, first byte %02x", got); $fatal; end
        fork
            begin
                repeat (64) @(posedge clk);
                send("A");
                repeat (200) @(posedge clk);
                send("Y");
            end
            begin
                receive(got);
                if (got != "B") begin $display("FAIL: first echo %02x", got); $fatal; end
                receive(got);
                if (got != "Z") begin $display("FAIL: second echo %02x", got); $fatal; end
            end
        join
        repeat (64) @(posedge clk);
        if (led !== 1'b0) begin $display("FAIL: LED should be off after halt"); $fatal; end
    endtask

    initial begin
        $readmemh(IMAGE_FILE, image);
        repeat (2000) @(posedge clk);       // the empty flash has been read and refused by now
        check_id();
        load(1'b1, "E");
        load(1'b0, "K");
        run_program();
        press(8);                           // like RTS: restarts from Code RAM
        run_program();
        program_flash();
        run_program();                      // booted from the flash it just wrote
        press(200);                         // a long press: reloads from the flash
        run_program();
        $display("uart_tb passed");
        $finish;
    end

    initial begin
        repeat (4000000) @(posedge clk);
        $display("FAIL: timeout");
        $fatal;
    end
endmodule
