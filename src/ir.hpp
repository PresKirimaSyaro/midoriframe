#pragma once

#include "frontend.hpp"

#include <iosfwd>

namespace mdr {

using ValueId = unsigned;
using EffectId = unsigned;

enum Source : unsigned { InstructionBits = 1, ProgramCounter = 2, Register = 4, Memory = 8 };
inline constexpr unsigned runtimeSources = Register | Memory;

struct Type {
    unsigned width;
    bool isSigned = false;
};

struct Value {
    std::string op;
    std::string detail;
    Type type;
    Stage stage;
    Location location;
    unsigned sources = 0;
    std::vector<ValueId> operands;
};

struct Predicate {
    ValueId value;
    bool positive;
    bool operator==(const Predicate&) const = default;
};
using Guard = std::vector<Predicate>;

struct Effect {
    std::string op;
    Stage stage;
    Location location;
    Guard guard;
    std::vector<ValueId> operands;
};

struct Read {
    ValueId value;
    ValueId address;
    Stage use;
    Guard guard;
    Guard decodeEnable;
};

struct Carry {
    ValueId value;
    Stage from;
    Stage to;
    bool refresh;
};

struct WritePath {
    Guard guard;
    std::vector<EffectId> updates;
    EffectId yield = 0;
    std::uint32_t initialMask = 0;
    Stage readyAfter = Stage::Decode;
};

struct InstructionIR {
    std::string name;
    ValueId match = 0;
    std::vector<Value> values;
    std::vector<Effect> effects;
    std::vector<Read> reads;
    std::vector<Carry> carries;
    std::vector<WritePath> writes;
    std::uint32_t initialMask = 0;
    unsigned readUpperBound = 0;
    bool promisePathsComplete = true;

    const Value& value(ValueId id) const { return values.at(id - 1); }
};

struct ModuleIR {
    std::vector<InstructionIR> instructions;
    unsigned readUpperBound = 0;
};

struct Options {
    bool narrowWrites = false;
};

ModuleIR lower(const Program& program, Options options = {});
void print(std::ostream& out, const ModuleIR& module, bool dependenciesOnly);

} // namespace mdr

