// The board top over its UART pins: echoes input and checks the output bytes.
// Program: read 2 bytes, echo each incremented, then halt.
`timescale 1ns/1ps
module uart_tb;
    logic clk = 0, rx = 1, button_n = 1;
    wire tx, led;
    localparam int BIT_CYCLES = 8;

    yadan_top #(.CLOCK_HZ(8), .BAUD(1), .IMEM_ADDR_WIDTH(6), .IMEM_INIT("build/uart_tb.hex")) dut (
        .clk(clk), .reset_button_n(button_n), .uart_rx_pin(rx), .uart_tx_pin(tx), .led(led));

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

    logic [7:0] got;
    initial begin
        fork
            begin
                repeat (64) @(posedge clk);
                send("A");
                repeat (200) @(posedge clk);
                send("Y");
            end
            begin
                receive(got);
                if (got != "B") begin $display("FAIL: first byte %02x", got); $fatal; end
                receive(got);
                if (got != "Z") begin $display("FAIL: second byte %02x", got); $fatal; end
            end
        join
        repeat (64) @(posedge clk);
        if (led !== 1'b0) begin $display("FAIL: LED should be off after halt"); $fatal; end
        $display("uart_tb passed");
        $finish;
    end

    initial begin
        repeat (200000) @(posedge clk);
        $display("FAIL: timeout");
        $fatal;
    end
endmodule
