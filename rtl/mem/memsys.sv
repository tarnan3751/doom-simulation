`default_nettype none
// Memory system (section 5): I-cache + D-cache + line arbiter + backend.
//
//   CPU I-port -> icache --+                    +-> sdram_ctrl (FAST=0) <-> SDRAM pins
//                          +-> mem_arb -> bk_* -+
//   CPU D-port -> dcache --+                    +-> mem_ideal  (FAST=1)
//
//   icache/dcache tags live in one true-dual-port block RAM (u_tags): port A
//   = I-cache entries (lower half), port B = D-cache entries (upper half).
//
// RAM offset: the low RAM_AW = 4 + SDRAM_ROW_BITS + SDRAM_COL_BITS bits of the
// address (25 = addr[24:0] for the 32 MB simulation geometry; 23 on the 8 MB
// board part). Higher address bits are ignored by caches and backend alike, so
// aliases stay coherent. SDRAM word index = offset >> 2 = {ba, row, col}.
module memsys #(
    parameter bit          FAST           = 1'b0,
    parameter int unsigned CLK_HZ         = 50_000_000,
    parameter int unsigned ICACHE_BYTES   = 8192,
    parameter int unsigned DCACHE_BYTES   = 16384,
    parameter int unsigned LINE_BYTES     = 32,
    parameter int unsigned SDRAM_ROW_BITS = 13,
    parameter int unsigned SDRAM_COL_BITS = 8
) (
    input  logic        clk,
    input  logic        rst,

    // CPU instruction port (slave, section 3)
    input  logic        i_req_valid,
    input  logic [31:0] i_req_addr,
    output logic        i_rsp_ready,
    output logic [31:0] i_rsp_rdata,

    // CPU data port, RAM range only (slave, section 3)
    input  logic        d_req_valid,
    input  logic        d_req_we,
    input  logic [31:0] d_req_addr,
    input  logic [3:0]  d_req_wstrb,
    input  logic [31:0] d_req_wdata,
    output logic        d_rsp_ready,
    output logic [31:0] d_rsp_rdata,

    // SDRAM pins (section 6). With FAST=1 they are driven to the idle/NOP state.
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

    // performance event strobes (one-cycle pulses)
    output logic        perf_ic_access,
    output logic        perf_ic_miss,
    output logic        perf_dc_access,
    output logic        perf_dc_miss
);
    localparam int unsigned RAM_AW = 4 + SDRAM_ROW_BITS + SDRAM_COL_BITS;
    localparam int unsigned LA_W   = RAM_AW - 5;

    localparam int unsigned I_IDX_W = $clog2(ICACHE_BYTES / 32);
    localparam int unsigned D_IDX_W = $clog2(DCACHE_BYTES / 32);
    localparam int unsigned I_TAG_W = RAM_AW - 5 - I_IDX_W;
    localparam int unsigned D_TAG_W = RAM_AW - 5 - D_IDX_W;
    localparam int unsigned T_AW    = ((I_IDX_W > D_IDX_W) ? I_IDX_W : D_IDX_W) + 1;
    localparam int unsigned T_W     = ((I_TAG_W > D_TAG_W) ? I_TAG_W : D_TAG_W) + 1;

    if (LINE_BYTES != 32) begin : g_bad_line
        $error("memsys: LINE_BYTES must be 32 (one 8-beat SDRAM burst)");
    end
    if (RAM_AW > 25) begin : g_bad_ram
        $error("memsys: RAM larger than the 32 MB RAM window");
    end

    // ------------------------------------------------------------- caches
    logic               it_en, it_we, dt_en, dt_we;
    logic [I_IDX_W-1:0] it_addr;
    logic [D_IDX_W-1:0] dt_addr;
    logic [I_TAG_W:0]   it_wdata, it_rdata;
    logic [D_TAG_W:0]   dt_wdata, dt_rdata;

    logic            ilp_req, ilp_gnt, ilp_rvalid;
    logic [LA_W-1:0] ilp_addr;
    logic [2:0]      ilp_word;
    logic            dlp_req, dlp_we, dlp_gnt, dlp_rvalid, dlp_wnext;
    logic [LA_W-1:0] dlp_addr;
    logic [2:0]      dlp_word;
    logic [31:0]     dlp_wdata;

    logic            bk_req, bk_we, bk_id, bk_gnt, bk_rvalid, bk_rid, bk_wnext;
    logic [LA_W-1:0] bk_addr;
    logic [2:0]      bk_word;
    logic [31:0]     bk_rdata;

    icache #(.CACHE_BYTES(ICACHE_BYTES), .RAM_AW(RAM_AW)) u_icache (
        .clk         (clk),
        .rst         (rst),
        .req_valid   (i_req_valid),
        .req_addr    (i_req_addr),
        .rsp_ready   (i_rsp_ready),
        .rsp_rdata   (i_rsp_rdata),
        .tag_en      (it_en),
        .tag_we      (it_we),
        .tag_addr    (it_addr),
        .tag_wdata   (it_wdata),
        .tag_rdata   (it_rdata),
        .lp_req      (ilp_req),
        .lp_addr     (ilp_addr),
        .lp_word     (ilp_word),
        .lp_gnt      (ilp_gnt),
        .lp_rvalid   (ilp_rvalid),
        .lp_rdata    (bk_rdata),
        .perf_access (perf_ic_access),
        .perf_miss   (perf_ic_miss)
    );

    dcache #(.CACHE_BYTES(DCACHE_BYTES), .RAM_AW(RAM_AW)) u_dcache (
        .clk         (clk),
        .rst         (rst),
        .req_valid   (d_req_valid),
        .req_we      (d_req_we),
        .req_addr    (d_req_addr),
        .req_wstrb   (d_req_wstrb),
        .req_wdata   (d_req_wdata),
        .rsp_ready   (d_rsp_ready),
        .rsp_rdata   (d_rsp_rdata),
        .tag_en      (dt_en),
        .tag_we      (dt_we),
        .tag_addr    (dt_addr),
        .tag_wdata   (dt_wdata),
        .tag_rdata   (dt_rdata),
        .lp_req      (dlp_req),
        .lp_we       (dlp_we),
        .lp_addr     (dlp_addr),
        .lp_word     (dlp_word),
        .lp_wdata    (dlp_wdata),
        .lp_gnt      (dlp_gnt),
        .lp_rvalid   (dlp_rvalid),
        .lp_rdata    (bk_rdata),
        .lp_wnext    (dlp_wnext),
        .perf_access (perf_dc_access),
        .perf_miss   (perf_dc_miss)
    );

    // ------------------------------------------------------------- shared tag RAM
    logic [T_W-1:0] ta_rdata, tb_rdata;

    ram_tdp #(.AW(T_AW), .W(T_W)) u_tags (
        .clk     (clk),
        .a_en    (it_en),
        .a_we    (it_we),
        .a_addr  ({1'b0, (T_AW-1)'(it_addr)}),
        .a_wdata (T_W'(it_wdata)),
        .a_rdata (ta_rdata),
        .b_en    (dt_en),
        .b_we    (dt_we),
        .b_addr  ({1'b1, (T_AW-1)'(dt_addr)}),
        .b_wdata (T_W'(dt_wdata)),
        .b_rdata (tb_rdata)
    );
    assign it_rdata = ta_rdata[I_TAG_W:0];
    assign dt_rdata = tb_rdata[D_TAG_W:0];

    // ------------------------------------------------------------- arbiter
    mem_arb #(.LA_W(LA_W)) u_arb (
        .clk       (clk),
        .rst       (rst),
        .i_req     (ilp_req),
        .i_addr    (ilp_addr),
        .i_word    (ilp_word),
        .i_gnt     (ilp_gnt),
        .i_rvalid  (ilp_rvalid),
        .d_req     (dlp_req),
        .d_we      (dlp_we),
        .d_addr    (dlp_addr),
        .d_word    (dlp_word),
        .d_gnt     (dlp_gnt),
        .d_rvalid  (dlp_rvalid),
        .bk_req    (bk_req),
        .bk_we     (bk_we),
        .bk_addr   (bk_addr),
        .bk_word   (bk_word),
        .bk_id     (bk_id),
        .bk_gnt    (bk_gnt),
        .bk_rvalid (bk_rvalid),
        .bk_rid    (bk_rid)
    );
    assign dlp_wnext = bk_wnext;

    // ------------------------------------------------------------- backend
    if (FAST) begin : g_ideal
        mem_ideal #(.LA_W(LA_W)) u_ideal (
            .clk       (clk),
            .rst       (rst),
            .bk_req    (bk_req),
            .bk_we     (bk_we),
            .bk_addr   (bk_addr),
            .bk_word   (bk_word),
            .bk_id     (bk_id),
            .bk_wdata  (dlp_wdata),
            .bk_gnt    (bk_gnt),
            .bk_rvalid (bk_rvalid),
            .bk_rid    (bk_rid),
            .bk_rdata  (bk_rdata),
            .bk_wnext  (bk_wnext)
        );

        // SDRAM pins idle (deselect)
        assign sdram_cke   = 1'b1;
        assign sdram_cs_n  = 1'b1;
        assign sdram_ras_n = 1'b1;
        assign sdram_cas_n = 1'b1;
        assign sdram_we_n  = 1'b1;
        assign sdram_ba    = '0;
        assign sdram_addr  = '0;
        assign sdram_dqm   = '0;
        assign sdram_dq_o  = '0;
        assign sdram_dq_oe = 1'b0;

        /* verilator lint_off UNUSEDSIGNAL */
        logic unused_dq;
        assign unused_dq = ^sdram_dq_i;
        /* verilator lint_on UNUSEDSIGNAL */
    end else begin : g_sdram
        sdram_ctrl #(
            .CLK_HZ   (CLK_HZ),
            .ROW_BITS (SDRAM_ROW_BITS),
            .COL_BITS (SDRAM_COL_BITS)
        ) u_sdram (
            .clk         (clk),
            .rst         (rst),
            .bk_req      (bk_req),
            .bk_we       (bk_we),
            .bk_addr     (bk_addr),
            .bk_word     (bk_word),
            .bk_id       (bk_id),
            .bk_wdata    (dlp_wdata),
            .bk_gnt      (bk_gnt),
            .bk_rvalid   (bk_rvalid),
            .bk_rid      (bk_rid),
            .bk_rdata    (bk_rdata),
            .bk_wnext    (bk_wnext),
            .sdram_cke   (sdram_cke),
            .sdram_cs_n  (sdram_cs_n),
            .sdram_ras_n (sdram_ras_n),
            .sdram_cas_n (sdram_cas_n),
            .sdram_we_n  (sdram_we_n),
            .sdram_ba    (sdram_ba),
            .sdram_addr  (sdram_addr),
            .sdram_dqm   (sdram_dqm),
            .sdram_dq_o  (sdram_dq_o),
            .sdram_dq_oe (sdram_dq_oe),
            .sdram_dq_i  (sdram_dq_i)
        );
    end

    // Unused upper tag-RAM bits of the narrower cache (constant zero).
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_tags;
    assign unused_tags = ^{ta_rdata, tb_rdata};
    /* verilator lint_on UNUSEDSIGNAL */
endmodule
`default_nettype wire
