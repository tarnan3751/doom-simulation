// rv32_muldiv - multi-cycle M-extension unit (EX stage).
//
// Multiply: operands registered (cycle 1), 33x33 signed product registered
// (cycle 2), result selected from the product register (cycle 3): the
// instruction spends 3 cycles in EX. The multiplier sits between two register
// stages so it maps onto DSP blocks.
// Divide: radix-2 restoring division on magnitudes, one quotient bit per
// cycle: magnitudes registered (cycle 1); normalisation (cycle 2): with
// n = clz(divisor) - clz(dividend) + 1 the quotient has at most n significant
// bits, so the first 32 - n iterations would only shift in zero quotient bits
// and the dividend's top bits: they are skipped by starting with
// {remainder, quotient} = dividend << (32 - n) (n < 1: quotient 0, remainder =
// dividend, no iteration); n iterations; result with sign fix: n + 3 cycles.
// Divide by zero finishes after the operand cycle (2 cycles).
// Corner cases follow the ISA: x/0 = -1 (all ones), x%0 = x,
// INT_MIN/-1 = INT_MIN, INT_MIN%-1 = 0.
//
// Handshake: `req` is high while an M instruction sits in EX; operands must be
// stable while req is high (the pipeline is frozen). `done` rises when the
// result is valid and stays high until `ack` (the instruction leaves EX).
`default_nettype none

module rv32_muldiv (
    input  logic        clk,
    input  logic        rst,
    input  logic        req,
    input  logic [2:0]  op,        // funct3: mul mulh mulhsu mulhu div divu rem remu
    input  logic [31:0] a,
    input  logic [31:0] b,
    input  logic        ack,
    output logic        done,
    output logic [31:0] result
);
    typedef enum logic [2:0] { S_IDLE, S_MUL, S_NORM, S_DIV, S_DONE } state_e;
    state_e state;

    // multiplier
    logic signed [32:0] mul_a, mul_b;
    logic        [63:0] mul_p;
    logic signed [65:0] mul_full;
    assign mul_full = mul_a * mul_b;

    // divider
    logic [31:0] div_rem, div_quo, div_dsr;
    logic [4:0]  div_cnt;
    logic        neg_q, neg_r;
    logic [32:0] div_shift;
    logic [32:0] div_diff;      // bit 32 = borrow (shifted remainder < divisor)
    assign div_shift = {div_rem, div_quo[31]};
    assign div_diff  = div_shift - {1'b0, div_dsr};

    function automatic logic [5:0] clz(input logic [31:0] v);
        clz = 6'd32;
        for (int i = 0; i < 32; i++) if (v[i]) clz = 6'(31 - i);
    endfunction
    logic [5:0]  za, zb;
    logic [63:0] div_norm;
    assign za       = clz(div_quo);
    assign zb       = clz(div_dsr);
    assign div_norm = {32'b0, div_quo} << (5'd31 - zb[4:0] + za[4:0]);   // shift 32 - n

    logic        op_is_div;   // op[2] = 1: div/divu/rem/remu
    logic        op_rem;      // remainder result
    logic        op_signed_div;
    logic        a_neg, b_neg;
    logic        r_is_div, r_rem, r_mul_low;
    assign op_is_div     = op[2];
    assign op_rem        = op[1];
    assign op_signed_div = !op[0];
    assign a_neg         = a[31];
    assign b_neg         = b[31];

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= S_IDLE;
        end else begin
            unique case (state)
                S_IDLE: if (req) begin
                    r_is_div  <= op_is_div;
                    r_rem     <= op_rem;
                    r_mul_low <= (op[1:0] == 2'b00);
                    if (!op_is_div) begin
                        // mul: s x s for mulh, s x u for mulhsu, u x u for mulhu (low word is sign-agnostic)
                        mul_a <= {(op[1:0] == 2'b01 || op[1:0] == 2'b10) && a_neg, a};
                        mul_b <= {(op[1:0] == 2'b01) && b_neg, b};
                        state <= S_MUL;
                    end else if (b == 32'b0) begin
                        div_quo <= 32'hFFFF_FFFF;
                        div_rem <= a;
                        neg_q   <= 1'b0;
                        neg_r   <= 1'b0;
                        state   <= S_DONE;
                    end else begin
                        div_quo <= (op_signed_div && a_neg) ? -a : a;
                        div_dsr <= (op_signed_div && b_neg) ? -b : b;
                        neg_q   <= op_signed_div && (a_neg ^ b_neg);
                        neg_r   <= op_signed_div && a_neg;
                        state   <= S_NORM;
                    end
                end
                S_NORM: begin
                    // divisor != 0, so zb <= 31
                    if (zb < za) begin
                        div_rem <= div_quo;
                        div_quo <= 32'b0;
                        state   <= S_DONE;
                    end else begin
                        {div_rem, div_quo} <= div_norm;
                        div_cnt <= 5'd31 - zb[4:0] + za[4:0];
                        state   <= S_DIV;
                    end
                end
                S_MUL: begin
                    mul_p <= mul_full[63:0];
                    state <= S_DONE;
                end
                S_DIV: begin
                    if (!div_diff[32]) begin
                        div_rem <= div_diff[31:0];
                        div_quo <= {div_quo[30:0], 1'b1};
                    end else begin
                        div_rem <= div_shift[31:0];
                        div_quo <= {div_quo[30:0], 1'b0};
                    end
                    div_cnt <= div_cnt + 5'd1;
                    if (div_cnt == 5'd31) state <= S_DONE;
                end
                S_DONE: if (ack) state <= S_IDLE;
                default: state <= S_IDLE;
            endcase
        end
    end

    assign done = (state == S_DONE);

    always_comb begin
        if (!r_is_div)  result = r_mul_low ? mul_p[31:0] : mul_p[63:32];
        else if (r_rem) result = neg_r ? -div_rem : div_rem;
        else            result = neg_q ? -div_quo : div_quo;
    end

    // mul_full[65:64] are sign copies that the 64-bit result does not need
    logic unused_mul;
    assign unused_mul = ^mul_full[65:64];

endmodule

`default_nettype wire
