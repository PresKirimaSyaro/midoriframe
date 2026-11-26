#include "verilog.hpp"
#include "constant.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

namespace mdr::rtl {
namespace {

std::string bits(Type type) {
    return std::string(type.isSigned ? "signed " : "") + "[" + std::to_string(type.width - 1) + ":0] ";
}

std::string stageTag(Stage stage) {
    if (stage == Stage::D) return "d";
    if (stage == Stage::E) return "e";
    return "m";
}

std::string cast(const std::string& expr, bool sign) {
    return std::string(sign ? "$signed(" : "$unsigned(") + expr + ')';
}

std::string truth(const std::string& expr) {
    return "((|" + expr + ") === 1'b1)";
}

class SharedLogic {
public:
    std::string wire(Type type, const std::string& expression, const std::string& preferred = {}) {
        auto key = std::tuple{type.width, type.isSigned, expression};
        if (auto cached = wires_.find(key); cached != wires_.end()) return cached->second;
        const auto name = preferred.empty() ? "v" + std::to_string(++next_) : preferred;
        output_ << "    wire " << bits(type) << name << " = " << expression << ";\n";
        wires_.emplace(std::move(key), name);
        return name;
    }

    std::string str() const { return output_.str(); }

private:
    std::map<std::tuple<unsigned, bool, std::string>, std::string> wires_;
    unsigned next_ = 0;
    std::ostringstream output_;
};


struct InstructionCode {
    std::string name;
    std::string match;
    std::string localDeclarations;
    std::string procedures;
    std::string requests;
    std::string control;
    std::string memory;
    std::array<std::string, 3> promise;
    std::array<std::string, 2> payload;
    std::array<unsigned, 2> payloadBits{};
};

struct PromiseNames {
    explicit PromiseNames(Stage stage) : tag(stageTag(stage)) {}
    std::string tag;
    std::string state = "state_" + tag;
    std::string bitmap = "bitmap_" + tag;
    std::string data = "data_" + tag;
    std::string started = "started_" + tag;
    std::string yielded = "yielded_" + tag;
    std::string ready = "ready_" + tag;
    std::string pending = "pending_" + tag;
    std::string raw = "raw_" + tag;
    std::string lag = "lag_" + tag;
};

class InstructionEmitter {
public:
    InstructionEmitter(const InstructionIR& ir, SharedLogic& shared)
        : ir_(ir), prefix_("i_" + ir.name + "_"), shared_(shared), constants_(ir) {}

    InstructionCode run() {
        code_.name = ir_.name;
        code_.match = truth(get(ir_.match, Stage::D));
        requests();
        control();
        memory();
        for (auto stage : {Stage::D, Stage::E, Stage::M}) promise(stage);
        code_.localDeclarations = declarations_.str();
        code_.procedures = procedures_.str();
        return std::move(code_);
    }


private:
    const Value& node(ValueId id) const { return ir_.value(id); }

    std::string wire(Type type, const std::string& expression) {
        return shared_.wire(type, expression);
    }
    std::string carry(Type type, const std::string& expression, Stage destination) {
        const auto index = static_cast<unsigned>(destination) - 1;
        const auto bank = destination == Stage::E ? "de_payload" : "em_payload";
        const auto part = "[" + std::to_string(code_.payloadBits[index]) +
            " +: " + std::to_string(type.width) + "]";
        code_.payloadBits[index] += type.width;
        code_.payload[index] += "                next_" + std::string(bank) + part +
            " = " + expression + ";\n";
        return wire(type, std::string(bank) + part);
    }


    std::string get(ValueId id, Stage stage) { return get(id, stage, node(id).type); }

    std::string get(ValueId id, Stage stage, Type requested) {
        const auto& v = node(id);
        const auto width = std::max(requested.width, v.type.width);
        const Type effective{width, requested.isSigned};
        const auto key = std::tuple{id, stage, width, effective.isSigned};
        if (auto cached = values_.find(key); cached != values_.end()) return cached->second;

        std::string name;
        if (v.stage >= Stage::D && v.stage < stage && (v.sources & runtimeSources)) {
            if (width != v.type.width || effective.isSigned != v.type.isSigned) {
                name = wire(effective, cast(get(id, stage), effective.isSigned));
            } else {
                const auto previous = static_cast<Stage>(static_cast<int>(stage) - 1);
                name = carry(v.type, get(id, previous), stage);
            }
        } else {
            name = evaluate(id, stage, effective);
        }
        values_.emplace(key, name);
        return name;
    }

