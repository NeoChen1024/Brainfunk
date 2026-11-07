// Block RAM behind a registered bank multiplexer.
//
// A read takes three clock edges: the block RAM, a register per bank, and a register
// after the multiplexer. Block RAM on the EG4S20 has a clock-to-out of about 4 ns, so
// nothing but a register can follow it at the speed the core runs.
//
// en stalls the read path, and rdata holds meanwhile. Reading an address in the same
// cycle it is written gives undefined data, as it can on real block RAM; simulation
// returns wrong data so that nothing comes to depend on it.
module ram_bank #(
    parameter int WIDTH      = 8,
    parameter int ADDR_WIDTH = 12,
    parameter     INIT       = "",  // $readmemh image, the whole of it, for every bank
    parameter int INIT_DEPTH = 1,   // words in the image
    parameter int INIT_SKIP  = 0    // words of it that belong to earlier banks
) (
    input  logic                  clk,
    input  logic                  en,
    input  logic [ADDR_WIDTH-1:0] raddr,
    output logic [WIDTH-1:0]      rdata,
    input  logic                  we,
    input  logic [ADDR_WIDTH-1:0] waddr,
    input  logic [WIDTH-1:0]      wdata
);
    logic [WIDTH-1:0] mem[0:(1 << ADDR_WIDTH) - 1];
    logic [WIDTH-1:0] ram_q;

    generate
        if (INIT != "") begin : g_init
            logic [WIDTH-1:0] image[0:INIT_DEPTH - 1];
            initial begin
                $readmemh(INIT, image);
                for (int i = 0; i < (1 << ADDR_WIDTH); i++) mem[i] = image[INIT_SKIP + i];
            end
        end
    endgenerate

    always_ff @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
`ifdef VERILATOR
        if (en) ram_q <= (we && waddr == raddr) ? ~mem[raddr] : mem[raddr];
`else
        if (en) ram_q <= mem[raddr];
`endif
        if (en) rdata <= ram_q;
    end

`ifdef VERILATOR
    // The cosimulator's access to the contents, which are spread across the banks.
    export "DPI-C" function ram_poke;
    export "DPI-C" function ram_peek;

    function automatic void ram_poke(input int addr, input longint value);
        mem[addr[ADDR_WIDTH-1:0]] = WIDTH'(value);
    endfunction

    function automatic longint ram_peek(input int addr);
        return longint'(mem[addr[ADDR_WIDTH-1:0]]);
    endfunction
`endif
endmodule

module banked_ram #(
    parameter int WIDTH      = 8,
    parameter int ADDR_WIDTH = 16,
    parameter int BANK_WIDTH = 12,  // address bits within one bank
    parameter     INIT       = ""
) (
    input  logic                  clk,
    input  logic                  en,
    input  logic [ADDR_WIDTH-1:0] raddr,
    output logic [WIDTH-1:0]      rdata,
    input  logic                  we,
    input  logic [ADDR_WIDTH-1:0] waddr,
    input  logic [WIDTH-1:0]      wdata
);
    localparam int BANKS = 1 << (ADDR_WIDTH - BANK_WIDTH);

    logic [BANKS*WIDTH-1:0] bank_rdata;
    logic [BANKS-1:0]       bank_we;

    generate
        for (genvar b = 0; b < BANKS; b++) begin : g_bank
            ram_bank #(
                .WIDTH(WIDTH),
                .ADDR_WIDTH(BANK_WIDTH),
                .INIT(INIT),
                .INIT_DEPTH(1 << ADDR_WIDTH),
                .INIT_SKIP(b << BANK_WIDTH)
            ) bank (
                .clk(clk),
                .en(en),
                .raddr(raddr[BANK_WIDTH-1:0]),
                .rdata(bank_rdata[b*WIDTH +: WIDTH]),
                .we(bank_we[b]),
                .waddr(waddr[BANK_WIDTH-1:0]),
                .wdata(wdata)
            );
        end

        if (BANKS == 1) begin : g_one
            assign bank_we = we;
            always_ff @(posedge clk) if (en) rdata <= bank_rdata;
        end else begin : g_many
            localparam int SELECT_BITS = ADDR_WIDTH - BANK_WIDTH;
            logic [SELECT_BITS-1:0] select_1, select_2;

            for (genvar b = 0; b < BANKS; b++) begin : g_we
                assign bank_we[b] = we && waddr[ADDR_WIDTH-1:BANK_WIDTH] == SELECT_BITS'(b);
            end

            always_ff @(posedge clk) begin
                if (en) begin
                    select_1 <= raddr[ADDR_WIDTH-1:BANK_WIDTH];
                    select_2 <= select_1;
                    rdata    <= bank_rdata[select_2*WIDTH +: WIDTH];
                end
            end
        end
    endgenerate
endmodule
