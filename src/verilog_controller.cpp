#include "verilog.hpp"
#include "constant.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string_view>
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


struct Drive {
    std::string guard;
    std::string value;
};

struct InstructionCode {
    std::string name;
    std::array<std::string, 3> match, localDeclarations, procedures, promise;
    std::array<std::vector<Drive>, 3> requests;
    std::vector<Drive> jumps, annuls, addresses, stores;
    std::array<std::vector<std::string>, 2> payload;
    std::array<std::vector<Drive>, 2> payloadDrivers;
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
    InstructionEmitter(const InstructionIR& ir, std::array<SharedLogic, 3>& shared)
        : ir_(ir), prefix_("i_" + ir.name + "_"), shared_(shared), constants_(ir) {}

    InstructionCode run() {
        code_.name = ir_.name;
        for (auto stage : {Stage::D, Stage::E, Stage::M})
            code_.match[static_cast<unsigned>(stage)] = truth(get(ir_.match, stage));
        requests();
        control();
        memory();
        for (auto stage : {Stage::D, Stage::E, Stage::M}) promise(stage);
        for (std::size_t stage = 0; stage < code_.match.size(); ++stage) {
            code_.localDeclarations[stage] = declarations_[stage].str();
            code_.procedures[stage] = procedures_[stage].str();
        }
        return std::move(code_);
    }


private:
    const Value& node(ValueId id) const { return ir_.value(id); }

    std::string wire(Stage stage, Type type, const std::string& expression) {
        return shared_[static_cast<unsigned>(stage)].wire(type, expression);
    }
    std::string carry(Type type, const std::string& expression, Stage destination) {
        const auto index = static_cast<unsigned>(destination) - 1;
        const auto bank = destination == Stage::E ? "de_payload" : "em_payload";
        const auto part = "[" + std::to_string(code_.payloadBits[index]) +
            " +: " + std::to_string(type.width) + "]";
        code_.payloadBits[index] += type.width;
        code_.payload[index].push_back(expression);
        return wire(destination, type, std::string(bank) + part);
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
                name = wire(stage, effective, cast(get(id, stage), effective.isSigned));
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
        return wire(stage, {32}, "rf_lookup(" + address + ", rf_values)");
    }
    std::string partRange(const Value& v, Stage stage) {
        const auto index = get(v.operands[1], stage);
        if (v.op == "bit") return '[' + index + ']';
        if (v.op == "slice" || v.detail == "-:")
            return '[' + index + " -: " + std::to_string(v.type.width) + ']';
        return '[' + index + " +: " + std::to_string(v.type.width) + ']';
    }

    std::string instructionSlice(const Value& value, Stage stage) {
        const auto& args = value.operands;
        if (node(args[0]).op != "ins") return {};
        const auto start = constants_.get(args[1]);
        if (!start || *start >= 32) return {};

        const auto width = value.type.width;
        unsigned low = static_cast<unsigned>(*start);
        if (value.op == "slice" || value.detail == "-:") {
            if (low + 1 < width) return {};
            low -= width - 1;
        }
        if (width > 32 - low) return {};
        const auto high = low + width - 1;
        const auto range = width == 1 ? std::to_string(low) :
            std::to_string(high) + ":" + std::to_string(low);
        std::string name = "ir_" + std::to_string(high) + "_" + std::to_string(low);
        if (high == 25 && low == 21) name = "rs";
        if (high == 20 && low == 16) name = "rt";
        if (high == 15 && low == 11) name = "rd";
        if (high == 31 && low == 26) name = "opcode";
        if (high == 5 && low == 0) name = "funct";
        if (high == 15 && low == 0) name = "imm16";
        return shared_[static_cast<unsigned>(stage)].wire({width}, "IR[" + range + "]", name);
    }


