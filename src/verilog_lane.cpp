#include "verilog.hpp"
#include "constant.hpp"

#include <algorithm>
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

class Lane {
public:
    Lane(const InstructionIR& ir, unsigned index, unsigned ports)
        : ir_(ir), index_(index), ports_(ports), constants_(ir) {}

    std::string run() {
        std::ostringstream out;
        out << "\n// " << ir_.name << "\nmodule mdr_instruction_" << index_ << "(\n"
            << "    input clk, d_on, e_on, m_on,\n"
            << "    input [31:0] d_ins, e_ins, m_ins, d_pc, e_pc, m_pc, word,\n"
            << "    input [`MDR_PW-1:0] ep, mp, wp,\n"
            << "    input [" << ports_ * 5 - 1 << ":0] read_addr,\n"
            << "    input [" << ports_ * 32 - 1 << ":0] read_data,\n"
            << "    output matched,\n"
            << "    output [31:0] req_d, req_e, req_m,\n"
            << "    output reg [`MDR_PW-1:0] out_d, out_e, out_m,\n"
            << "    output do_jump, do_annul, store,\n"
            << "    output [31:0] dest, addr, data\n);\n";

        const auto match = truth(get(ir_.match, Stage::D));
        connections_ << "    assign matched = " << match << ";\n";
        requests();
        control();
        memory();
        for (auto stage : {Stage::D, Stage::E, Stage::M}) promise(stage);
        out << declarations_.str() << helpers() << connections_.str() << procedures_.str();
        out << "endmodule\n";
        return out.str();
    }

private:
    const Value& node(ValueId id) const { return ir_.value(id); }

    std::string wire(Type type, const std::string& expression) {
        const auto name = "v" + std::to_string(++signal_);
        declarations_ << "    wire " << bits(type) << name << ";\n";
        connections_ << "    assign " << name << " = " << expression << ";\n";
        return name;
    }

    std::string latch(Type type, const std::string& expression) {
        const auto name = "q" + std::to_string(++signal_);
        declarations_ << "    reg " << bits(type) << name << ";\n";
        procedures_ << "    always @(posedge clk) " << name << " <= " << expression << ";\n";
        return name;
    }

    std::string get(ValueId id, Stage stage) { return get(id, stage, node(id).type); }

    std::string get(ValueId id, Stage stage, Type requested) {
        const auto& v = node(id);
        const auto width = std::max(requested.width, v.type.width);
        const Type effective{width, requested.isSigned};
        const auto key = std::tuple{id, stage, width, effective.isSigned};
        if (auto cached = values_.find(key); cached != values_.end()) return cached->second;

        std::string name;
        if (v.stage >= Stage::D && v.stage < stage && v.sources != 0) {
            if (width != v.type.width || effective.isSigned != v.type.isSigned) {
                name = wire(effective, cast(get(id, stage), effective.isSigned));
            } else {
                const auto previous = static_cast<Stage>(static_cast<int>(stage) - 1);
                name = latch(v.type, get(id, previous));
            }
        } else {
            name = evaluate(id, stage, effective);
        }
        values_.emplace(key, name);
        return name;
    }

    std::string read(ValueId id, Stage stage) {
        const auto key = std::pair{id, stage};
        if (auto cached = rfValues_.find(key); cached != rfValues_.end()) return cached->second;
        const auto address = get(node(id).operands[0], Stage::D);
        std::string result;
        if (stage == Stage::D) {
            result = wire({32}, "offer(" + address + ", offer(" + address + ", offer(" + address +
                ", rf_lookup(" + address + ", read_addr, read_data), wp), mp), ep)");
        } else {
            const auto previous = static_cast<Stage>(static_cast<int>(stage) - 1);
            const auto carried = latch({32}, read(id, previous));
            const auto previousAddress = previous == Stage::D ? address : rfAddresses_.at({id, previous});
            const auto currentAddress = latch({5}, previousAddress);
            rfAddresses_.emplace(key, currentAddress);
            std::string expression = "offer(" + currentAddress + ", " + carried + ", wp)";
            if (stage == Stage::E) expression = "offer(" + currentAddress + ", " + expression + ", mp)";
            result = wire({32}, expression);
        }
        rfValues_.emplace(key, result);
        return result;
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
            const auto temp = "part" + std::to_string(++signal_);
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
            std::string expression = "d_on ? (32'd0";
            for (const auto& rf : ir_.reads) {
                if (rf.use != use) continue;
                expression += " | ((" + guard(rf.decodeEnable, Stage::D) + ") ? (32'd1 << " +
                    get(rf.address, Stage::D) + ") : 32'd0)";
            }
            connections_ << "    assign req_" << stageTag(use) << " = " << expression << ") : 32'd0;\n";
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
        connections_ << "    assign do_jump = d_on && (" << redirect << ");\n"
                     << "    assign do_annul = d_on && (" << annul << ");\n"
                     << "    assign dest = d_on ? (" << destination << ") : 32'd0;\n";
    }

