`default_nettype none
// Read-only direct-mapped instruction cache, slave of the section-3 port
// (rtl/mem/README.md, "Caches").
//
// Organisation (CACHE_BYTES / 32 lines of 8 words), offset = req_addr[RAM_AW-1:0]:
//   word address = offset[IDX_W+4:2] (data RAM), index = offset[IDX_W+4:5] (tag RAM),
//   tag = offset[RAM_AW-1:IDX_W+5].
// The data and tag RAMs are read at the accept edge with req_addr; in the LOOKUP
// cycle rsp_ready = tag valid && tag match. A miss writes its tag at once,
// requests the line from a flop in the next cycle and writes the beats (critical
// word first) as they arrive (fill_mask); requests to the line being filled
// complete as soon as their word is in, other hits as usual, another miss waits.
// Response word: lp_rdata in the cycle its beat arrives (beat_now); in the cycle
// after, last_d (the RAM read of a word written at the same edge is not usable)
// while the RAM re-reads it; else the data RAM output.
module icache #(
    parameter int unsigned CACHE_BYTES = 8192,
    parameter int unsigned RAM_AW      = 25,        // RAM byte-offset bits
    localparam int unsigned LINES = CACHE_BYTES / 32,
    localparam int unsigned IDX_W = $clog2(LINES),
    localparam int unsigned WA_W  = IDX_W + 3,      // data RAM word address
    localparam int unsigned TAG_W = RAM_AW - 5 - IDX_W,
    localparam int unsigned LA_W  = RAM_AW - 5      // line address
) (
    input  logic             clk,
    input  logic             rst,

    // CPU instruction port (section 3)
    input  logic             req_valid,
    input  logic [31:0]      req_addr,
    output logic             rsp_ready,
    output logic [31:0]      rsp_rdata,

    // tag RAM port (synchronous, read data holds while en == 0 or we == 1)
    output logic             tag_en,
    output logic             tag_we,
    output logic [IDX_W-1:0] tag_addr,
    output logic [TAG_W:0]   tag_wdata,     // {valid, tag}
    input  logic [TAG_W:0]   tag_rdata,

    // line port to the arbiter (read only)
    output logic             lp_req,
    output logic [LA_W-1:0]  lp_addr,
    output logic [2:0]       lp_word,
    input  logic             lp_gnt,
    input  logic             lp_rvalid,
    input  logic [31:0]      lp_rdata,

    // performance strobes
    output logic             perf_access,   // pulse in the cycle after each accept
    output logic             perf_miss      // pulse in the cycle after a missing lookup
);
    if (LINES < 2 || (2**IDX_W) != LINES || CACHE_BYTES % 32 != 0) begin : g_bad_size
        $error("icache: CACHE_BYTES must be a power of two >= 64");
    end

    // LOOKUP: first cycle after an accept; WAIT: the request waits for a beat
    // of the fill, or (miss) for the fill to end
    typedef enum logic [1:0] {S_INIT, S_READY, S_LOOKUP, S_WAIT} state_t;
    state_t state;

    // Held request: word address within the RAM (offset[RAM_AW-1:2]).
    // During S_INIT the index field doubles as the tag-clear counter.
    logic [RAM_AW-3:0] req_q;
    logic [IDX_W-1:0]  req_idx;
    logic [TAG_W-1:0]  req_tag;
    logic [2:0]        req_w;
    assign req_w   = req_q[2:0];
    assign req_idx = req_q[IDX_W+2:3];
    assign req_tag = req_q[RAM_AW-3:IDX_W+3];

    // fill
    logic            fill_on;       // beats outstanding
    logic [LA_W-1:0] fill_line;     // {tag, idx} of the line being / last filled
    logic [2:0]      fill_w;        // word written by the next beat
    logic [2:0]      fill_cnt;      // beats received
    logic [7:0]      fill_mask;     // words written
    logic            lp_req_q;
    logic            last_v;        // a beat was written at the last edge ...
    logic [2:0]      last_w;        // ... to this word ...
    logic [31:0]     last_d;        // ... with this data
    logic            miss_q;

    // ------------------------------------------------------------------ lookup
    logic hit, same_line, in_fill, beat, beat_now, coll, ok, miss, start;
    assign hit       = tag_rdata[TAG_W] && (tag_rdata[TAG_W-1:0] == req_tag);
    assign same_line = (req_q[RAM_AW-3:3] == fill_line);
    assign in_fill   = fill_on && same_line;
    assign beat      = fill_on && lp_rvalid;
    assign beat_now  = in_fill && lp_rvalid && (fill_w == req_w);
    assign coll      = same_line && last_v && (last_w == req_w);
    assign ok        = in_fill ? (fill_mask[req_w] || beat_now) : hit;
    assign miss      = !in_fill && !hit;
    assign start     = ((state == S_LOOKUP) || (state == S_WAIT)) && miss && !fill_on;

    assign rsp_ready = (state == S_READY) || (((state == S_LOOKUP) || (state == S_WAIT)) && ok);

    // ------------------------------------------------------------------ data RAM
    // Read address: straight from the CPU when a request is presented (the
    // master only asserts req_valid while rsp_ready == 1), else the held word
    // (re-read of a word that was written at the read edge).
    logic [WA_W-1:0] d_raddr;
    logic [31:0]     d_rdata;
    assign d_raddr = req_valid ? req_addr[WA_W+1:2] : req_q[WA_W-1:0];

    ram_sdp #(.AW(WA_W), .W(32)) u_data (
        .clk   (clk),
        .we    (beat),
        .waddr ({fill_line[IDX_W-1:0], fill_w}),
        .wdata (lp_rdata),
        .re    (req_valid || coll),
        .raddr (d_raddr),
        .rdata (d_rdata)
    );

    assign rsp_rdata = beat_now ? lp_rdata : coll ? last_d : d_rdata;

    // ------------------------------------------------------------------ tag RAM
    // written at the start of a fill (no request can be accepted then)
    assign tag_en    = req_valid || (state == S_INIT) || start;
    assign tag_we    = (state == S_INIT) || start;
    assign tag_addr  = req_valid ? req_addr[IDX_W+4:5] : req_idx;
    assign tag_wdata = (state == S_INIT) ? '0 : {1'b1, req_tag};

    // ------------------------------------------------------------------ line port
    assign lp_req  = lp_req_q;
    assign lp_addr = fill_line;
    assign lp_word = fill_w;

    // ------------------------------------------------------------------ FSM
    always_ff @(posedge clk) begin
        if (rst) begin
            state     <= S_INIT;
            req_q     <= '0;
            fill_on   <= 1'b0;
            fill_line <= '0;
            fill_w    <= '0;
            fill_cnt  <= '0;
            fill_mask <= '0;
            lp_req_q  <= 1'b0;
            last_v    <= 1'b0;
            miss_q    <= 1'b0;
        end else begin
            miss_q <= (state == S_LOOKUP) && miss;
            last_v <= beat;

            // accept: a legal master only presents a request while
            // rsp_ready == 1, so req_valid alone qualifies the capture
            if (req_valid) req_q <= req_addr[RAM_AW-1:2];

            unique case (state)
                S_INIT: begin
                    // clear one valid bit per cycle
                    req_q[IDX_W+2:3] <= req_idx + 1'b1;
                    if (req_idx == IDX_W'(LINES - 1)) state <= S_READY;
                end
                S_READY: begin
                    if (req_valid) state <= S_LOOKUP;
                end
                S_LOOKUP, S_WAIT: begin
                    if (ok) state <= req_valid ? S_LOOKUP : S_READY;
                    else    state <= S_WAIT;
                end
                default: state <= S_INIT;
            endcase

            if (start) begin
                fill_on   <= 1'b1;
                fill_line <= req_q[RAM_AW-3:3];
                fill_w    <= req_w;             // critical word first
                fill_cnt  <= '0;
                fill_mask <= '0;
                lp_req_q  <= 1'b1;
            end
            if (lp_gnt) lp_req_q <= 1'b0;
            if (beat) begin
                fill_w            <= fill_w + 1'b1;
                fill_cnt          <= fill_cnt + 1'b1;
                fill_mask[fill_w] <= 1'b1;
                if (fill_cnt == 3'd7) fill_on <= 1'b0;
            end
        end
    end

    always_ff @(posedge clk) begin
        if (beat) begin
            last_w <= fill_w;
            last_d <= lp_rdata;
        end
    end

    assign perf_access = (state == S_LOOKUP);
    assign perf_miss   = miss_q;

    // Address bits outside the RAM offset and the byte offset are not used.
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_addr;
    assign unused_addr = ^{req_addr[31:RAM_AW], req_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */
endmodule
`default_nettype wire
