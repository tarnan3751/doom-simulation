`default_nettype none
// Ideal line memory for the FAST build: a plain 32 MB array that serves line
// requests in the minimum number of cycles a synchronous memory can.
//
//   read : request granted in cycle t (bk_gnt combinational, whenever idle);
//          beats in cycles t+1 .. t+8, in wrap order from bk_word.
//   write: granted in cycle t; words consumed in cycles t+1 .. t+8 (bk_wnext).
// Back-to-back requests run without bubbles.
//
// The array is /* verilator public */ so the board can preload and inspect it:
// word i holds RAM byte offsets 4i .. 4i+3 (little endian), i.e. the same
// indexing as the SDRAM chip model. Simulation only (not meant for synthesis).
module mem_ideal #(
    parameter int unsigned LA_W = 20           // line address bits (<= 20)
) (
    input  logic            clk,
    input  logic            rst,

    input  logic            bk_req,
    input  logic            bk_we,
    input  logic [LA_W-1:0] bk_addr,
    input  logic [2:0]      bk_word,
    input  logic            bk_id,
    input  logic [31:0]     bk_wdata,
    output logic            bk_gnt,
    output logic            bk_rvalid,
    output logic            bk_rid,
    output logic [31:0]     bk_rdata,
    output logic            bk_wnext
);
    localparam int unsigned WORDS_AW = 23;     // 2**23 words = 32 MB

    if (LA_W + 3 > WORDS_AW) begin : g_bad_size
        $error("mem_ideal: line address too wide for the 32 MB array");
    end

    // Public by contract (board preload/inspection); the optimisation note is
    // irrelevant for a plain storage array.
    /* verilator lint_off UNOPTPUBLIC */
    logic [31:0] mem [0:(2**WORDS_AW)-1] /* verilator public */;
    /* verilator lint_on UNOPTPUBLIC */

    typedef enum logic [1:0] {S_IDLE, S_RD, S_WR} state_t;
    state_t          state;
    logic [LA_W-1:0] line_q;
    logic [2:0]      word_q;
    logic [2:0]      cnt;

    assign bk_gnt   = (state == S_IDLE) && bk_req;
    assign bk_wnext = (state == S_WR);

    function automatic logic [WORDS_AW-1:0] widx(input logic [LA_W-1:0] line, input logic [2:0] w);
        return WORDS_AW'({line, w});
    endfunction

    always_ff @(posedge clk) begin
        if (rst) begin
            state     <= S_IDLE;
            bk_rvalid <= 1'b0;
            bk_rid    <= 1'b0;
            line_q    <= '0;
            word_q    <= '0;
            cnt       <= '0;
        end else begin
            bk_rvalid <= 1'b0;
            unique case (state)
                S_IDLE: begin
                    if (bk_req) begin
                        line_q <= bk_addr;
                        cnt    <= '0;
                        if (bk_we) begin
                            state <= S_WR;
                        end else begin
                            bk_rdata  <= mem[widx(bk_addr, bk_word)];
                            bk_rvalid <= 1'b1;
                            bk_rid    <= bk_id;
                            word_q    <= bk_word + 3'd1;
                            cnt       <= 3'd1;
                            state     <= S_RD;
                        end
                    end
                end
                S_RD: begin
                    bk_rdata  <= mem[widx(line_q, word_q)];
                    bk_rvalid <= 1'b1;
                    word_q    <= word_q + 3'd1;
                    cnt       <= cnt + 3'd1;
                    if (cnt == 3'd7) state <= S_IDLE;
                end
                S_WR: begin
                    mem[widx(line_q, cnt)] <= bk_wdata;
                    cnt <= cnt + 3'd1;
                    if (cnt == 3'd7) state <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end
endmodule
`default_nettype wire
