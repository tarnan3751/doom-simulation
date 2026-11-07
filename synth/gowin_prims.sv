// Blackbox declarations of the Gowin primitives used by rtl/fpga, for the
// yosys-slang frontend only (synthesis flow file, not design RTL).
//
// yosys-slang needs every instantiated module declared, with its parameters,
// before elaboration; the Yosys cell library (+/gowin/cells_sim.v) cannot be
// used for that (read_verilog -lib drops the parameters and slang rejects the
// overrides). Port names, directions and parameter names below are copied from
// .tools/oss-cad-suite/share/yosys/gowin/cells_sim.v and cells_xtra_gw2a.v, so
// the cells in the netlist are exactly the ones nextpnr-himbaechel and
// gowin_pack expect. Simulation never reads this file (only `make -C synth lint`).
`default_nettype none
// Empty bodies: nothing is driven or used here (Verilator lint of the board top).
/* verilator lint_off UNDRIVEN */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off DECLFILENAME */

(* blackbox *)
module rPLL #(
    parameter FCLKIN           = "100.0",
    parameter DYN_IDIV_SEL     = "false",
    parameter integer IDIV_SEL = 0,
    parameter DYN_FBDIV_SEL    = "false",
    parameter integer FBDIV_SEL = 0,
    parameter DYN_ODIV_SEL     = "false",
    parameter integer ODIV_SEL = 8,
    parameter PSDA_SEL         = "0000",
    parameter DYN_DA_EN        = "false",
    parameter DUTYDA_SEL       = "1000",
    parameter CLKOUT_FT_DIR    = 1'b1,
    parameter CLKOUTP_FT_DIR   = 1'b1,
    parameter integer CLKOUT_DLY_STEP  = 0,
    parameter integer CLKOUTP_DLY_STEP = 0,
    parameter CLKFB_SEL        = "internal",
    parameter CLKOUT_BYPASS    = "false",
    parameter CLKOUTP_BYPASS   = "false",
    parameter CLKOUTD_BYPASS   = "false",
    parameter integer DYN_SDIV_SEL = 2,
    parameter CLKOUTD_SRC      = "CLKOUT",
    parameter CLKOUTD3_SRC     = "CLKOUT",
    parameter DEVICE           = "GW1N-1"
) (
    output wire       CLKOUT,
    output wire       CLKOUTP,
    output wire       CLKOUTD,
    output wire       CLKOUTD3,
    output wire       LOCK,
    input  wire       CLKIN,
    input  wire       CLKFB,
    input  wire [5:0] FBDSEL,
    input  wire [5:0] IDSEL,
    input  wire [5:0] ODSEL,
    input  wire [3:0] DUTYDA,
    input  wire [3:0] PSDA,
    input  wire [3:0] FDLY,
    input  wire       RESET,
    input  wire       RESET_P
);
endmodule

(* blackbox *)
module CLKDIV #(
    parameter DIV_MODE = "2",
    parameter GSREN    = "false"
) (
    input  wire HCLKIN,
    input  wire RESETN,
    input  wire CALIB,
    output wire CLKOUT
);
endmodule

(* blackbox *)
module OSER10 #(
    parameter GSREN = "false",
    parameter LSREN = "true"
) (
    output wire Q,
    input  wire D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    input  wire FCLK,
    input  wire PCLK,
    input  wire RESET
);
endmodule

(* blackbox *)
module TLVDS_OBUF (
    input  wire I,
    output wire O,
    output wire OB
);
endmodule

(* blackbox *)
module ODDR #(
    parameter TXCLK_POL = 0,
    parameter INIT      = 0
) (
    input  wire D0,
    input  wire D1,
    input  wire TX,
    input  wire CLK,
    output wire Q0,
    output wire Q1
);
endmodule

(* blackbox *)
module IOBUF (
    output wire O,
    inout  wire IO,
    input  wire I,
    input  wire OEN
);
endmodule

/* verilator lint_on DECLFILENAME */
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNDRIVEN */
`default_nettype wire