    void memory() {
        std::string address = "32'd0", data = "32'd0", write = "1'b0";
        for (const auto& e : ir_.effects) {
            if (e.op != "dm.read" && !e.op.starts_with("dm.write.")) continue;
            for (auto test : e.guard)
                if (node(test.value).sources & Memory)
                    throw Error(e.location, "memory access enable cannot depend on a memory result");
            const auto condition = guard(e.guard, Stage::M);
            ValueId addressId;
            if (e.op == "dm.read") {
                addressId = node(e.operands[0]).operands[0];
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
        connections_ << "    assign store = m_on && (" << write << ");\n"
                     << "    assign addr = m_on ? (" << address << ") : 32'd0;\n"
                     << "    assign data = m_on ? (" << data << ") : 32'd0;\n";
    }


    void promise(Stage stage) {
        const auto tag = stageTag(stage);
        const std::string input = stage == Stage::D ? "101'd0" : stage == Stage::E ? "ep" : "mp";
        const auto state = "state_" + tag;
        const auto bitmap = "bitmap_" + tag;
        const auto data = "data_" + tag;
        const auto started = "started_" + tag;
        const auto yielded = "yielded_" + tag;
        const auto ready = "ready_" + tag;
        const auto pending = "pending_" + tag;
        const auto raw = "raw_" + tag;
        const auto lag = "lag_" + tag;
        declarations_ << "    wire [`MDR_PW-1:0] " << state << " = " << input << ";\n"
            << "    reg [31:0] " << bitmap << ", " << data << ", " << pending << ", " << raw << ";\n"
            << "    reg " << started << ", " << yielded << ", " << ready << ";\n"
            << "    reg [1:0] " << lag << ";\n";

        std::ostringstream code;
        code << "    always @* begin\n"
             << "        " << bitmap << " = " << state << "[`MDR_BITMAP];\n"
             << "        " << data << " = " << state << "[`MDR_DATA];\n"
             << "        " << started << " = " << state << "[`MDR_STARTED];\n"
             << "        " << yielded << " = " << state << "[`MDR_YIELDED];\n"
             << "        " << pending << " = 0; " << raw << " = 0; " << lag << " = 0; " << ready << " = 0;\n"
             << "        out_" << tag << " = 0;\n"
             << "        if (" << tag << "_on) begin\n";

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
        code << "            " << raw << " = (" << started << " ? " << bitmap << " : " << pending
             << ") & 32'hfffffffe;\n"
             << "            " << ready << " = " << lag << " == 0 && " << yielded
             << " && " << raw << " != 0 && (" << bitmap << " & (" << bitmap << " - 32'd1)) == 0;\n"
             << "            out_" << tag << " = {" << ready << ", " << yielded << ", " << started
             << ", " << lag << ", " << raw << ", " << bitmap << ", " << data << "};\n"
             << "        end\n    end\n";
        procedures_ << code.str();
    }

    std::string helpers() const {
        std::ostringstream out;
        out << "    function [31:0] rf_lookup;\n"
            << "        input [4:0] number;\n"
            << "        input [" << ports_ * 5 - 1 << ":0] addresses;\n"
            << "        input [" << ports_ * 32 - 1 << ":0] results;\n"
            << "        integer p;\n"
            << "        begin\n"
            << "            rf_lookup = 32'd0;\n"
            << "            for (p = 0; p < " << ports_ << "; p = p + 1)\n"
            << "                if (number != 0 && addresses[5*p +: 5] == number)\n"
            << "                    rf_lookup = results[32*p +: 32];\n"
            << "        end\n"
            << "    endfunction\n";
        out << R"RTL(    function [31:0] offer;
        input [4:0] number;
        input [31:0] previous;
        input [`MDR_PW-1:0] producer;
        begin
            if (number != 0 && producer[`MDR_READY] && producer[32+number])
                offer = producer[`MDR_DATA];
            else offer = previous;
        end
    endfunction
)RTL";
        return out.str();
    }

    const InstructionIR& ir_;
    unsigned index_;
    unsigned ports_;
    detail::Constants constants_;
    unsigned signal_ = 0;
    std::map<std::tuple<ValueId, Stage, unsigned, bool>, std::string> values_;
    std::map<std::pair<ValueId, Stage>, std::string> rfValues_;
    std::map<std::pair<ValueId, Stage>, std::string> rfAddresses_;
    std::ostringstream declarations_;
    std::ostringstream connections_;
    std::ostringstream procedures_;
};

} // namespace

std::string lane(const InstructionIR& ir, unsigned index, unsigned ports) {
    return Lane(ir, index, ports).run();
}

} // namespace mdr::rtl

