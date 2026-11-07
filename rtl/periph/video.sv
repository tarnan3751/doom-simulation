`default_nettype none
// Video block: control registers, palette RAM, framebuffer (4 byte-lane RAMs) and, when
// FAST=0, the 5-stage scan-out pipeline S1..S5 behind the vid_timing counters (S0). Stages
// and pin timing: rtl/periph/README.md. vid_* outputs lag the counters by 5 clk, all equally.
module video #(
    parameter bit FAST = 1'b0
) (
    input  logic        clk,
    input  logic        rst,

    // control block 0x1000_3xxx
    input  logic        ctl_wr,       // accepted store
    input  logic [9:0]  ctl_off,      // word offset within the block (addr[11:2])
    output logic [31:0] ctl_rdata,    // combinational read of the register at ctl_off

    // framebuffer 0x1001_xxxx
    input  logic        fb_wr,        // accepted store with addr[31:16] == 0x1001
    input  logic [13:0] fb_widx,      // addr[15:2]

    input  logic [3:0]  wstrb,
    input  logic [31:0] wdata,

    output logic        vid_flip,

    output logic        vid_pix_ce,
    output logic        vid_hsync,
    output logic        vid_vsync,
    output logic        vid_de,
    output logic [7:0]  vid_r,
    output logic [7:0]  vid_g,
    output logic [7:0]  vid_b
);
    localparam int FB_WORDS = 16000;

    // ---- memories (synchronous read, no reset) -------------------------------
    // no_rw_check: when a CPU store and the scan-out read hit the same word in the
    // same cycle, the scan-out may get either value (one pixel of one frame), so
    // synthesis must not add read-during-write logic around these RAMs.
    // The palette is mapped to distributed (LUT) RAM: its read stays synchronous
    // (LUT-RAM read into the S4 register), and it leaves the FPGA's block RAMs to
    // the framebuffer, caches and branch predictor, which need all of them.
    // fb0..fb3: byte lanes 0..3 (bits 7:0 .. 31:24)
    (* no_rw_check *) logic [7:0]  fb0 [0:FB_WORDS-1] /* verilator public_flat_rw */;
    (* no_rw_check *) logic [7:0]  fb1 [0:FB_WORDS-1] /* verilator public_flat_rw */;
    (* no_rw_check *) logic [7:0]  fb2 [0:FB_WORDS-1] /* verilator public_flat_rw */;
    (* no_rw_check *) logic [7:0]  fb3 [0:FB_WORDS-1] /* verilator public_flat_rw */;
    (* no_rw_check *) (* ram_style = "distributed" *)
    logic [23:0] palette [0:255]    /* verilator public_flat_rw */;   // 0x00RRGGBB

    wire fb_hit = fb_wr && (fb_widx < 14'(FB_WORDS));

    always_ff @(posedge clk) begin
        if (fb_hit && wstrb[0]) fb0[fb_widx] <= wdata[7:0];
        if (fb_hit && wstrb[1]) fb1[fb_widx] <= wdata[15:8];
        if (fb_hit && wstrb[2]) fb2[fb_widx] <= wdata[23:16];
        if (fb_hit && wstrb[3]) fb3[fb_widx] <= wdata[31:24];
    end

    wire pal_wr = ctl_wr && (ctl_off[9:8] == 2'b01);   // 0x...3400-0x...37FC
    always_ff @(posedge clk) begin
        if (pal_wr) begin
            if (wstrb[0]) palette[ctl_off[7:0]][7:0]   <= wdata[7:0];
            if (wstrb[1]) palette[ctl_off[7:0]][15:8]  <= wdata[15:8];
            if (wstrb[2]) palette[ctl_off[7:0]][23:16] <= wdata[23:16];
        end
    end

    // ---- VID_FLIP ----------------------------------------------------------
    logic [31:0] flip_cnt;
    wire flip_wr = ctl_wr && (ctl_off == 10'd0);
    always_ff @(posedge clk) begin
        if (rst) begin
            flip_cnt <= '0;
            vid_flip <= 1'b0;
        end else begin
            vid_flip <= flip_wr;
            if (flip_wr) flip_cnt <= flip_cnt + 32'd1;
        end
    end

    logic        vblank;
    logic [15:0] frame_cnt;

    always_comb begin
        case (ctl_off)
            10'd0:   ctl_rdata = flip_cnt;
            10'd1:   ctl_rdata = {frame_cnt, 15'b0, vblank};
            default: ctl_rdata = 32'b0;
        endcase
    end

    if (FAST) begin : g_fast
        assign vblank     = 1'b1;
        assign frame_cnt  = 16'b0;
        assign vid_pix_ce = 1'b0;
        assign vid_hsync  = 1'b1;
        assign vid_vsync  = 1'b1;
        assign vid_de     = 1'b0;
        assign vid_r      = 8'h00;
        assign vid_g      = 8'h00;
        assign vid_b      = 8'h00;
    end else begin : g_scan
        logic        ce;
        // verilator lint_off UNUSEDSIGNAL
        logic [9:0]  hcnt, vcnt;      // hcnt[0] unused: pixels are doubled
        // verilator lint_on UNUSEDSIGNAL
        logic        t_hs_n, t_vs_n, t_de, t_fb;

        vid_timing u_timing (
            .clk(clk), .rst(rst), .ce(ce), .hcnt(hcnt), .vcnt(vcnt),
            .hs_n(t_hs_n), .vs_n(t_vs_n), .de(t_de), .fb_act(t_fb), .frame_cnt(frame_cnt)
        );
        assign vblank = !((vcnt >= 10'd40) && (vcnt < 10'd440));

        // S1
        // verilator lint_off UNUSEDSIGNAL
        wire [9:0] rowfull = vcnt - 10'd40;   // only [8:1] used: lines are doubled
        // verilator lint_on UNUSEDSIGNAL
        logic [8:0] col1;
        logic [7:0] row1;
        logic       fb1v, de1, hs1, vs1;
        always_ff @(posedge clk) begin
            col1 <= hcnt[9:1];
            row1 <= rowfull[8:1];
        end
        always_ff @(posedge clk) begin
            if (rst) begin
                fb1v <= 1'b0; de1 <= 1'b0; hs1 <= 1'b1; vs1 <= 1'b1;
            end else begin
                fb1v <= t_fb; de1 <= t_de; hs1 <= t_hs_n; vs1 <= t_vs_n;
            end
        end

        // S2
        logic [15:0] addr2;
        logic        fb2v, de2, hs2, vs2;
        always_ff @(posedge clk) begin
            addr2 <= fb1v ? (16'({row1, 8'b0}) + 16'({row1, 6'b0}) + 16'(col1)) : 16'b0;
        end
        always_ff @(posedge clk) begin
            if (rst) begin
                fb2v <= 1'b0; de2 <= 1'b0; hs2 <= 1'b1; vs2 <= 1'b1;
            end else begin
                fb2v <= fb1v; de2 <= de1; hs2 <= hs1; vs2 <= vs1;
            end
        end

        // S3: framebuffer read
        logic [7:0] rd0, rd1, rd2, rd3;
        logic [1:0] lane3;
        logic       fb3v, de3, hs3, vs3;
        wire [13:0] raddr = addr2[15:2];
        always_ff @(posedge clk) begin
            rd0   <= fb0[raddr];
            rd1   <= fb1[raddr];
            rd2   <= fb2[raddr];
            rd3   <= fb3[raddr];
            lane3 <= addr2[1:0];
        end
        always_ff @(posedge clk) begin
            if (rst) begin
                fb3v <= 1'b0; de3 <= 1'b0; hs3 <= 1'b1; vs3 <= 1'b1;
            end else begin
                fb3v <= fb2v; de3 <= de2; hs3 <= hs2; vs3 <= vs2;
            end
        end

        // S4: palette read
        logic [7:0]  pidx;
        logic [23:0] prgb;
        logic        fb4v, de4, hs4, vs4;
        always_comb begin
            case (lane3)
                2'd0:    pidx = rd0;
                2'd1:    pidx = rd1;
                2'd2:    pidx = rd2;
                default: pidx = rd3;
            endcase
        end
        always_ff @(posedge clk) prgb <= palette[pidx];
        always_ff @(posedge clk) begin
            if (rst) begin
                fb4v <= 1'b0; de4 <= 1'b0; hs4 <= 1'b1; vs4 <= 1'b1;
            end else begin
                fb4v <= fb3v; de4 <= de3; hs4 <= hs3; vs4 <= vs3;
            end
        end

        // S5: outputs; ce travels with the data (ce_d[k] is high in the first cycle a new
        // pixel is visible at stage k)
        logic [5:0] ce_d;
        always_ff @(posedge clk) begin
            if (rst) begin
                ce_d      <= 6'b000001;   // pixel 0 (counters at 0,0) gets its pulse when reset ends
                vid_hsync <= 1'b1;
                vid_vsync <= 1'b1;
                vid_de    <= 1'b0;
                {vid_r, vid_g, vid_b} <= '0;
            end else begin
                ce_d      <= {ce_d[4:0], ce};
                vid_hsync <= hs4;
                vid_vsync <= vs4;
                vid_de    <= de4;
                {vid_r, vid_g, vid_b} <= fb4v ? prgb : 24'b0;
            end
        end
        assign vid_pix_ce = ce_d[5];
    end
endmodule
`default_nettype wire
