`default_nettype none
// DVI pixel path without the I/O primitives: system-clock pixel stream ->
// clock-domain crossing -> three TMDS encoders (pixel clock). Pure RTL so it can
// be simulated (synth/tb); tn20k_dvi_tx adds the serialisers and LVDS buffers.
// NEVER TESTED ON HARDWARE.
//
// DVI channel mapping: channel 0 = blue and carries {C1, C0} = {VSYNC, HSYNC}
// during blanking, channel 1 = green, channel 2 = red, control 00 on channels 1/2.
// Sync levels are passed through as the SoC drives them (active low, as the
// 640x480 mode requires). While the FIFO is priming (after reset, or after a
// slip) all channels send control symbols with both syncs inactive.
module tn20k_dvi_enc (
    // system side (soc_top video outputs, all registered in soc_top)
    input  wire        clk_sys,
    input  wire        rst_sys,
    input  wire        vid_pix_ce,       // high in the first of the two cycles a pixel is on the pins
    input  wire        vid_hsync,
    input  wire        vid_vsync,
    input  wire        vid_de,
    input  wire  [7:0] vid_r,
    input  wire  [7:0] vid_g,
    input  wire  [7:0] vid_b,

    // pixel side
    input  wire        clk_pix,
    input  wire        rst_pix,
    output logic [9:0] sym0,             // blue + syncs, q[0] first on the wire
    output logic [9:0] sym1,             // green
    output logic [9:0] sym2,             // red
    output logic       sym_valid,        // sym* carry FIFO data (0 while priming); for test benches
    output logic       slip              // sticky: the FIFO under/overran
);
    logic [26:0] px;
    logic        px_valid;
    tn20k_vid_cdc #(.W(27), .AW(4)) u_cdc (
        .wclk   (clk_sys),
        .wrst   (rst_sys),
        .we     (vid_pix_ce),
        .wdata  ({vid_de, vid_vsync, vid_hsync, vid_r, vid_g, vid_b}),
        .rclk   (clk_pix),
        .rrst   (rst_pix),
        .rdata  (px),
        .rvalid (px_valid),
        .slip   (slip)
    );

    wire de = px_valid && px[26];
    wire vs = px_valid ? px[25] : 1'b1;      // inactive (high) while priming
    wire hs = px_valid ? px[24] : 1'b1;

    tn20k_tmds_enc u_enc_b (.clk(clk_pix), .rst(rst_pix), .de(de), .d(px[7:0]),   .c0(hs),   .c1(vs),   .q(sym0));
    tn20k_tmds_enc u_enc_g (.clk(clk_pix), .rst(rst_pix), .de(de), .d(px[15:8]),  .c0(1'b0), .c1(1'b0), .q(sym1));
    tn20k_tmds_enc u_enc_r (.clk(clk_pix), .rst(rst_pix), .de(de), .d(px[23:16]), .c0(1'b0), .c1(1'b0), .q(sym2));

    always_ff @(posedge clk_pix) begin
        if (rst_pix) sym_valid <= 1'b0;
        else         sym_valid <= px_valid;
    end
endmodule
`default_nettype wire
