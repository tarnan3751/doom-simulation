`default_nettype none
// MMIO slave for the non-RAM half of the CPU data port (ARCHITECTURE.md sections 3 and 7).
// Every accepted request is performed at the accept edge; rsp_ready is 1 from the first
// cycle after reset and rsp_rdata is a register that only changes at an accept edge.
module mmio #(
    parameter bit          FAST     = 1'b0,
    parameter int unsigned CLK_HZ   = 50_000_000,
    parameter int unsigned UART_DIV = 434
) (
    input  logic        clk,
    input  logic        rst,

    // data port slave (section 3), MMIO range
    input  logic        req_valid,
    input  logic        req_we,
    // verilator lint_off UNUSEDSIGNAL
    input  logic [31:0] req_addr,      // [1:0] unused: registers are word-aligned
    // verilator lint_on UNUSEDSIGNAL
    input  logic [3:0]  req_wstrb,
    input  logic [31:0] req_wdata,
    output logic        rsp_ready,
    output logic [31:0] rsp_rdata,

    // board side
    output logic        uart_tx,
    input  logic        key_valid,
    input  logic [8:0]  key_data,
    output logic        vid_pix_ce,
    output logic        vid_hsync,
    output logic        vid_vsync,
    output logic        vid_de,
    output logic [7:0]  vid_r,
    output logic [7:0]  vid_g,
    output logic [7:0]  vid_b,

    // observation
    output logic        halted,
    output logic [31:0] exit_code,
    output logic        vid_flip,
    output logic        uart_tx_valid,
    output logic [7:0]  uart_tx_byte
);
    logic ready_q;
    logic [31:0] rdata_q;
    assign rsp_ready = ready_q;
    assign rsp_rdata = rdata_q;

    wire acc = req_valid && ready_q;
    wire wr  = acc && req_we;
    wire rd  = acc && !req_we;

    // ---- decode ------------------------------------------------------------
    wire        io_blk = (req_addr[31:16] == 16'h1000);
    wire        sel_uart = io_blk && (req_addr[15:12] == 4'h0);
    wire        sel_sys  = io_blk && (req_addr[15:12] == 4'h1);
    wire        sel_kbd  = io_blk && (req_addr[15:12] == 4'h2);
    wire        sel_vid  = io_blk && (req_addr[15:12] == 4'h3);
    wire        sel_fb   = (req_addr[31:16] == 16'h1001);
    wire [9:0]  off      = req_addr[11:2];

    // ---- UART --------------------------------------------------------------
    logic u_ready, u_accepted;
    uart_tx #(.UART_DIV(UART_DIV)) u_uart (
        .clk(clk), .rst(rst),
        .wr_en(wr && sel_uart && off == 10'd0), .wr_data(req_wdata[7:0]),
        .tx(uart_tx), .ready(u_ready), .accepted(u_accepted)
    );
    always_ff @(posedge clk) begin
        if (rst) begin
            uart_tx_valid <= 1'b0;
            uart_tx_byte  <= 8'h00;
        end else begin
            uart_tx_valid <= u_accepted;
            if (u_accepted) uart_tx_byte <= req_wdata[7:0];
        end
    end

    // ---- system control ----------------------------------------------------
    logic [31:0] sys_rd;
    sysctl #(.FAST(FAST), .CLK_HZ(CLK_HZ)) u_sys (
        .clk(clk), .rst(rst), .wr_en(wr && sel_sys), .off(off), .wdata(req_wdata),
        .rdata(sys_rd), .halted(halted), .exit_code(exit_code)
    );

    // ---- keyboard ----------------------------------------------------------
    logic [31:0] kbd_rd;
    kbd u_kbd (
        .clk(clk), .rst(rst), .key_valid(key_valid), .key_data(key_data),
        .rd_en(rd && sel_kbd), .off(off), .rdata(kbd_rd)
    );

    // ---- video -------------------------------------------------------------
    logic [31:0] vid_rd;
    video #(.FAST(FAST)) u_video (
        .clk(clk), .rst(rst),
        .ctl_wr(wr && sel_vid), .ctl_off(off), .ctl_rdata(vid_rd),
        .fb_wr(wr && sel_fb), .fb_widx(req_addr[15:2]),
        .wstrb(req_wstrb), .wdata(req_wdata),
        .vid_flip(vid_flip),
        .vid_pix_ce(vid_pix_ce), .vid_hsync(vid_hsync), .vid_vsync(vid_vsync),
        .vid_de(vid_de), .vid_r(vid_r), .vid_g(vid_g), .vid_b(vid_b)
    );

    // ---- read mux ----------------------------------------------------------
    logic [31:0] rd_mux;
    always_comb begin
        rd_mux = 32'b0;
        if (sel_uart && off == 10'd1) rd_mux = {31'b0, u_ready};
        if (sel_sys)                  rd_mux = sys_rd;
        if (sel_kbd)                  rd_mux = kbd_rd;
        if (sel_vid)                  rd_mux = vid_rd;
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            ready_q <= 1'b0;
            rdata_q <= 32'b0;
        end else begin
            ready_q <= 1'b1;
            if (acc) rdata_q <= req_we ? 32'b0 : rd_mux;
        end
    end
endmodule
`default_nettype wire
