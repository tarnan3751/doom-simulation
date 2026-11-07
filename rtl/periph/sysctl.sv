`default_nettype none
// System control registers: SYS_CLK_HZ (R), SYS_EXIT (W), SYS_FLAGS (R).
module sysctl #(
    parameter bit          FAST   = 1'b0,
    parameter int unsigned CLK_HZ = 50_000_000
) (
    input  logic        clk,
    input  logic        rst,
    input  logic        wr_en,       // accepted store into the block
    input  logic [9:0]  off,         // word offset (addr[11:2])
    input  logic [31:0] wdata,
    output logic [31:0] rdata,       // combinational read of register at `off`
    output logic        halted,
    output logic [31:0] exit_code
);
    always_comb begin
        case (off)
            10'd0:   rdata = 32'(CLK_HZ);
            10'd2:   rdata = {31'b0, FAST};
            default: rdata = 32'b0;
        endcase
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            halted    <= 1'b0;
            exit_code <= 32'b0;
        end else if (wr_en && off == 10'd1) begin
            halted    <= 1'b1;
            exit_code <= wdata;
        end
    end
endmodule
`default_nettype wire
