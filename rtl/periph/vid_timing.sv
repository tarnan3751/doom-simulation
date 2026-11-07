`default_nettype none
// 640x480@60 timing generator, 800x525 total, advancing on `ce` (every other clk).
// Sync pulses are active low. Counters are the "stage 0" of the scan-out pipeline.
module vid_timing (
    input  logic        clk,
    input  logic        rst,
    output logic        ce,          // pixel enable: counters advance at the end of a cycle with ce=1
    output logic [9:0]  hcnt,        // 0..799
    output logic [9:0]  vcnt,        // 0..524
    output logic        hs_n,        // from the counters (combinational)
    output logic        vs_n,
    output logic        de,          // 640x480 active area
    output logic        fb_act,      // inside the 640x400 framebuffer window (lines 40..439)
    output logic [15:0] frame_cnt    // increments when vsync asserts
);
    localparam int H_ACTIVE = 640, H_FP = 16, H_SYNC = 96, H_BP = 48;
    localparam int H_TOTAL  = H_ACTIVE + H_FP + H_SYNC + H_BP;   // 800
    localparam int V_ACTIVE = 480, V_FP = 10, V_SYNC = 2, V_BP = 33;
    localparam int V_TOTAL  = V_ACTIVE + V_FP + V_SYNC + V_BP;   // 525
    localparam int FB_Y0 = 40, FB_Y1 = 440;                      // active lines [FB_Y0, FB_Y1)

    wire h_act = (hcnt < 10'(H_ACTIVE));
    assign de     = h_act && (vcnt < 10'(V_ACTIVE));
    assign fb_act = h_act && (vcnt >= 10'(FB_Y0)) && (vcnt < 10'(FB_Y1));
    assign hs_n   = !((hcnt >= 10'(H_ACTIVE + H_FP)) && (hcnt < 10'(H_ACTIVE + H_FP + H_SYNC)));
    assign vs_n   = !((vcnt >= 10'(V_ACTIVE + V_FP)) && (vcnt < 10'(V_ACTIVE + V_FP + V_SYNC)));

    always_ff @(posedge clk) begin
        if (rst) begin
            ce        <= 1'b0;
            hcnt      <= '0;
            vcnt      <= '0;
            frame_cnt <= '0;
        end else begin
            ce <= ~ce;
            if (ce) begin
                if (hcnt == 10'(H_TOTAL - 1)) begin
                    hcnt <= '0;
                    if (vcnt == 10'(V_TOTAL - 1)) vcnt <= '0;
                    else                          vcnt <= vcnt + 10'd1;
                    if (vcnt == 10'(V_ACTIVE + V_FP - 1)) frame_cnt <= frame_cnt + 16'd1;
                end else begin
                    hcnt <= hcnt + 10'd1;
                end
            end
        end
    end
endmodule
`default_nettype wire
