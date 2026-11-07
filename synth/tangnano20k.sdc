# Clock constraints for tangnano20k_top (nextpnr-himbaechel generic SDC).
#
# nextpnr does not propagate frequencies through the rPLLs/CLKDIV, so every
# generated clock is declared on its net (names from rtl/fpga/tangnano20k_top.sv).
# Periods are the exact PLL outputs (tn20k_clocks.sv):
#   clk_sys   50.4 MHz  (SoC; also the --freq default)
#   clk_sdram 50.4 MHz  (only clocks the ODDR that forwards it to O_sdram_clk)
#   clk_pix   25.2 MHz  (TMDS encoders, FIFO read side)
#   clk_ser  126.0 MHz  (only OSER10 FCLK and CLKDIV: no fabric paths)
#
# Clock-domain crossings (clk_sys <-> clk_pix, only inside tn20k_vid_cdc) are
# asynchronous by design. This nextpnr build parses set_false_path but ignores it
# ("does not do anything (yet)"), so none is written here; nextpnr reports those
# paths separately as cross-domain paths and does not count them in either
# clock's Fmax (synth/README.md, "Timing settings").
create_clock -period 19.841 -name clk_sys   [get_nets clk_sys]
create_clock -period 19.841 -name clk_sdram [get_nets clk_sdram]
create_clock -period 39.683 -name clk_pix   [get_nets clk_pix]
create_clock -period 7.937  -name clk_ser   [get_nets clk_ser]
