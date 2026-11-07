`default_nettype none
// Direct-mapped write-back / write-allocate data cache, slave of the section-3
// port (RAM range of the CPU data port; rtl/mem/README.md, "Caches").
//
// Organisation (CACHE_BYTES / 32 lines of 8 words), offset = req_addr[RAM_AW-1:0]:
//   word address = offset[IDX_W+4:2], index = offset[IDX_W+4:5],
//   tag = offset[RAM_AW-1:IDX_W+5].
// Storage: data in eight byte-lane RAMs holding one aligned word pair per
// address; {valid, tag} through the external tag port, written only at the start
// of a miss and by the reset sweep; dirty bits in a LUT RAM (4 per word,
// read-modify-write); the write buffer wb_mem (victim of a dirty miss) and the
// store buffer sb_* (one store hit made during a refill).
// Hit path: tag RAM output -> compare -> rsp_ready (LOOKUP cycle). A store hit
// writes its lanes and sets its dirty bit at the end of the LOOKUP cycle, or is
// posted to the store buffer during a refill. The bypass (byp_*) captures, at
// each accept, the lanes of the requested word written at that edge or buffered
// after it and overrides those lanes of the RAM output.
// Miss: tag and dirty bit written at once, refill requested from a flop in the
// next cycle; a dirty victim is copied to the write buffer in four pair reads
// (vx_*), during which nothing completes; beats are written as they arrive
// (fill_mask), a store miss merges its bytes into the first one.
// Response word: the beat in the cycle it arrives (beat_now), then hold_q; else
// the (bypassed) data RAM output.
module dcache #(
    parameter int unsigned CACHE_BYTES = 16384,
    parameter int unsigned RAM_AW      = 25,
    localparam int unsigned LINES = CACHE_BYTES / 32,
    localparam int unsigned IDX_W = $clog2(LINES),
    localparam int unsigned WA_W  = IDX_W + 3,
    localparam int unsigned TAG_W = RAM_AW - 5 - IDX_W,
    localparam int unsigned LA_W  = RAM_AW - 5
) (
    input  logic             clk,
    input  logic             rst,

    // CPU data port, RAM range (section 3)
    input  logic             req_valid,
    input  logic             req_we,
    input  logic [31:0]      req_addr,
    input  logic [3:0]       req_wstrb,
    input  logic [31:0]      req_wdata,
    output logic             rsp_ready,
    output logic [31:0]      rsp_rdata,

    // tag RAM port
    output logic             tag_en,
    output logic             tag_we,
    output logic [IDX_W-1:0] tag_addr,
    output logic [TAG_W:0]   tag_wdata,     // {valid, tag}
    input  logic [TAG_W:0]   tag_rdata,

    // line port to the arbiter
    output logic             lp_req,
    output logic             lp_we,         // 1 = write back a line
    output logic [LA_W-1:0]  lp_addr,
    output logic [2:0]       lp_word,
    output logic [31:0]      lp_wdata,      // current write-back word
    input  logic             lp_gnt,
    input  logic             lp_rvalid,
    input  logic [31:0]      lp_rdata,
    input  logic             lp_wnext,      // lp_wdata consumed; next word next cycle

    output logic             perf_access,
    output logic             perf_miss
);
    if (LINES < 4 || (2**IDX_W) != LINES || CACHE_BYTES % 32 != 0) begin : g_bad_size
        $error("dcache: CACHE_BYTES must be a power of two >= 128");
    end

    // LOOKUP: first cycle after an accept; WAIT: the request waits for a beat,
    // the victim copy, the end of the fill or an empty write buffer
    typedef enum logic [1:0] {S_INIT, S_READY, S_LOOKUP, S_WAIT} state_t;
    state_t state;

    // held request
    logic [RAM_AW-3:0] req_q;         // word address; index field = init counter in S_INIT
    logic              req_we_q;
    logic [3:0]        req_wstrb_q;
    logic [31:0]       req_wdata_q;
    logic [IDX_W-1:0]  req_idx;
    logic [TAG_W-1:0]  req_tag;
    logic [2:0]        req_w;
    assign req_w   = req_q[2:0];
    assign req_idx = req_q[IDX_W+2:3];
    assign req_tag = req_q[RAM_AW-3:IDX_W+3];

    // fill
    logic            fill_on;         // beats outstanding
    logic            own;             // the held request started the fill
    logic [LA_W-1:0] fill_line;       // {tag, idx} of the line being / last filled
    logic [2:0]      fill_w;          // word written by the next beat
    logic [2:0]      fill_cnt;        // beats received
    logic [7:0]      fill_mask;       // words written
    logic            lp_req_q;        // line request pending (refill, or with lp_we_q write-back)
    logic            lp_we_q;
    logic            miss_q;
    logic            hold_v;          // hold_q is the response
    logic [31:0]     hold_q;

    // victim copy and write buffer
    logic            vx_on;           // pair reads after the first one
    logic [1:0]      vx_p;            // pair read next
    logic            vx_wr;           // the RAM output is a victim pair ...
    logic [1:0]      vx_wp;           // ... this one
    logic            wb_full;         // the buffer holds a line not yet written back
    logic            wb_asked;        // its write has been requested
    logic [2:0]      wb_ptr;          // word on lp_wdata
    logic [LA_W-1:0] wb_line;
    logic [63:0]     wb_mem [0:3];

    // store buffer: a store hit completed during a fill, written when the fill is over
    logic            sb_v;
    logic [WA_W-1:0] sb_a;
    logic [3:0]      sb_strb;
    logic [31:0]     sb_data;

    // ------------------------------------------------------------------ lookup
    logic hit, same_line, in_fill, beat, beat_now, miss, start, lw, victim_dirty;
    logic ok;
    assign hit       = tag_rdata[TAG_W] && (tag_rdata[TAG_W-1:0] == req_tag);
    assign same_line = (req_q[RAM_AW-3:3] == fill_line);
    assign in_fill   = fill_on && same_line;
    assign beat      = fill_on && lp_rvalid;
    assign beat_now  = in_fill && lp_rvalid && (fill_w == req_w) && (own || !req_we_q);
    assign lw        = (state == S_LOOKUP) || (state == S_WAIT);
    // nothing completes during the victim copy; the miss itself and a load from
    // the line being filled need their word; any other load a hit, any other
    // store a hit and a free store buffer
    always_comb begin
        if (vx_on)                              ok = 1'b0;
        else if (own || (in_fill && !req_we_q)) ok = fill_mask[req_w] || beat_now;
        else                                    ok = hit && !(req_we_q && sb_v);
    end
    // a miss starts after the previous fill, with an empty store buffer, and
    // with an empty write buffer - or, if its victim is clean and its line not
    // the buffered one, while the write-back has not been requested yet or has
    // been granted (it then overtakes it, or waits in the backend)
    assign miss      = !own && !in_fill && !hit;
    assign start     = lw && miss && !fill_on && !sb_v &&
                       (!wb_full || (!victim_dirty && !lp_req_q && (req_q[RAM_AW-3:3] != wb_line)));

    assign rsp_ready = (state == S_READY) || (lw && ok);

    // a store hit is written at once, or (fill in progress) posted to the store
    // buffer, which is written at the first edge without a fill
    logic store_hit, post, drain;
    assign store_hit = lw && ok && req_we_q && !own;
    assign post      = store_hit && fill_on;
    assign drain     = sb_v && !fill_on;

    // ------------------------------------------------------------------ dirty bits
    localparam int unsigned DRT_AW = (IDX_W > 2) ? IDX_W - 2 : 1;
    logic [3:0]        dirty_mem [0:(2**DRT_AW)-1];
    logic [DRT_AW-1:0] drt_addr;
    logic [3:0]        drt_rword, drt_wword;
    logic              drt_we, drt_bit;

    if (IDX_W > 2) begin : g_drt_addr
        assign drt_addr = req_idx[IDX_W-1:2];
    end else begin : g_drt_addr0
        assign drt_addr = '0;
    end

    // the new line is dirty from the start of a store miss (its store is
    // merged into the first beat); a store hit sets the bit
    assign drt_rword = dirty_mem[drt_addr];
    assign drt_bit   = drt_rword[req_idx[1:0]];
    assign drt_we    = store_hit || start;
    always_comb begin
        drt_wword = drt_rword;
        drt_wword[req_idx[1:0]] = req_we_q;
    end
    always_ff @(posedge clk) begin
        if (drt_we) dirty_mem[drt_addr] <= drt_wword;
    end

    assign victim_dirty = tag_rdata[TAG_W] && drt_bit;

    // ------------------------------------------------------------------ data RAM
    // word addresses {idx, w}; the RAM is addressed by pair {idx, w[2:1]}
    logic [WA_W-1:0] d_raddr, d_waddr;
    logic            d_re;
    logic [3:0]      d_we;
    logic [31:0]     d_wdata;
    logic [63:0]     d_rdata;

    assign d_raddr = req_valid ? req_addr[WA_W+1:2] : {req_idx, vx_on ? vx_p : req_w[2:1], 1'b0};
    assign d_re    = req_valid || vx_on || (start && victim_dirty);
    assign d_waddr = beat ? {fill_line[IDX_W-1:0], fill_w} : sb_v ? sb_a : {req_idx, req_w};
    assign d_we    = beat ? 4'hF : drain ? sb_strb : ((store_hit && !fill_on) ? req_wstrb_q : 4'h0);

    // a beat carries the store of a store miss (first beat) and a buffered
    // store to its word
    logic sb_beat;
    assign sb_beat = sb_v && (sb_a == d_waddr);
    for (genvar i = 0; i < 4; i++) begin : g_lane
        always_comb begin
            if (beat && !(own && (fill_cnt == 3'd0) && req_we_q && req_wstrb_q[i]) && !(sb_beat && sb_strb[i]))
                d_wdata[8*i +: 8] = lp_rdata[8*i +: 8];
            else if (sb_v)                      // (a buffered store implies !own)
                d_wdata[8*i +: 8] = sb_data[8*i +: 8];
            else
                d_wdata[8*i +: 8] = req_wdata_q[8*i +: 8];
        end

        for (genvar h = 0; h < 2; h++) begin : g_half
            ram_sdp #(.AW(WA_W-1), .W(8)) u_lane (
                .clk   (clk),
                .we    (d_we[i] && (d_waddr[0] == 1'(h))),
                .waddr (d_waddr[WA_W-1:1]),
                .wdata (d_wdata[8*i +: 8]),
                .re    (d_re),
                .raddr (d_raddr[WA_W-1:1]),
                .rdata (d_rdata[32*h + 8*i +: 8])
            );
        end
    end

    // read-during-write bypass (see header); CPU reads only. Besides the lanes
    // written at the read edge it takes those of a store that is buffered
    // after the edge (posted now, or pending), which override them.
    logic [3:0]  byp_mask_q;
    logic [31:0] byp_data_q, ram_word;
    logic        byp_wr, byp_post, byp_sb;
    assign byp_wr   = d_waddr == d_raddr;
    assign byp_post = post && ({req_idx, req_w} == d_raddr);
    assign byp_sb   = sb_v && fill_on && (sb_a == d_raddr);
    always_ff @(posedge clk) begin
        if (rst) begin
            byp_mask_q <= '0;
        end else if (req_valid) begin
            for (int i = 0; i < 4; i++) begin
                byp_mask_q[i] <= 1'b1;
                if (byp_post && req_wstrb_q[i])  byp_data_q[8*i +: 8] <= req_wdata_q[8*i +: 8];
                else if (byp_sb && sb_strb[i])   byp_data_q[8*i +: 8] <= sb_data[8*i +: 8];
                else if (byp_wr && d_we[i])      byp_data_q[8*i +: 8] <= d_wdata[8*i +: 8];
                else                             byp_mask_q[i] <= 1'b0;
            end
        end
    end
    for (genvar i = 0; i < 4; i++) begin : g_out
        assign ram_word[8*i +: 8] = byp_mask_q[i] ? byp_data_q[8*i +: 8] : d_rdata[32*req_w[0] + 8*i +: 8];
    end
    assign rsp_rdata = beat_now ? d_wdata : hold_v ? hold_q : ram_word;

    // ------------------------------------------------------------------ tag RAM
    // written at the start of a miss (no request can be accepted then)
    assign tag_en    = req_valid || (state == S_INIT) || start;
    assign tag_we    = (state == S_INIT) || start;
    assign tag_addr  = req_valid ? req_addr[IDX_W+4:5] : req_idx;
    assign tag_wdata = (state == S_INIT) ? '0 : {1'b1, req_tag};

    // ------------------------------------------------------------------ line port
    assign lp_req   = lp_req_q;
    assign lp_we    = lp_we_q;
    assign lp_addr  = lp_we_q ? wb_line : fill_line;
    assign lp_word  = fill_w;
    assign lp_wdata = wb_mem[wb_ptr[2:1]][32*wb_ptr[0] +: 32];

    always_ff @(posedge clk) begin
        if (vx_wr) wb_mem[vx_wp] <= d_rdata;
    end

    // ------------------------------------------------------------------ FSM
    always_ff @(posedge clk) begin
        if (rst) begin
            state       <= S_INIT;
            req_q       <= '0;
            req_we_q    <= 1'b0;
            req_wstrb_q <= '0;
            req_wdata_q <= '0;
            fill_on     <= 1'b0;
            own         <= 1'b0;
            fill_line   <= '0;
            fill_w      <= '0;
            fill_cnt    <= '0;
            fill_mask   <= '0;
            lp_req_q    <= 1'b0;
            lp_we_q     <= 1'b0;
            miss_q      <= 1'b0;
            hold_v      <= 1'b0;
            vx_on       <= 1'b0;
            vx_p        <= '0;
            vx_wr       <= 1'b0;
            vx_wp       <= '0;
            wb_full     <= 1'b0;
            wb_asked    <= 1'b0;
            wb_ptr      <= '0;
            wb_line     <= '0;
            sb_v        <= 1'b0;
        end else begin
            miss_q <= (state == S_LOOKUP) && miss;

            // accept: a legal master only presents a request while
            // rsp_ready == 1 (READY, or LOOKUP/WAIT completing), so req_valid
            // alone qualifies the capture (keeps the hit off these enables)
            if (req_valid) begin
                req_q       <= req_addr[RAM_AW-1:2];
                req_we_q    <= req_we;
                req_wstrb_q <= req_wstrb;
                req_wdata_q <= req_wdata;
            end

            unique case (state)
                S_INIT: begin
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

            // response of a request completed by its beat
            if (req_valid)     hold_v <= 1'b0;
            else if (beat_now) hold_v <= 1'b1;
            if (beat_now) hold_q <= d_wdata;
            if (req_valid) own <= 1'b0;

            // miss: refill, and copy a dirty victim into the write buffer
            vx_wr <= d_re && !req_valid;
            vx_wp <= d_raddr[2:1];
            if (start) begin
                own       <= 1'b1;
                fill_on   <= 1'b1;
                fill_line <= req_q[RAM_AW-3:3];
                fill_w    <= req_w;             // critical word first
                fill_cnt  <= '0;
                fill_mask <= '0;
                lp_req_q  <= 1'b1;
                lp_we_q   <= 1'b0;
                if (victim_dirty) begin
                    vx_on   <= 1'b1;
                    vx_p    <= req_w[2:1] + 1'b1;
                    wb_full <= 1'b1;
                    wb_line <= {tag_rdata[TAG_W-1:0], req_idx};
                end
            end
            if (vx_on) begin
                vx_p <= vx_p + 1'b1;
                if (vx_p + 1'b1 == req_w[2:1]) vx_on <= 1'b0;
            end
            // write-back once the fill is over and no miss starts
            if (lp_gnt) begin
                lp_req_q <= 1'b0;
            end else if (wb_full && !wb_asked && !lp_req_q && !fill_on && !start) begin
                lp_req_q  <= 1'b1;
                lp_we_q   <= 1'b1;
                wb_asked  <= 1'b1;
            end
            if (post) begin
                sb_v    <= 1'b1;
                sb_a    <= {req_idx, req_w};
                sb_strb <= req_wstrb_q;
                sb_data <= req_wdata_q;
            end else if (drain) begin
                sb_v    <= 1'b0;
            end
            if (lp_wnext) begin
                wb_ptr <= wb_ptr + 1'b1;
                if (wb_ptr == 3'd7) begin
                    wb_full  <= 1'b0;
                    wb_asked <= 1'b0;
                end
            end
            if (beat) begin
                fill_w            <= fill_w + 1'b1;
                fill_cnt          <= fill_cnt + 1'b1;
                fill_mask[fill_w] <= 1'b1;
                if (fill_cnt == 3'd7) fill_on <= 1'b0;
            end
        end
    end

    assign perf_access = (state == S_LOOKUP);
    assign perf_miss   = miss_q;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_addr;
    assign unused_addr = ^{req_addr[31:RAM_AW], req_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */
endmodule
`default_nettype wire