    std::string read(ValueId id, Stage stage) {
        const auto address = get(node(id).operands[0], stage);
        return wire({32}, "rf_lookup(" + address + ", " + stageTag(stage) + "_rf_values)");
    }
    std::string partRange(const Value& v, Stage stage) {
        const auto index = get(v.operands[1], stage);
        if (v.op == "bit") return '[' + index + ']';
        if (v.op == "slice" || v.detail == "-:")
            return '[' + index + " -: " + std::to_string(v.type.width) + ']';
        return '[' + index + " +: " + std::to_string(v.type.width) + ']';
    }

    std::string evaluate(ValueId id, Stage stage, Type type) {
        const auto& v = node(id);
        const auto& a = v.operands;
        auto self = [&](std::size_t operand) { return get(a[operand], stage); };
        auto sized = [&](std::size_t operand) { return get(a[operand], stage, type); };
        auto emit = [&](const std::string& expression) { return wire(type, expression); };

        if (v.op == "ins" || v.op == "pc") return emit(cast(stageTag(stage) + '_' + v.op, type.isSigned));
        if (v.op == "literal") return emit(cast(v.detail, type.isSigned));
        if (v.op == "undef") return emit(std::to_string(type.width) + "'bx");
        if (v.op == "grf.read") return emit(cast(read(id, stage), type.isSigned));
        if (v.op == "dm.read") {
            const auto address = self(0);
            std::string data = "word";
            if (v.detail == "1") data += "[{" + address + "[1:0], 3'b0} +: 8]";
            if (v.detail == "2") data += "[{" + address + "[1], 4'b0} +: 16]";
            return emit(cast(data, type.isSigned));
        }
        if (v.op == "align.word") {
            const auto data = self(0);
            return emit(cast("{" + data + "[31:2], 2'b0}", type.isSigned));
        }
        if (v.op == "fit") {
            const Type argType{v.type.width, node(a[0]).type.isSigned};
            const auto rhs = get(a[0], stage, argType);
            if (node(a[0]).type.width <= v.type.width && !argType.isSigned &&
                    type.width == v.type.width && !type.isSigned) return rhs;
            const auto raw = wire(v.type, rhs);
            return emit(cast(raw, type.isSigned));
        }
        if (v.op == "signed") return emit(cast(self(0), type.isSigned));
        if (v.op == "unary") {
            const bool propagate = detail::isOneOf(v.detail, {"+", "-", "~"});
            return emit(v.detail + (propagate ? sized(0) : self(0)));
        }
        if (v.op == "binary") {
            if (detail::comparison(v.detail)) {
                const auto x = node(a[0]).type;
                const auto y = node(a[1]).type;
                const Type operands{std::max(x.width, y.width), x.isSigned && y.isSigned};
                return emit(get(a[0], stage, operands) + ' ' + v.detail + ' ' + get(a[1], stage, operands));
            }
            if (v.detail == "&&" || v.detail == "||") return emit(self(0) + ' ' + v.detail + ' ' + self(1));
            if (detail::shift(v.detail) || v.detail == "**") return emit(sized(0) + ' ' + v.detail + ' ' + self(1));
            return emit(sized(0) + ' ' + v.detail + ' ' + sized(1));
        }
        if (v.op == "phi") {
            // A procedural if treats X as false and each assigned arm is already fitted.
            const auto raw = wire(v.type, truth(self(0)) + " ? " + self(1) + " : " + self(2));
            return emit(cast(raw, type.isSigned));
        }
        if (v.op == "select") return emit(self(0) + " ? " + sized(1) + " : " + sized(2));
        if (v.op == "slice" && node(a[0]).op == "ins" && v.type.width == 5) {
            const auto high = constants_.get(a[1]);
            const char* field = high == 25 ? "rs" : high == 20 ? "rt" : high == 15 ? "rd" : nullptr;
            if (field) {
                const auto tag = stageTag(stage);
                const auto source = tag + "_ins[" + std::to_string(*high) + ":"
                    + std::to_string(*high - 4) + "]";
                const auto signal = shared_.wire({5}, source, tag + "_" + field);
                if (type.width == 5 && !type.isSigned) return signal;
                return emit(cast(signal, type.isSigned));
            }
        }
        if (detail::isOneOf(v.op, {"bit", "slice", "indexed"}))
            return emit(cast(self(0) + partRange(v, stage), type.isSigned));
        if (v.op == "concat") {
            std::string expression = "{";
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) expression += ", ";
                expression += self(i);
            }
            return emit(cast(expression + '}', type.isSigned));
        }
        if (v.op == "repeat") {
            const auto count = v.type.width / node(a[1]).type.width;
            return emit(cast("{" + std::to_string(count) + "{" + self(1) + "}}", type.isSigned));
        }
        if (v.op == "insert") {
            const auto& part = node(a[0]);
            const auto old = get(part.operands[0], stage);
            const auto rhs = self(1);
            const auto range = partRange(part, stage);
            const auto temp = prefix_ + "part" + std::to_string(++signal_);
            declarations_ << "    reg " << bits(v.type) << temp << ";\n";
            procedures_ << "    always @* begin\n        " << temp << " = " << old << ";\n"
                        << "        " << temp << range << " = " << rhs << ";\n    end\n";
            return emit(cast(temp, type.isSigned));
        }
        throw Error(v.location, "cannot lower operation '" + v.op + "' to Verilog");
    }

