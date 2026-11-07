`default_nettype none
// Simple dual-port synchronous RAM: one write port, one read port, one clock.
//
// * Read is synchronous with a read enable: `rdata` changes only at an edge with
//   `re == 1` and holds otherwise (maps to the block-RAM clock enable).
// * Not reset (the array must map to block RAM).
// * Read-during-write to the same address: Verilator returns the old word; on
//   the FPGA the result is unspecified (`no_rw_check` tells Yosys not to add
//   emulation logic). Every user of this module either cannot collide or adds
//   its own explicit bypass, so the hardware value never matters.
module ram_sdp #(
    parameter int unsigned AW = 10,
    parameter int unsigned W  = 32
) (
    input  logic          clk,
    input  logic          we,
    input  logic [AW-1:0] waddr,
    input  logic [W-1:0]  wdata,
    input  logic          re,
    input  logic [AW-1:0] raddr,
    output logic [W-1:0]  rdata
);
    (* no_rw_check *)
    logic [W-1:0] mem [0:(2**AW)-1];

    always_ff @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
    end

    always_ff @(posedge clk) begin
        if (re) rdata <= mem[raddr];
    end
endmodule
`default_nettype wire
