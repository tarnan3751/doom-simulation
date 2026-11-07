// rv32_decode - combinational RV32IM + Zicsr decoder (ID stage).
//
// Strict decoding: every encoding that is not an implemented instruction
// (including 16-bit encodings, reserved funct fields, slli/srli/srai with
// shamt[5] set, unknown CSRs and writes to read-only CSRs) sets `illegal`.
// When `illegal` is set no other side-effect flag is set and rd = 0.
`default_nettype none

module rv32_decode
    import rv32_pkg::*;
(
    input  logic [31:0] insn,
    output decoded_t    d
);
    logic [6:0]  opcode;
    logic [4:0]  rd, rs1, rs2;
    logic [2:0]  f3;
    logic [6:0]  f7;
    logic [31:0] imm_i, imm_s, imm_b, imm_u, imm_j;

    assign opcode = insn[6:0];
    assign rd     = insn[11:7];
    assign f3     = insn[14:12];
    assign rs1    = insn[19:15];
    assign rs2    = insn[24:20];
    assign f7     = insn[31:25];

    assign imm_i = {{20{insn[31]}}, insn[31:20]};
    assign imm_s = {{20{insn[31]}}, insn[31:25], insn[11:7]};
    assign imm_b = {{19{insn[31]}}, insn[31], insn[7], insn[30:25], insn[11:8], 1'b0};
    assign imm_u = {insn[31:12], 12'b0};
    assign imm_j = {{11{insn[31]}}, insn[31], insn[19:12], insn[20], insn[30:21], 1'b0};

    function automatic logic is_link(input logic [4:0] r);
        is_link = (r == 5'd1) || (r == 5'd5);
    endfunction

    logic legal;
    logic writes_rd;

    always_comb begin
        d          = '0;
        d.alu_op   = ALU_ADD;
        d.res_sel  = RES_ALU;
        d.rs1      = rs1;
        d.rs2      = rs2;
        d.funct3   = f3;
        legal      = 1'b0;
        writes_rd  = 1'b0;

        unique case (opcode)
            OPC_LUI: begin
                legal = 1'b1; writes_rd = 1'b1;
                d.imm = imm_u; d.res_sel = RES_PCIMM;
            end
            OPC_AUIPC: begin
                legal = 1'b1; writes_rd = 1'b1;
                d.imm = imm_u; d.res_sel = RES_PCIMM;
            end
            OPC_JAL: begin
                legal = 1'b1; writes_rd = 1'b1;
                d.imm = imm_j; d.res_sel = RES_PC4; d.is_jal = 1'b1;
                d.is_call = is_link(rd);
            end
            OPC_JALR: begin
                legal = (f3 == 3'b000); writes_rd = 1'b1;
                d.imm = imm_i; d.res_sel = RES_PC4; d.is_jalr = 1'b1;
                d.uses_rs1 = 1'b1;
                d.is_call = is_link(rd);
                d.is_ret  = (rd == 5'd0) && is_link(rs1);
            end
            OPC_BRANCH: begin
                legal = (f3 != 3'b010) && (f3 != 3'b011);
                d.imm = imm_b; d.is_branch = 1'b1;
                d.uses_rs1 = 1'b1; d.uses_rs2 = 1'b1;
            end
            OPC_LOAD: begin
                legal = (f3 == 3'b000) || (f3 == 3'b001) || (f3 == 3'b010) ||
                        (f3 == 3'b100) || (f3 == 3'b101);
                writes_rd = 1'b1;
                d.imm = imm_i; d.is_load = 1'b1; d.uses_rs1 = 1'b1;
            end
            OPC_STORE: begin
                legal = (f3 == 3'b000) || (f3 == 3'b001) || (f3 == 3'b010);
                d.imm = imm_s; d.is_store = 1'b1;
                d.uses_rs1 = 1'b1; d.uses_rs2 = 1'b1;
            end
            OPC_OPIMM: begin
                writes_rd = 1'b1;
                d.imm = imm_i; d.alu_b_imm = 1'b1; d.uses_rs1 = 1'b1;
                legal = 1'b1;
                unique case (f3)
                    3'b000: d.alu_op = ALU_ADD;
                    3'b010: d.alu_op = ALU_SLT;
                    3'b011: d.alu_op = ALU_SLTU;
                    3'b100: d.alu_op = ALU_XOR;
                    3'b110: d.alu_op = ALU_OR;
                    3'b111: d.alu_op = ALU_AND;
                    3'b001: begin
                        d.alu_op = ALU_SLL;
                        legal    = (f7 == 7'b0000000);
                    end
                    3'b101: begin
                        d.alu_op = f7[5] ? ALU_SRA : ALU_SRL;
                        legal    = (f7 == 7'b0000000) || (f7 == 7'b0100000);
                    end
                    default: ;
                endcase
            end
            OPC_OP: begin
                writes_rd = 1'b1;
                d.uses_rs1 = 1'b1; d.uses_rs2 = 1'b1;
                if (f7 == 7'b0000001) begin
                    legal = 1'b1; d.is_md = 1'b1; d.res_sel = RES_MD;
                end else begin
                    legal = (f7 == 7'b0000000) ||
                            ((f7 == 7'b0100000) && ((f3 == 3'b000) || (f3 == 3'b101)));
                    unique case (f3)
                        3'b000: d.alu_op = f7[5] ? ALU_SUB : ALU_ADD;
                        3'b001: d.alu_op = ALU_SLL;
                        3'b010: d.alu_op = ALU_SLT;
                        3'b011: d.alu_op = ALU_SLTU;
                        3'b100: d.alu_op = ALU_XOR;
                        3'b101: d.alu_op = f7[5] ? ALU_SRA : ALU_SRL;
                        3'b110: d.alu_op = ALU_OR;
                        3'b111: d.alu_op = ALU_AND;
                        default: ;
                    endcase
                end
            end
            OPC_FENCE: begin
                // fence / fence.i execute as no-ops
                legal = (f3 == 3'b000) || (f3 == 3'b001);
            end
            OPC_SYSTEM: begin
                if (f3 == 3'b000) begin
                    unique case (insn)
                        32'h0000_0073: begin legal = 1'b1; d.ecall  = 1'b1; end
                        32'h0010_0073: begin legal = 1'b1; d.ebreak = 1'b1; end
                        32'h3020_0073: begin legal = 1'b1; d.mret   = 1'b1; end
                        32'h1050_0073: begin legal = 1'b1; end            // wfi = nop
                        default: ;
                    endcase
                end else if (f3 != 3'b100) begin
                    // CSRRW/S/C (f3[2]=0) and CSRRWI/SI/CI (f3[2]=1)
                    d.is_csr    = 1'b1;
                    d.res_sel   = RES_CSR;
                    d.csr_imm   = f3[2];
                    d.uses_rs1  = !f3[2];
                    d.imm       = {27'b0, rs1};           // uimm for the immediate forms
                    d.csr_write = (f3[1:0] == 2'b01) || (rs1 != 5'd0);
                    writes_rd   = 1'b1;
                    legal       = csr_exists(insn[31:20]) &&
                                  !(d.csr_write && (insn[31:30] == 2'b11));
                end
            end
            default: ;
        endcase

        if (insn[1:0] != 2'b11) legal = 1'b0;   // 16-bit (compressed) encodings

        d.rd = (writes_rd && legal) ? rd : 5'd0;

        // An illegal instruction keeps its (meaningless) immediate and register
        // fields - they feed only datapath logic whose result is discarded - so
        // that the immediate does not depend on the legality check (timing).
        // Every flag with a side effect is cleared.
        if (!legal) begin
            d.illegal   = 1'b1;
            d.ecall     = 1'b0;
            d.ebreak    = 1'b0;
            d.mret      = 1'b0;
            d.uses_rs1  = 1'b0;
            d.uses_rs2  = 1'b0;
            d.res_sel   = RES_ALU;
            d.is_branch = 1'b0;
            d.is_jal    = 1'b0;
            d.is_jalr   = 1'b0;
            d.is_call   = 1'b0;
            d.is_ret    = 1'b0;
            d.is_load   = 1'b0;
            d.is_store  = 1'b0;
            d.is_md     = 1'b0;
            d.is_csr    = 1'b0;
            d.csr_write = 1'b0;
        end
    end

endmodule

`default_nettype wire
