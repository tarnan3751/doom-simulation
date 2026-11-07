// rv32_core - RV32IM + Zicsr + Zicntr, machine mode only, 5-stage pipeline.
//
//   IF  : the fetch response for f_pc is consumed; the next fetch request
//         (predicted next PC, or the EX redirect target) is presented.
//   ID  : decode, register read (write-through), forwarding selection,
//         branch-target precompute, load-use detection.
//   EX  : ALU / AGU / branch resolution / CSR access / mul-div, exception
//         detection, trap entry and mret, D-port request.
//   MEM : the D-port response is consumed (load alignment).
//   WB  : register write; the retire trace is registered from here.
//
// Stall/flush equations and design decisions: rtl/core/README.md.
`default_nettype none

module rv32_core
    import rv32_pkg::*;
#(
    parameter logic [31:0] RESET_PC    = 32'h8000_0000,
    parameter int unsigned BTB_ENTRIES = 256,    // power of two
    parameter int unsigned BHT_ENTRIES = 1024,   // power of two
    parameter int unsigned RAS_DEPTH   = 8,
    parameter bit          COVERAGE    = 1'b1    // simulation: compute cov_events (tests/core); 0: constant 0
) (
    input  logic        clk,
    input  logic        rst,                // synchronous, active high

    // instruction port (section 3)
    output logic        imem_req_valid,
    output logic [31:0] imem_req_addr,
    input  logic        imem_rsp_ready,
    input  logic [31:0] imem_rsp_rdata,

    // data port (section 3)
    output logic        dmem_req_valid,
    output logic        dmem_req_we,
    output logic [31:0] dmem_req_addr,
    output logic [3:0]  dmem_req_wstrb,
    output logic [31:0] dmem_req_wdata,
    input  logic        dmem_rsp_ready,
    input  logic [31:0] dmem_rsp_rdata,

    // retire trace: at most one instruction per cycle
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

    // performance event strobes (one-cycle pulses)
    output logic        perf_branch,
    output logic        perf_branch_miss,
    output logic        perf_jump,
    output logic        perf_jump_miss
);

    // =====================================================================
    // Global pipeline control
    // =====================================================================
    logic advance;      // the whole pipeline moves one step at the next edge
    logic redirect;     // EX: wrong next PC in IF/ID (mispredict, trap, mret)
    logic load_use;     // ID needs the result of the load in EX
    logic fetch_go;     // a fetch request is issued (IF -> ID moves)
    logic ex_busy;      // EX holds a mul/div whose result is not ready yet
    logic [31:0] ex_npc;

    assign advance  = !rst && imem_rsp_ready && dmem_rsp_ready && !ex_busy;
    assign fetch_go = advance && !(load_use && !redirect);

    // =====================================================================
    // IF
    // =====================================================================
    logic        f_valid;     // a fetch for f_pc has been issued
    logic [31:0] f_pc;
    logic        bp_taken;
    logic [31:0] bp_target;
    logic [1:0]  bp_cnt;
    logic [31:0] npc;

    always_comb begin
        if (redirect)      npc = ex_npc;
        else if (!f_valid) npc = f_pc;            // first fetch after reset
        else if (bp_taken) npc = bp_target;
        else               npc = f_pc + 32'd4;
    end

    assign imem_req_valid = fetch_go;
    assign imem_req_addr  = npc;

    always_ff @(posedge clk) begin
        if (rst) begin
            f_valid <= 1'b0;
            f_pc    <= RESET_PC;
        end else if (fetch_go) begin
            f_valid <= 1'b1;
            f_pc    <= npc;
        end
    end

    // ---- IF/ID ----
    logic        d_valid;
    logic [31:0] d_pc, d_insn;
    logic [31:2] d_pred_tgt;
    logic        d_pred_taken;
    logic [1:0]  d_pred_cnt;

    always_ff @(posedge clk) begin
        if (rst) begin
            d_valid <= 1'b0;
        end else if (fetch_go) begin
            d_valid <= f_valid && !redirect;
        end
        if (fetch_go) begin
            d_pc         <= f_pc;
            d_insn       <= imem_rsp_rdata;
            d_pred_taken <= bp_taken;
            d_pred_tgt   <= bp_target[31:2];
            d_pred_cnt   <= bp_cnt;
        end
    end

    // =====================================================================
    // ID
    // =====================================================================
    decoded_t    dec;
    logic [4:0]  d_rs1, d_rs2;
    logic [31:0] rf_rdata1, rf_rdata2;
    logic [31:0] id_pcimm;
    logic        id_tgt_match;
    logic [31:2] id_jr_p0, id_jr_p1;
    fwd_e        id_fwd_a, id_fwd_b;

    rv32_decode u_decode (.insn(d_insn), .d(dec));

    assign d_rs1 = d_insn[19:15];
    assign d_rs2 = d_insn[24:20];

    // regfile write port (from WB)
    logic        w_valid;
    logic [4:0]  w_rd;
    logic [31:0] w_result;
    logic        rf_we;
    assign rf_we = advance && w_valid;

    rv32_regfile u_regfile (
        .clk    (clk),
        .raddr1 (d_rs1),     .rdata1 (rf_rdata1),
        .raddr2 (d_rs2),     .rdata2 (rf_rdata2),
        .we     (rf_we),     .waddr  (w_rd),     .wdata (w_result)
    );

    // lui: imm; auipc/jal/branch: pc + imm (jalr target is formed in EX)
    assign id_pcimm     = (d_insn[6:0] == OPC_LUI) ? dec.imm : d_pc + dec.imm;
    assign id_tgt_match = (id_pcimm[31:2] == d_pred_tgt);
    // jalr target check without an adder on the redirect path:
    //   (rs1 + imm)[31:2] == pred[31:2]
    //   <=> rs1[31:2] == pred[31:2] - imm[31:2] - carry(rs1[1:0] + imm[1:0])
    // both right-hand sides are formed here, EX picks one by the 2-bit carry.
    assign id_jr_p0     = d_pred_tgt - dec.imm[31:2];
    assign id_jr_p1     = id_jr_p0 - 30'd1;

    // EX-stage state needed for hazard detection
    logic        e_valid;
    // rs1/rs2/uses_rs* of the decoded word are only needed in ID
    /* verilator lint_off UNUSEDSIGNAL */
    decoded_t    e_dec;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        m_valid;
    logic [4:0]  m_rd;

    // Forwarding selection: when this instruction is in EX, the instruction
    // now in EX will be in MEM and the one now in MEM will be in WB.
    function automatic fwd_e fwd_sel(input logic [4:0] rs, input logic ex_v, input logic [4:0] ex_rd,
                                     input logic mem_v, input logic [4:0] mem_rd);
        if (rs != 5'd0 && ex_v && ex_rd == rs)        fwd_sel = FWD_MEM;
        else if (rs != 5'd0 && mem_v && mem_rd == rs) fwd_sel = FWD_WB;
        else                                          fwd_sel = FWD_REG;
    endfunction
    assign id_fwd_a = fwd_sel(d_rs1, e_valid, e_dec.rd, m_valid, m_rd);
    assign id_fwd_b = fwd_sel(d_rs2, e_valid, e_dec.rd, m_valid, m_rd);

    assign load_use = d_valid && e_valid && e_dec.is_load && (e_dec.rd != 5'd0) &&
                      ((dec.uses_rs1 && d_rs1 == e_dec.rd) ||
                       (dec.uses_rs2 && d_rs2 == e_dec.rd));

    // ---- ID/EX ----
    logic [31:0] e_pc, e_insn, e_rs1v, e_rs2v, e_pcimm;
    logic [31:2] e_jr_p0, e_jr_p1;
    fwd_e        e_fwd_a, e_fwd_b;
    logic        e_tgt_match, e_pred_taken;
    logic [1:0]  e_pred_cnt;

    always_ff @(posedge clk) begin
        if (rst) begin
            e_valid <= 1'b0;
        end else if (advance) begin
            e_valid <= d_valid && !redirect && !load_use;
        end
        if (advance) begin
            e_pc         <= d_pc;
            e_insn       <= d_insn;
            e_dec        <= dec;
            e_rs1v       <= rf_rdata1;
            e_rs2v       <= rf_rdata2;
            e_fwd_a      <= id_fwd_a;
            e_fwd_b      <= id_fwd_b;
            e_pcimm      <= id_pcimm;
            e_tgt_match  <= id_tgt_match;
            e_pred_taken <= d_pred_taken;
            e_jr_p0      <= id_jr_p0;
            e_jr_p1      <= id_jr_p1;
            e_pred_cnt   <= d_pred_cnt;
        end
    end

    // =====================================================================
    // EX
    // =====================================================================
    logic [31:0] m_result;
    logic [31:0] fwd_a, fwd_b, alu_b, alu_y, agu, e_pc4;

    always_comb begin
        unique case (e_fwd_a)
            FWD_MEM: fwd_a = m_result;
            FWD_WB:  fwd_a = w_result;
            default: fwd_a = e_rs1v;
        endcase
        unique case (e_fwd_b)
            FWD_MEM: fwd_b = m_result;
            FWD_WB:  fwd_b = w_result;
            default: fwd_b = e_rs2v;
        endcase
    end

    assign alu_b = e_dec.alu_b_imm ? e_dec.imm : fwd_b;
    rv32_alu u_alu (.op(e_dec.alu_op), .a(fwd_a), .b(alu_b), .y(alu_y));

    assign agu   = fwd_a + e_dec.imm;      // load/store address, jalr target
    assign e_pc4 = e_pc + 32'd4;

    // ---- branch / jump resolution ----
    logic        br_cond, taken, tgt_match, mispredict, jr_carry, jr_match;
    logic [31:0] tgt;

    always_comb begin
        unique case (e_dec.funct3)
            3'b000:  br_cond = (fwd_a == fwd_b);
            3'b001:  br_cond = (fwd_a != fwd_b);
            3'b100:  br_cond = ($signed(fwd_a) <  $signed(fwd_b));
            3'b101:  br_cond = ($signed(fwd_a) >= $signed(fwd_b));
            3'b110:  br_cond = (fwd_a <  fwd_b);
            3'b111:  br_cond = (fwd_a >= fwd_b);
            default: br_cond = 1'b0;
        endcase
    end

    assign taken      = (e_dec.is_branch && br_cond) || e_dec.is_jal || e_dec.is_jalr;
    assign tgt        = e_dec.is_jalr ? {agu[31:1], 1'b0} : e_pcimm;
    assign jr_carry   = ({1'b0, fwd_a[1:0]} + {1'b0, e_dec.imm[1:0]}) >= 3'd4;
    assign jr_match   = (fwd_a[31:2] == (jr_carry ? e_jr_p1 : e_jr_p0));
    assign tgt_match  = e_dec.is_jalr ? jr_match : e_tgt_match;
    assign mispredict = (taken != e_pred_taken) || (taken && !tgt_match);

    // ---- exceptions ----
    logic        mis_tgt, mis_mem, exc;
    logic [3:0]  exc_cause;
    logic [31:0] exc_tval;
    logic [1:0]  msize;

    assign msize   = e_dec.funct3[1:0];
    assign mis_tgt = taken && tgt[1];
    assign mis_mem = (e_dec.is_load || e_dec.is_store) &&
                     (((msize == 2'b01) && agu[0]) || ((msize == 2'b10) && (agu[1:0] != 2'b00)));
    assign exc     = e_dec.illegal || e_dec.ecall || e_dec.ebreak || mis_tgt || mis_mem;

    always_comb begin
        if (e_dec.illegal)      begin exc_cause = EXC_ILLEGAL;     exc_tval = e_insn; end
        else if (e_dec.ebreak)  begin exc_cause = EXC_BREAKPOINT;  exc_tval = e_pc;   end
        else if (e_dec.ecall)   begin exc_cause = EXC_ECALL_M;     exc_tval = 32'b0;  end
        else if (mis_tgt)       begin exc_cause = EXC_INSN_MISALIGNED; exc_tval = tgt; end
        else if (e_dec.is_load) begin exc_cause = EXC_LOAD_MISALIGNED; exc_tval = agu; end
        else                    begin exc_cause = EXC_STORE_MISALIGNED; exc_tval = agu; end
    end

    // ---- CSR file ----
    logic [31:0] csr_rdata, csr_src, csr_wdata, mtvec, mepc;
    logic        commit;
    assign commit  = advance && e_valid;
    assign csr_src = e_dec.csr_imm ? e_dec.imm : fwd_a;
    always_comb begin
        unique case (e_dec.funct3[1:0])
            2'b01:   csr_wdata = csr_src;
            2'b10:   csr_wdata = csr_rdata | csr_src;
            default: csr_wdata = csr_rdata & ~csr_src;
        endcase
    end

    rv32_csr u_csr (
        .clk        (clk),
        .rst        (rst),
        .raddr      (e_insn[31:20]),
        .rdata      (csr_rdata),
        .commit     (commit),
        .wen        (e_dec.is_csr && e_dec.csr_write && !exc),
        .waddr      (e_insn[31:20]),
        .wdata      (csr_wdata),
        .trap       (exc),
        .trap_cause (exc_cause),
        .trap_pc    (e_pc[31:2]),
        .trap_tval  (exc_tval),
        .mret       (e_dec.mret && !exc),
        .mtvec      (mtvec),
        .mepc       (mepc)
    );

    // ---- next PC in program order and redirect ----
    always_comb begin
        if (exc)              ex_npc = mtvec;
        else if (e_dec.mret)  ex_npc = mepc;
        else if (taken)       ex_npc = tgt;
        else                  ex_npc = e_pc4;
    end
    assign redirect = e_valid && (exc || e_dec.mret || mispredict);

    // ---- mul / div ----
    logic        md_done;
    logic [31:0] md_result;
    rv32_muldiv u_muldiv (
        .clk    (clk),
        .rst    (rst),
        .req    (e_valid && e_dec.is_md),
        .op     (e_dec.funct3),
        .a      (fwd_a),
        .b      (fwd_b),
        .ack    (advance),
        .done   (md_done),
        .result (md_result)
    );
    assign ex_busy = e_valid && e_dec.is_md && !md_done;

    // ---- EX result ----
    logic [31:0] ex_result;
    always_comb begin
        unique case (e_dec.res_sel)
            RES_PCIMM: ex_result = e_pcimm;
            RES_PC4:   ex_result = e_pc4;
            RES_CSR:   ex_result = csr_rdata;
            RES_MD:    ex_result = md_result;
            default:   ex_result = alu_y;
        endcase
    end

    // ---- D-port request ----
    logic [31:0] st_rep, st_wdata;
    logic [3:0]  st_strb;
    always_comb begin
        unique case (msize)
            2'b00:   begin st_rep = {4{fwd_b[7:0]}};  st_strb = 4'b0001 << agu[1:0]; end
            2'b01:   begin st_rep = {2{fwd_b[15:0]}}; st_strb = agu[1] ? 4'b1100 : 4'b0011; end
            default: begin st_rep = fwd_b;            st_strb = 4'b1111; end
        endcase
        for (int i = 0; i < 4; i++) st_wdata[8*i +: 8] = st_strb[i] ? st_rep[8*i +: 8] : 8'h00;
    end

    assign dmem_req_valid = advance && e_valid && (e_dec.is_load || e_dec.is_store) && !mis_mem;
    assign dmem_req_we    = e_dec.is_store;
    assign dmem_req_addr  = agu;
    assign dmem_req_wstrb = e_dec.is_store ? st_strb : 4'b0000;
    assign dmem_req_wdata = st_wdata;

    // ---- branch predictor ----
    rv32_bpred #(
        .BTB_ENTRIES (BTB_ENTRIES),
        .BHT_ENTRIES (BHT_ENTRIES),
        .RAS_DEPTH   (RAS_DEPTH)
    ) u_bpred (
        .clk           (clk),
        .rst           (rst),
        .lu_en         (fetch_go),
        .lu_pc         (npc),
        .f_pc          (f_pc),
        .pred_taken    (bp_taken),
        .pred_target   (bp_target),
        .pred_cnt      (bp_cnt),
        .f_adv         (fetch_go && f_valid && !redirect),
        .up_en         (commit),
        .up_pc         (e_pc),
        .up_branch     (e_dec.is_branch),
        .up_jump       (e_dec.is_jal || e_dec.is_jalr),
        .up_call       (e_dec.is_call),
        .up_ret        (e_dec.is_ret),
        .up_trap       (exc),
        .up_taken      (taken),
        .up_target     (tgt),
        .up_cnt        (e_pred_cnt),
        .up_pred_taken (e_pred_taken),
        .up_redirect   (redirect)
    );

    // ---- EX/MEM ----
    logic        m_trap, m_load;
    logic [2:0]  m_f3;
    logic [1:0]  m_alo;
    logic [31:0] m_pc, m_insn, m_npc, m_maddr, m_wdata;
    logic [3:0]  m_wstrb;

    always_ff @(posedge clk) begin
        if (rst) begin
            m_valid <= 1'b0;
        end else if (advance) begin
            m_valid <= e_valid;
        end
        if (advance) begin
            m_trap   <= exc;
            m_rd     <= exc ? 5'd0 : e_dec.rd;
            m_result <= ex_result;
            m_load   <= e_dec.is_load && !exc;
            m_f3     <= e_dec.funct3;
            m_alo    <= agu[1:0];
            m_pc     <= e_pc;
            m_insn   <= e_insn;
            m_npc    <= ex_npc;
            m_maddr  <= ((e_dec.is_load || e_dec.is_store) && !exc) ? agu : 32'b0;
            m_wstrb  <= (e_dec.is_store && !exc) ? st_strb : 4'b0000;
            m_wdata  <= (e_dec.is_store && !exc) ? st_wdata : 32'b0;
        end
    end

    // =====================================================================
    // MEM
    // =====================================================================
    logic [31:0] ld_shift, ld_data, m_wbval;
    assign ld_shift = dmem_rsp_rdata >> {m_alo, 3'b000};
    always_comb begin
        unique case (m_f3)
            3'b000:  ld_data = {{24{ld_shift[7]}},  ld_shift[7:0]};
            3'b001:  ld_data = {{16{ld_shift[15]}}, ld_shift[15:0]};
            3'b100:  ld_data = {24'b0, ld_shift[7:0]};
            3'b101:  ld_data = {16'b0, ld_shift[15:0]};
            default: ld_data = ld_shift;
        endcase
    end
    assign m_wbval = m_load ? ld_data : m_result;

    // ---- MEM/WB ----
    logic        w_trap;
    logic [31:0] w_pc, w_insn, w_npc, w_maddr, w_wdata;
    logic [3:0]  w_wstrb;

    always_ff @(posedge clk) begin
        if (rst) begin
            w_valid <= 1'b0;
        end else if (advance) begin
            w_valid <= m_valid;
        end
        if (advance) begin
            w_rd     <= m_rd;
            w_result <= m_wbval;
            w_trap   <= m_trap;
            w_pc     <= m_pc;
            w_insn   <= m_insn;
            w_npc    <= m_npc;
            w_maddr  <= m_maddr;
            w_wstrb  <= m_wstrb;
            w_wdata  <= m_wdata;
        end
    end

    // =====================================================================
    // WB: register write (above) and retire trace (registered)
    // =====================================================================
    always_ff @(posedge clk) begin
        if (rst) begin
            trace_valid <= 1'b0;
        end else begin
            trace_valid <= advance && w_valid;
        end
        if (advance && w_valid) begin
            trace_trap      <= w_trap;
            trace_pc        <= w_pc;
            trace_insn      <= w_insn;
            trace_rd        <= w_rd;
            trace_rd_wdata  <= (w_rd != 5'd0) ? w_result : 32'b0;
            trace_mem_addr  <= w_maddr;
            trace_mem_wstrb <= w_wstrb;
            trace_mem_wdata <= w_wdata;
            trace_next_pc   <= w_npc;
        end
    end

    // ---- performance strobes (registered) ----
    always_ff @(posedge clk) begin
        if (rst) begin
            perf_branch      <= 1'b0;
            perf_branch_miss <= 1'b0;
            perf_jump        <= 1'b0;
            perf_jump_miss   <= 1'b0;
        end else begin
            perf_branch      <= commit && e_dec.is_branch;
            perf_branch_miss <= commit && e_dec.is_branch && mispredict && !exc;
            perf_jump        <= commit && (e_dec.is_jal || e_dec.is_jalr);
            perf_jump_miss   <= commit && (e_dec.is_jal || e_dec.is_jalr) && mispredict && !exc;
        end
    end

`ifndef SYNTHESIS
    // ---------------------------------------------------------------------
    // Simulation-only observation points for the testbench's functional
    // coverage (tests/core). Purely passive: nothing in the design reads them.
    // ---------------------------------------------------------------------
    logic [47:0] cov_events /* verilator public_flat_rd */;
    logic        cov_cf, cov_xfer;
    logic [31:0] cov_ma, cov_mb;     // divide operand magnitudes
    function automatic int cov_len(input logic [31:0] v);
        cov_len = 0;
        for (int i = 0; i < 32; i++) if (v[i]) cov_len = i + 1;
    endfunction
    assign cov_ma = !COVERAGE ? '0 : (!e_dec.funct3[0] && fwd_a[31]) ? -fwd_a : fwd_a;
    assign cov_mb = !COVERAGE ? '0 : (!e_dec.funct3[0] && fwd_b[31]) ? -fwd_b : fwd_b;
    assign cov_cf   = e_dec.is_branch || e_dec.is_jal || e_dec.is_jalr;
    assign cov_xfer = advance && d_valid && !load_use && !redirect;   // ID -> EX transfer
    always_comb begin
        cov_events     = '0;
        cov_events[0]  = commit && e_dec.uses_rs1 && e_fwd_a == FWD_MEM;
        cov_events[1]  = commit && e_dec.uses_rs1 && e_fwd_a == FWD_WB;
        cov_events[2]  = commit && e_dec.uses_rs2 && e_fwd_b == FWD_MEM;
        cov_events[3]  = commit && e_dec.uses_rs2 && e_fwd_b == FWD_WB;
        cov_events[4]  = commit && e_dec.is_store && e_fwd_b != FWD_REG;
        cov_events[5]  = commit && (e_dec.is_load || e_dec.is_store) && e_fwd_a != FWD_REG;
        cov_events[6]  = commit && e_dec.is_branch && (e_fwd_a != FWD_REG || e_fwd_b != FWD_REG);
        cov_events[7]  = commit && e_dec.is_jalr && e_fwd_a != FWD_REG;
        cov_events[8]  = commit && e_dec.is_csr && !e_dec.csr_imm && e_fwd_a != FWD_REG;
        cov_events[9]  = commit && e_dec.is_md && (e_fwd_a != FWD_REG || e_fwd_b != FWD_REG);
        cov_events[10] = advance && load_use && !redirect;
        cov_events[11] = advance && load_use && redirect;
        cov_events[12] = cov_xfer && rf_we && w_rd != 5'd0 &&
                         ((dec.uses_rs1 && w_rd == d_rs1) || (dec.uses_rs2 && w_rd == d_rs2));
        cov_events[13] = commit && e_dec.is_branch && mispredict && !exc;
        cov_events[14] = commit && (e_dec.is_jal || e_dec.is_jalr) && mispredict && !exc;
        cov_events[15] = commit && !cov_cf && e_pred_taken;
        cov_events[16] = commit && exc;
        cov_events[17] = commit && e_dec.mret && !exc;
        cov_events[18] = commit && e_dec.is_ret && !mispredict && !exc;
        cov_events[19] = commit && e_dec.is_branch && taken && !mispredict && !exc;
        cov_events[20] = e_valid && e_dec.is_md && !md_done;
        cov_events[21] = e_valid && e_dec.is_md && md_done && !(imem_rsp_ready && dmem_rsp_ready);
        cov_events[22] = !rst && !imem_rsp_ready && dmem_rsp_ready;
        cov_events[23] = !rst && imem_rsp_ready && !dmem_rsp_ready;
        cov_events[24] = !rst && !imem_rsp_ready && !dmem_rsp_ready;
        cov_events[25] = m_valid && m_load && !dmem_rsp_ready;
        cov_events[26] = dmem_req_valid && dmem_req_we;
        cov_events[27] = commit && mis_mem;
        cov_events[28] = commit && mis_tgt;
        cov_events[29] = commit && e_dec.illegal;
        cov_events[30] = commit && (e_dec.ecall || e_dec.ebreak);
        cov_events[31] = commit && e_dec.is_csr && e_dec.csr_write && !exc;
        cov_events[32] = commit && e_dec.is_csr && e_dec.csr_write && !exc &&
                         (e_insn[31:20] == CSR_MINSTRET || e_insn[31:20] == CSR_MINSTRETH);
        cov_events[33] = commit && e_dec.is_call && !exc;
        cov_events[34] = commit && e_dec.is_md && e_dec.funct3[2] && fwd_b == 32'b0;          // divide by zero
        cov_events[35] = commit && e_dec.is_md && e_dec.funct3[2] && !e_dec.funct3[0] &&
                         fwd_a == 32'h8000_0000 && fwd_b == 32'hFFFF_FFFF;                     // INT_MIN / -1
        cov_events[36] = advance && redirect && load_use;                                      // flush beats stall
        cov_events[37] = redirect && !advance;                                                 // redirect held by a stall
        cov_events[38] = load_use && !advance;                                                 // load-use during a stall
        cov_events[39] = commit && e_dec.is_load && e_fwd_a == FWD_WB && e_dec.uses_rs1;
        cov_events[40] = commit && e_dec.is_store && e_fwd_b == FWD_WB;                         // load -> store data
        cov_events[41] = commit && (e_dec.is_jal || e_dec.is_jalr) && !mispredict && !exc;    // jump predicted
        cov_events[42] = commit && e_dec.is_branch && !taken && !exc;
        cov_events[43] = commit && e_dec.is_md && e_dec.funct3[2] && cov_mb != 0 &&            // divide with 8..24
                         cov_len(cov_ma) >= cov_len(cov_mb) + 7 && cov_len(cov_ma) <= cov_len(cov_mb) + 23;  // quotient bits
        cov_events[47] = commit;           // not a coverage point: lets the testbench check mcycle reads exactly
        if (!COVERAGE) cov_events = '0;    // soc_top: nobody looks, so the simulator need not compute them
    end
`endif

endmodule

`default_nettype wire
