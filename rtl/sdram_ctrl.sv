// Controller for the EG4S20's embedded SDRAM: 2M x 32, 4 banks, 11 row and 8 column bits.
//
// It moves whole 8-word lines, one at a time, always with auto-precharge. Timings are in
// clock cycles at 168 MHz and were measured to work with room to spare; see doc/rtl.md.
//
// A request is taken in the cycle req_ready is high. Then:
//   read:  rd_valid is high for 8 cycles with rd_data, in order.
//   write: wr_fetch is high for 8 cycles, and word k must be on wr_data three cycles after
//          the k-th of those. A client reading block RAM gets this from its read latency.
//
// por also restarts the SDRAM's initialization; nothing else stops refresh.
module sdram_ctrl #(
    parameter int BOOT_CYCLES = 16,   // wait after power-up; the part wants at least 100 us
    parameter int TRCD        = 4,    // ACT to READ or WRITE; wr_fetch's timing relies on this value
    parameter int TRP         = 4,    // precharge to ACT
    parameter int TRFC        = 12,   // REF to anything
    parameter int TWR         = 2,    // last write data to precharge
    parameter int REFRESH     = 1170, // cycles between REF: 7 us, twice as often as required
    parameter int CAPTURE     = 4     // READ on the pins to first word in rd_data: CAS latency 3 + 1
) (
    input  logic        clk,
    input  logic        por,
    output logic        init_done,

    input  logic        req_valid,
    input  logic        req_write,
    input  logic [17:0] req_line,
    output logic        req_ready,

    output logic        rd_valid,
    output logic [31:0] rd_data,
    output logic        wr_fetch,
    input  logic [31:0] wr_data,

    output logic        sd_ras_n,
    output logic        sd_cas_n,
    output logic        sd_we_n,
    output logic [10:0] sd_addr,
    output logic [1:0]  sd_ba,
    output logic [31:0] sd_dq_out,
    output logic        sd_dq_oe = 1'b0,
    input  logic [31:0] sd_dq_in
);
    localparam logic [2:0] CMD_NOP = 3'b111, CMD_ACT = 3'b011, CMD_READ = 3'b101, CMD_WRITE = 3'b100,
                           CMD_PRE = 3'b010, CMD_REF = 3'b001, CMD_MRS = 3'b000;

    // CAS latency 3, sequential burst of 8.
    localparam logic [10:0] MODE = 11'b000_0011_0011;

    // Cycles from READ or WRITE on the pins until the next ACT may follow.
    localparam int READ_RECOVER  = 8 + TRP;
    localparam int WRITE_RECOVER = 8 + TWR + TRP;

    typedef enum logic [2:0] {ST_BOOT, ST_INIT_PRE, ST_INIT_REF, ST_INIT_MRS, ST_IDLE, ST_ACT, ST_RW, ST_WAIT} state_t;
    state_t state;

    logic [24:0] boot_count;
    logic        boot_done;
    logic [3:0]  init_refs;
    logic [7:0]  wait_count;
    logic        wait_done; // wait_count is zero; every wait starts above it
    logic [10:0] refresh_count;
    logic        refresh_due;

    logic        write_q;
    logic [17:0] line_q;
    logic [3:0]  fetch_count;
    logic [CAPTURE:0] read_shift;
    logic [3:0]  read_left;
    logic [3:0]  write_left;

    logic [2:0]  cmd = CMD_NOP; // the part sees pins from configuration on
    assign {sd_ras_n, sd_cas_n, sd_we_n} = cmd;

    assign req_ready = state == ST_IDLE && !refresh_due;

    always_ff @(posedge clk) begin
        cmd       <= CMD_NOP;
        sd_dq_out <= wr_data;
        rd_data   <= sd_dq_in;

        // Read data arrives a fixed time after READ, whatever the state machine is doing.
        read_shift <= {read_shift[CAPTURE-1:0], 1'b0};
        if (read_shift[CAPTURE-1]) read_left <= 4'd8;
        else if (read_left != 4'd0) read_left <= read_left - 4'd1;
        rd_valid <= read_shift[CAPTURE-1] || read_left > 4'd1;

        // Write data follows WRITE, and is fetched ahead of it.
        if (write_left != 4'd0) write_left <= write_left - 4'd1;
        sd_dq_oe <= write_left != 4'd0;
        if (fetch_count != 4'd0) fetch_count <= fetch_count - 4'd1;
        if (fetch_count == 4'd1) wr_fetch <= 1'b0;

        wait_done <= wait_count == 8'd1;
        refresh_due <= refresh_count == 11'(REFRESH - 1);
        if (refresh_due || refresh_count == 11'(REFRESH - 1)) refresh_count <= refresh_count;
        else refresh_count <= refresh_count + 11'd1;

        if (por) begin
            state         <= ST_BOOT;
            boot_count    <= '0;
            boot_done     <= 1'b0;
            init_done     <= 1'b0;
            refresh_count <= '0;
            refresh_due   <= 1'b0;
            read_shift    <= '0;
            read_left     <= '0;
            rd_valid      <= 1'b0;
            write_left    <= '0;
            sd_dq_oe      <= 1'b0;
            fetch_count   <= '0;
            wr_fetch      <= 1'b0;
        end else begin
            unique case (state)
                ST_BOOT: begin
                    boot_count <= boot_count + 25'd1;
                    boot_done  <= boot_count == 25'(BOOT_CYCLES);
                    if (boot_done) state <= ST_INIT_PRE;
                end

                ST_INIT_PRE: begin
                    cmd        <= CMD_PRE;
                    sd_addr    <= 11'b100_0000_0000; // all banks
                    init_refs  <= '0;
                    wait_count <= 8'(TRP);
                    state      <= ST_INIT_REF;
                end

                ST_INIT_REF: begin
                    wait_count <= wait_count - 8'd1;
                    if (wait_done) begin
                        cmd        <= CMD_REF;
                        init_refs  <= init_refs + 4'd1;
                        wait_count <= 8'(TRFC);
                        if (init_refs == 4'd7) state <= ST_INIT_MRS;
                    end
                end

                ST_INIT_MRS: begin
                    wait_count <= wait_count - 8'd1;
                    if (wait_done) begin
                        cmd        <= CMD_MRS;
                        sd_addr    <= MODE;
                        sd_ba      <= 2'b00;
                        wait_count <= 8'd2;
                        state      <= ST_WAIT;
                    end
                end

                ST_IDLE: begin
                    init_done <= 1'b1;
                    if (refresh_due) begin
                        cmd           <= CMD_REF;
                        refresh_due   <= 1'b0;
                        refresh_count <= '0;
                        wait_count    <= 8'(TRFC);
                        state         <= ST_WAIT;
                    end else if (req_valid) begin
                        write_q <= req_write;
                        line_q  <= req_line;
                        state   <= ST_ACT;
                    end
                end

                ST_ACT: begin
                    cmd        <= CMD_ACT;
                    sd_ba      <= line_q[6:5];
                    sd_addr    <= line_q[17:7];
                    wait_count <= 8'(TRCD - 1);
                    if (write_q) begin
                        fetch_count <= 4'd8;
                        wr_fetch    <= 1'b1;
                    end
                    state      <= ST_RW;
                end

                ST_RW: begin
                    wait_count <= wait_count - 8'd1;
                    if (wait_done) begin
                        sd_addr <= {3'b100, line_q[4:0], 3'b000}; // A10: auto-precharge
                        sd_ba   <= line_q[6:5];
                        if (write_q) begin
                            cmd        <= CMD_WRITE;
                            sd_dq_oe   <= 1'b1;
                            write_left <= 4'd7;
                            wait_count <= 8'(WRITE_RECOVER - 2);
                        end else begin
                            cmd           <= CMD_READ;
                            read_shift[0] <= 1'b1;
                            wait_count    <= 8'(READ_RECOVER - 2);
                        end
                        state <= ST_WAIT;
                    end
                end

                default: begin // ST_WAIT
                    wait_count <= wait_count - 8'd1;
                    if (wait_done) state <= ST_IDLE;
                end
            endcase
        end
    end
endmodule
