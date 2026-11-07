`default_nettype none
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
// Behavioural stub of memsys for tb_soc: a small word memory on the D port with
// programmable response latency (`lat` extra cycles with rsp_ready low) and an
// optional initialisation delay after reset.
module memsys #(
    parameter bit          FAST           = 1'b1,
    parameter int unsigned CLK_HZ         = 50_000_000,
    parameter int unsigned ICACHE_BYTES   = 8192,
    parameter int unsigned DCACHE_BYTES   = 16384,
    parameter int unsigned SDRAM_ROW_BITS = 13,
    parameter int unsigned SDRAM_COL_BITS = 8
) (
    input  logic        clk,
    input  logic        rst,
    input  logic        i_req_valid,
    input  logic [31:0] i_req_addr,
    output logic        i_rsp_ready,
    output logic [31:0] i_rsp_rdata,
    input  logic        d_req_valid,
    input  logic        d_req_we,
    input  logic [31:0] d_req_addr,
    input  logic [3:0]  d_req_wstrb,
    input  logic [31:0] d_req_wdata,
    output logic        d_rsp_ready,
    output logic [31:0] d_rsp_rdata,
    output logic        sdram_cke,
    output logic        sdram_cs_n,
    output logic        sdram_ras_n,
    output logic        sdram_cas_n,
    output logic        sdram_we_n,
    output logic [1:0]  sdram_ba,
    output logic [SDRAM_ROW_BITS-1:0] sdram_addr,
    output logic [3:0]  sdram_dqm,
    output logic [31:0] sdram_dq_o,
    output logic        sdram_dq_oe,
    input  logic [31:0] sdram_dq_i,
    output logic        perf_ic_access,
    output logic        perf_ic_miss,
    output logic        perf_dc_access,
    output logic        perf_dc_miss
);
    logic [31:0] mem [0:1023]    /* verilator public_flat_rw */;
    logic [7:0]  lat             /* verilator public_flat_rw */;
    logic [7:0]  init_delay      /* verilator public_flat_rw */;
    logic        ctl_ic_miss     /* verilator public_flat_rw */;
    logic [31:0] d_acc_count     /* verilator public_flat_rw */;
    logic        d_req_seen_when_not_ready /* verilator public_flat_rw */;

    logic        ready_q;
    logic [31:0] rdata_q;
    logic [7:0]  cnt;
    wire  [9:0]  idx = d_req_addr[11:2];

    assign d_rsp_ready = ready_q;
    assign d_rsp_rdata = rdata_q;

    always_ff @(posedge clk) begin
        perf_ic_access <= 1'b0;
        perf_dc_access <= 1'b0;
        perf_dc_miss   <= 1'b0;
        if (rst) begin
            ready_q <= 1'b0;
            cnt     <= init_delay;
            rdata_q <= 32'b0;
            d_acc_count <= 32'b0;
            d_req_seen_when_not_ready <= 1'b0;
        end else begin
            if (d_req_valid && !ready_q) d_req_seen_when_not_ready <= 1'b1;
            if (!ready_q) begin
                if (cnt <= 8'd1) ready_q <= 1'b1;
                cnt <= cnt - 8'd1;
            end else if (d_req_valid) begin
                d_acc_count    <= d_acc_count + 32'd1;
                perf_dc_access <= 1'b1;
                perf_dc_miss   <= (lat != 8'd0);
                if (d_req_we) begin
                    if (d_req_wstrb[0]) mem[idx][7:0]   <= d_req_wdata[7:0];
                    if (d_req_wstrb[1]) mem[idx][15:8]  <= d_req_wdata[15:8];
                    if (d_req_wstrb[2]) mem[idx][23:16] <= d_req_wdata[23:16];
                    if (d_req_wstrb[3]) mem[idx][31:24] <= d_req_wdata[31:24];
                    rdata_q <= 32'hDEAD_0000 | {22'b0, idx};   // store: junk, must not matter
                end else begin
                    rdata_q <= mem[idx];
                end
                if (lat != 8'd0) begin
                    ready_q <= 1'b0;
                    cnt     <= lat;
                end
            end
        end
    end

    // instruction side: always ready, returns the address
    logic [31:0] irdata_q;
    assign i_rsp_ready = 1'b1;
    assign i_rsp_rdata = irdata_q;
    always_ff @(posedge clk) begin
        if (i_req_valid) irdata_q <= i_req_addr;
        if (!rst) begin
            perf_ic_access <= i_req_valid;
        end
        perf_ic_miss <= ctl_ic_miss;
    end

    assign sdram_cke = 1'b0; assign sdram_cs_n = 1'b1; assign sdram_ras_n = 1'b1;
    assign sdram_cas_n = 1'b1; assign sdram_we_n = 1'b1; assign sdram_ba = 2'b0;
    assign sdram_addr = '0; assign sdram_dqm = 4'b0; assign sdram_dq_o = 32'b0;
    assign sdram_dq_oe = 1'b0;
endmodule
`default_nettype wire
