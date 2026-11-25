#include "verilog.hpp"

#include <sstream>

namespace mdr::rtl {

std::string top(unsigned instructionCount, unsigned ports) {
    std::ostringstream out;
    out << "`include \"Promise.vh\"\n\n"
        << "module mips(input clk, reset);\n"
        << "    localparam N = " << instructionCount << ", PORTS = " << ports << ";\n";
    out << R"RTL(    reg [31:0] pc, D_IR, D_PC, E_IR, E_PC, M_IR, M_PC, W_PC;
    reg D_valid;
    wire [31:0] ins;
    wire [N-1:0] matches, D_select;
    reg [N-1:0] E_select, M_select;
    reg [`MDR_PW-1:0] E_promise, M_promise, W_promise;

    wire [`MDR_PW-1:0] d_result [0:N-1];
    wire [`MDR_PW-1:0] e_result [0:N-1];
    wire [`MDR_PW-1:0] m_result [0:N-1];
    wire [31:0] request_d [0:N-1], request_e [0:N-1], request_m [0:N-1];
    wire [31:0] jump_addr [0:N-1], mem_addr [0:N-1], mem_data [0:N-1];
    wire [N-1:0] jump_en, annul_en, mem_we;

    reg [`MDR_PW-1:0] next_E, next_M, next_W;
    reg [31:0] use_D, use_E, use_M;
    reg [31:0] npc, DM_addr, DM_WD;
    wire [31:0] DM_RD;
    reg [PORTS*5-1:0] read_addr;
    wire [PORTS*32-1:0] read_data;
    reg stall;
    reg [4:0] write_addr;
    integer lane, r, port, k;

    IM instruction_memory(.pc(pc), .instr(ins));
    DM data_memory(.clk(clk), .reset(reset), .we(|mem_we), .addr(DM_addr),
                   .wd(DM_WD), .this_PC(M_PC), .rd(DM_RD));
    RF #(.PORTS(PORTS)) registers(.clk(clk), .reset(reset), .we(W_promise[`MDR_READY]),
                   .read_addr(read_addr), .read_data(read_data), .wa(write_addr),
                   .wd(W_promise[`MDR_DATA]), .this_PC(W_PC));

    always @* begin
        write_addr = 5'd0;
        for (k = 31; k > 0; k = k - 1)
            if (W_promise[32+k]) write_addr = k[4:0];
    end

    always @* begin
        next_E = 0;
        next_M = 0;
        next_W = 0;
        use_D = 0;
        use_E = 0;
        use_M = 0;
        npc = |annul_en ? D_PC + 32'd8 : pc + 32'd4;
        DM_addr = 0;
        DM_WD = 0;
        for (lane = 0; lane < N; lane = lane + 1) begin
            next_E = next_E | d_result[lane];
            next_M = next_M | e_result[lane];
            next_W = next_W | m_result[lane];
            use_D = use_D | request_d[lane];
            use_E = use_E | request_e[lane];
            use_M = use_M | request_m[lane];
            if (jump_en[lane]) npc = jump_addr[lane];
            DM_addr = DM_addr | mem_addr[lane];
            DM_WD = DM_WD | mem_data[lane];
        end
    end

    function hazard;
        input [4:0] number;
        input [1:0] tuse;
        input [`MDR_PW-1:0] e, m, w;
        begin
            if (e[64+number]) hazard = e[`MDR_LAG] > tuse;
            else if (m[64+number]) hazard = m[`MDR_LAG] > tuse;
            else if (w[64+number]) hazard = w[`MDR_LAG] > tuse;
            else hazard = 1'b0;
        end
    endfunction

    always @* begin
        read_addr = 0;
        stall = 0;
        port = 0;
        for (r = 1; r < 32; r = r + 1) begin
            if (use_D[r] || use_E[r] || use_M[r]) begin
                if (port < PORTS) read_addr[5*port +: 5] = r[4:0];
                port = port + 1;
                stall = stall | hazard(r[4:0], use_D[r] ? 2'd0 : use_E[r] ? 2'd1 : 2'd2,
                                       E_promise, M_promise, W_promise);
            end
        end
    end

    always @(posedge clk) begin
        if (reset) begin
            pc <= 32'h0000_3000;
            D_valid <= 0;
            D_IR <= 0;
            D_PC <= 0;
            E_IR <= 0;
            E_PC <= 0;
            M_IR <= 0;
            M_PC <= 0;
            W_PC <= 0;
            E_select <= 0;
            M_select <= 0;
            E_promise <= 0;
            M_promise <= 0;
            W_promise <= 0;
        end else begin
            if (!stall) begin
                pc <= npc;
                D_valid <= ~|annul_en;
                D_IR <= ins;
                D_PC <= pc;
            end
            E_select <= stall ? {N{1'b0}} : D_select;
            E_promise <= stall ? 0 : next_E;
            E_IR <= D_IR;
            E_PC <= D_PC;
            M_select <= E_select;
            M_promise <= next_M;
            M_IR <= E_IR;
            M_PC <= E_PC;
            W_promise <= next_W;
            W_PC <= M_PC;
        end
    end

)RTL";
    for (unsigned i = 0; i < instructionCount; ++i) {
        out << "    assign D_select[" << i << "] = D_valid && matches[" << i << ']';
        if (i == 1) out << " && !matches[0]";
        if (i > 1) out << " && !(|matches[" << i - 1 << ":0])";
        out << ";\n";
        out << "    mdr_instruction_" << i << " instruction_" << i << "(\n"
            << "        .clk(clk), .d_on(D_select[" << i << "]), .e_on(E_select[" << i
            << "]), .m_on(M_select[" << i << "]),\n"
            << "        .d_ins(D_IR), .e_ins(E_IR), .m_ins(M_IR), .d_pc(D_PC), .e_pc(E_PC), .m_pc(M_PC),\n"
            << "        .ep(E_promise), .mp(M_promise), .wp(W_promise), .word(DM_RD),\n"
            << "        .read_addr(read_addr), .read_data(read_data), .matched(matches[" << i << "]),\n"
            << "        .req_d(request_d[" << i << "]), .req_e(request_e[" << i << "]), .req_m(request_m[" << i
            << "]),\n"
            << "        .out_d(d_result[" << i << "]), .out_e(e_result[" << i << "]), .out_m(m_result[" << i
            << "]),\n"
            << "        .do_jump(jump_en[" << i << "]), .do_annul(annul_en[" << i << "]), .dest(jump_addr[" << i
            << "]),\n"
            << "        .addr(mem_addr[" << i << "]), .data(mem_data[" << i << "]), .store(mem_we[" << i << "])\n"
            << "    );\n";
    }
    out << "endmodule\n";
    return out.str();
}

} // namespace mdr::rtl