    std::string evaluate(ValueId id, Stage stage, Type type) {
        const auto& v = node(id);
        const auto& a = v.operands;
        auto self = [&](std::size_t operand) { return get(a[operand], stage); };
        auto sized = [&](std::size_t operand) { return get(a[operand], stage, type); };
        auto emit = [&](const std::string& expression) { return wire(stage, type, expression); };

        if (v.op == "ins" || v.op == "pc") return emit(cast(v.op == "ins" ? "IR" : "PC", type.isSigned));
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
            const auto raw = wire(stage, v.type, rhs);
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
            const auto raw = wire(stage, v.type, truth(self(0)) + " ? " + self(1) + " : " + self(2));
            return emit(cast(raw, type.isSigned));
        }
        if (v.op == "select") return emit(self(0) + " ? " + sized(1) + " : " + sized(2));
        if (detail::isOneOf(v.op, {"bit", "slice", "indexed"})) {
            const auto slice = instructionSlice(v, stage);
            if (slice.empty()) return emit(cast(self(0) + partRange(v, stage), type.isSigned));
            if (type.width == v.type.width && !type.isSigned) return slice;
            return emit(cast(slice, type.isSigned));
        }
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
            declarations_[static_cast<unsigned>(stage)] << "    reg " << bits(v.type) << temp << ";\n";
            procedures_[static_cast<unsigned>(stage)] << "    always @* begin\n        " << temp << " = " << old << ";\n"
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
        for (const auto& rf : ir_.reads) {
            code_.requests[static_cast<unsigned>(rf.use)].push_back(
                {guard(rf.decodeEnable, Stage::D), "32'd1 << " + get(rf.address, Stage::D)});
        }
    }

    void control() {
        for (const auto& e : ir_.effects) {
            if (e.op == "jump") {
                code_.jumps.push_back({guard(e.guard, Stage::D), get(e.operands[0], Stage::D)});
            } else if (e.op == "annul_delay_slot") {
                code_.annuls.push_back({guard(e.guard, Stage::D), ""});
            }
        }
    }

    void memory() {
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
                code_.stores.push_back({condition, get(e.operands[1], Stage::M)});
            }
            code_.addresses.push_back({condition, get(addressId, Stage::M)});
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
    std::array<SharedLogic, 3>& shared_;
    InstructionCode code_;
    detail::Constants constants_;
    unsigned signal_ = 0;
    std::map<std::tuple<ValueId, Stage, unsigned, bool>, std::string> values_;
    std::array<std::ostringstream, 3> declarations_, procedures_;
};

std::string reduceOr(const std::vector<std::string>& terms) {
    if (terms.empty()) return "1'b0";
    std::string expression = "|{";
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i) expression += ", ";
        expression += terms[i];
    }
    return expression + '}';
}

std::string flag(const InstructionCode& code) { return "ins_" + code.name; }

void emitDecode(std::ostream& out, const std::vector<InstructionCode>& instructions, std::size_t stage) {
    out << "\n    // First matching definition wins, even if encodings overlap.\n";
    std::string previous;
    std::vector<std::string> decoded;
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const auto& code = instructions[i];
        const auto matched = "match_" + code.name;
        const auto selected = flag(code);
        out << "    wire " << matched << ";\n"
            << "    assign " << matched << " = " << code.match[stage] << ";\n"
            << "    wire " << selected << ";\n"
            << "    assign " << selected << " = valid && " << matched;
        if (!previous.empty()) out << " && !" << previous;
        out << ";\n";
        decoded.push_back(selected);

        if (i + 1 < instructions.size()) {
            const auto prefix = "matched_through_" + code.name;
            out << "    wire " << prefix << ";\n"
                << "    assign " << prefix << " = ";
            if (!previous.empty()) out << previous << " || ";
            out << matched << ";\n";
            previous = prefix;
        }
    }
    out << "    wire decoded;\n    assign decoded = " << reduceOr(decoded) << ";\n";
}

class SignalEmitter {
public:
    SignalEmitter(std::ostream& out, const std::vector<InstructionCode>& instructions)
        : out_(out), instructions_(instructions) {}

