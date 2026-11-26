`include "Promise.vh"

module DEreg #(parameter PAYLOAD_BITS = 1, RF_BITS = 38)(
    input clk, reset, stall,
    input [31:0] D_IR, D_PC,
    input D_valid,
    input [`MDR_PW-1:0] D_promise,
    input [PAYLOAD_BITS-1:0] D_payload,
    input [RF_BITS-1:0] D_operands,
    output reg [31:0] E_IR, E_PC,
    output reg E_valid,
    output reg [`MDR_PW-1:0] E_promise,
    output reg [PAYLOAD_BITS-1:0] E_payload,
    output reg [RF_BITS-1:0] E_operands
);
    always @(posedge clk) begin
        if (reset || stall) begin
            E_IR <= 0;
            E_PC <= 0;
            E_valid <= 0;
            E_promise <= 0;
            E_payload <= 0;
            E_operands <= 0;
        end else begin
            E_IR <= D_IR;
            E_PC <= D_PC;
            E_valid <= D_valid;
            E_promise <= D_promise;
            E_payload <= D_payload;
            E_operands <= D_operands;
        end
    end
endmodule

