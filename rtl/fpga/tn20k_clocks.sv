`default_nettype none
// Tang Nano 20K clock generation (GW2AR-18). NEVER TESTED ON HARDWARE.
//
// PLL_CASCADE = 1 (default):
//   27 MHz --> rPLL A (VCO 504 MHz) --CLKOUT----> clk_ser  126.0 MHz  (TMDS serializer, 5 x pixel, DDR)
//                                   --CLKOUTD/6--> 21.0 MHz --> rPLL B (VCO 806.4 MHz)
//                                                                  --CLKOUT---> clk_sys    50.4 MHz
//                                                                  --CLKOUTP--> clk_sdram  50.4 MHz, phase SDRAM_PHASE
//              clk_ser --> CLKDIV /5 --> clk_pix 25.2 MHz (OSER10 PCLK, TMDS encoders)
//
// PLL_CASCADE = 0 (fallback, one rPLL):
//   27 MHz --> rPLL (VCO 504 MHz) --CLKOUT 252 MHz--> CLKDIV /5 --> clk_sys 50.4 MHz (= clk_sdram)
//                                 --CLKOUTD/2-------> clk_ser 126 MHz --> CLKDIV /5 --> clk_pix 25.2 MHz
//
// Why:
// * soc_top produces its pixel stream in the system clock domain at exactly clk/2
//   (vid_pix_ce every other cycle), so the system clock must be EXACTLY twice the
//   pixel clock (frequency-locked), or the clock-crossing FIFO in tn20k_dvi_tx would
//   over/underflow. 25.2 MHz is the CEA-861 60.00 Hz pixel clock of 640x480 with
//   the 800x525 raster the SoC generates; then clk_sys = 50.4 MHz.
// * 50.4 MHz cannot be made from 27 MHz by one rPLL output: 50.4/27 = 28/15 needs
//   IDIV = 15, i.e. a 1.8 MHz phase detector, below the 3 MHz minimum. Cascading B
//   behind A (21 MHz = 126/6 reference, 21 * 12/5 = 50.4) makes every clock an exact
//   rational multiple of the crystal: sys : pix : ser = 2 : 1 : 5 exactly.
// * In the cascade, rPLL B is the system PLL, so the SDRAM clock can be a
//   phase-shifted copy of the system clock (CLKOUTP, 22.5 degree steps). See
//   synth/README.md for the phase (default 315 degrees: the SDRAM clock edge
//   leads the system clock edge by 45 degrees, 2.48 ns, giving the command/data
//   outputs hold margin at the SDRAM). The cascade (PLL B's reference from PLL A's
//   CLKOUTD over the clock network) is accepted by nextpnr and gowin_pack; that it
//   works on silicon is NOT verified.
// * The fallback avoids the cascade, but clk_sdram is then the system clock itself
//   (no phase shift possible): the forwarded SDRAM clock edge and the IOB-registered
//   command/data edges leave the FPGA at the same time, i.e. about zero hold margin
//   at the SDRAM (synth/README.md). It uses one rPLL and two CLKDIVs.
//
// locked: all PLLs locked (asynchronous; synchronise before use).
module tn20k_clocks #(
    parameter bit PLL_CASCADE = 1'b1,
    parameter     SDRAM_PHASE = "1110"     // rPLL PSDA_SEL: clk_sdram phase, 22.5 degree steps (14 = 315 deg)
) (
    input  wire clk27,
    output wire clk_sys,
    output wire clk_sdram,
    output wire clk_pix,
    output wire clk_ser,
    output wire locked
);
    wire lock_a;

    if (PLL_CASCADE) begin : g_cascade
        wire ref21, lock_b;

        // rPLL A: 27 * 14 / 3 = 126 MHz (PFD 9 MHz, VCO 504 MHz), CLKOUTD = 126 / 6 = 21 MHz
        rPLL #(
            .FCLKIN       ("27.0"),
            .IDIV_SEL     (2),
            .FBDIV_SEL    (13),
            .ODIV_SEL     (4),
            .DYN_SDIV_SEL (6),
            .DEVICE       ("GW2AR-18C")
        ) u_pll_a (
            .CLKIN   (clk27),
            .CLKOUT  (clk_ser),
            .CLKOUTP (),
            .CLKOUTD (ref21),
            .CLKOUTD3(),
            .LOCK    (lock_a),
            .CLKFB   (1'b0),
            .FBDSEL  (6'b0),
            .IDSEL   (6'b0),
            .ODSEL   (6'b0),
            .PSDA    (4'b0),
            .DUTYDA  (4'b0),
            .FDLY    (4'b0),
            .RESET   (1'b0),
            .RESET_P (1'b0)
        );

        // rPLL B: 21 * 12 / 5 = 50.4 MHz (PFD 4.2 MHz, VCO 806.4 MHz); held in reset until A locks
        rPLL #(
            .FCLKIN       ("21.0"),
            .IDIV_SEL     (4),
            .FBDIV_SEL    (11),
            .ODIV_SEL     (16),
            .PSDA_SEL     (SDRAM_PHASE),
            .DEVICE       ("GW2AR-18C")
        ) u_pll_b (
            .CLKIN   (ref21),
            .CLKOUT  (clk_sys),
            .CLKOUTP (clk_sdram),
            .CLKOUTD (),
            .CLKOUTD3(),
            .LOCK    (lock_b),
            .CLKFB   (1'b0),
            .FBDSEL  (6'b0),
            .IDSEL   (6'b0),
            .ODSEL   (6'b0),
            .PSDA    (4'b0),
            .DUTYDA  (4'b0),
            .FDLY    (4'b0),
            .RESET   (!lock_a),
            .RESET_P (1'b0)
        );
        assign locked = lock_a && lock_b;
    end else begin : g_single
        wire c252;

        // rPLL: 27 * 28 / 3 = 252 MHz (PFD 9 MHz, VCO 504 MHz), CLKOUTD = 252 / 2 = 126 MHz
        rPLL #(
            .FCLKIN       ("27.0"),
            .IDIV_SEL     (2),
            .FBDIV_SEL    (27),
            .ODIV_SEL     (2),
            .DYN_SDIV_SEL (2),
            .DEVICE       ("GW2AR-18C")
        ) u_pll (
            .CLKIN   (clk27),
            .CLKOUT  (c252),
            .CLKOUTP (),
            .CLKOUTD (clk_ser),
            .CLKOUTD3(),
            .LOCK    (lock_a),
            .CLKFB   (1'b0),
            .FBDSEL  (6'b0),
            .IDSEL   (6'b0),
            .ODSEL   (6'b0),
            .PSDA    (4'b0),
            .DUTYDA  (4'b0),
            .FDLY    (4'b0),
            .RESET   (1'b0),
            .RESET_P (1'b0)
        );
        CLKDIV #(.DIV_MODE("5")) u_div_sys (
            .HCLKIN (c252),
            .RESETN (lock_a),
            .CALIB  (1'b0),
            .CLKOUT (clk_sys)
        );
        assign clk_sdram = clk_sys;
        assign locked    = lock_a;
    end

    // pixel clock = serializer clock / 5, phase-aligned with it (required by OSER10)
    CLKDIV #(.DIV_MODE("5")) u_div_pix (
        .HCLKIN (clk_ser),
        .RESETN (lock_a),
        .CALIB  (1'b0),
        .CLKOUT (clk_pix)
    );
endmodule
`default_nettype wire
