`default_nettype none
// Line-port arbiter: I-cache (reads only) and D-cache (reads and write-backs)
// onto the single backend port. Round robin on a tie, so neither cache can be
// starved even when both keep missing. The request id (0 = I, 1 = D) travels
// with the request; the backend returns it with every read beat, so beats are
// routed correctly even when the backend has already accepted the next request.
// Write-back data and lp_wnext belong to the D-cache (the only writer) and are
// wired directly in memsys.
module mem_arb #(
    parameter int unsigned LA_W = 20
) (
    input  logic            clk,
    input  logic            rst,

    input  logic            i_req,
    input  logic [LA_W-1:0] i_addr,
    input  logic [2:0]      i_word,
    output logic            i_gnt,
    output logic            i_rvalid,

    input  logic            d_req,
    input  logic            d_we,
    input  logic [LA_W-1:0] d_addr,
    input  logic [2:0]      d_word,
    output logic            d_gnt,
    output logic            d_rvalid,

    output logic            bk_req,
    output logic            bk_we,
    output logic [LA_W-1:0] bk_addr,
    output logic [2:0]      bk_word,
    output logic            bk_id,
    input  logic            bk_gnt,
    input  logic            bk_rvalid,
    input  logic            bk_rid
);
    logic prio_d;   // D wins a tie
    logic sel_d;

    assign sel_d    = d_req && (!i_req || prio_d);
    assign bk_req   = i_req || d_req;
    assign bk_we    = sel_d && d_we;
    assign bk_addr  = sel_d ? d_addr : i_addr;
    assign bk_word  = sel_d ? d_word : i_word;
    assign bk_id    = sel_d;
    assign i_gnt    = bk_gnt && !sel_d;
    assign d_gnt    = bk_gnt && sel_d;
    assign i_rvalid = bk_rvalid && !bk_rid;
    assign d_rvalid = bk_rvalid && bk_rid;

    always_ff @(posedge clk) begin
        if (rst)         prio_d <= 1'b0;
        else if (bk_gnt) prio_d <= !sel_d;
    end
endmodule
`default_nettype wire
