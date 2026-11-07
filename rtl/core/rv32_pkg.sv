// rv32_pkg - shared types and constants of the rv32_core pipeline.
`default_nettype none

package rv32_pkg;

    // ---- major opcodes (insn[6:0]) ----------------------------------------
    localparam logic [6:0] OPC_LUI    = 7'b0110111;
    localparam logic [6:0] OPC_AUIPC  = 7'b0010111;
    localparam logic [6:0] OPC_JAL    = 7'b1101111;
    localparam logic [6:0] OPC_JALR   = 7'b1100111;
    localparam logic [6:0] OPC_BRANCH = 7'b1100011;
    localparam logic [6:0] OPC_LOAD   = 7'b0000011;
    localparam logic [6:0] OPC_STORE  = 7'b0100011;
    localparam logic [6:0] OPC_OPIMM  = 7'b0010011;
    localparam logic [6:0] OPC_OP     = 7'b0110011;
    localparam logic [6:0] OPC_FENCE  = 7'b0001111;
    localparam logic [6:0] OPC_SYSTEM = 7'b1110011;

    // ---- ALU operations ----------------------------------------------------
    typedef enum logic [3:0] {
        ALU_ADD, ALU_SUB, ALU_SLL, ALU_SLT, ALU_SLTU,
        ALU_XOR, ALU_SRL, ALU_SRA, ALU_OR, ALU_AND
    } alu_op_e;

    // ---- source of the EX-stage result -------------------------------------
    typedef enum logic [2:0] {
        RES_ALU,    // ALU output
        RES_PCIMM,  // value precomputed in ID: imm (lui) or pc+imm (auipc)
        RES_PC4,    // link address pc+4 (jal/jalr)
        RES_CSR,    // CSR read value
        RES_MD      // mul/div unit
    } res_sel_e;

    // ---- forwarding source of an EX operand (selected in ID, registered) ---
    typedef enum logic [1:0] {
        FWD_REG,    // value read from the register file in ID
        FWD_MEM,    // result of the instruction now in MEM
        FWD_WB      // result of the instruction now in WB
    } fwd_e;

    // ---- BTB entry kinds ---------------------------------------------------
    typedef enum logic [1:0] {
        BK_BRANCH,  // conditional branch: direction from the BHT counter
        BK_JUMP,    // jal / jalr that is neither a call nor a return
        BK_CALL,    // jal / jalr with rd = x1 or x5: push the return address
        BK_RET      // jalr x0, 0(x1|x5): target from the return-address stack
    } btb_kind_e;

    // ---- exception causes (mcause) -------------------------------------------
    localparam logic [3:0] EXC_INSN_MISALIGNED  = 4'd0;
    localparam logic [3:0] EXC_ILLEGAL          = 4'd2;
    localparam logic [3:0] EXC_BREAKPOINT       = 4'd3;
    localparam logic [3:0] EXC_LOAD_MISALIGNED  = 4'd4;
    localparam logic [3:0] EXC_STORE_MISALIGNED = 4'd6;
    localparam logic [3:0] EXC_ECALL_M          = 4'd11;

    // ---- CSR addresses -----------------------------------------------------
    localparam logic [11:0] CSR_MSTATUS   = 12'h300;
    localparam logic [11:0] CSR_MISA      = 12'h301;
    localparam logic [11:0] CSR_MIE       = 12'h304;
    localparam logic [11:0] CSR_MTVEC     = 12'h305;
    localparam logic [11:0] CSR_MSCRATCH  = 12'h340;
    localparam logic [11:0] CSR_MEPC      = 12'h341;
    localparam logic [11:0] CSR_MCAUSE    = 12'h342;
    localparam logic [11:0] CSR_MTVAL     = 12'h343;
    localparam logic [11:0] CSR_MIP       = 12'h344;
    localparam logic [11:0] CSR_MCYCLE    = 12'hB00;
    localparam logic [11:0] CSR_MINSTRET  = 12'hB02;
    localparam logic [11:0] CSR_MCYCLEH   = 12'hB80;
    localparam logic [11:0] CSR_MINSTRETH = 12'hB82;
    localparam logic [11:0] CSR_CYCLE     = 12'hC00;
    localparam logic [11:0] CSR_INSTRET   = 12'hC02;
    localparam logic [11:0] CSR_CYCLEH    = 12'hC80;
    localparam logic [11:0] CSR_INSTRETH  = 12'hC82;
    localparam logic [11:0] CSR_MVENDORID = 12'hF11;
    localparam logic [11:0] CSR_MARCHID   = 12'hF12;
    localparam logic [11:0] CSR_MIMPID    = 12'hF13;
    localparam logic [11:0] CSR_MHARTID   = 12'hF14;

    // misa: MXL=1 (32 bit), extensions I and M
    localparam logic [31:0] MISA_VALUE = 32'h4000_1100;

    // Is this CSR address implemented?
    function automatic logic csr_exists(input logic [11:0] a);
        case (a)
            CSR_MSTATUS, CSR_MISA, CSR_MIE, CSR_MTVEC, CSR_MSCRATCH, CSR_MEPC,
            CSR_MCAUSE, CSR_MTVAL, CSR_MIP, CSR_MCYCLE, CSR_MINSTRET,
            CSR_MCYCLEH, CSR_MINSTRETH, CSR_CYCLE, CSR_INSTRET, CSR_CYCLEH,
            CSR_INSTRETH, CSR_MVENDORID, CSR_MARCHID, CSR_MIMPID,
            CSR_MHARTID: csr_exists = 1'b1;
            default:     csr_exists = 1'b0;
        endcase
    endfunction

    // ---- decoded instruction (ID -> EX) ------------------------------------
    typedef struct packed {
        logic        illegal;    // raise illegal-instruction exception in EX
        logic        ecall;
        logic        ebreak;
        logic        mret;
        logic [4:0]  rs1;
        logic [4:0]  rs2;
        logic [4:0]  rd;         // destination, 0 when the instruction writes no register
        logic        uses_rs1;
        logic        uses_rs2;
        logic [31:0] imm;        // immediate (uimm for CSR*I forms)
        alu_op_e     alu_op;
        logic        alu_b_imm;  // ALU operand B = imm (else rs2)
        res_sel_e    res_sel;
        logic        is_branch;
        logic        is_jal;
        logic        is_jalr;
        logic        is_call;    // jal/jalr with rd in {x1, x5}
        logic        is_ret;     // jalr with rd = x0 and rs1 in {x1, x5}
        logic        is_load;
        logic        is_store;
        logic [2:0]  funct3;     // branch condition / load-store size / md op / csr op
        logic        is_md;
        logic        is_csr;
        logic        csr_write;  // the CSR instruction writes its CSR
        logic        csr_imm;    // CSR source operand is the uimm
    } decoded_t;

endpackage

`default_nettype wire
