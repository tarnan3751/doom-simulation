// rv32_regfile - 31 x 32 integer register file.
//
// Two asynchronous read ports, one write port, write-through (a read of the
// register being written in the same cycle returns the new value), x0 reads 0.
// The array is not reset so that it maps to distributed (LUT) RAM.
`default_nettype none

module rv32_regfile (
    input  logic        clk,
    input  logic [4:0]  raddr1,
    output logic [31:0] rdata1,
    input  logic [4:0]  raddr2,
    output logic [31:0] rdata2,
    input  logic        we,          // ignored when waddr == 0
    input  logic [4:0]  waddr,
    input  logic [31:0] wdata
);
    logic [31:0] regs [0:31];
    logic        wen;

    assign wen = we && (waddr != 5'd0);

    always_ff @(posedge clk) begin
        if (wen) regs[waddr] <= wdata;
    end

    always_comb begin
        if (raddr1 == 5'd0)                 rdata1 = 32'b0;
        else if (wen && waddr == raddr1)    rdata1 = wdata;
        else                                rdata1 = regs[raddr1];

        if (raddr2 == 5'd0)                 rdata2 = 32'b0;
        else if (wen && waddr == raddr2)    rdata2 = wdata;
        else                                rdata2 = regs[raddr2];
    end

endmodule

`default_nettype wire
