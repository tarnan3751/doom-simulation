`default_nettype none
// Keyboard: 16-entry FIFO of {pressed, code}. Events arriving while full are dropped
// (including an event arriving in the same cycle as a pop from a full FIFO).
module kbd (
    input  logic       clk,
    input  logic       rst,
    input  logic       key_valid,
    input  logic [8:0] key_data,
    input  logic       rd_en,        // accepted load from the block
    input  logic [9:0] off,          // word offset (addr[11:2])
    output logic [31:0] rdata        // combinational read at `off`
);
    logic [8:0] mem [0:15];
    logic [3:0] rptr, wptr;
    logic [4:0] count;

    wire nonempty = (count != 5'd0);
    wire full     = count[4];
    wire pop      = rd_en && off == 10'd0 && nonempty;
    wire push     = key_valid && !full;

    always_comb begin
        case (off)
            10'd0:   rdata = nonempty ? {1'b1, 22'b0, mem[rptr]} : 32'b0;
            10'd1:   rdata = {31'b0, nonempty};
            default: rdata = 32'b0;
        endcase
    end

    always_ff @(posedge clk) begin
        if (push) mem[wptr] <= key_data;
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            rptr  <= '0;
            wptr  <= '0;
            count <= '0;
        end else begin
            if (push) wptr <= wptr + 4'd1;
            if (pop)  rptr <= rptr + 4'd1;
            count <= count + {4'b0, push} - {4'b0, pop};
        end
    end
endmodule
`default_nettype wire
