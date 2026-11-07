`default_nettype none
// TMDS encoder, DVI 1.0 section 3.3.3 (figure 3-5), one channel. NEVER TESTED ON HARDWARE;
// verified against an independent C++ model of the spec algorithm (synth/tb/tb_tmds_enc.cpp).
//
// One symbol per clk. Output q is registered; q[0] is the first bit on the wire.
//   de = 1: 8b/10b transition-minimised, DC-balanced data symbol from d (running
//           disparity `cnt` kept across symbols)
//   de = 0: control symbol for {c1, c0}; cnt is reset to 0
// Latency: inputs at edge t appear on q after edge t (one register).
module tn20k_tmds_enc (
    input  wire        clk,
    input  wire        rst,
    input  wire        de,
    input  wire  [7:0] d,
    input  wire        c0,
    input  wire        c1,
    output logic [9:0] q
);
    // ---- stage 1: transition minimisation (q_m) -------------------------------
    function automatic logic [8:0] minimise(input logic [7:0] x);
        logic [3:0] n1;
        logic       xn;
        logic [8:0] m;
        n1 = '0;
        for (int i = 0; i < 8; i++) n1 = n1 + {3'b0, x[i]};
        xn   = (n1 > 4'd4) || ((n1 == 4'd4) && !x[0]);     // XNOR coding
        m[0] = x[0];
        for (int i = 1; i < 8; i++) m[i] = xn ? !(m[i-1] ^ x[i]) : (m[i-1] ^ x[i]);
        m[8] = !xn;
        return m;
    endfunction

    logic [8:0] qm;
    assign qm = minimise(d);

    // ---- stage 2: DC balance ---------------------------------------------------
    logic signed [4:0] cnt;          // running disparity (ones minus zeros sent), |cnt| <= 10
    logic        [3:0] n1_qm;
    logic signed [4:0] diff;         // N1(q_m[7:0]) - N0(q_m[7:0]), even, -8..8
    logic        [9:0] q_n;
    logic signed [4:0] cnt_n;

    always_comb begin
        n1_qm = '0;
        for (int i = 0; i < 8; i++) n1_qm = n1_qm + {3'b0, qm[i]};
        diff = $signed({1'b0, n1_qm}) - 5'sd4;       // N1 - 4
        diff = diff + diff;                          // N1 - N0 = 2*N1 - 8

        if (!de) begin
            cnt_n = 5'sd0;
            unique case ({c1, c0})
                2'b00:   q_n = 10'b1101010100;
                2'b01:   q_n = 10'b0010101011;
                2'b10:   q_n = 10'b0101010100;
                default: q_n = 10'b1010101011;
            endcase
        end else if (cnt == 5'sd0 || diff == 5'sd0) begin
            q_n = {!qm[8], qm[8], qm[8] ? qm[7:0] : ~qm[7:0]};
            cnt_n = qm[8] ? cnt + diff : cnt - diff;
        end else if ((cnt > 5'sd0 && diff > 5'sd0) || (cnt < 5'sd0 && diff < 5'sd0)) begin
            q_n = {1'b1, qm[8], ~qm[7:0]};
            cnt_n = cnt + (qm[8] ? 5'sd2 : 5'sd0) - diff;
        end else begin
            q_n = {1'b0, qm[8], qm[7:0]};
            cnt_n = cnt - (qm[8] ? 5'sd0 : 5'sd2) + diff;
        end
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            cnt <= 5'sd0;
            q   <= 10'b1101010100;       // control symbol 00
        end else begin
            cnt <= cnt_n;
            q   <= q_n;
        end
    end
endmodule
`default_nettype wire