    template<class Drives>
    void emit(std::string_view signal, unsigned width, Drives drives, bool activeOnly = false) {
        std::map<std::string, std::map<std::string, std::vector<std::string>>> groups;
        for (const auto& code : instructions_) {
            const auto& choices = drives(code);
            for (const auto& drive : choices) {
                auto& instructions = groups[activeOnly ? "" : drive.value][drive.guard];
                const auto name = flag(code);
                if (std::find(instructions.begin(), instructions.end(), name) == instructions.end())
                    instructions.push_back(name);
            }
        }
        const auto zero = std::to_string(width) + "'d0";
        std::vector<std::string> terms;
        for (const auto& [value, guards] : groups) {
            std::vector<std::string> enables;
            for (const auto& [guard, instructions] : guards)
                enables.push_back(condition(guard, instructions));
            if (activeOnly) {
                terms = std::move(enables);
            } else {
                auto select = enables.size() == 1 ? enables.front() : reduceOr(enables);
                terms.push_back("(" + select + " ? (" + value + ") : " + zero + ")");
            }
        }
        out_ << "    assign " << signal << " = ";
        if (activeOnly) {
            out_ << reduceOr(terms);
        } else if (terms.empty()) {
            out_ << zero;
        } else {
            for (std::size_t i = 0; i < terms.size(); ++i) {
                if (i) out_ << "\n               | ";
                out_ << terms[i];
            }
        }
        out_ << ";\n";
    }

private:
    std::string condition(const std::string& guard, const std::vector<std::string>& instructions) {
        if (guard == "1'b1" && instructions.size() == 1) return instructions.front();
        const auto key = std::pair{guard, instructions};
        if (auto it = enables_.find(key); it != enables_.end()) return it->second;

        const auto base = "enable_" + instructions.front().substr(4);
        const auto count = ++enableCounts_[base];
        const auto name = base + "__" + std::to_string(count);
        out_ << "    wire " << name << ";\n"
             << "    assign " << name << " = " << reduceOr(instructions);
        if (guard != "1'b1") out_ << " && (" << guard << ')';
        out_ << ";\n";
        enables_.emplace(key, name);
        return name;
    }

    std::ostream& out_;
    const std::vector<InstructionCode>& instructions_;
    std::map<std::pair<std::string, std::vector<std::string>>, std::string> enables_;
    std::map<std::string, unsigned> enableCounts_;
};

void emitPromise(std::ostream& out, const std::vector<InstructionCode>& instructions, Stage stage) {
    const auto n = PromiseNames(stage);
    const std::string input = stage == Stage::D ? "101'd0" : stage == Stage::E ? "ep" : "mp";
    const auto index = static_cast<unsigned>(stage);
    std::map<std::string, std::vector<std::string>> actions;
    for (const auto& code : instructions)
        if (!code.promise[index].empty())
            actions[code.promise[index]].push_back(flag(code));

    out << "\n    // Update the Promise for the next pipeline boundary.\n"
        << "    wire [`MDR_PW-1:0] " << n.state << " = " << input << ";\n"
        << "    reg [31:0] " << n.bitmap << ", " << n.data << ", " << n.pending << ", " << n.raw << ";\n"
        << "    reg " << n.started << ", " << n.yielded << ", " << n.ready << ";\n"
        << "    reg [1:0] " << n.lag << ";\n"
        << "    always @* begin\n"
        << "        " << n.bitmap << " = " << n.state << "[`MDR_BITMAP];\n"
        << "        " << n.data << " = " << n.state << "[`MDR_DATA];\n"
        << "        " << n.started << " = " << n.state << "[`MDR_STARTED];\n"
        << "        " << n.yielded << " = " << n.state << "[`MDR_YIELDED];\n"
        << "        " << n.pending << " = 0; " << n.lag << " = 0;\n";
    for (const auto& [body, flags] : actions)
        out << "        if (" << reduceOr(flags) << ") begin\n" << body << "        end\n";
    out << "        " << n.raw << " = (" << n.started << " ? " << n.bitmap << " : " << n.pending
        << ") & 32'hfffffffe;\n"
        << "        " << n.ready << " = " << n.lag << " == 0 && " << n.yielded
        << " && " << n.raw << " != 0 && (" << n.bitmap << " & (" << n.bitmap << " - 32'd1)) == 0;\n"
        << "        promise_out = decoded ? {" << n.ready
        << ", " << n.yielded << ", " << n.started << ", " << n.lag
        << ", " << n.raw << ", " << n.bitmap << ", " << n.data << "} : 0;\n"
        << "    end\n";
}

