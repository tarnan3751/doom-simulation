`default_nettype none
// Timing-coverage probe for nextpnr-himbaechel / GW2A (synth/scripts/probes.py).
//
// A deliberately slow cone (three chained 18-bit adders, ~6-7 ns after place and
// route) drives exactly ONE pin group of a block RAM or multiplier, or is driven
// by one of its outputs. Everything else in the probe is short (LFSR stimulus,
// one-flop pipelines). If nextpnr analyses that pin group, the reported critical
// path is the slow cone (and ends/starts at the hard block); if it does not, the
// cone silently disappears from the analysis and some trivial path is reported.
//
// MODE (string):
//   ref      cone -> fabric flop                       (reference: cone delay)
//   dp_ada   cone -> DP block RAM, port A address (write port)
//   dp_adb   cone -> DP block RAM, port B address (read port)
//   dp_ceb   cone -> DP block RAM, port B clock enable
//   dp_dib   cone -> DP block RAM, port B write data
//   dp_wreb  cone -> DP block RAM, port B write enable
//   dp_dob   DP block RAM port B data out -> cone -> flop
//   sdp_ada  cone -> SDP block RAM write address
//   sdp_adb  cone -> SDP block RAM read address
//   sdp_ceb  cone -> SDP block RAM read enable
//   dsp_in   cone -> MULT18X18 input -> flop
//   dsp_out  flop -> MULT18X18 -> cone -> flop
module timing_probe #(
    parameter string MODE = "ref"
) (
    input  wire  clk,
    output logic o
);
    logic [35:0] lf = '0;
    always_ff @(posedge clk) lf <= {lf[34:0], lf[35] ^ lf[24] ^ 1'b1};

    logic [17:0] ar, br;
    always_ff @(posedge clk) begin ar <= lf[17:0]; br <= lf[35:18]; end

    // the slow cone, from fabric flops
    logic [17:0] a1, a2, a3;
    assign a1 = ar + br;
    assign a2 = a1 + {a1[8:0], a1[17:9]};
    assign a3 = a2 + {a2[4:0], a2[17:5]};
    wire [11:0] cone12 = a3[11:0] ^ {a3[17:12], a3[17:12]};

    function automatic logic [17:0] cone_of(input logic [17:0] x);
        logic [17:0] y1, y2;
        y1 = x + {x[8:0], x[17:9]};
        y2 = y1 + {y1[4:0], y1[17:5]};
        return y2 + {y2[2:0], y2[17:3]};
    endfunction

    logic [35:0] p, q;

    if (MODE == "ref") begin : g_ref
        always_ff @(posedge clk) p <= 36'(a3);
    end else if (MODE == "sdp_ada" || MODE == "sdp_adb" || MODE == "sdp_ceb") begin : g_sdp
        // 512 x 32: forces the SDP primitive (true dual port is at most 18 bits wide)
        (* no_rw_check *) logic [31:0] mem [0:511];
        logic [31:0] rd;
        wire [8:0] wa = (MODE == "sdp_ada") ? cone12[8:0] : lf[18:10];
        wire [8:0] ra = (MODE == "sdp_adb") ? cone12[8:0] : lf[28:20];
        wire       re = (MODE == "sdp_ceb") ? (a3[17] ^ a3[2]) : lf[5];
        always_ff @(posedge clk) begin
            if (lf[3]) mem[wa] <= {lf[31:16], lf[15:0]};
            if (re)    rd <= mem[ra];
        end
        always_ff @(posedge clk) p <= 36'(rd);
    end else if (MODE == "dsp_in" || MODE == "dsp_out") begin : g_dsp
        logic [35:0] m;
        assign m = (MODE == "dsp_in") ? 36'(a3 * br) : 36'(ar * br);
        if (MODE == "dsp_in") begin : g_in
            always_ff @(posedge clk) p <= m;
        end else begin : g_out
            always_ff @(posedge clk) p <= {cone_of(m[35:18]), cone_of(m[17:0])};
        end
    end else begin : g_dp
        // 4096 x 4, two ports: one DPB
        (* no_rw_check *) logic [3:0] mem [0:4095];
        logic [3:0] rda, rdb;
        wire [11:0] ada = (MODE == "dp_ada")  ? cone12 : lf[21:10];
        wire [11:0] adb = (MODE == "dp_adb")  ? cone12 : lf[29:18];
        wire        ceb = (MODE == "dp_ceb")  ? (a3[17] ^ a3[2]) : lf[6];
        wire [3:0]  dib = (MODE == "dp_dib")  ? (a3[17:14] ^ a3[3:0]) : lf[35:32];
        wire        web = (MODE == "dp_wreb") ? (a3[16] ^ a3[1]) :
                          (MODE == "dp_dib")  ? lf[4] : 1'b0;
        always_ff @(posedge clk) begin
            if (lf[3]) mem[ada] <= lf[33:30]; else rda <= mem[ada];
        end
        always_ff @(posedge clk) begin
            if (ceb) begin
                if (web) mem[adb] <= dib; else rdb <= mem[adb];
            end
        end
        if (MODE == "dp_dob") begin : g_dob
            always_ff @(posedge clk) p <= {cone_of({rdb, rdb, rdb, rdb, rdb[1:0]}), 4'b0, rda, 6'b0};
        end else begin : g_nodob
            always_ff @(posedge clk) p <= {28'b0, rdb, rda};
        end
    end

    always_ff @(posedge clk) begin q <= p; o <= ^q; end
endmodule
`default_nettype wire
