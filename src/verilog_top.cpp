#include "verilog.hpp"

#include <sstream>

namespace mdr::rtl {

std::string top(const PipelineLayout& layout) {
    std::ostringstream out;
    out << "`include \"Promise.vh\"\n\n"
        << "module mips(input clk, reset);\n"
        << "    localparam PORTS = " << layout.rfPorts
        << ", DE_BITS = " << layout.deBits << ", EM_BITS = " << layout.emBits << ";\n";
    out << R"RTL(    localparam RF_BITS = PORTS * 38;
    reg [31:0] pc;
    wire [31:0] ins, D_IR, D_PC, E_IR, E_PC, M_IR, M_PC, W_PC;
    wire D_valid, E_valid, M_valid;
    wire [31:0] req_d, req_e, req_m;
    wire [`MDR_PW-1:0] D_promise, E_promise, M_promise, W_promise, next_M, next_W;
    wire [DE_BITS-1:0] D_payload, E_payload;
    wire [EM_BITS-1:0] E_next_payload, M_payload;
    wire [RF_BITS-1:0] D_operands, E_operands, E_next_operands, M_operands;
    wire [31:0] D_dest, E_dest, M_dest, DM_addr, DM_WD, DM_RD;
    wire D_jump, E_jump, M_jump, D_annul, E_annul, M_annul;
    wire D_may_control_later, E_may_control_later, mem_we, stall;
    reg waiting_E, waiting_M, pending_annul;

    // A late branch advances while PC holds the address of its delay slot.
    // Only the stage that owns that held slot can redirect or annul it.
    wire D_wait = !stall && D_may_control_later && !D_jump;
    wire E_wait = waiting_E && E_may_control_later && !E_jump;
    wire control_wait = D_wait || E_wait;
    wire jump_en = waiting_M ? M_jump : waiting_E ? E_jump : D_jump;
    wire [31:0] jump_addr = waiting_M ? M_dest : waiting_E ? E_dest : D_dest;
    wire annul_en = waiting_M ? pending_annul || M_annul :
                    waiting_E ? pending_annul || E_annul : D_annul;
    wire [31:0] npc = jump_en ? jump_addr : pc + 32'd4;
    wire [PORTS*5-1:0] read_addr;
    wire [PORTS*32-1:0] read_data;
    wire [4:0] write_addr;

    FDreg FDreg(.clk(clk), .reset(reset), .stall(stall), .annul(control_wait || annul_en),
                .F_IR(ins), .F_PC(pc), .D_valid(D_valid), .D_IR(D_IR), .D_PC(D_PC));
    DEreg #(.PAYLOAD_BITS(DE_BITS), .RF_BITS(RF_BITS)) DEreg(
                .clk(clk), .reset(reset), .stall(stall),
                .D_IR(D_IR), .D_PC(D_PC), .D_valid(D_valid), .D_promise(D_promise),
                .D_payload(D_payload), .D_operands(D_operands),
                .E_IR(E_IR), .E_PC(E_PC), .E_valid(E_valid), .E_promise(E_promise),
                .E_payload(E_payload), .E_operands(E_operands));
    EMreg #(.PAYLOAD_BITS(EM_BITS), .RF_BITS(RF_BITS)) EMreg(
                .clk(clk), .reset(reset),
                .E_IR(E_IR), .E_PC(E_PC), .E_valid(E_valid), .E_promise(next_M),
                .E_payload(E_next_payload), .E_operands(E_next_operands),
                .M_IR(M_IR), .M_PC(M_PC), .M_valid(M_valid), .M_promise(M_promise),
                .M_payload(M_payload), .M_operands(M_operands));
    MWreg MWreg(.clk(clk), .reset(reset), .M_PC(M_PC), .M_promise(next_W),
                .W_PC(W_PC), .W_promise(W_promise));

    DCtrl DCtrl(.valid(D_valid), .IR(D_IR), .PC(D_PC),
                .ep(E_promise), .mp(M_promise), .wp(W_promise),
                .read_addr(read_addr), .read_data(read_data),
                .req_d(req_d), .req_e(req_e), .req_m(req_m),
                .next_de_payload(D_payload), .d_rf_next(D_operands),
                .promise_out(D_promise),
                .do_jump(D_jump), .do_annul(D_annul), .dest(D_dest),
                .may_control_later(D_may_control_later));
    ECtrl ECtrl(.valid(E_valid), .IR(E_IR), .PC(E_PC),
                .ep(E_promise), .mp(M_promise), .wp(W_promise),
                .de_payload(E_payload), .e_rf(E_operands),
                .next_em_payload(E_next_payload), .e_rf_next(E_next_operands),
                .promise_out(next_M), .do_jump(E_jump), .do_annul(E_annul), .dest(E_dest),
                .may_control_later(E_may_control_later));
    MCtrl MCtrl(.valid(M_valid), .IR(M_IR), .PC(M_PC),
                .mp(M_promise), .wp(W_promise), .word(DM_RD),
                .em_payload(M_payload), .m_rf(M_operands),
                .promise_out(next_W), .addr(DM_addr), .data(DM_WD), .store(mem_we),
                .do_jump(M_jump), .do_annul(M_annul), .dest(M_dest));
    STALL STALL(.req_d(req_d), .req_e(req_e), .req_m(req_m),
                .ep(E_promise), .mp(M_promise), .wp(W_promise), .stall(stall));

    // Decode the final one-hot bitmap for the RF write port.
    function [4:0] destination;
        input [31:0] bitmap;
        integer r;
        begin
            destination = 0;
            for (r = 31; r > 0; r = r - 1)
                if (bitmap[r]) destination = r[4:0];
        end
    endfunction
    assign write_addr = destination(W_promise[`MDR_BITMAP]);

    IM instruction_memory(.pc(pc), .instr(ins));
    DM data_memory(.clk(clk), .reset(reset), .we(mem_we), .addr(DM_addr),
                   .wd(DM_WD), .this_PC(M_PC), .rd(DM_RD));
    RF #(.PORTS(PORTS)) registers(.clk(clk), .reset(reset), .we(W_promise[`MDR_READY]),
                   .read_addr(read_addr), .read_data(read_data), .wa(write_addr),
                   .wd(W_promise[`MDR_DATA]), .this_PC(W_PC));

    always @(posedge clk) begin
        if (reset) begin
            pc <= 32'h0000_3000;
            waiting_E <= 0;
            waiting_M <= 0;
            pending_annul <= 0;
        end else begin
            if (!stall && !control_wait) pc <= npc;
            waiting_E <= D_wait;
            waiting_M <= E_wait;
            pending_annul <= control_wait && annul_en;
        end
    end
endmodule
)RTL";
    return out.str();
}

} // namespace mdr::rtl

