`default_nettype none
// Reset generator for one clock domain of the Tang Nano 20K top. NEVER TESTED ON HARDWARE.
//
// rst (synchronous to clk, active high) is asserted
//   * from FPGA configuration (power-up value 1),
//   * while `arst` is high (PLLs not locked; asynchronous assert, because the clock
//     may not be running then), and
//   * for 2**HOLD_BITS clk cycles after the last cycle in which `arst` or the
//     synchronised button request `req` was seen. This also debounces a push
//     button (any bounce restarts the hold time) and gives every consumer a long
//     reset pulse.
// Release is synchronous: rst falls at a clk edge, so it can drive the SoC's
// synchronous reset (ARCHITECTURE.md section 1) directly.
module tn20k_reset #(
    parameter int unsigned HOLD_BITS = 16      // 2**16 cycles = 1.3 ms at 50.4 MHz
) (
    input  wire  clk,
    input  wire  arst,      // asynchronous, active high (e.g. !pll_locked)
    input  wire  req,       // asynchronous, active high (e.g. a reset button)
    output logic rst
);
    // The declaration initialisers are the FPGA power-up (configuration) values:
    // the reset is active from the very first clock edge, before arst or the
    // counter have had any effect. Intentional, hence the narrow waiver.
    /* verilator lint_off PROCASSINIT */
    // arst: asynchronous assert, synchronous release (3 stages)
    logic [2:0] a_sync = 3'b111;
    always_ff @(posedge clk or posedge arst) begin
        if (arst) a_sync <= 3'b111;
        else      a_sync <= {a_sync[1:0], 1'b0};
    end

    // button: plain two-flop synchroniser
    logic [1:0] r_sync = 2'b00;
    always_ff @(posedge clk) r_sync <= {r_sync[0], req};

    // hold counter: its MSB says "quiet for 2**HOLD_BITS cycles"
    logic [HOLD_BITS:0] hold = '0;
    always_ff @(posedge clk) begin
        if (a_sync[2] || r_sync[1]) hold <= '0;
        else if (!hold[HOLD_BITS])  hold <= hold + 1'b1;
    end

    logic rst_q = 1'b1;
    /* verilator lint_on PROCASSINIT */
    always_ff @(posedge clk) rst_q <= !hold[HOLD_BITS];
    assign rst = rst_q;
endmodule
`default_nettype wire