    std::string guard(const Guard& tests, Stage stage, bool project = false) {
        std::string expression;
        for (auto term : tests) {
            const auto& v = node(term.value);
            if (project && v.stage > stage && (v.sources & runtimeSources)) continue;
            if (!expression.empty()) expression += " && ";
            if (!term.positive) expression += "!";
            expression += truth(get(term.value, stage));
        }
        return expression.empty() ? "1'b1" : expression;
    }

    void requests() {
        for (auto use : {Stage::D, Stage::E, Stage::M}) {
            std::string expression = "32'd0";
            for (const auto& rf : ir_.reads) {
                if (rf.use != use) continue;
                expression += " | ((" + guard(rf.decodeEnable, Stage::D) + ") ? (32'd1 << " +
                    get(rf.address, Stage::D) + ") : 32'd0)";
            }
            code_.requests += "                req_" + stageTag(use) + " = " + expression + ";\n";
        }
    }

    void control() {
        std::string redirect = "1'b0", annul = "1'b0", destination = "32'd0";
        for (const auto& e : ir_.effects) {
            if (e.op == "jump") {
                const auto target = get(e.operands[0], Stage::D);
                const auto condition = guard(e.guard, Stage::D);
                redirect += " || (" + condition + ')';
                destination = "(" + condition + ") ? " + target + " : (" + destination + ')';
            } else if (e.op == "annul_delay_slot") {
                annul += " || (" + guard(e.guard, Stage::D) + ')';
            }
        }
        code_.control = "                do_jump = " + redirect + ";\n" +
            "                do_annul = " + annul + ";\n" +
            "                dest = " + destination + ";\n";
    }

    void memory() {
        std::string address = "32'd0", data = "32'd0", read = "1'b0", write = "1'b0";
        for (const auto& e : ir_.effects) {
            if (e.op != "dm.read" && !e.op.starts_with("dm.write.")) continue;
            for (auto test : e.guard)
                if (node(test.value).sources & Memory)
                    throw Error(e.location, "memory access enable cannot depend on a memory result");
            const auto condition = guard(e.guard, Stage::M);
            ValueId addressId;
            if (e.op == "dm.read") {
                addressId = node(e.operands[0]).operands[0];
                read += " || (" + condition + ')';
            } else {
                if (e.op != "dm.write.4") throw Error(e.location, "P5 Verilog output supports only whole-word stores");
                if (node(e.operands[0]).sources & Memory || node(e.operands[1]).sources & Memory)
                    throw Error(e.location, "memory write cannot depend on a memory result");
                addressId = e.operands[0];
                const auto value = get(e.operands[1], Stage::M);
                data = "(" + condition + ") ? " + value + " : (" + data + ')';
                write += " || (" + condition + ')';
            }
            address = "(" + condition + ") ? " + get(addressId, Stage::M) + " : (" + address + ')';
        }
        if (read != "1'b0" || write != "1'b0") {
            code_.memory = "                store = " + write + ";\n" +
                "                addr = " + address + ";\n" +
                "                data = " + data + ";\n";
        }
    }


