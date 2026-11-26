module FDreg(
    input clk, reset, stall, annul,
    input [31:0] F_IR, F_PC,
    output reg D_valid,
    output reg [31:0] D_IR, D_PC
);
    always @(posedge clk) begin
        if (reset) begin
            D_valid <= 0;
            D_IR <= 0;
            D_PC <= 0;
        end else if (!stall) begin
            D_valid <= !annul;
            D_IR <= annul ? 0 : F_IR;
            D_PC <= annul ? 0 : F_PC;
        end
    end
endmodule

