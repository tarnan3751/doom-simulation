`default_nettype none
// SDR SDRAM controller: one 8-beat burst per cache line, closed-page policy
// (every READ/WRITE carries auto-precharge, so all banks are idle between
// transactions and a refresh never waits for a precharge).
//
// Pins (section 6): every sdram_* output is a flip-flop; sdram_dq_i is captured
// in a flip-flop (dq_i_q) and only that register is used. The chip samples the
// pins at the edge after they were loaded, so all spacing rules below are
// between "issue edges" (the edge that loads a command into the pin flops).
//
// Timing: cycles derived from CLK_HZ (table of section 6, rtl/mem/README.md).
// Sequence per transaction (issue edges, e = READ/WRITE edge):
//   ACT at e - T_RCD; READ/WRITE (A10 = 1) at e;
//   read : beat n captured by dq_i_q at e + 1 + CL + n, delivered (bk_rvalid)
//          in the following cycle; next ACT/REF no earlier than
//          max(e + BL, ACT + tRAS) + tRP and ACT + tRC;
//   write: beat n driven from edge e + n (dq_oe = 1); bk_wnext pulses in the
//          cycle each word is taken; next ACT/REF no earlier than
//          max(e + BL - 1 + tWR, ACT + tRAS) + tRP and ACT + tRC.
//   A write is only started once the chip has stopped driving the bus of the
//   previous read, plus one idle turnaround cycle.
// Refresh: one pending refresh per T_REFI cycles after init, issued before any
// new request whenever the controller is idle. Init: power-up wait, PRECHARGE
// ALL, INIT_REFS x AUTO REFRESH, MODE REGISTER SET (CL 2, sequential, BL 8).
module sdram_ctrl #(
    parameter int unsigned CLK_HZ   = 50_000_000,
    parameter int unsigned ROW_BITS = 13,
    parameter int unsigned COL_BITS = 8,
    localparam int unsigned LA_W    = 2 + ROW_BITS + COL_BITS - 3
) (
    input  logic                clk,
    input  logic                rst,

    input  logic                bk_req,
    input  logic                bk_we,
    input  logic [LA_W-1:0]     bk_addr,
    input  logic [2:0]          bk_word,
    input  logic                bk_id,
    input  logic [31:0]         bk_wdata,
    output logic                bk_gnt,
    output logic                bk_rvalid,
    output logic                bk_rid,
    output logic [31:0]         bk_rdata,
    output logic                bk_wnext,

    output logic                sdram_cke,
    output logic                sdram_cs_n,
    output logic                sdram_ras_n,
    output logic                sdram_cas_n,
    output logic                sdram_we_n,
    output logic [1:0]          sdram_ba,
    output logic [ROW_BITS-1:0] sdram_addr,
    output logic [3:0]          sdram_dqm,
    output logic [31:0]         sdram_dq_o,
    output logic                sdram_dq_oe,
    input  logic [31:0]         sdram_dq_i
);
    if (ROW_BITS < 11 || COL_BITS < 4 || COL_BITS > 10) begin : g_bad_geom
        $error("sdram_ctrl: needs ROW_BITS >= 11 (A10 = auto precharge) and 4 <= COL_BITS <= 10");
    end

    // ------------------------------------------------------------- timing
    function automatic int unsigned ps2cyc(input longint unsigned ps);
        longint unsigned prod;
        prod = ps * longint'(CLK_HZ);
        return int'((prod + 64'd999_999_999_999) / 64'd1_000_000_000_000);
    endfunction
    function automatic int unsigned max2(input int unsigned a, input int unsigned b);
        return (a > b) ? a : b;
    endfunction

    localparam int unsigned CL        = 2;
    localparam int unsigned BL        = 8;
    localparam int unsigned INIT_REFS = 8;            // contract: >= 2
    localparam int unsigned T_POWERUP = ps2cyc(64'd200_000_000);
    localparam int unsigned T_RP      = max2(1, ps2cyc(64'd18_000));
    localparam int unsigned T_RCD     = max2(1, ps2cyc(64'd18_000));
    localparam int unsigned T_RAS     = max2(1, ps2cyc(64'd42_000));
    localparam int unsigned T_RC      = max2(1, ps2cyc(64'd60_000));
    localparam int unsigned T_RFC     = max2(1, ps2cyc(64'd60_000));
    localparam int unsigned T_WR      = 2;
    localparam int unsigned T_MRD     = 2;
    localparam int unsigned T_REFI    = int'((longint'(CLK_HZ) * 64) / 4_096_000);

    // Idle cycles to insert after the issue edge of a command before the next
    // command may be issued ("cnt" semantics: issue happens in the cycle in
    // which cnt == 0, so a value of C means C empty cycles).
    localparam int unsigned W_PRE = T_RP - 1;
    localparam int unsigned W_REF = T_RFC - 1;
    localparam int unsigned W_MRS = T_MRD - 1;
    // after a READ issued at e: next >= max(e+BL, e-T_RCD+T_RAS) + T_RP, >= e-T_RCD+T_RC
    localparam int unsigned W_RD  = max2(max2(BL + T_RP, max2(T_RAS, T_RCD) - T_RCD + T_RP),
                                         max2(T_RC, T_RCD) - T_RCD) - 1;
    // after the last write beat issued at e+BL-1:
    //   next >= max(e+BL-1+T_WR, e-T_RCD+T_RAS) + T_RP, >= e-T_RCD+T_RC
    localparam int unsigned W_WR  = max2(max2(T_WR + T_RP,
                                              max2(T_RAS, T_RCD + BL - 1) - (T_RCD + BL - 1) + T_RP),
                                         max2(T_RC, T_RCD + BL) - (T_RCD + BL - 1)) - 1;
    // the chip drives the bus for a READ issued at e during the cycles ending
    // at e+1+CL .. e+CL+BL; a write may drive from the cycle after one idle cycle
    localparam int unsigned W_DRAIN = CL + BL - 1;

    localparam int unsigned CNT_W = $clog2(T_POWERUP + 1);

    // mode register: burst write, CL = 2, sequential, BL = 8
    localparam logic [ROW_BITS-1:0] MODE = ROW_BITS'(13'b000_0_00_010_0_011);

    // commands {ras_n, cas_n, we_n}
    localparam logic [2:0] C_NOP = 3'b111, C_ACT = 3'b011, C_READ = 3'b101,
                           C_WRITE = 3'b100, C_PRE = 3'b010, C_REF = 3'b001,
                           C_MRS = 3'b000;

    // ------------------------------------------------------------- state
    typedef enum logic [2:0] {S_PWR, S_IREF, S_IMRS, S_IDLE, S_ACT, S_WBURST} state_t;
    state_t          state;
    logic [CNT_W-1:0] cnt;
    logic [3:0]      iref;
    logic [2:0]      wbeat;

    logic                we_l, id_l;
    logic [1:0]          ba_l;
    logic [COL_BITS-4:0] colhi_l;
    logic [2:0]          word_l;

    logic            init_done;
    logic [$clog2(T_REFI+1)-1:0] ref_timer;
    logic [2:0]      ref_pend;

    logic [3:0]      rd_ins;          // read beats still to enter the pipe
    logic [3:0]      rd_drain;        // cycles until the bus is free for a write
    logic [CL+1:0]   rdp;             // read beat pipe
    logic [CL+1:0]   ridp;            // request id per pipe stage
    logic            rd_id;

    logic [31:0]     dq_i_q;

    // pin registers
    logic [2:0]          cmd_q;
    // Power-up values (= the reset values; the FPGA's flop INIT): before the
    // first reset edge the chip already sees CKE low, CS# high and a released
    // bus, so nothing it samples before reset can be taken as a command. The
    // synchronous reset below still sets them; the initialiser only covers the
    // cycles before it.
    /* verilator lint_off PROCASSINIT */
    logic                cs_n_q  = 1'b1;
    logic                cke_q   = 1'b0;
    logic                dq_oe_q = 1'b0;
    /* verilator lint_on PROCASSINIT */
    logic [1:0]          ba_q;
    logic [ROW_BITS-1:0] a_q;
    logic [31:0]         dq_o_q;

    // address fields of a line address {ba, row, col[COL_BITS-1:3]}
    logic [1:0]          req_ba;
    logic [ROW_BITS-1:0] req_row;
    logic [COL_BITS-4:0] req_colhi;
    assign req_ba    = bk_addr[LA_W-1 -: 2];
    assign req_row   = bk_addr[COL_BITS-3 +: ROW_BITS];
    assign req_colhi = bk_addr[COL_BITS-4:0];

    // READ/WRITE address: column, A10 = 1 (auto precharge), other bits 0
    logic [ROW_BITS-1:0] col_addr;
    always_comb begin
        col_addr = '0;
        col_addr[COL_BITS-1:0] = {colhi_l, word_l};
        col_addr[10] = 1'b1;
    end

    // ------------------------------------------------------------- control
    logic can_issue, do_ref, rw_issue, ref_inc;
    assign can_issue = (state == S_IDLE) && (cnt == '0);
    assign ref_inc   = init_done && (ref_timer == '0) && (ref_pend != 3'd7);
    assign do_ref    = can_issue && (ref_pend != '0);
    assign bk_gnt    = can_issue && (ref_pend == '0) && bk_req && (!bk_we || rd_drain == '0);
    assign rw_issue  = (state == S_ACT) && (cnt == '0);
    assign bk_wnext  = (rw_issue && we_l) || (state == S_WBURST);

    always_ff @(posedge clk) begin
        if (rst) begin
            state     <= S_PWR;
            cnt       <= CNT_W'(T_POWERUP);
            iref      <= '0;
            wbeat     <= '0;
            we_l      <= 1'b0;
            id_l      <= 1'b0;
            ba_l      <= '0;
            colhi_l   <= '0;
            word_l    <= '0;
            init_done <= 1'b0;
            ref_timer <= '0;
            ref_pend  <= '0;
            rd_ins    <= '0;
            rd_drain  <= '0;
            rdp       <= '0;
            ridp      <= '0;
            rd_id     <= 1'b0;
            cmd_q     <= C_NOP;
            cs_n_q    <= 1'b1;           // deselect while in reset
            cke_q     <= 1'b0;
            ba_q      <= '0;
            a_q       <= '0;
            dq_o_q    <= '0;
            dq_oe_q   <= 1'b0;
        end else begin
            // pin defaults: NOP, bus released
            cke_q   <= 1'b1;
            cs_n_q  <= 1'b0;
            cmd_q   <= C_NOP;
            dq_oe_q <= 1'b0;

            unique case (state)
                S_PWR: begin
                    if (cnt == '0) begin
                        cmd_q   <= C_PRE;
                        a_q     <= '0;
                        a_q[10] <= 1'b1;         // all banks
                        cnt     <= CNT_W'(W_PRE);
                        iref    <= 4'(INIT_REFS - 1);
                        state   <= S_IREF;
                    end else begin
                        cnt <= cnt - 1'b1;
                    end
                end
                S_IREF: begin
                    if (cnt == '0) begin
                        cmd_q <= C_REF;
                        cnt   <= CNT_W'(W_REF);
                        if (iref == '0) state <= S_IMRS;
                        else            iref  <= iref - 1'b1;
                    end else begin
                        cnt <= cnt - 1'b1;
                    end
                end
                S_IMRS: begin
                    if (cnt == '0) begin
                        cmd_q     <= C_MRS;
                        ba_q      <= '0;
                        a_q       <= MODE;
                        cnt       <= CNT_W'(W_MRS);
                        init_done <= 1'b1;
                        state     <= S_IDLE;
                    end else begin
                        cnt <= cnt - 1'b1;
                    end
                end
                S_IDLE: begin
                    if (cnt != '0) begin
                        cnt <= cnt - 1'b1;
                    end else if (do_ref) begin
                        cmd_q <= C_REF;
                        cnt   <= CNT_W'(W_REF);
                    end else if (bk_gnt) begin
                        cmd_q   <= C_ACT;
                        ba_q    <= req_ba;
                        a_q     <= req_row;
                        we_l    <= bk_we;
                        id_l    <= bk_id;
                        ba_l    <= req_ba;
                        colhi_l <= req_colhi;
                        word_l  <= bk_we ? 3'd0 : bk_word;
                        cnt    <= CNT_W'(T_RCD - 1);
                        state  <= S_ACT;
                    end
                end
                S_ACT: begin
                    if (cnt != '0) begin
                        cnt <= cnt - 1'b1;
                    end else begin
                        ba_q <= ba_l;
                        a_q  <= col_addr;
                        if (we_l) begin
                            cmd_q   <= C_WRITE;
                            dq_o_q  <= bk_wdata;
                            dq_oe_q <= 1'b1;
                            wbeat   <= 3'd1;
                            state   <= S_WBURST;
                        end else begin
                            cmd_q <= C_READ;
                            cnt   <= CNT_W'(W_RD);
                            rd_id <= id_l;
                            state <= S_IDLE;
                        end
                    end
                end
                S_WBURST: begin
                    dq_o_q  <= bk_wdata;
                    dq_oe_q <= 1'b1;
                    wbeat   <= wbeat + 1'b1;
                    if (wbeat == 3'd7) begin
                        cnt   <= CNT_W'(W_WR);
                        state <= S_IDLE;
                    end
                end
                default: state <= S_PWR;
            endcase

            // refresh scheduling
            if (init_done) begin
                if (ref_timer == '0) ref_timer <= $bits(ref_timer)'(T_REFI - 1);
                else                 ref_timer <= ref_timer - 1'b1;
            end else begin
                ref_timer <= $bits(ref_timer)'(T_REFI - 1);
            end
            if (ref_inc && !do_ref)      ref_pend <= ref_pend + 1'b1;
            else if (do_ref && !ref_inc) ref_pend <= ref_pend - 1'b1;

            // read beat tracking
            if (rw_issue && !we_l) begin
                rd_ins   <= 4'(BL - 1);
                rd_drain <= 4'(W_DRAIN);
            end else begin
                if (rd_ins != '0)   rd_ins   <= rd_ins - 1'b1;
                if (rd_drain != '0) rd_drain <= rd_drain - 1'b1;
            end
            rdp  <= {rdp[CL:0],  (rw_issue && !we_l) || (rd_ins != '0)};
            ridp <= {ridp[CL:0], (rw_issue && !we_l) ? id_l : rd_id};
        end
    end

    // input capture register (every cycle)
    always_ff @(posedge clk) begin
        dq_i_q <= sdram_dq_i;
    end

    assign bk_rvalid = rdp[CL+1];
    assign bk_rid    = ridp[CL+1];
    assign bk_rdata  = dq_i_q;

    assign sdram_cke   = cke_q;
    assign sdram_cs_n  = cs_n_q;
    assign sdram_ras_n = cmd_q[2];
    assign sdram_cas_n = cmd_q[1];
    assign sdram_we_n  = cmd_q[0];
    assign sdram_ba    = ba_q;
    assign sdram_addr  = a_q;
    assign sdram_dqm   = 4'b0000;          // all lanes enabled (held during bursts)
    assign sdram_dq_o  = dq_o_q;
    assign sdram_dq_oe = dq_oe_q;
endmodule
`default_nettype wire