    void promise(Stage stage) {
        const auto names = PromiseNames(stage);
        const auto& bitmap = names.bitmap;
        const auto& data = names.data;
        const auto& started = names.started;
        const auto& yielded = names.yielded;
        const auto& pending = names.pending;
        const auto& lag = names.lag;
        std::ostringstream code;


        for (const auto& e : ir_.effects) {
            const bool update = e.op == "promise.update";
            if (e.stage != stage || (!update && e.op != "promise.yield")) continue;
            const auto condition = guard(e.guard, stage);
            const auto value = get(e.operands[0], stage);
            code << "            if (" << condition << ") begin\n";
            if (update) {
                code << "                " << bitmap << " = " << started << " ? " << bitmap
                     << " & " << value << " : " << value << ";\n"
                     << "                " << started << " = 1;\n";
            } else {
                code << "                " << data << " = " << value << ";\n"
                     << "                " << yielded << " = 1;\n";
            }
            code << "            end\n";
        }

        // At the next boundary, use the latest remaining relevant event on
        // all paths still possible. Earlier guards can eliminate faster/slower alternatives.
        for (const auto& e : ir_.effects) {
            const bool update = e.op == "promise.update";
            if (e.stage <= stage || (!update && e.op != "promise.yield")) continue;
            const auto condition = guard(e.guard, stage, true);
            const auto distance = static_cast<int>(e.stage) - static_cast<int>(stage);
            code << "            if (" << condition << ") begin\n";
            if (update) {
                const auto fixed = constants_.get(e.operands[0]);
                const auto mask = hex32(fixed ? static_cast<std::uint32_t>(*fixed) : ir_.initialMask);
                code << "                " << pending << " = " << pending << " | " << mask << ";\n";
                if (fixed) {
                    // An intersection that cannot remove any current target has no timing cost.
                    code << "                if (!" << started << " || (|" << '(' << bitmap << " & ~"
                         << mask << "))) begin\n";
                }
                code << "                if (" << lag << " < 2'd" << distance << ") " << lag
                     << " = 2'd" << distance << ";\n";
                if (fixed) code << "                end\n";
            } else {
                code << "                if (!" << yielded << " && " << lag << " < 2'd" << distance
                     << ") " << lag << " = 2'd" << distance << ";\n";
            }
            code << "            end\n";
        }
        code_.promise[static_cast<unsigned>(stage)] = code.str();
    }

    const InstructionIR& ir_;
    std::string prefix_;
    SharedLogic& shared_;
    InstructionCode code_;
    detail::Constants constants_;
    unsigned signal_ = 0;
    std::map<std::tuple<ValueId, Stage, unsigned, bool>, std::string> values_;
    std::ostringstream declarations_;
    std::ostringstream procedures_;
};

std::string decodeFlag(const InstructionCode& code, Stage stage) {
    return (stage == Stage::D ? "" : std::string(stageName(stage)) + "_") + "ins_" + code.name;
}

template<class Body>
void emitCase(std::ostream& out, const std::vector<InstructionCode>& instructions,
              Stage stage, Body body) {
    out << "        case (1'b1)\n";
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const auto& fragment = body(instructions[i]);
        if (!fragment.empty())
            out << "            " << decodeFlag(instructions[i], stage) << ": begin\n" << fragment << "            end\n";
    }
    out << "            default: begin end\n        endcase\n";
}

template<class Body>
void emitComb(std::ostream& out, const std::vector<InstructionCode>& instructions,
              Stage stage, std::string_view defaults, Body body) {
    // Verilog doesn't schedule an always @* block with an empty sensitivity list.
    const bool populated = std::any_of(instructions.begin(), instructions.end(),
        [&](const InstructionCode& code) { return !body(code).empty(); });
    out << "\n    always @(" << (populated ? "*" : stageTag(stage) + "_opcode") << ") begin\n"
        << "        " << defaults << '\n';
    if (populated) emitCase(out, instructions, stage, body);
    out << "    end\n";
}



