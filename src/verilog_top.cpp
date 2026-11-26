#include "verilog.hpp"

#include <sstream>

namespace mdr::rtl {

std::string top(const PipelineLayout& layout) {
    std::ostringstream out;
    out << "`include \"Promise.vh\"\n\n"
        << "module mips(input clk, reset);\n"
        << "    localparam OP_BITS = " << layout.opBits << ", PORTS = " << layout.rfPorts
        << ", DE_BITS = " << layout.deBits << ", EM_BITS = " << layout.emBits << ";\n";
    out << R"RTL(    localparam RF_BITS = PORTS * 38;
    reg [31:0] pc;
    wire [31:0] ins, D_IR, D_PC, E_IR, E_PC, M_IR, M_PC, W_PC;
    wire D_valid;
    wire [OP_BITS-1:0] D_opcode, E_opcode, M_opcode;
    wire [`MDR_PW-1:0] D_promise, E_promise, M_promise, W_promise, next_M, next_W;
    wire [DE_BITS-1:0] D_payload, E_payload;
    wire [EM_BITS-1:0] E_next_payload, M_payload;
    wire [RF_BITS-1:0] D_operands, E_operands, E_next_operands, M_operands;
    wire [31:0] jump_addr, DM_addr, DM_WD, DM_RD;
    wire jump_en, annul_en, mem_we, stall;
    wire [31:0] npc = jump_en ? jump_addr : annul_en ? D_PC + 32'd8 : pc + 32'd4;
    wire [PORTS*5-1:0] read_addr;
    wire [PORTS*32-1:0] read_data;
    wire [4:0] write_addr;

    FDreg FDreg(.clk(clk), .reset(reset), .stall(stall), .annul(annul_en),
                .F_IR(ins), .F_PC(pc), .D_valid(D_valid), .D_IR(D_IR), .D_PC(D_PC));
    DEreg #(.OP_BITS(OP_BITS), .PAYLOAD_BITS(DE_BITS), .RF_BITS(RF_BITS)) DEreg(
                .clk(clk), .reset(reset), .stall(stall),
                .D_IR(D_IR), .D_PC(D_PC), .D_opcode(D_opcode), .D_promise(D_promise),
                .D_payload(D_payload), .D_operands(D_operands),
                .E_IR(E_IR), .E_PC(E_PC), .E_opcode(E_opcode), .E_promise(E_promise),
                .E_payload(E_payload), .E_operands(E_operands));
    EMreg #(.OP_BITS(OP_BITS), .PAYLOAD_BITS(EM_BITS), .RF_BITS(RF_BITS)) EMreg(
                .clk(clk), .reset(reset),
                .E_IR(E_IR), .E_PC(E_PC), .E_opcode(E_opcode), .E_promise(next_M),
                .E_payload(E_next_payload), .E_operands(E_next_operands),
                .M_IR(M_IR), .M_PC(M_PC), .M_opcode(M_opcode), .M_promise(M_promise),
                .M_payload(M_payload), .M_operands(M_operands));
    MWreg MWreg(.clk(clk), .reset(reset), .M_PC(M_PC), .M_promise(next_W),
                .W_PC(W_PC), .W_promise(W_promise));

    Controller Controller(.d_valid(D_valid),
                .d_opcode(D_opcode), .e_opcode(E_opcode), .m_opcode(M_opcode),
                .d_ins(D_IR), .e_ins(E_IR), .m_ins(M_IR),
                .d_pc(D_PC), .e_pc(E_PC), .m_pc(M_PC), .word(DM_RD),
                .ep(E_promise), .mp(M_promise), .wp(W_promise),
                .read_addr(read_addr), .read_data(read_data),
                .e_rf(E_operands), .m_rf(M_operands), .d_rf_next(D_operands), .e_rf_next(E_next_operands),
                .de_payload(E_payload), .em_payload(M_payload),
                .next_de_payload(D_payload), .next_em_payload(E_next_payload),
                .stall(stall), .write_addr(write_addr),
                .out_d(D_promise), .out_e(next_M), .out_m(next_W),
                .do_jump(jump_en), .do_annul(annul_en), .dest(jump_addr),
                .addr(DM_addr), .data(DM_WD), .store(mem_we));

    IM instruction_memory(.pc(pc), .instr(ins));
    DM data_memory(.clk(clk), .reset(reset), .we(mem_we), .addr(DM_addr),
                   .wd(DM_WD), .this_PC(M_PC), .rd(DM_RD));
    RF #(.PORTS(PORTS)) registers(.clk(clk), .reset(reset), .we(W_promise[`MDR_READY]),
                   .read_addr(read_addr), .read_data(read_data), .wa(write_addr),
                   .wd(W_promise[`MDR_DATA]), .this_PC(W_PC));

    always @(posedge clk) begin
        if (reset) pc <= 32'h0000_3000;
        else if (!stall) pc <= npc;
    end
endmodule
)RTL";
    return out.str();
}

} // namespace mdr::rtl

