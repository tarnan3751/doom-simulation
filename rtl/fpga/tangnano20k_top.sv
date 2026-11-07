`default_nettype none
// Sipeed Tang Nano 20K (GW2AR-LV18QN88C8/I7) board top level for soc_top.
//
//                       *** NEVER TESTED ON HARDWARE ***
// It synthesises, places, routes and packs to a bitstream with the open flow
// (synth/Makefile), and its FPGA-only parts are simulated (synth/tb), but nobody
// has run it on a board. Pin numbers and electrical assumptions: see
// tangnano20k.cst (every pin not confirmed from a local source is marked) and
// synth/README.md.
//
// What is connected
//   clocks   27 MHz crystal (pin 4) -> tn20k_clocks: clk_sys 50.4 MHz, clk_sdram
//            (same, phase-shifted), clk_pix 25.2 MHz, clk_ser 126 MHz
//   reset    PLLs locked AND button S1 released, held 2**16 cycles (tn20k_reset)
//   SDRAM    the GW2AR-18's in-package 8 MB SDR SDRAM. Its pads are reached
//            through top-level ports with these exact names (O_sdram_*,
//            IO_sdram_dq); nextpnr places them from the chip database's SiP pin
//            map, so they are not in the .cst. 2M x 32, 4 banks, 11 row / 8 col
//            bits = soc_top SDRAM_ROW_BITS 11, SDRAM_COL_BITS 8.
//            dq: soc_top's split dq_o / dq_oe / dq_i -> one IOBUF per bit.
//            O_sdram_clk: clk_sdram forwarded through an ODDR (D0=1, D1=0), so
//            the clock leaves through the same kind of IOB register as the
//            command/address pins (their flops are packed into the IOBs by
//            nextpnr --vopt oreg_in_iob; dq capture flops by ireg_in_iob).
//   serial   soc_top uart_tx -> pin 69 (to the on-board BL616 USB-UART)
//   video    DVI over the HDMI connector (tn20k_dvi_tx), when DVI = 1
//   LEDs     active low: led_n[0] heartbeat (clk_sys / 2**25, ~1.5 Hz, out of reset only),
//            led_n[1] halted (SYS_EXIT written), led_n[2] PLLs locked,
//            led_n[3] video FIFO slipped (sticky); led_n[5:4] off
//   keyboard key_valid/key_data tied to 0: would be fed by a PS/2 or USB-HID
//            receiver translating to Doom key codes (ARCHITECTURE.md section 7)
//   not connected (logic removed by synthesis): exit_code, vid_flip,
//            uart_tx_valid/byte, the retire trace and all cnt_* counters
//
// What is missing for a real run (see synth/README.md): a way to load the
// program into SDRAM (the simulation board preloads it; here SDRAM is empty
// after power-up) and storage for the WAD (the 0x8080_0000.. window of the
// memory map is beyond the 8 MB part and aliases the program).
//
// DVI = 0 builds the SoC without a video output: soc_top's video pins are left
// open, so synthesis also removes the scan-out pipeline, framebuffer and palette
// (used to measure the cost of the video path).
module tangnano20k_top #(
    parameter bit         DVI             = 1'b1,
    parameter bit         PLL_CASCADE     = 1'b1,    // 0: one-PLL fallback (no SDRAM clock phase shift)
    parameter             SDRAM_PHASE     = "1110",  // clk_sdram phase, PSDA_SEL steps of 22.5 deg (315 deg)
    parameter bit         BTN_ACTIVE_HIGH = 1'b1     // S1 reads 1 when pressed (UNVERIFIED, see the .cst)
) (
    input  wire        clk27,
    input  wire        btn_s1,          // reset button S1
    output wire        uart_tx,
    output wire [5:0]  led_n,

    output wire        tmds_clk_p,
    output wire        tmds_clk_n,
    output wire        tmds_d0_p,
    output wire        tmds_d0_n,
    output wire        tmds_d1_p,
    output wire        tmds_d1_n,
    output wire        tmds_d2_p,
    output wire        tmds_d2_n,

    output wire        O_sdram_clk,
    output wire        O_sdram_cke,
    output wire        O_sdram_cs_n,
    output wire        O_sdram_ras_n,
    output wire        O_sdram_cas_n,
    output wire        O_sdram_wen_n,
    output wire [3:0]  O_sdram_dqm,
    output wire [10:0] O_sdram_addr,
    output wire [1:0]  O_sdram_ba,
    inout  wire [31:0] IO_sdram_dq
);
    localparam int unsigned CLK_HZ   = 50_400_000;
    localparam int unsigned UART_DIV = 438;           // 115 068 baud (-0.11 %)

    // ---- clocks and resets -------------------------------------------------------
    wire clk_sys, clk_sdram, clk_pix, clk_ser, locked;
    tn20k_clocks #(.PLL_CASCADE(PLL_CASCADE), .SDRAM_PHASE(SDRAM_PHASE)) u_clk (
        .clk27     (clk27),
        .clk_sys   (clk_sys),
        .clk_sdram (clk_sdram),
        .clk_pix   (clk_pix),
        .clk_ser   (clk_ser),
        .locked    (locked)
    );

    wire rst_sys, rst_pix;
    wire btn_reset = BTN_ACTIVE_HIGH ? btn_s1 : !btn_s1;
    tn20k_reset u_rst_sys (.clk(clk_sys), .arst(!locked), .req(btn_reset), .rst(rst_sys));
    tn20k_reset #(.HOLD_BITS(4)) u_rst_pix (.clk(clk_pix), .arst(!locked), .req(1'b0), .rst(rst_pix));

    // ---- the SoC -------------------------------------------------------------------
    wire        sd_cke, sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [1:0]  sd_ba;
    wire [10:0] sd_addr;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_o, sd_dq_i;
    wire        vid_pix_ce, vid_hsync, vid_vsync, vid_de;
    wire [7:0]  vid_r, vid_g, vid_b;
    wire        halted;

    soc_top #(
        .FAST           (1'b0),
        .CLK_HZ         (CLK_HZ),
        .UART_DIV       (UART_DIV),
        .ICACHE_BYTES   (8192),
        .DCACHE_BYTES   (16384),
        .SDRAM_ROW_BITS (11),
        .SDRAM_COL_BITS (8)
    ) u_soc (
        .clk            (clk_sys),
        .rst            (rst_sys),
        .sdram_cke      (sd_cke),
        .sdram_cs_n     (sd_cs_n),
        .sdram_ras_n    (sd_ras_n),
        .sdram_cas_n    (sd_cas_n),
        .sdram_we_n     (sd_we_n),
        .sdram_ba       (sd_ba),
        .sdram_addr     (sd_addr),
        .sdram_dqm      (sd_dqm),
        .sdram_dq_o     (sd_dq_o),
        .sdram_dq_oe    (sd_dq_oe),
        .sdram_dq_i     (sd_dq_i),
        .uart_tx        (uart_tx),
        .key_valid      (1'b0),
        .key_data       (9'd0),
        .vid_pix_ce     (vid_pix_ce),
        .vid_hsync      (vid_hsync),
        .vid_vsync      (vid_vsync),
        .vid_de         (vid_de),
        .vid_r          (vid_r),
        .vid_g          (vid_g),
        .vid_b          (vid_b),
        .halted         (halted),
        .exit_code      (),
        .vid_flip       (),
        .uart_tx_valid  (),
        .uart_tx_byte   (),
        .trace_valid    (),
        .trace_trap     (),
        .trace_pc       (),
        .trace_insn     (),
        .trace_rd       (),
        .trace_rd_wdata (),
        .trace_mem_addr (),
        .trace_mem_wstrb(),
        .trace_mem_wdata(),
        .trace_next_pc  (),
        .cnt_cycle      (),
        .cnt_instret    (),
        .cnt_ic_access  (),
        .cnt_ic_miss    (),
        .cnt_dc_access  (),
        .cnt_dc_miss    (),
        .cnt_branch     (),
        .cnt_branch_miss(),
        .cnt_jump       (),
        .cnt_jump_miss  (),
        .cnt_flip       ()
    );

    // ---- embedded SDRAM ------------------------------------------------------------
    // soc_top drives every sdram_* output from a flop (dqm is a constant 0). nextpnr
    // (--vopt oreg_in_iob / ireg_in_iob) moves the command/address flops and the dq
    // capture flops into the IOBs, so their pin timing is fixed and matches the
    // forwarded clock. The dq output flops stay in the fabric: the IOBUF enable is
    // the inverted, shared dq_oe flop (see synth/README.md, SDRAM timing).
    ODDR u_sdram_clk (.D0(1'b1), .D1(1'b0), .TX(1'b0), .CLK(clk_sdram), .Q0(O_sdram_clk), .Q1());

    assign O_sdram_cke   = sd_cke;
    assign O_sdram_cs_n  = sd_cs_n;
    assign O_sdram_ras_n = sd_ras_n;
    assign O_sdram_cas_n = sd_cas_n;
    assign O_sdram_wen_n = sd_we_n;
    assign O_sdram_ba    = sd_ba;
    assign O_sdram_addr  = sd_addr;
    assign O_sdram_dqm   = sd_dqm;

    for (genvar i = 0; i < 32; i++) begin : g_dq
        IOBUF u_dq (.O(sd_dq_i[i]), .IO(IO_sdram_dq[i]), .I(sd_dq_o[i]), .OEN(!sd_dq_oe));
    end

    // ---- video ---------------------------------------------------------------------
    wire slip;
    if (DVI) begin : g_dvi
        tn20k_dvi_tx u_dvi (
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
            .clk_ser    (clk_ser),
            .tmds_clk_p (tmds_clk_p),
            .tmds_clk_n (tmds_clk_n),
            .tmds_d0_p  (tmds_d0_p),
            .tmds_d0_n  (tmds_d0_n),
            .tmds_d1_p  (tmds_d1_p),
            .tmds_d1_n  (tmds_d1_n),
            .tmds_d2_p  (tmds_d2_p),
            .tmds_d2_n  (tmds_d2_n),
            .slip       (slip)
        );
    end else begin : g_no_dvi
        // video pins of soc_top stay open: scan-out, framebuffer and palette are removed
        TLVDS_OBUF u_ob_d0 (.I(1'b0), .O(tmds_d0_p),  .OB(tmds_d0_n));
        TLVDS_OBUF u_ob_d1 (.I(1'b0), .O(tmds_d1_p),  .OB(tmds_d1_n));
        TLVDS_OBUF u_ob_d2 (.I(1'b0), .O(tmds_d2_p),  .OB(tmds_d2_n));
        TLVDS_OBUF u_ob_ck (.I(1'b0), .O(tmds_clk_p), .OB(tmds_clk_n));
        assign slip = 1'b0;
    end

    // ---- LEDs (active low) ---------------------------------------------------------
    logic [24:0] beat;
    always_ff @(posedge clk_sys) beat <= beat + 1'b1;

    logic [1:0] slip_s;                            // to clk_sys for the LED (cosmetic)
    always_ff @(posedge clk_sys) slip_s <= {slip_s[0], slip};

    // heartbeat only blinks while the SoC is out of reset (a stuck reset, e.g. a
    // wrong button polarity, shows as a dark LED 0 with LED 2 lit)
    assign led_n = ~{2'b00, slip_s[1], locked, halted, beat[24] && !rst_sys};
endmodule
`default_nettype wire
