// rv32_csr - machine-mode CSRs, trap entry/return state and counters.
//
// Read and written by the instruction in EX; CSR writes, trap entry, mret and the
// minstret increment take effect at the edge where it leaves EX (`commit`, the
// commit point: rtl/core/README.md). A counter write replaces that increment.
`default_nettype none

module rv32_csr
    import rv32_pkg::*;
(
    input  logic        clk,
    input  logic        rst,

    // read port (combinational)
    input  logic [11:0] raddr,
    output logic [31:0] rdata,

    // commit of the instruction in EX
    input  logic        commit,      // a valid instruction leaves EX this cycle
    input  logic        wen,         // ... and it is a CSR instruction that writes (and does not trap)
    input  logic [11:0] waddr,
    input  logic [31:0] wdata,
    input  logic        trap,        // ... and it raised an exception
    input  logic [3:0]  trap_cause,
    input  logic [31:2] trap_pc,
    input  logic [31:0] trap_tval,
    input  logic        mret,        // ... and it is an mret (no exception)

    output logic [31:0] mtvec,
    output logic [31:0] mepc
);
    logic        mstatus_mie, mstatus_mpie;
    logic [31:0] mie_q, mscratch_q, mcause_q, mtval_q;
    logic [29:0] mtvec_q, mepc_q;
    logic [63:0] mcycle_q, minstret_q;

    assign mtvec = {mtvec_q, 2'b00};
    assign mepc  = {mepc_q, 2'b00};

    always_comb begin
        unique case (raddr)
            CSR_MSTATUS:  rdata = {19'b0, 2'b11, 3'b0, mstatus_mpie, 3'b0, mstatus_mie, 3'b0};
            CSR_MISA:     rdata = MISA_VALUE;
            CSR_MIE:      rdata = mie_q;
            CSR_MTVEC:    rdata = mtvec;
            CSR_MSCRATCH: rdata = mscratch_q;
            CSR_MEPC:     rdata = mepc;
            CSR_MCAUSE:   rdata = mcause_q;
            CSR_MTVAL:    rdata = mtval_q;
            CSR_MCYCLE,   CSR_CYCLE:    rdata = mcycle_q[31:0];
            CSR_MCYCLEH,  CSR_CYCLEH:   rdata = mcycle_q[63:32];
            CSR_MINSTRET, CSR_INSTRET:  rdata = minstret_q[31:0];
            CSR_MINSTRETH,CSR_INSTRETH: rdata = minstret_q[63:32];
            default:      rdata = 32'b0;   // mip, mhartid, mvendorid, marchid, mimpid
        endcase
    end

    logic w;
    assign w = commit && wen;

    always_ff @(posedge clk) begin
        if (rst) begin
            mstatus_mie  <= 1'b0;
            mstatus_mpie <= 1'b0;
            mie_q        <= 32'b0;
            mtvec_q      <= 30'b0;
            mscratch_q   <= 32'b0;
            mepc_q       <= 30'b0;
            mcause_q     <= 32'b0;
            mtval_q      <= 32'b0;
            mcycle_q     <= 64'b0;
            minstret_q   <= 64'b0;
        end else begin
            // ---- cycle counter: every clock, unless written ----
            if (w && waddr == CSR_MCYCLE)       mcycle_q <= {mcycle_q[63:32], wdata};
            else if (w && waddr == CSR_MCYCLEH) mcycle_q <= {wdata, mcycle_q[31:0]};
            else                                mcycle_q <= mcycle_q + 64'd1;

            // ---- retired-instruction counter ----
            if (w && waddr == CSR_MINSTRET)       minstret_q <= {minstret_q[63:32], wdata};
            else if (w && waddr == CSR_MINSTRETH) minstret_q <= {wdata, minstret_q[31:0]};
            else if (commit && !trap)             minstret_q <= minstret_q + 64'd1;

            if (commit && trap) begin
                mepc_q       <= trap_pc;
                mcause_q     <= {28'b0, trap_cause};
                mtval_q      <= trap_tval;
                mstatus_mpie <= mstatus_mie;
                mstatus_mie  <= 1'b0;
            end else if (commit && mret) begin
                mstatus_mie  <= mstatus_mpie;
                mstatus_mpie <= 1'b1;
            end else if (w) begin
                unique case (waddr)
                    CSR_MSTATUS: begin
                        mstatus_mie  <= wdata[3];
                        mstatus_mpie <= wdata[7];
                    end
                    CSR_MIE:      mie_q      <= wdata;
                    CSR_MTVEC:    mtvec_q    <= wdata[31:2];
                    CSR_MSCRATCH: mscratch_q <= wdata;
                    CSR_MEPC:     mepc_q     <= wdata[31:2];
                    CSR_MCAUSE:   mcause_q   <= wdata;
                    CSR_MTVAL:    mtval_q    <= wdata;
                    default: ;    // misa, mip: writes ignored; counters handled above
                endcase
            end
        end
    end

endmodule

`default_nettype wire
