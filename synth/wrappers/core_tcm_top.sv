`default_nettype none
// Measurement wrapper (not part of the design): rv32_core alone, with a unified
// 8 KB tightly-coupled memory instead of memsys, so the core's own resource use
// and paths can be measured on the GW2AR-18.
//
//   I-port -> TCM port A (read only)      D-port -> TCM port B (read/write, byte lanes)
//
// Both ports follow ARCHITECTURE.md section 3 with zero wait states: rsp_ready is
// a flop that is 1 from the second cycle after reset, rsp_rdata is the RAM output
// (read only at an accept, so it holds). The TCM is written by the core's stores,
// so nothing is constant and synthesis keeps the whole core. The trace outputs
// are left open (as on the board); `o` keeps the store path observable.
module core_tcm_top (
    input  wire clk,
    input  wire rst_btn,
    output logic o
);
    logic rst = 1'b1;
    always_ff @(posedge clk) rst <= rst_btn;

    logic        i_valid, d_valid, d_we, ready;
    logic [31:0] i_addr, d_addr, d_wdata, i_rdata, d_rdata;
    logic [3:0]  d_wstrb;

    always_ff @(posedge clk) ready <= !rst;

    rv32_core u_core (
        .clk(clk), .rst(rst),
        .imem_req_valid(i_valid), .imem_req_addr(i_addr),
        .imem_rsp_ready(ready), .imem_rsp_rdata(i_rdata),
        .dmem_req_valid(d_valid), .dmem_req_we(d_we), .dmem_req_addr(d_addr),
        .dmem_req_wstrb(d_wstrb), .dmem_req_wdata(d_wdata),
        .dmem_rsp_ready(ready), .dmem_rsp_rdata(d_rdata),
        .trace_valid(), .trace_trap(), .trace_pc(), .trace_insn(), .trace_rd(),
        .trace_rd_wdata(), .trace_mem_addr(), .trace_mem_wstrb(), .trace_mem_wdata(),
        .trace_next_pc(),
        .perf_branch(), .perf_branch_miss(), .perf_jump(), .perf_jump_miss()
    );

    (* no_rw_check *) logic [31:0] tcm [0:2047];
    always_ff @(posedge clk) begin
        if (i_valid) i_rdata <= tcm[i_addr[12:2]];
    end
    always_ff @(posedge clk) begin
        if (d_valid) begin
            if (d_we) begin
                for (int i = 0; i < 4; i++) if (d_wstrb[i]) tcm[d_addr[12:2]][8*i +: 8] <= d_wdata[8*i +: 8];
            end else begin
                d_rdata <= tcm[d_addr[12:2]];
            end
        end
    end

    always_ff @(posedge clk) o <= d_valid && d_we && (d_addr[12:2] == 11'h7FF) && ^d_wdata;
endmodule
`default_nettype wire