void emitPromise(std::ostream& out, const std::vector<InstructionCode>& instructions, Stage stage) {
    const auto n = PromiseNames(stage);
    const std::string input = stage == Stage::D ? "101'd0" : stage == Stage::E ? "ep" : "mp";
    out << "    wire [`MDR_PW-1:0] " << n.state << " = " << input << ";\n"
        << "    reg [31:0] " << n.bitmap << ", " << n.data << ", " << n.pending << ", " << n.raw << ";\n"
        << "    reg " << n.started << ", " << n.yielded << ", " << n.ready << ";\n"
        << "    reg [1:0] " << n.lag << ";\n"
        << "    always @* begin\n"
        << "        " << n.bitmap << " = " << n.state << "[`MDR_BITMAP];\n"
        << "        " << n.data << " = " << n.state << "[`MDR_DATA];\n"
        << "        " << n.started << " = " << n.state << "[`MDR_STARTED];\n"
        << "        " << n.yielded << " = " << n.state << "[`MDR_YIELDED];\n"
        << "        " << n.pending << " = 0; " << n.lag << " = 0;\n";
    emitCase(out, instructions, stage, [stage](const InstructionCode& code) -> const std::string& {
        return code.promise[static_cast<unsigned>(stage)];
    });
    out << "        " << n.raw << " = (" << n.started << " ? " << n.bitmap << " : " << n.pending
        << ") & 32'hfffffffe;\n"
        << "        " << n.ready << " = " << n.lag << " == 0 && " << n.yielded
        << " && " << n.raw << " != 0 && (" << n.bitmap << " & (" << n.bitmap << " - 32'd1)) == 0;\n"
        << "        out_" << n.tag << " = " << n.tag << "_opcode == 0 ? 0 : {" << n.ready
        << ", " << n.yielded << ", " << n.started << ", " << n.lag
        << ", " << n.raw << ", " << n.bitmap << ", " << n.data << "};\n"
        << "    end\n";
}

