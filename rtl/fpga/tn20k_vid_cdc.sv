`default_nettype none
// Pixel stream clock-domain crossing: system clock (writes, one entry per
// vid_pix_ce, i.e. every other cycle) -> pixel clock (one entry per cycle).
// NEVER TESTED ON HARDWARE; simulated in synth/tb (two unrelated-phase clocks).
//
// The two clocks are frequency-locked (clk_sys = 2 x clk_pix exactly, both from
// the same crystal through the PLLs, tn20k_clocks.sv) but their phase is unknown,
// so this is a standard asynchronous FIFO: binary pointers for addressing, Gray
// pointers crossing through two-flop synchronisers. Storage is a 2**AW-entry
// LUT RAM (written in wclk, read asynchronously in rclk).
//
// Read side: after reset it waits until the FIFO is half full ("priming"), then
// pops one entry every rclk cycle. Because the average rates are exactly equal,
// the fill level then stays at half +- the synchroniser latency and never
// reaches empty or full. If it does (a clock glitch, a PLL losing lock), the read
// side goes back to priming and `slip` is set (sticky until rrst); meanwhile
// `rvalid` = 0 and the consumer sends blanking. A full FIFO drops the write and
// also sets `slip` (through a synchroniser), once the reader has started (so a
// reader that is released from reset late does not raise a false alarm).
//
// The only paths between the domains are the Gray-pointer synchronisers and the
// LUT-RAM read (written >= 2 rclk cycles before the read address can select it).
// They are asynchronous by design; nextpnr cannot be told so (its set_false_path
// is a no-op), see synth/README.md for how they are reported.
module tn20k_vid_cdc #(
    parameter int unsigned W  = 27,
    parameter int unsigned AW = 4
) (
    input  wire          wclk,
    input  wire          wrst,
    input  wire          we,
    input  wire  [W-1:0] wdata,

    input  wire          rclk,
    input  wire          rrst,
    output logic [W-1:0] rdata,
    output logic         rvalid,
    output logic         slip
);
    function automatic logic [AW:0] bin2gray(input logic [AW:0] b);
        return b ^ (b >> 1);
    endfunction
    function automatic logic [AW:0] gray2bin(input logic [AW:0] g);
        logic [AW:0] b;
        b[AW] = g[AW];
        for (int i = int'(AW) - 1; i >= 0; i--) b[i] = b[i+1] ^ g[i];
        return b;
    endfunction

    logic [W-1:0] mem [0:(2**AW)-1];

    logic [AW:0] wbin, wgray, rgray_w1, rgray_w2;       // write side
    logic [AW:0] rbin, rgray, wgray_r1, wgray_r2;       // read side
    logic [AW:0] wbin_r, level;
    logic        wfull, rempty, running;
    logic        wovf, warmed, ovf_r1, ovf_r2, rslip;

    // ---------------------------------------------------------------- write side
    assign wfull = (wgray == {~rgray_w2[AW:AW-1], rgray_w2[AW-2:0]});

    always_ff @(posedge wclk) begin
        if (we && !wfull) mem[wbin[AW-1:0]] <= wdata;
    end
    always_ff @(posedge wclk) begin
        if (wrst) begin
            wbin <= '0; wgray <= '0; rgray_w1 <= '0; rgray_w2 <= '0;
            wovf <= 1'b0; warmed <= 1'b0;
        end else begin
            rgray_w1 <= rgray;
            rgray_w2 <= rgray_w1;
            if (rgray_w2 != '0)            warmed <= 1'b1;   // the reader has popped
            if (we && wfull && warmed)     wovf   <= 1'b1;
            if (we && !wfull) begin
                wbin  <= wbin + 1'b1;
                wgray <= bin2gray(wbin + 1'b1);
            end
        end
    end

    // ---------------------------------------------------------------- read side
    assign wbin_r = gray2bin(wgray_r2);
    assign level  = wbin_r - rbin;
    assign rempty = (wgray_r2 == rgray);

    always_ff @(posedge rclk) begin
        if (rrst) begin
            rbin <= '0; rgray <= '0; wgray_r1 <= '0; wgray_r2 <= '0;
            running <= 1'b0; rvalid <= 1'b0; rslip <= 1'b0; ovf_r1 <= 1'b0; ovf_r2 <= 1'b0;
        end else begin
            wgray_r1 <= wgray;
            wgray_r2 <= wgray_r1;
            ovf_r1   <= wovf;
            ovf_r2   <= ovf_r1;
            rvalid   <= 1'b0;
            if (!running) begin
                if (level >= (AW+1)'(2**(AW-1))) running <= 1'b1;
            end else if (rempty) begin
                running <= 1'b0;
                rslip   <= 1'b1;
            end else begin
                rvalid <= 1'b1;
                rbin   <= rbin + 1'b1;
                rgray  <= bin2gray(rbin + 1'b1);
            end
        end
    end
    always_ff @(posedge rclk) rdata <= mem[rbin[AW-1:0]];

    assign slip = rslip || ovf_r2;
endmodule
`default_nettype wire
