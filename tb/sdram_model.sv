// Simulation stand-in for rtl/board/sdram_phy.sv: a 2M x 32 SDRAM, burst of 8, CAS latency 3,
// that stops the simulation on anything the part would not accept, or would get wrong.
//
// Timing limits are the conservative ones the controller is meant to meet. dq reads as junk
// whenever the part is not driving it, and as junk for an unwritten or decayed word.
module sdram_phy (
    input  logic        clk,
    input  logic        sdclk,  // ignored: this model is cycle-accurate on clk
    input  logic        ras_n,
    input  logic        cas_n,
    input  logic        we_n,
    input  logic [10:0] addr,
    input  logic [1:0]  ba,
    input  logic [31:0] dq_out,
    input  logic        dq_oe,
    output logic [31:0] dq_in
);
    localparam int TRCD = 4, TRP = 4, TRC = 12, TRFC = 12, TRAS = 8, TWR = 2, REFRESH_MAX = 2620;

    logic [31:0] mem[0:(1 << 21) - 1];

    logic [3:0]    open = '0;
    logic [10:0]   open_row[0:3];
    int unsigned   now = 0, act_at[0:3], ready_at[0:3];
    int unsigned   ref_ready_at = 0, last_refresh = 0;
    logic          initialized = 1'b0;
    int            init_refreshes = 0;
    logic          all_precharged = 1'b0;

    logic [20:0]   burst_addr;
    int            read_left = 0, write_left = 0, read_delay = 0;
    logic [1:0]    burst_bank;
    logic [31:0]   lfsr = 32'hACE1_2345;
    logic          driving = 1'b0;
    logic [31:0]   driven;

    function automatic void fail(input string what);
        $display("SDRAM model: %s at cycle %0d", what, now);
        $fatal;
    endfunction

    always_ff @(posedge clk) begin
        logic [2:0] cmd;
        cmd = {ras_n, cas_n, we_n};
        now <= now + 1;
        lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};

        // Bursts in progress.
        driving <= 1'b0;
        if (read_delay != 0) read_delay <= read_delay - 1;
        else if (read_left != 0) begin
            driving    <= 1'b1;
            driven     <= mem[burst_addr];
            burst_addr <= {burst_addr[20:3], burst_addr[2:0] + 3'd1};
            read_left  <= read_left - 1;
        end
        if (write_left != 0) begin
            if (!dq_oe) fail("write data not driven");
            mem[burst_addr] <= dq_out;
            burst_addr <= {burst_addr[20:3], burst_addr[2:0] + 3'd1};
            write_left <= write_left - 1;
        end else if (dq_oe && cmd != 3'b100) begin
            fail("dq driven outside a write");
        end

        if (initialized && now - last_refresh > REFRESH_MAX) fail("refresh overdue");

        unique case (cmd)
            3'b111: ; // NOP
            3'b011: begin // ACT
                if (!initialized) fail("ACT before initialization");
                if (open[ba]) fail("ACT to an open bank");
                if (now < ready_at[ba]) fail("ACT too soon after precharge (tRP, tRC)");
                if (now < ref_ready_at) fail("ACT too soon after REF (tRFC)");
                open[ba]     <= 1'b1;
                open_row[ba] <= addr;
                act_at[ba]   <= now;
            end
            3'b101, 3'b100: begin // READ, WRITE
                if (!open[ba]) fail("READ or WRITE to a closed bank");
                if (now - act_at[ba] < TRCD) fail("READ or WRITE too soon after ACT (tRCD)");
                if (read_left != 0 || write_left != 0) fail("burst during a burst");
                if (addr[10] != 1'b1) fail("expected auto-precharge");
                if (addr[2:0] != 3'b000) fail("expected a line-aligned burst");
                burst_addr <= {open_row[ba], ba, addr[7:0]};
                if (cmd == 3'b101) begin
                    read_left  <= 8;
                    read_delay <= 1;   // driven before the third edge from here
                end else begin
                    if (!dq_oe) fail("write data not driven with WRITE");
                    mem[{open_row[ba], ba, addr[7:0]}] <= dq_out;
                    burst_addr <= {open_row[ba], ba, addr[7:3], 3'b001};
                    write_left <= 7;
                end
                open[ba]     <= 1'b0;
                if (now - act_at[ba] + 8 < TRAS) fail("precharge too soon after ACT (tRAS)");
                // Auto-precharge starts when the burst has finished.
                ready_at[ba] <= now + 8 + TRP + (cmd == 3'b100 ? TWR : 0);
                if (now + 8 + TRP < act_at[ba] + TRC) fail("tRC");
            end
            3'b001: begin // REF
                if (|open) fail("REF with a bank open");
                if (now < ref_ready_at) fail("REF too soon after REF (tRFC)");
                for (int b = 0; b < 4; b++) if (now < ready_at[b]) fail("REF too soon after precharge");
                ref_ready_at <= now + TRFC;
                last_refresh <= now;
                init_refreshes <= init_refreshes + 1;
            end
            3'b010: begin // PRECHARGE
                if (!addr[10]) fail("expected precharge all");
                all_precharged <= 1'b1;
                for (int b = 0; b < 4; b++) ready_at[b] <= now + TRP;
            end
            3'b000: begin // MODE REGISTER SET
                if (!all_precharged) fail("MRS before precharge");
                if (init_refreshes < 8) fail("MRS before 8 refreshes");
                if (now < ref_ready_at) fail("MRS too soon after REF");
                if (addr != 11'b000_0011_0011) fail("unexpected mode");
                initialized  <= 1'b1;
                last_refresh <= now;
            end
            default: fail("illegal command");
        endcase
    end

    assign dq_in = driving ? driven : lfsr;

    initial begin
        for (int b = 0; b < 4; b++) begin
            act_at[b]   = 0;
            ready_at[b] = 0;
        end
        for (int i = 0; i < (1 << 21); i++) mem[i] = 32'hDEAD_0000 + i;
    end

    export "DPI-C" function sdram_poke;
    export "DPI-C" function sdram_peek;

    function automatic void sdram_poke(input int word, input int value);
        mem[word[20:0]] = value;
    endfunction

    function automatic int sdram_peek(input int word);
        return mem[word[20:0]];
    endfunction
endmodule
