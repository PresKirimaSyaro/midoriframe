module RF #(parameter PORTS = 1)(
    input clk, reset, we,
    input [PORTS*5-1:0] read_addr,
    output [PORTS*32-1:0] read_data,
    input [4:0] wa,
    input [31:0] wd, this_PC
);
    reg [31:0] r [0:31];
    integer i;
    genvar p;
    generate
        for (p = 0; p < PORTS; p = p + 1) begin: reads
            wire [4:0] addr = read_addr[5*p +: 5];
            assign read_data[32*p +: 32] = addr == 0 ? 32'd0 : r[addr];
        end
    endgenerate

    always @(posedge clk) begin
        if (reset) begin
            for (i = 0; i < 32; i = i + 1)
                r[i] <= 32'd0;
        end else if (we && wa != 0) begin
            r[wa] <= wd;
            $display("%d@%h: $%d <= %h", $time, this_PC, wa, wd);
        end
    end
endmodule

