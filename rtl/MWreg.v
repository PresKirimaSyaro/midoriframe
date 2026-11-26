`include "Promise.vh"

module MWreg(
    input clk, reset,
    input [31:0] M_PC,
    input [`MDR_PW-1:0] M_promise,
    output reg [31:0] W_PC,
    output reg [`MDR_PW-1:0] W_promise
);
    always @(posedge clk) begin
        if (reset) begin
            W_PC <= 0;
            W_promise <= 0;
        end else begin
            W_PC <= M_PC;
            W_promise <= M_promise;
        end
    end
endmodule

