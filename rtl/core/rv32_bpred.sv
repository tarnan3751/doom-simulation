// rv32_bpred - branch predictor: BTB + bimodal 2-bit counters + return-address stack
// (rtl/core/README.md, "Branch prediction").
//
// BTB (block RAM): read with the fetch request address when a fetch is issued, so
// its output belongs to the instruction in IF and holds across stalls. BHT (LUT
// RAM): read asynchronously with f_pc. For the instruction in IF
//   hit    = entry.valid && entry.tag == tag(f_pc)        (partial tag)
//   taken  = hit && (entry.kind != BRANCH || counter[1])
//   target = entry.kind == RET ? RAS top : {f_pc[31:23], entry.tgt, 2'b00}
// Updates come from the instruction leaving EX; the RAS is pushed/popped in IF
// and its speculative pointer restored from the committed one on a redirect.
// Every prediction is checked in EX, so nothing here affects architectural state.
`default_nettype none

module rv32_bpred
    import rv32_pkg::*;
#(
    parameter int unsigned BTB_ENTRIES = 256,
    parameter int unsigned BHT_ENTRIES = 1024,
    parameter int unsigned RAS_DEPTH   = 8
) (
    input  logic        clk,
    input  logic        rst,

    // lookup, issued together with the fetch request
    input  logic        lu_en,
    input  logic [31:0] lu_pc,

    // prediction for the instruction currently in IF
    input  logic [31:0] f_pc,
    output logic        pred_taken,
    output logic [31:0] pred_target,
    output logic [1:0]  pred_cnt,
    input  logic        f_adv,          // the IF instruction moves on to ID (not flushed)

    // resolution of the instruction leaving EX
    input  logic        up_en,
    input  logic [31:0] up_pc,
    input  logic        up_branch,
    input  logic        up_jump,        // jal or jalr
    input  logic        up_call,
    input  logic        up_ret,
    input  logic        up_trap,
    input  logic        up_taken,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] up_target,      // only target[22:2] is stored
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [1:0]  up_cnt,
    input  logic        up_pred_taken,
    input  logic        up_redirect
);
    localparam int unsigned BTB_IW = $clog2(BTB_ENTRIES);
    localparam int unsigned BHT_IW = $clog2(BHT_ENTRIES);
    localparam int unsigned RAS_IW = (RAS_DEPTH > 1) ? $clog2(RAS_DEPTH) : 1;
    localparam int unsigned TGT_LO = 2;
    localparam int unsigned TGT_HI = 22;                     // BTB stores target[22:2]
    localparam int unsigned TGT_W  = TGT_HI - TGT_LO + 1;
    localparam int unsigned TAG_W  = (2 + BTB_IW + 12 <= 32) ? 12 : (30 - BTB_IW);
    // entry = valid + kind + tag + target = 36 bits for 256 entries (one 512x36 BSRAM)

    typedef struct packed {
        logic             valid;
        btb_kind_e        kind;
        logic [TAG_W-1:0] tag;
        logic [TGT_W-1:0] tgt;
    } btb_entry_t;

    // The index/tag functions deliberately look at a slice of the PC only.
    /* verilator lint_off UNUSEDSIGNAL */
    function automatic logic [BTB_IW-1:0] btb_idx(input logic [31:0] pc);
        btb_idx = pc[2 +: BTB_IW];
    endfunction
    function automatic logic [TAG_W-1:0] btb_tag(input logic [31:0] pc);
        btb_tag = pc[2 + BTB_IW +: TAG_W];
    endfunction
    function automatic logic [BHT_IW-1:0] bht_idx(input logic [31:0] pc);
        bht_idx = pc[2 +: BHT_IW];
    endfunction
    /* verilator lint_on UNUSEDSIGNAL */
    function automatic logic [RAS_IW-1:0] ras_inc(input logic [RAS_IW-1:0] p);
        ras_inc = (32'(p) == RAS_DEPTH - 1) ? '0 : p + 1'b1;
    endfunction
    function automatic logic [RAS_IW-1:0] ras_dec(input logic [RAS_IW-1:0] p);
        ras_dec = (p == '0) ? RAS_IW'(RAS_DEPTH - 1) : p - 1'b1;
    endfunction

    // ---------------------------------------------------------------- BTB
    // no_rw_check: a lookup in the cycle the same entry is updated may read either
    // value on the FPGA (only a prediction, checked in EX), so synthesis must not
    // add read-during-write logic in front of the lookup output.
    (* no_rw_check *) btb_entry_t btb_mem [0:BTB_ENTRIES-1];
    btb_entry_t btb_q;
    btb_entry_t btb_wentry;
    logic       btb_we;
    logic       up_cf;

    assign up_cf  = up_branch || up_jump;
    assign btb_we = up_en && ((up_cf && up_taken && !up_trap) || (!up_cf && up_pred_taken));

    always_comb begin
        btb_wentry.valid = up_cf && up_taken && !up_trap;
        btb_wentry.kind  = up_branch ? BK_BRANCH : up_ret ? BK_RET : up_call ? BK_CALL : BK_JUMP;
        btb_wentry.tag   = btb_tag(up_pc);
        btb_wentry.tgt   = up_target[TGT_HI:TGT_LO];
    end

    always_ff @(posedge clk) begin
        if (btb_we) btb_mem[btb_idx(up_pc)] <= btb_wentry;
        if (lu_en)  btb_q <= btb_mem[btb_idx(lu_pc)];
    end

    // ---------------------------------------------------------------- BHT
    // Distributed (LUT) RAM, read asynchronously in IF (see the header).
    // no_rw_check: synthesis may turn the read with a registered address (f_pc)
    // into a clocked read; a lookup in the cycle the same counter is updated may
    // then read either value on the FPGA (only a prediction).
    (* no_rw_check *) (* ram_style = "distributed" *)
    logic [1:0] bht_mem [0:BHT_ENTRIES-1];
    logic [1:0] bht_q;
    logic [1:0] bht_new;
    logic       bht_we;

    assign bht_we = up_en && up_branch && !up_trap;
    always_comb begin
        if (up_taken) bht_new = (up_cnt == 2'b11) ? 2'b11 : up_cnt + 2'b01;
        else          bht_new = (up_cnt == 2'b00) ? 2'b00 : up_cnt - 2'b01;
    end

    always_ff @(posedge clk) begin
        if (bht_we) bht_mem[bht_idx(up_pc)] <= bht_new;
    end
    assign bht_q = bht_mem[bht_idx(f_pc)];     // counter of the instruction in IF

    // ---------------------------------------------------------------- RAS
    logic [29:0]       ras [0:RAS_DEPTH-1];
    logic [RAS_IW-1:0] ras_sp;    // speculative top-of-stack
    logic [RAS_IW-1:0] ras_cp;    // committed top-of-stack
    logic [RAS_IW-1:0] ras_cp_next;

    logic btb_hit;
    assign btb_hit = btb_q.valid && (btb_q.tag == btb_tag(f_pc));

    assign pred_taken  = btb_hit && ((btb_q.kind != BK_BRANCH) || bht_q[1]);
    assign pred_target = (btb_q.kind == BK_RET) ? {ras[ras_sp], 2'b00}
                                                : {f_pc[31:TGT_HI+1], btb_q.tgt, 2'b00};
    assign pred_cnt    = bht_q;

    always_comb begin
        ras_cp_next = ras_cp;
        if (up_en && !up_trap) begin
            if (up_call)     ras_cp_next = ras_inc(ras_cp);
            else if (up_ret) ras_cp_next = ras_dec(ras_cp);
        end
    end

    logic [29:0] f_pc4;
    assign f_pc4 = f_pc[31:2] + 30'd1;

    always_ff @(posedge clk) begin
        if (rst) begin
            ras_sp <= '0;
            ras_cp <= '0;
        end else begin
            ras_cp <= ras_cp_next;
            if (up_en && up_redirect) begin
                ras_sp <= ras_cp_next;
                if (up_call && !up_trap) ras[ras_cp_next] <= up_pc[31:2] + 30'd1;
            end else if (f_adv && btb_hit) begin
                if (btb_q.kind == BK_CALL) begin
                    ras_sp              <= ras_inc(ras_sp);
                    ras[ras_inc(ras_sp)] <= f_pc4;
                end else if (btb_q.kind == BK_RET) begin
                    ras_sp <= ras_dec(ras_sp);
                end
            end
        end
    end

endmodule

`default_nettype wire
