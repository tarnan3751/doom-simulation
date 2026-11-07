`default_nettype none
// SoC top level: rv32_core + memsys + mmio, data-port split and event counters. Glue only.
module soc_top #(
    parameter bit          FAST           = 1'b0,
    parameter int unsigned CLK_HZ         = 50_000_000,
    parameter int unsigned UART_DIV       = 434,
    parameter int unsigned ICACHE_BYTES   = 8192,
    parameter int unsigned DCACHE_BYTES   = 16384,
    parameter int unsigned SDRAM_ROW_BITS = 13,
    parameter int unsigned SDRAM_COL_BITS = 8
) (
    input  logic        clk,
    input  logic        rst,

    // SDRAM (section 6)
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

    // serial
    output logic        uart_tx,

    // keyboard events from the board
    input  logic        key_valid,
    input  logic [8:0]  key_data,

    // video scan-out (constant/idle when FAST=1)
    output logic        vid_pix_ce,
    output logic        vid_hsync,
    output logic        vid_vsync,
    output logic        vid_de,
    output logic [7:0]  vid_r,
    output logic [7:0]  vid_g,
    output logic [7:0]  vid_b,

    // run control / observation (board only; unconnected on FPGA)
    output logic        halted,
    output logic [31:0] exit_code,
    output logic        vid_flip,
    output logic        uart_tx_valid,
    output logic [7:0]  uart_tx_byte,

    // retire trace, straight from rv32_core
    output logic        trace_valid,
    output logic        trace_trap,
    output logic [31:0] trace_pc,
    output logic [31:0] trace_insn,
    output logic [4:0]  trace_rd,
    output logic [31:0] trace_rd_wdata,
    output logic [31:0] trace_mem_addr,
    output logic [3:0]  trace_mem_wstrb,
    output logic [31:0] trace_mem_wdata,
    output logic [31:0] trace_next_pc,

    // free-running event counters (64 bit, reset to 0)
    output logic [63:0] cnt_cycle,
    output logic [63:0] cnt_instret,
    output logic [63:0] cnt_ic_access,
    output logic [63:0] cnt_ic_miss,
    output logic [63:0] cnt_dc_access,
    output logic [63:0] cnt_dc_miss,
    output logic [63:0] cnt_branch,
    output logic [63:0] cnt_branch_miss,
    output logic [63:0] cnt_jump,
    output logic [63:0] cnt_jump_miss,
    output logic [63:0] cnt_flip
);
    // ---- core <-> memory ports ----------------------------------------------
    logic        imem_req_valid, imem_rsp_ready;
    logic [31:0] imem_req_addr, imem_rsp_rdata;

    logic        dmem_req_valid, dmem_req_we, dmem_rsp_ready;
    logic [31:0] dmem_req_addr, dmem_req_wdata, dmem_rsp_rdata;
    logic [3:0]  dmem_req_wstrb;

    logic perf_branch, perf_branch_miss, perf_jump, perf_jump_miss;
    logic perf_ic_access, perf_ic_miss, perf_dc_access, perf_dc_miss;

    rv32_core #(.COVERAGE(1'b0)) u_core (    // cov_events unused here: let the simulator skip them
        .clk(clk), .rst(rst),
        .imem_req_valid(imem_req_valid), .imem_req_addr(imem_req_addr),
        .imem_rsp_ready(imem_rsp_ready), .imem_rsp_rdata(imem_rsp_rdata),
        .dmem_req_valid(dmem_req_valid), .dmem_req_we(dmem_req_we),
        .dmem_req_addr(dmem_req_addr), .dmem_req_wstrb(dmem_req_wstrb),
        .dmem_req_wdata(dmem_req_wdata),
        .dmem_rsp_ready(dmem_rsp_ready), .dmem_rsp_rdata(dmem_rsp_rdata),
        .trace_valid(trace_valid), .trace_trap(trace_trap), .trace_pc(trace_pc),
        .trace_insn(trace_insn), .trace_rd(trace_rd), .trace_rd_wdata(trace_rd_wdata),
        .trace_mem_addr(trace_mem_addr), .trace_mem_wstrb(trace_mem_wstrb),
        .trace_mem_wdata(trace_mem_wdata), .trace_next_pc(trace_next_pc),
        .perf_branch(perf_branch), .perf_branch_miss(perf_branch_miss),
        .perf_jump(perf_jump), .perf_jump_miss(perf_jump_miss)
    );

    // ---- data-port split: addr[31] = 1 -> RAM (memsys), else MMIO -------------
    // Only the selected slave sees req_valid. The slave that owns the response is the one
    // that accepted the most recent request (sel_ram_q, updated at each accept).
    // rsp_ready is the AND of both slaves' rsp_ready: a slave with nothing outstanding
    // holds rsp_ready = 1 (R2), so this equals the busy slave's ready, and it also keeps
    // the core waiting while either slave is still initialising after reset (R6).
    logic        d_rsp_ready, m_rsp_ready;
    logic [31:0] d_rsp_rdata, m_rsp_rdata;
    logic        sel_ram_q;

    wire sel_ram = dmem_req_addr[31];

    always_ff @(posedge clk) begin
        if (rst)                 sel_ram_q <= 1'b0;
        else if (dmem_req_valid) sel_ram_q <= sel_ram;
    end

    assign dmem_rsp_ready = d_rsp_ready & m_rsp_ready;
    assign dmem_rsp_rdata = sel_ram_q ? d_rsp_rdata : m_rsp_rdata;

    memsys #(
        .FAST(FAST), .CLK_HZ(CLK_HZ),
        .ICACHE_BYTES(ICACHE_BYTES), .DCACHE_BYTES(DCACHE_BYTES),
        .SDRAM_ROW_BITS(SDRAM_ROW_BITS), .SDRAM_COL_BITS(SDRAM_COL_BITS)
    ) u_mem (
        .clk(clk), .rst(rst),
        .i_req_valid(imem_req_valid), .i_req_addr(imem_req_addr),
        .i_rsp_ready(imem_rsp_ready), .i_rsp_rdata(imem_rsp_rdata),
        .d_req_valid(dmem_req_valid & sel_ram), .d_req_we(dmem_req_we),
        .d_req_addr(dmem_req_addr), .d_req_wstrb(dmem_req_wstrb),
        .d_req_wdata(dmem_req_wdata),
        .d_rsp_ready(d_rsp_ready), .d_rsp_rdata(d_rsp_rdata),
        .sdram_cke(sdram_cke), .sdram_cs_n(sdram_cs_n), .sdram_ras_n(sdram_ras_n),
        .sdram_cas_n(sdram_cas_n), .sdram_we_n(sdram_we_n), .sdram_ba(sdram_ba),
        .sdram_addr(sdram_addr), .sdram_dqm(sdram_dqm), .sdram_dq_o(sdram_dq_o),
        .sdram_dq_oe(sdram_dq_oe), .sdram_dq_i(sdram_dq_i),
        .perf_ic_access(perf_ic_access), .perf_ic_miss(perf_ic_miss),
        .perf_dc_access(perf_dc_access), .perf_dc_miss(perf_dc_miss)
    );

    mmio #(.FAST(FAST), .CLK_HZ(CLK_HZ), .UART_DIV(UART_DIV)) u_mmio (
        .clk(clk), .rst(rst),
        .req_valid(dmem_req_valid & ~sel_ram), .req_we(dmem_req_we),
        .req_addr(dmem_req_addr), .req_wstrb(dmem_req_wstrb), .req_wdata(dmem_req_wdata),
        .rsp_ready(m_rsp_ready), .rsp_rdata(m_rsp_rdata),
        .uart_tx(uart_tx), .key_valid(key_valid), .key_data(key_data),
        .vid_pix_ce(vid_pix_ce), .vid_hsync(vid_hsync), .vid_vsync(vid_vsync),
        .vid_de(vid_de), .vid_r(vid_r), .vid_g(vid_g), .vid_b(vid_b),
        .halted(halted), .exit_code(exit_code), .vid_flip(vid_flip),
        .uart_tx_valid(uart_tx_valid), .uart_tx_byte(uart_tx_byte)
    );

    // ---- event counters -------------------------------------------------------
    always_ff @(posedge clk) begin
        if (rst) begin
            cnt_cycle       <= '0;
            cnt_instret     <= '0;
            cnt_ic_access   <= '0;
            cnt_ic_miss     <= '0;
            cnt_dc_access   <= '0;
            cnt_dc_miss     <= '0;
            cnt_branch      <= '0;
            cnt_branch_miss <= '0;
            cnt_jump        <= '0;
            cnt_jump_miss   <= '0;
            cnt_flip        <= '0;
        end else begin
            cnt_cycle <= cnt_cycle + 64'd1;
            if (trace_valid && !trace_trap) cnt_instret     <= cnt_instret     + 64'd1;
            if (perf_ic_access)             cnt_ic_access   <= cnt_ic_access   + 64'd1;
            if (perf_ic_miss)               cnt_ic_miss     <= cnt_ic_miss     + 64'd1;
            if (perf_dc_access)             cnt_dc_access   <= cnt_dc_access   + 64'd1;
            if (perf_dc_miss)               cnt_dc_miss     <= cnt_dc_miss     + 64'd1;
            if (perf_branch)                cnt_branch      <= cnt_branch      + 64'd1;
            if (perf_branch_miss)           cnt_branch_miss <= cnt_branch_miss + 64'd1;
            if (perf_jump)                  cnt_jump        <= cnt_jump        + 64'd1;
            if (perf_jump_miss)             cnt_jump_miss   <= cnt_jump_miss   + 64'd1;
            if (vid_flip)                   cnt_flip        <= cnt_flip        + 64'd1;
        end
    end
endmodule
`default_nettype wire
