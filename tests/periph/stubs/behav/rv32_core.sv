`default_nettype none
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
// Behavioural stub of rv32_core for tb_soc: a scripted bus master. The C++ testbench
// writes the cmd_* variables; the stub obeys the master side of the port contract
// (request only in a cycle in which rsp_ready == 1).
module rv32_core #(
    parameter logic [31:0] RESET_PC    = 32'h8000_0000,
    parameter bit          COVERAGE    = 1'b1
) (
    input  logic        clk,
    input  logic        rst,
    output logic        imem_req_valid,
    output logic [31:0] imem_req_addr,
    input  logic        imem_rsp_ready,
    input  logic [31:0] imem_rsp_rdata,
    output logic        dmem_req_valid,
    output logic        dmem_req_we,
    output logic [31:0] dmem_req_addr,
    output logic [3:0]  dmem_req_wstrb,
    output logic [31:0] dmem_req_wdata,
    input  logic        dmem_rsp_ready,
    input  logic [31:0] dmem_rsp_rdata,
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
    output logic        perf_branch,
    output logic        perf_branch_miss,
    output logic        perf_jump,
    output logic        perf_jump_miss
);
    logic        cmd_pending /* verilator public_flat_rw */;
    logic        cmd_we      /* verilator public_flat_rw */;
    logic [31:0] cmd_addr    /* verilator public_flat_rw */;
    logic [3:0]  cmd_wstrb   /* verilator public_flat_rw */;
    logic [31:0] cmd_wdata   /* verilator public_flat_rw */;
    logic        cmd_ifetch  /* verilator public_flat_rw */;
    logic        cmd_trace_valid /* verilator public_flat_rw */;
    logic        cmd_trace_trap  /* verilator public_flat_rw */;
    logic        cmd_branch      /* verilator public_flat_rw */;
    logic        cmd_branch_miss /* verilator public_flat_rw */;
    logic        cmd_jump        /* verilator public_flat_rw */;
    logic        cmd_jump_miss   /* verilator public_flat_rw */;
    // what the core sees on its data port
    logic        mon_valid  /* verilator public_flat_rd */;
    logic        mon_ready  /* verilator public_flat_rd */;
    logic [31:0] mon_rdata  /* verilator public_flat_rd */;
    assign mon_valid = dmem_req_valid;
    assign mon_ready = dmem_rsp_ready;
    assign mon_rdata = dmem_rsp_rdata;

    assign dmem_req_valid = cmd_pending & dmem_rsp_ready;
    assign dmem_req_we    = dmem_req_valid ? cmd_we : 1'b0;
    assign dmem_req_addr  = cmd_addr;
    assign dmem_req_wstrb = (dmem_req_valid && cmd_we) ? cmd_wstrb : 4'b0;
    assign dmem_req_wdata = cmd_wdata;
    assign imem_req_valid = cmd_ifetch & imem_rsp_ready;
    assign imem_req_addr  = RESET_PC;

    always_ff @(posedge clk) begin
        if (rst) cmd_pending <= 1'b0;
        else if (dmem_req_valid) cmd_pending <= 1'b0;
    end

    assign trace_valid = cmd_trace_valid;
    assign trace_trap = cmd_trace_trap;
    assign trace_pc = 32'b0; assign trace_insn = 32'b0; assign trace_rd = 5'b0;
    assign trace_rd_wdata = 32'b0; assign trace_mem_addr = 32'b0; assign trace_mem_wstrb = 4'b0;
    assign trace_mem_wdata = 32'b0; assign trace_next_pc = 32'b0;
    assign perf_branch = cmd_branch; assign perf_branch_miss = cmd_branch_miss;
    assign perf_jump = cmd_jump; assign perf_jump_miss = cmd_jump_miss;
endmodule
`default_nettype wire