void emitOperands(std::ostream& out, unsigned ports, Stage stage) {
    out << "    localparam PORTS = " << ports << ";\n" << R"RTL(
    // Each shared RF slot contains a register number, value, and M-use bit.
    wire [PORTS*38-1:0] rf_values;

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

)RTL";
    if (stage == Stage::D) {
        out << R"RTL(    integer r, port;
    always @* begin
        read_addr = 0;
        port = 0;
        for (r = 1; r < 32; r = r + 1)
            if (req_d[r] || req_e[r] || req_m[r]) begin
                if (port < PORTS) read_addr[5*port +: 5] = r[4:0];
                port = port + 1;
            end
    end

)RTL";
    }
    out << "    genvar p;\n    generate\n        for (p = 0; p < PORTS; p = p + 1) begin: rf_forward\n";
    switch (stage) {
    case Stage::D:
        out << R"RTL(            wire [4:0] number = read_addr[5*p +: 5];
            wire [31:0] value = offer(number, offer(number, offer(number, read_data[32*p +: 32], wp), mp), ep);
            assign rf_values[38*p +: 38] = {req_m[number], value, number};
            assign d_rf_next[38*p +: 38] = (req_e[number] || req_m[number]) ? rf_values[38*p +: 38] : 38'd0;
)RTL";
        break;
    case Stage::E:
        out << R"RTL(            wire [4:0] number = e_rf[38*p +: 5];
            wire [31:0] value = offer(number, offer(number, e_rf[38*p+5 +: 32], wp), mp);
            assign rf_values[38*p +: 38] = {e_rf[38*p+37], value, number};
            assign e_rf_next[38*p +: 38] = e_rf[38*p+37] ? rf_values[38*p +: 38] : 38'd0;
)RTL";
        break;
    case Stage::M:
        out << R"RTL(            wire [4:0] number = m_rf[38*p +: 5];
            wire [31:0] value = offer(number, m_rf[38*p+5 +: 32], wp);
            assign rf_values[38*p +: 38] = {1'b0, value, number};
)RTL";
        break;
    default: break;
    }
    out << "        end\n    endgenerate\n";
}

std::string packedPayload(const InstructionCode& code, std::size_t bank, unsigned width) {
    const auto& values = code.payload[bank];
    if (values.empty()) return "";
    std::vector<std::string> fields(values.rbegin(), values.rend());
    if (const auto pad = width - code.payloadBits[bank]; pad != 0)
        fields.insert(fields.begin(), std::to_string(pad) + "'d0");
    if (fields.size() == 1) return fields.front();
    std::string value = "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) value += ", ";
        value += fields[i];
    }
    return value + '}';
}