std::string operandNetwork(unsigned ports) {
    std::ostringstream out;
    out << "    localparam PORTS = " << ports << ";\n";
    out << R"RTL(
    // Each shared RF slot carries its address, forwarded value, and M-use bit.
    wire [PORTS*38-1:0] d_rf_values, e_rf_values, m_rf_values;

    function [31:0] rf_lookup;
        input [4:0] number;
        input [PORTS*38-1:0] operands;
        integer p;
        begin
            rf_lookup = 0;
            for (p = 0; p < PORTS; p = p + 1)
                if (number != 0 && operands[38*p +: 5] == number)
                    rf_lookup = operands[38*p+5 +: 32];
        end
    endfunction

    function [31:0] offer;
        input [4:0] number;
        input [31:0] previous;
        input [`MDR_PW-1:0] producer;
        begin
            if (number != 0 && producer[`MDR_READY] && producer[32+number])
                offer = producer[`MDR_DATA];
            else offer = previous;
        end
    endfunction

    reg [31:0] req_d, req_e, req_m;

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

    reg rf_stall;
    integer r, port, k;
    always @* begin
        read_addr = 0;
        rf_stall = 0;
        port = 0;
        for (r = 1; r < 32; r = r + 1) begin
            if (req_d[r] || req_e[r] || req_m[r]) begin
                if (port < PORTS) read_addr[5*port +: 5] = r[4:0];
                port = port + 1;
                rf_stall = rf_stall | hazard(r[4:0], req_d[r] ? 2'd0 : req_e[r] ? 2'd1 : 2'd2, ep, mp, wp);
            end
        end
    end

    assign stall = rf_stall;

    always @* begin
        write_addr = 5'd0;
        for (k = 31; k > 0; k = k - 1)
            if (wp[32+k]) write_addr = k[4:0];
    end


    genvar p;
    generate
        for (p = 0; p < PORTS; p = p + 1) begin: rf_forward
            wire [4:0] da = read_addr[5*p +: 5];
            wire [4:0] ea = e_rf[38*p +: 5];
            wire [4:0] ma = m_rf[38*p +: 5];
            wire [31:0] dv = offer(da, offer(da, offer(da, read_data[32*p +: 32], wp), mp), ep);
            wire [31:0] ev = offer(ea, offer(ea, e_rf[38*p+5 +: 32], wp), mp);
            wire [31:0] mv = offer(ma, m_rf[38*p+5 +: 32], wp);
            assign d_rf_values[38*p +: 38] = {req_m[da], dv, da};
            assign e_rf_values[38*p +: 38] = {e_rf[38*p+37], ev, ea};
            assign m_rf_values[38*p +: 38] = {1'b0, mv, ma};
            assign d_rf_next[38*p +: 38] = (req_e[da] || req_m[da]) ? d_rf_values[38*p +: 38] : 38'd0;
            assign e_rf_next[38*p +: 38] = e_rf[38*p+37] ? e_rf_values[38*p +: 38] : 38'd0;
        end
    endgenerate
)RTL";
    return out.str();
}


} // namespace

std::string controller(const ModuleIR& module, PipelineLayout& layout) {
    SharedLogic shared;
    std::vector<InstructionCode> instructions;
    for (const auto& instruction : module.instructions) {
        instructions.push_back(InstructionEmitter(instruction, shared).run());
        layout.deBits = std::max(layout.deBits, instructions.back().payloadBits[0]);
        layout.emBits = std::max(layout.emBits, instructions.back().payloadBits[1]);
    }

    std::ostringstream out;
    out << "`include \"Promise.vh\"\n\nmodule Controller(\n"
        << "    input d_valid,\n"
        << "    input [" << layout.opBits - 1 << ":0] e_opcode, m_opcode,\n"
        << "    output reg [" << layout.opBits - 1 << ":0] d_opcode,\n"
        << "    input [31:0] d_ins, e_ins, m_ins, d_pc, e_pc, m_pc, word,\n"
        << "    input [`MDR_PW-1:0] ep, mp, wp,\n"
        << "    output reg [" << layout.rfPorts * 5 - 1 << ":0] read_addr,\n"
        << "    input [" << layout.rfPorts * 32 - 1 << ":0] read_data,\n"
        << "    input [" << layout.rfPorts * 38 - 1 << ":0] e_rf, m_rf,\n"
        << "    output [" << layout.rfPorts * 38 - 1 << ":0] d_rf_next, e_rf_next,\n"
        << "    input [" << layout.deBits - 1 << ":0] de_payload,\n"
        << "    input [" << layout.emBits - 1 << ":0] em_payload,\n"
        << "    output reg [" << layout.deBits - 1 << ":0] next_de_payload,\n"
        << "    output reg [" << layout.emBits - 1 << ":0] next_em_payload,\n"
        << "    output reg [`MDR_PW-1:0] out_d, out_e, out_m,\n"
        << "    output reg do_jump, do_annul, store,\n"
        << "    output stall,\n    output reg [4:0] write_addr,\n"
        << "    output reg [31:0] dest, addr, data\n);\n";

    out << operandNetwork(layout.rfPorts);
    for (const auto& instruction : instructions) out << instruction.localDeclarations;
    out << shared.str();
    for (const auto& instruction : instructions) out << instruction.procedures;

    out << "\n    // Decode once in D and carry the selected operation through E and M.\n"
        << "    always @* begin\n        if (!d_valid) d_opcode = 0;\n";
    for (std::size_t i = 0; i < instructions.size(); ++i)
        out << "        else if (" << instructions[i].match << ") d_opcode = " << i + 1 << ";\n";
    out << "        else d_opcode = 0;\n    end\n";
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const auto& instruction = instructions[i];
        for (auto stage : {Stage::D, Stage::E, Stage::M}) {
            out << "    wire " << decodeFlag(instruction, stage) << " = "
                << stageTag(stage) << "_opcode == " << i + 1 << ";\n";
        }
    }
    emitComb(out, instructions, Stage::D, "req_d = 0; req_e = 0; req_m = 0;",
        [](const auto& code) -> const std::string& { return code.requests; });
    emitComb(out, instructions, Stage::D, "do_jump = 0; do_annul = 0; dest = 0;",
        [](const auto& code) -> const std::string& { return code.control; });
    emitComb(out, instructions, Stage::M, "addr = 0; data = 0; store = 0;",
        [](const auto& code) -> const std::string& { return code.memory; });

    for (auto stage : {Stage::D, Stage::E, Stage::M}) emitPromise(out, instructions, stage);

    for (std::size_t bank = 0; bank < 2; ++bank) {
        const auto stage = bank == 0 ? Stage::D : Stage::E;
        const std::string name = bank == 0 ? "de_payload" : "em_payload";
        emitComb(out, instructions, stage, "next_" + name + " = 0;",
            [bank](const auto& code) -> const std::string& { return code.payload[bank]; });
    }

    out << "endmodule\n";
    return out.str();
}

} // namespace mdr::rtl

