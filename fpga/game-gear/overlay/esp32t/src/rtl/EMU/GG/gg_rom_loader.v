// Header validation and MiSTer ROM download replay through the real RAM-cart cache.
// The synchronous 2:1 clocks share a PLL: ramcart's address-qualified stall is
// asserted combinationally on an address change, and drops only after its hclk read.
module gg_rom_loader(
    input clk, reset, input [7:0] q, input stall,
    input [23:0] rom_a, input rom_rd, output [21:0] address, output rd,
    output reg loaded = 0, output bad, output [7:0] rom_do,
    output reg [24:0] dl_a = 0, output reg [7:0] dl_d = 0,
    output reg dl_en = 0,
    // CG-117 diag (gg13diag): settle = wait 15 clocks after the stall drops before a byte is taken;
    // bad_idx / bad_val = the first marker byte that was read wrong (15 = none) and what was read
    input settle, output reg [3:0] bad_idx = 4'd15, output reg [7:0] bad_val = 8'd0,
    // gg14diag: the 12 header bytes as they were read; the clocks the first byte was stalled (255 = more)
    output reg [95:0] hdr_rd = 96'd0, output reg [7:0] stall_n = 8'd0
);
    localparam [63:0] MAGIC = 64'h314d4f5248434747;
    localparam HEADER=0, VALIDATE=1, REPLAY=2, POST=3, RUN=4, BAD=5;
    reg [2:0] state = HEADER;
    reg [3:0] index = 0;
    reg [63:0] magic = 0;
    reg [31:0] size = 0;
    reg [23:0] offset = 0, mask = 0, mask512 = 0;
    reg header512 = 0;
    reg [6:0] gap = 16;
    function [23:0] smear;
        input [23:0] v;
        reg [23:0] w;
        begin w=v|(v>>1); w=w|(w>>2); w=w|(w>>4); w=w|(w>>8); smear=w|(w>>16); end
    endfunction
    wire [23:0] mapped = header512 ? ((rom_a + 24'd512) & mask512) : (rom_a & mask);
    assign address = state == RUN ? (22'h008000 + mapped[21:0]) :
                     state == REPLAY ? (22'h008000 + offset[21:0]) : (22'h150 + {18'd0,index});
    // CG-106: while the game runs only the core's real ROM reads reach the cache (rom_rd = RD and MREQ below
    // C000). With rd always on, RAM, stack and refresh addresses were looked up as well and filled lines.
    assign rd = !reset && (state == HEADER || state == REPLAY || (state == RUN && rom_rd));
    assign rom_do = q;
    assign bad = state == BAD;
    always @(posedge clk) begin
        dl_en <= 0;
        if (state == HEADER && index == 0 && stall && stall_n != 8'd255) stall_n <= stall_n + 1'b1;
        if (reset) begin
            loaded <= 0; state <= HEADER; index <= 0; magic <= 0;
            size <= 0; offset <= 0; gap <= 16; dl_a <= 0; dl_d <= 0; bad_idx <= 4'd15; bad_val <= 8'd0;
            hdr_rd <= 96'd0; stall_n <= 8'd0;
        end else if (settle && stall && (state == HEADER || state == REPLAY)) gap <= 7'd15;
        else if (gap != 0) gap <= gap - 1'b1;
        else case (state)
            HEADER: if (!stall) begin
                if (index < 12) hdr_rd[8*index +: 8] <= q;
                if (index < 8) magic <= {q,magic[63:8]};
                if (index < 8 && bad_idx == 4'd15 && q != MAGIC[8*index[2:0] +: 8]) begin bad_idx <= index; bad_val <= q; end
                else size <= {q,size[31:8]};
                index <= index + 1'b1; gap <= 15;
                if (index == 11) state <= VALIDATE;
            end
            VALIDATE: begin
                // Linear port is 4 MiB including the prefix. Reject overflow and empty images.
                if (magic != 64'h314d4f5248434747 || size < 1024 || size > 32'h3f8000) state <= BAD;
                else begin
                    mask <= smear(size[23:0]-1'b1);
                    mask512 <= smear(size[23:0]-24'd513);
                    header512 <= size[9:0] == 10'd512;
                    state <= REPLAY;
                end
            end
            REPLAY: if (!stall) begin
                dl_en <= 1; dl_a <= {1'b0,offset}; dl_d <= q;
                offset <= offset + 1'b1; gap <= 15;
                if (offset + 1'b1 == size[23:0]) begin state <= POST; gap <= 127; end
            end
            POST: begin loaded <= 1; state <= RUN; end
            default: ;
        endcase
    end
endmodule