void emitPorts(std::ostream& out, Stage stage, const PipelineLayout& layout) {
    const auto ports = layout.rfPorts;
    const auto rfBits = ports * 38;
    const char* name = stage == Stage::D ? "DCtrl" : stage == Stage::E ? "ECtrl" : "MCtrl";
    out << "`include \"Promise.vh\"\n\nmodule " << name << "(\n"
        << "    input valid,\n    input [31:0] IR, PC,\n";
    switch (stage) {
    case Stage::D:
        out << "    input [`MDR_PW-1:0] ep, mp, wp,\n"
            << "    input [" << ports * 32 - 1 << ":0] read_data,\n"
            << "    output reg [" << ports * 5 - 1 << ":0] read_addr,\n"
            << "    output [" << rfBits - 1 << ":0] d_rf_next,\n"
            << "    output [" << layout.deBits - 1 << ":0] next_de_payload,\n"
            << "    output [31:0] req_d, req_e, req_m, dest,\n"
            << "    output do_jump, do_annul,\n";
        break;
    case Stage::E:
        out << "    input [`MDR_PW-1:0] ep, mp, wp,\n"
            << "    input [" << rfBits - 1 << ":0] e_rf,\n"
            << "    input [" << layout.deBits - 1 << ":0] de_payload,\n"
            << "    output [" << rfBits - 1 << ":0] e_rf_next,\n"
            << "    output [" << layout.emBits - 1 << ":0] next_em_payload,\n";
        break;
    case Stage::M:
        out << "    input [`MDR_PW-1:0] mp, wp,\n"
            << "    input [31:0] word,\n"
            << "    input [" << rfBits - 1 << ":0] m_rf,\n"
            << "    input [" << layout.emBits - 1 << ":0] em_payload,\n"
            << "    output [31:0] addr, data,\n"
            << "    output store,\n";
        break;
    default: break;
    }
    out << "    output reg [`MDR_PW-1:0] promise_out\n);\n";
}

} // namespace

std::array<std::string, 3> controllers(const ModuleIR& module, PipelineLayout& layout) {
    std::array<SharedLogic, 3> shared;
    std::vector<InstructionCode> instructions;
    for (const auto& instruction : module.instructions) {
        instructions.push_back(InstructionEmitter(instruction, shared).run());
        layout.deBits = std::max(layout.deBits, instructions.back().payloadBits[0]);
        layout.emBits = std::max(layout.emBits, instructions.back().payloadBits[1]);
    }

    const std::array widths{layout.deBits, layout.emBits};
    for (auto& instruction : instructions)
        for (std::size_t bank = 0; bank < widths.size(); ++bank) {
            auto payload = packedPayload(instruction, bank, widths[bank]);
            if (!payload.empty()) instruction.payloadDrivers[bank].push_back({"1'b1", std::move(payload)});
        }

    std::array<std::string, 3> files;
    for (const auto stage : {Stage::D, Stage::E, Stage::M}) {
        const auto index = static_cast<unsigned>(stage);
        std::ostringstream out;
        emitPorts(out, stage, layout);
        emitOperands(out, layout.rfPorts, stage);
        for (const auto& instruction : instructions) out << instruction.localDeclarations[index];
        out << shared[index].str();
        for (const auto& instruction : instructions) out << instruction.procedures[index];
        emitDecode(out, instructions, index);

        SignalEmitter signals(out, instructions);
        if (stage == Stage::D) {
            for (auto use : {Stage::D, Stage::E, Stage::M})
                signals.emit("req_" + stageTag(use), 32, [use](const InstructionCode& code) -> const auto& {
                    return code.requests[static_cast<unsigned>(use)];
                });
            signals.emit("do_jump", 1, [](const InstructionCode& code) -> const auto& { return code.jumps; }, true);
            signals.emit("do_annul", 1, [](const InstructionCode& code) -> const auto& { return code.annuls; }, true);
            signals.emit("dest", 32, [](const InstructionCode& code) -> const auto& { return code.jumps; });
        } else if (stage == Stage::M) {
            signals.emit("addr", 32, [](const InstructionCode& code) -> const auto& { return code.addresses; });
            signals.emit("data", 32, [](const InstructionCode& code) -> const auto& { return code.stores; });
            signals.emit("store", 1, [](const InstructionCode& code) -> const auto& { return code.stores; }, true);
        }
        if (stage != Stage::M) {
            const auto size = stage == Stage::D ? layout.deBits : layout.emBits;
            signals.emit(stage == Stage::D ? "next_de_payload" : "next_em_payload", size,
                [index](const InstructionCode& code) -> const auto& { return code.payloadDrivers[index]; });
        }
        emitPromise(out, instructions, stage);
        out << "endmodule\n";
        files[index] = out.str();
    }
    return files;
}

} // namespace mdr::rtl

