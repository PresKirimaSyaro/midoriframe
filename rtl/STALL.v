`include "Promise.vh"

module STALL(
    input [31:0] req_d, req_e, req_m,
    input [`MDR_PW-1:0] ep, mp, wp,
    output stall
);
    function check;
        input [4:0] number;
        input [1:0] tuse;
        input [`MDR_PW-1:0] e, m, w;
        begin
            if (e[64+number]) check = e[`MDR_LAG] > tuse;
            else if (m[64+number]) check = m[`MDR_LAG] > tuse;
            else if (w[64+number]) check = w[`MDR_LAG] > tuse;
            else check = 1'b0;
        end
    endfunction

    wire [31:1] waiting;
    genvar r;
    generate
        for (r = 1; r < 32; r = r + 1) begin: raw_dependency
            localparam [4:0] NUMBER = r;
            wire [1:0] tuse = req_d[r] ? 2'd0 : req_e[r] ? 2'd1 : 2'd2;
            assign waiting[r] = (req_d[r] | req_e[r] | req_m[r]) && check(NUMBER, tuse, ep, mp, wp);
        end
    endgenerate
    assign stall = |waiting;
endmodule

