`default_nettype none
// UART transmitter, 8N1, UART_DIV clock cycles per bit, line idles high.
// wr_en with ready==1 starts a frame; wr_en while busy is ignored.
module uart_tx #(
    parameter int unsigned UART_DIV = 434
) (
    input  logic       clk,
    input  logic       rst,
    input  logic       wr_en,      // a store to UART_TX was accepted
    input  logic [7:0] wr_data,
    output logic       tx,         // serial line (flop output)
    output logic       ready,      // 1 = a byte may be written
    output logic       accepted    // wr_en && ready (combinational, for the debug pulse)
);
    localparam int unsigned DW = (UART_DIV > 1) ? $clog2(UART_DIV) : 1;
    localparam logic [DW-1:0] DIV_LAST = DW'(UART_DIV - 1);

    logic          busy;
    logic [DW-1:0] div;
    logic [3:0]    nleft;   // bits still to load after the current one
    logic [8:0]    sh;      // remaining data bits (LSB first) followed by the stop bit
    logic          txr;

    assign ready    = ~busy;
    assign accepted = wr_en & ~busy;
    assign tx       = txr;

    always_ff @(posedge clk) begin
        if (rst) begin
            busy  <= 1'b0;
            txr   <= 1'b1;
            div   <= '0;
            nleft <= '0;
            sh    <= '1;
        end else if (!busy) begin
            if (wr_en) begin
                busy  <= 1'b1;
                txr   <= 1'b0;                 // start bit
                sh    <= {1'b1, wr_data};      // data, then stop bit
                nleft <= 4'd9;
                div   <= '0;
            end
        end else if (div == DIV_LAST) begin
            div <= '0;
            if (nleft == 4'd0) begin
                busy <= 1'b0;                  // stop bit finished
            end else begin
                txr   <= sh[0];
                sh    <= {1'b1, sh[8:1]};
                nleft <= nleft - 4'd1;
            end
        end else begin
            div <= div + 1'b1;
        end
    end
endmodule
`default_nettype wire
