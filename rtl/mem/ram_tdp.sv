`default_nettype none
// True dual-port synchronous RAM, one clock, two independent read/write ports.
// Each port does a read or a write per cycle (en=1); during a write the port's
// read data holds ("no change" mode). Not reset (maps to one Gowin DPB).
//
// Used by memsys to hold the I-cache tags (port A) and the D-cache tags
// (port B) in one block RAM. The two ports never address the same word (the
// address MSB separates the halves), so cross-port collisions cannot occur;
// `no_rw_check` lets Yosys map the array without collision emulation.
module ram_tdp #(
    parameter int unsigned AW = 10,
    parameter int unsigned W  = 16
) (
    input  logic          clk,

    input  logic          a_en,
    input  logic          a_we,
    input  logic [AW-1:0] a_addr,
    input  logic [W-1:0]  a_wdata,
    output logic [W-1:0]  a_rdata,

    input  logic          b_en,
    input  logic          b_we,
    input  logic [AW-1:0] b_addr,
    input  logic [W-1:0]  b_wdata,
    output logic [W-1:0]  b_rdata
);
    (* no_rw_check *)
    logic [W-1:0] mem [0:(2**AW)-1];

    always_ff @(posedge clk) begin
        if (a_en) begin
            if (a_we) mem[a_addr] <= a_wdata;
            else      a_rdata     <= mem[a_addr];
        end
    end

    always_ff @(posedge clk) begin
        if (b_en) begin
            if (b_we) mem[b_addr] <= b_wdata;
            else      b_rdata     <= mem[b_addr];
        end
    end
endmodule
`default_nettype wire
