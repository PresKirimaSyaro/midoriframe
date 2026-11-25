module DM(
    input clk, reset, we,
    input [31:0] addr, wd, this_PC,
    output [31:0] rd
);
    reg [31:0] mem [0:3071];
    wire [11:0] index = addr[13:2];
    integer i;

    assign rd = mem[index];

    always @(posedge clk) begin
        if (reset) begin
            for (i = 0; i < 3072; i = i + 1)
                mem[i] <= 32'd0;
        end else if (we) begin
            mem[index] <= wd;
            $display("%d@%h: *%h <= %h", $time, this_PC, {addr[31:2], 2'b00}, wd);
        end
    end
endmodule

