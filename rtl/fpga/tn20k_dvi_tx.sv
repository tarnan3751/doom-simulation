`default_nettype none
// DVI 1.0 transmitter over the Tang Nano 20K HDMI connector. NEVER TESTED ON HARDWARE.
//
//   clk_sys domain: soc_top's registered video pins, one pixel per vid_pix_ce
//        |  tn20k_dvi_enc: async FIFO (16 x 27 bit LUT RAM) + 3 x TMDS encoder (clk_pix)
//   OSER10 (10:1, DDR on clk_ser = 5 x clk_pix), one per lane, 3 data + clock
//   TLVDS_OBUF -> HDMI pins (pins 33-40 are true-LVDS pairs; gowin_pack rejects ELVDS there)
//
// OSER10 sends D0 first, matching TMDS (symbol bit 0 first). The clock lane sends
// 1111100000 every pixel, i.e. a 25.2 MHz clock.
module tn20k_dvi_tx (
    input  wire        clk_sys,
    input  wire        rst_sys,
    input  wire        vid_pix_ce,
    input  wire        vid_hsync,
    input  wire        vid_vsync,
    input  wire        vid_de,
    input  wire  [7:0] vid_r,
    input  wire  [7:0] vid_g,
    input  wire  [7:0] vid_b,

    // pixel / serial clocks (tn20k_clocks; clk_pix = CLKDIV/5 of clk_ser)
    input  wire        clk_pix,
    input  wire        rst_pix,
    input  wire        clk_ser,

    output wire        tmds_clk_p,
    output wire        tmds_clk_n,
    output wire        tmds_d0_p,
    output wire        tmds_d0_n,
    output wire        tmds_d1_p,
    output wire        tmds_d1_n,
    output wire        tmds_d2_p,
    output wire        tmds_d2_n,

    output wire        slip           // clk_pix domain, sticky: the pixel FIFO under/overran
);
    logic [9:0] sym [0:3];

    tn20k_dvi_enc u_enc (
        .clk_sys    (clk_sys),
        .rst_sys    (rst_sys),
        .vid_pix_ce (vid_pix_ce),
        .vid_hsync  (vid_hsync),
        .vid_vsync  (vid_vsync),
        .vid_de     (vid_de),
        .vid_r      (vid_r),
        .vid_g      (vid_g),
        .vid_b      (vid_b),
        .clk_pix    (clk_pix),
        .rst_pix    (rst_pix),
        .sym0       (sym[0]),
        .sym1       (sym[1]),
        .sym2       (sym[2]),
        .sym_valid  (),
        .slip       (slip)
    );
    assign sym[3] = 10'b0000011111;                // TMDS clock lane

    logic [3:0] ser;
    for (genvar i = 0; i < 4; i++) begin : g_ser
        OSER10 #(.GSREN("false"), .LSREN("true")) u_oser (
            .Q     (ser[i]),
            .D0    (sym[i][0]), .D1(sym[i][1]), .D2(sym[i][2]), .D3(sym[i][3]), .D4(sym[i][4]),
            .D5    (sym[i][5]), .D6(sym[i][6]), .D7(sym[i][7]), .D8(sym[i][8]), .D9(sym[i][9]),
            .FCLK  (clk_ser),
            .PCLK  (clk_pix),
            .RESET (rst_pix)
        );
    end
    TLVDS_OBUF u_ob_d0 (.I(ser[0]), .O(tmds_d0_p),  .OB(tmds_d0_n));
    TLVDS_OBUF u_ob_d1 (.I(ser[1]), .O(tmds_d1_p),  .OB(tmds_d1_n));
    TLVDS_OBUF u_ob_d2 (.I(ser[2]), .O(tmds_d2_p),  .OB(tmds_d2_n));
    TLVDS_OBUF u_ob_ck (.I(ser[3]), .O(tmds_clk_p), .OB(tmds_clk_n));
endmodule
`default_nettype wire
