`include "Promise.vh"

module EMreg #(parameter OP_BITS = 1, PAYLOAD_BITS = 1, RF_BITS = 38)(
    input clk, reset,
    input [31:0] E_IR, E_PC,
    input [OP_BITS-1:0] E_opcode,
    input [`MDR_PW-1:0] E_promise,
    input [PAYLOAD_BITS-1:0] E_payload,
    input [RF_BITS-1:0] E_operands,
    output reg [31:0] M_IR, M_PC,
    output reg [OP_BITS-1:0] M_opcode,
    output reg [`MDR_PW-1:0] M_promise,
    output reg [PAYLOAD_BITS-1:0] M_payload,
    output reg [RF_BITS-1:0] M_operands
);
    always @(posedge clk) begin
        if (reset) begin
            M_IR <= 0;
            M_PC <= 0;
            M_opcode <= 0;
            M_promise <= 0;
            M_payload <= 0;
            M_operands <= 0;
        end else begin
            M_IR <= E_IR;
            M_PC <= E_PC;
            M_opcode <= E_opcode;
            M_promise <= E_promise;
            M_payload <= E_payload;
            M_operands <= E_operands;
        end
    end
endmodule

