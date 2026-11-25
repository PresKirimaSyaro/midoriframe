module IM(input [31:0] pc, output [31:0] instr);
    reg [31:0] codes [0:4095];
    wire [31:0] offset = pc - 32'h0000_3000;
    integer i;

    initial begin
        for (i = 0; i < 4096; i = i + 1)
            codes[i] = 32'd0;
        $readmemh("code.txt", codes);
    end

    assign instr = codes[offset[13:2]];
endmodule

