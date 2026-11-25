#include "ir.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string_view>

namespace mdr {
namespace {

void values(std::ostream& out, const std::vector<ValueId>& args) {
    for (auto id : args) out << " %" << id;
}

void guard(std::ostream& out, const Guard& terms) {
    if (terms.empty()) out << "always";
    const char* sep = "";
    for (auto term : terms) {
        out << sep << (term.positive ? "" : "!") << '%' << term.value;
        sep = " && ";
    }
}

std::string hex(std::uint32_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

void mark(std::vector<bool>& keep, const Guard& tests) {
    for (auto part : tests) keep[part.value] = true;
}

std::vector<bool> dependentValues(const InstructionIR& ir) {
    std::vector<bool> keep(ir.values.size() + 1);
    keep[ir.match] = true;
    for (const auto& effect : ir.effects) {
        mark(keep, effect.guard);
        for (auto operand : effect.operands) keep[operand] = true;
    }
    for (const auto& read : ir.reads) {
        keep[read.address] = true;
        keep[read.value] = true;
        mark(keep, read.guard);
        mark(keep, read.decodeEnable);
    }
    for (const auto& carry : ir.carries) keep[carry.value] = true;
    for (auto id = static_cast<ValueId>(ir.values.size()); id > 0; --id)
        if (keep[id])
            for (auto arg : ir.value(id).operands) keep[arg] = true;
    return keep;
}

void printInstruction(std::ostream& out, const InstructionIR& ir, std::size_t priority, bool deps) {
    const auto selected = deps ? dependentValues(ir) : std::vector<bool>{};
    out << "\ninstruction " << ir.name << " priority=" << priority
        << " match=%" << ir.match << " {\n  values:\n";
    for (ValueId id = 1; id <= ir.values.size(); ++id) {
        if (deps && !selected[id]) continue;
        const auto& value = ir.value(id);
        out << "    %" << id << " @" << stageName(value.stage) << ' '
            << (value.type.isSigned ? 's' : 'u') << value.type.width << ' ' << value.op;
        if (!value.detail.empty()) out << ' ' << std::quoted(value.detail);
        values(out, value.operands);
        if (!deps) out << " ; " << value.location.line << ':' << value.location.column;
        out << '\n';
    }
    out << "  effects:\n";
    for (EffectId id = 1; id <= ir.effects.size(); ++id) {
        const auto& effect = ir.effects[id - 1];
        out << "    #" << id << " @" << stageName(effect.stage) << ' ' << effect.op;
        values(out, effect.operands);
        out << " if ";
        guard(out, effect.guard);
        out << '\n';
    }
    out << "  register_reads: upper_ports=" << ir.readUpperBound << '\n';
    for (const auto& read : ir.reads) {
        out << "    %" << read.value << " r=%" << read.address << " use=" << stageName(read.use)
            << " tuse=" << static_cast<int>(read.use) << " decode_enable=";
        guard(out, read.decodeEnable);
        out << " use_guard=";
        guard(out, read.guard);
        out << '\n';
    }
    out << "  pipeline_carries:\n";
    for (const auto& carry : ir.carries)
        out << "    " << stageName(carry.from) << "->" << stageName(carry.to)
            << " %" << carry.value << (carry.refresh ? " rf-address+candidate" : " value") << '\n';
    out << "  promise: initial_raw_upper=" << hex(ir.initialMask)
        << " update=assign-first-then-intersect empty=no-write commit=W\n";
    if (!ir.promisePathsComplete) out << "    paths=deferred (use ordered guarded effects)\n";
    for (const auto& path : ir.writes) {
        out << "    if ";
        guard(out, path.guard);
        out << " candidates=" << hex(path.initialMask) << " updates=";
        const char* sep = "[";
        for (auto update : path.updates) {
            out << sep << '#' << update;
            sep = ",";
        }
        if (path.updates.empty()) out << '[';
        out << "] yield=";
        if (path.yield) out << '#' << path.yield;
        else out << '-';
        if (path.readyAfter != Stage::Decode) {
            out << " ready_by=" << stageName(path.readyAfter) << "->"
                << stageName(static_cast<Stage>(static_cast<int>(path.readyAfter) + 1));
        }
        out << '\n';
    }
    out << "}\n";
}

} // namespace

void print(std::ostream& out, const ModuleIR& module, bool dependenciesOnly) {
    out << "mdrframe-ir 0.1 " << (dependenciesOnly ? "dependencies" : "full")
        << "\nunit instructions=" << module.instructions.size()
        << " rf_upper_ports=" << module.readUpperBound << " decode=first-match miss=no-effect\n";
    for (std::size_t i = 0; i < module.instructions.size(); ++i)
        printInstruction(out, module.instructions[i], i, dependenciesOnly);
}

} // namespace mdr

