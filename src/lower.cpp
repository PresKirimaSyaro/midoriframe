#include "constant.hpp"

#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace mdr {
void analyze(InstructionIR& ir);

namespace {
using namespace detail;
constexpr unsigned maxWidth = 1'048'576;
constexpr unsigned maxIterations = 65'536;

struct Slot {
    unsigned width;
    ValueId current = 0;
    bool readOnly = false;
    std::vector<std::pair<unsigned, unsigned>> defined;
};
using Scope = std::map<std::string, Slot, std::less<>>;

class Lowerer {
public:
    Lowerer(const Program& program, const Instruction& source, Options options)
        : program_(program), source_(source), options_(options), constants_(ir_) {
        ir_.name = source.name;
        scopes_.emplace_back();
    }

    InstructionIR run() {
        const auto at = program_.expr(source_.match).location;
        auto beforeMatch = [&](const Stmt& stmt) {
            return std::tie(stmt.location.line, stmt.location.column) < std::tie(at.line, at.column);
        };
        for (const auto& stmt : source_.declarations)
            if (beforeMatch(stmt)) declare(stmt);
        ir_.match = expression(source_.match);
        if ((value(ir_.match).sources & ~InstructionBits) || value(ir_.match).type.width != 1)
            throw Error(at, "match requires a one-bit expression derived from ins and constants");
        for (const auto& stmt : source_.declarations)
            if (!beforeMatch(stmt)) declare(stmt);
        for (const auto& stage : source_.stages) {
            stage_ = stage.stage;
            block(stage.statements);
        }
        analyze(ir_);
        return std::move(ir_);
    }

private:
    const Value& value(ValueId id) const { return ir_.value(id); }
    ValueId make(std::string op, std::string detail, Type type, Location at,
                 std::vector<ValueId> operands = {}, unsigned sources = 0) {
        return makeAt(stage_, std::move(op), std::move(detail), type, at, std::move(operands), sources);
    }
    ValueId makeAt(Stage stage, std::string op, std::string detail, Type type, Location at,
                   std::vector<ValueId> operands = {}, unsigned sources = 0) {
        if (type.width == 0 || type.width > maxWidth) throw Error(at, "unsupported vector width");
        for (auto id : operands) sources |= value(id).sources;
        ir_.values.push_back({std::move(op), std::move(detail), type, stage, at, sources, std::move(operands)});
        return static_cast<ValueId>(ir_.values.size());
    }
    ValueId fit(ValueId id, unsigned width, Location at, const std::string& name = {}) {
        return make("fit", name, {width}, at, {id});
    }
    Slot& slot(std::string_view name, Location at) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
            if (auto entry = scope->find(name); entry != scope->end()) return entry->second;
        throw Error(at, "unknown variable '" + std::string(name) + "'");
    }
    void bind(Location at, const std::string& name, Slot data) {
        if (name == "ins" || name == "pc" || !scopes_.back().emplace(name, data).second)
            throw Error(at, "duplicate or reserved variable '" + name + "'");
    }
    unsigned positive(ValueId id, Location at, std::string_view description) const {
        const auto n = constants_.integer(id, at, description);
        if (n < 1 || n > maxWidth) throw Error(at, std::string(description) + " is out of range");
        return static_cast<unsigned>(n);
    }
    ValueId input(bool pc, Location at) {
        if (pc && stage_ == Stage::Decode) throw Error(at, "pc is only available in a stage block");
        auto& id = pc ? pc_ : ins_;
        if (!id) id = makeAt(Stage::Decode, pc ? "pc" : "ins", "", {32}, at, {},
                             pc ? ProgramCounter : InstructionBits);
        return id;
    }

    ValueId expression(ExprId id) {
        const auto& expr = program_.expr(id);
        const auto& args = expr.operands;
        switch (expr.kind) {
        case ExprKind::Literal: {
            const auto literal = parseLiteral(expr.text, expr.location);
            return make("literal", expr.text, {literal.width, literal.isSigned}, expr.location);
        }
        case ExprKind::Name: {
            if (expr.text == "ins" || expr.text == "pc") return input(expr.text == "pc", expr.location);
            auto& binding = slot(expr.text, expr.location);
            if (!covers(definedHere(binding), 0, binding.width)) throw Error(expr.location, "variable '" + expr.text + "' may be uninitialized");
            return binding.current;
        }
        case ExprKind::Call: return call(expr);
        case ExprKind::Unary: {
            const auto arg = expression(args[0]);
            const auto type = isOneOf(expr.text, {"+", "-", "~"}) ? value(arg).type : Type{1};
            return make("unary", expr.text, type, expr.location, {arg});
        }
        case ExprKind::Binary: {
            const auto lhs = expression(args[0]);
            const auto rhs = expression(args[1]);
            const auto left = value(lhs).type;
            const auto right = value(rhs).type;
            Type type{std::max(left.width, right.width), left.isSigned && right.isSigned};
            if (comparison(expr.text) || isOneOf(expr.text, {"&&", "||"})) type = {1};
            if (shift(expr.text) || expr.text == "**") type = left;
            return make("binary", expr.text, type, expr.location, {lhs, rhs});
        }
        case ExprKind::Select: return select(expr);
        case ExprKind::Bit:
        case ExprKind::Slice:
        case ExprKind::IndexedSlice:
            return readSelection(expr);
        case ExprKind::Concat: {
            std::vector<ValueId> parts;
            unsigned width = 0;
            for (auto arg : args) {
                const auto id = expression(arg);
                if (value(id).type.width > maxWidth - width)
                    throw Error(expr.location, "concatenation is too wide");
                width += value(id).type.width;
                parts.push_back(id);
            }
            return make("concat", "", {width}, expr.location, std::move(parts));
        }
        case ExprKind::Repeat: {
            const auto count = expression(args[0]);
            const auto part = expression(args[1]);
            const auto n = positive(count, expr.location, "replication count");
            if (value(part).type.width > maxWidth / n)
                throw Error(expr.location, "replication is too wide");
            return make("repeat", "", {n * value(part).type.width}, expr.location, {count, part});
        }
        }
        throw Error(expr.location, "unsupported expression");
    }

    bool enter(Predicate predicate) {
        for (const auto& term : guard_)
            if (term.value == predicate.value) return term.positive == predicate.positive;
        guard_.push_back(predicate);
        return true;
    }

    ValueId select(const Expr& expr) {
        const auto& args = expr.operands;
        const auto condition = expression(args[0]);
        const auto truth = constants_.get(condition);
        const auto outer = guard_;
        const auto wasActive = active_;
        auto arm = [&](ExprId id, bool sense) {
            const bool reachable = enter({condition, sense});
            active_ = wasActive && reachable && (!truth || (*truth != 0) == sense);
            if (truth) guard_ = outer;
            auto result = expression(id);
            guard_ = outer;
            active_ = wasActive;
            return result;
        };
        // Both operands determine the Verilog width, including a statically inactive arm.
        const auto yes = arm(args[1], true);
        const auto no = arm(args[2], false);
        const auto lhs = value(yes).type;
        const auto rhs = value(no).type;
        return make("select", "", {std::max(lhs.width, rhs.width), lhs.isSigned && rhs.isSigned},
                    expr.location, {condition, yes, no});
    }

    using Ranges = std::vector<std::pair<unsigned, unsigned>>;

    static bool covers(const Ranges& ranges, unsigned begin, unsigned end) {
        return std::any_of(ranges.begin(), ranges.end(), [&](auto range) {
            return begin >= range.first && end <= range.second;
        });
    }

    static Ranges intersection(const Ranges& left, const Ranges& right) {
        Ranges result;
        for (auto a : left) {
            for (auto b : right) {
                const auto begin = std::max(a.first, b.first);
                const auto end = std::min(a.second, b.second);
                if (begin < end) result.emplace_back(begin, end);
            }
        }
        return result;
    }

    Ranges definedHere(const Slot& slot) const {
        if (!slot.current || covers(slot.defined, 0, slot.width)) return slot.defined;
        return definedHere(slot.current);
    }

    Ranges definedHere(ValueId id) const {
        const auto found = partialDefinitions_.find(id);
        if (found == partialDefinitions_.end()) return {{0, value(id).type.width}};
        const auto& node = value(id);
        if (node.op != "phi" || covers(found->second, 0, node.type.width)) return found->second;
        for (auto term : guard_)
            if (node.operands[0] == term.value) return definedHere(node.operands[term.positive ? 1 : 2]);
        return intersection(definedHere(node.operands[1]), definedHere(node.operands[2]));
    }

    ValueId undef(const std::string& name, unsigned width, Location at) {
        const auto id = make("undef", name, {width}, at);
        partialDefinitions_[id] = {};
        return id;
    }

    static void define(Slot& slot, unsigned begin, unsigned end) {
        if (begin == end) return;
        slot.defined.emplace_back(begin, end);
        std::sort(slot.defined.begin(), slot.defined.end());
        std::vector<std::pair<unsigned, unsigned>> merged;
        for (const auto& range : slot.defined) {
            if (!merged.empty() && range.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, range.second);
            else
                merged.push_back(range);
        }
        slot.defined = std::move(merged);
    }

    std::optional<std::pair<unsigned, unsigned>> selectedRange(ValueId part, unsigned width) const {
        const auto& selected = value(part);
        const auto start = constants_.get(selected.operands[1]);
        if (!start) return {};
        auto lo = *start;
        if (selected.op == "slice" || selected.detail == "-:") {
            if (lo < selected.type.width - 1) return {};
            lo -= selected.type.width - 1;
        }
        if (lo >= width || selected.type.width > width - lo) return {};
        return std::pair{static_cast<unsigned>(lo), static_cast<unsigned>(lo) + selected.type.width};
    }

    ValueId readSelection(const Expr& expr) {
        const auto& base = program_.expr(expr.operands[0]);
        if (base.kind != ExprKind::Name || base.text == "ins" || base.text == "pc")
            return selection(expr, expression(expr.operands[0]));
        const auto& binding = slot(base.text, base.location);
        if (!binding.current) throw Error(base.location, "variable '" + base.text + "' may be uninitialized");
        const auto part = selection(expr, binding.current);
        const auto range = selectedRange(part, binding.width);
        const auto defined = definedHere(binding);
        if (!covers(defined, 0, binding.width) && (!range || !covers(defined, range->first, range->second)))
            throw Error(base.location, "selection from '" + base.text + "' may be uninitialized");
        return part;
    }

    ValueId decodeValue(ValueId id) {
        const auto node = value(id);
        if (node.stage == Stage::Decode) return id;
        if (auto cached = decodeCopies_.find(id); cached != decodeCopies_.end()) return cached->second;
        std::vector<ValueId> operands;
        for (auto arg : node.operands) operands.push_back(decodeValue(arg));
        const auto result = makeAt(Stage::Decode, node.op, node.detail, node.type, node.location,
                                   std::move(operands), node.sources);
        decodeCopies_.emplace(id, result);
        return result;
    }

    ValueId selection(const Expr& expr, ValueId base) {
        const auto& args = expr.operands;
        const auto start = expression(args[1]);
        if (expr.kind == ExprKind::Bit) return make("bit", "", {1}, expr.location, {base, start});
        const auto end = expression(args[2]);
        if (expr.kind == ExprKind::IndexedSlice) {
            const auto width = positive(end, expr.location, "part-select width");
            return make("indexed", expr.text, {width}, expr.location, {base, start, end});
        }
        const auto high = constants_.integer(start, expr.location, "slice upper bound");
        const auto low = constants_.integer(end, expr.location, "slice lower bound");
        if (low < 0 || high < low || high - low >= maxWidth) throw Error(expr.location, "invalid slice bounds");
        return make("slice", "", {static_cast<unsigned>(high - low + 1)}, expr.location, {base, start, end});
    }

    void arity(const Expr& expr, std::size_t expected) const {
        if (expr.operands.size() != expected)
            throw Error(expr.location, expr.text + " expects " + std::to_string(expected) + " argument(s)");
    }
    void requireStage(const Expr& expr, Stage required) const {
        if (stage_ != required) throw Error(expr.location, expr.text + " is only available in " + stageName(required));
    }
    unsigned memorySize(ValueId id, Location at) const {
        const auto n = constants_.integer(id, at, "memory size");
        if (n != 1 && n != 2 && n != 4) throw Error(at, "memory size must be 1, 2, or 4");
        return static_cast<unsigned>(n);
    }
    ValueId memoryAddress(ExprId id, unsigned size, Location at) {
        auto address = fit(expression(id), 32, at, "dm-address");
        if (size == 4) address = make("align.word", "", {32}, at, {address});
        return address;
    }
    ValueId call(const Expr& expr) {
        if (expr.text == "signed") {
            arity(expr, 1);
            const auto arg = expression(expr.operands[0]);
            return make("signed", "", {value(arg).type.width, true}, expr.location, {arg});
        }
        if (expr.text == "grf_read") {
            arity(expr, 1);
            if (stage_ == Stage::Decode) throw Error(expr.location, "grf_read needs a stage block");
            auto address = expression(expr.operands[0]);
            if (value(address).sources & runtimeSources)
                throw Error(expr.location, "grf_read address must be known at decode");
            address = makeAt(Stage::Decode, "fit", "grf-address", {5}, expr.location, {decodeValue(address)});
            if (const auto index = constants_.get(address); index && *index == 0)
                return make("literal", "32'd0", {32}, expr.location);
            const auto data = make("grf.read", "", {32}, expr.location, {address}, Register);
            if (active_) {
                Guard enable;
                for (auto term : guard_)
                    if ((value(term.value).sources & runtimeSources) == 0) enable.push_back({decodeValue(term.value), term.positive});
                ir_.reads.push_back({data, address, stage_, guard_, std::move(enable)});
            }
            return data;
        }
        if (expr.text == "dm_read") {
            arity(expr, 2);
            requireStage(expr, Stage::M);
            const auto size = memorySize(expression(expr.operands[1]), expr.location);
            const auto address = memoryAddress(expr.operands[0], size, expr.location);
            if (value(address).sources & Memory)
                throw Error(expr.location, "memory address cannot depend on a load result");
            const auto data = make("dm.read", std::to_string(size), {8 * size}, expr.location, {address}, Memory);
            effect("dm.read", expr.location, {data});
            return data;
        }
        throw Error(expr.location, "unknown expression primitive '" + expr.text + "'");
    }

    void effect(std::string op, Location at, std::vector<ValueId> operands = {}) {
        if (!active_) return;
        auto category = [](std::string_view operation) -> std::string_view {
            if (operation.starts_with("dm.")) return "memory access";
            if (operation == "promise.yield") return "yield";
            if (operation == "jump" || operation == "annul_delay_slot") return operation;
            return {};
        };
        const auto resource = category(op);
        if (!resource.empty()) {
            auto implies = [](const Guard& a, const Guard& b) {
                return std::all_of(a.begin(), a.end(), [&](Predicate term) {
                    return std::find(b.begin(), b.end(), term) != b.end();
                });
            };
            for (const auto& previous : ir_.effects) {
                if (category(previous.op) == resource &&
                    (implies(previous.guard, guard_) || implies(guard_, previous.guard)))
                    throw Error(at, "multiple " + std::string(resource) + " operations on a path");
            }
        }
        ir_.effects.push_back({std::move(op), stage_, at, guard_, std::move(operands)});
    }

    void effectCall(const Expr& expr) {
        if (expr.text == "jump" || expr.text == "annul_delay_slot") {
            requireStage(expr, Stage::D);
            arity(expr, expr.text == "jump" ? 1 : 0);
            std::vector<ValueId> args;
            if (!expr.operands.empty())
                args.push_back(fit(expression(expr.operands[0]), 32, expr.location, "jump-address"));
            effect(expr.text, expr.location, std::move(args));
            return;
        }
        if (expr.text != "dm_write") throw Error(expr.location, "unknown effect primitive '" + expr.text + "'");
        requireStage(expr, Stage::M);
        unsigned size = 4;
        if (options_.narrowWrites && expr.operands.size() == 3) {
            size = memorySize(expression(expr.operands[1]), expr.location);
            if (size == 4) throw Error(expr.location, "use two arguments for a word store");
        } else {
            arity(expr, 2);
        }
        auto address = memoryAddress(expr.operands.front(), size, expr.location);
        auto data = fit(expression(expr.operands.back()), size * 8, expr.location, "dm-data");
        effect("dm.write." + std::to_string(size), expr.location, {address, data});
    }

    void declare(const Stmt& stmt) {
        const auto width = positive(expression(stmt.first), stmt.location, "vector width");
        const bool outerAlias = stage_ == Stage::Decode && stmt.second != noExpr;
        ValueId current = 0;
        if (stmt.second) {
            const auto initializer = expression(stmt.second);
            if (outerAlias && (value(initializer).sources & ~InstructionBits))
                throw Error(stmt.location, "outer initializer must depend only on ins and constants");
            current = fit(initializer, width, stmt.location, stmt.name);
        }
        Slot binding{width, current, outerAlias, {}};
        if (current) define(binding, 0, width);
        bind(stmt.location, stmt.name, std::move(binding));
    }

    void assign(const Stmt& stmt) {
        const auto& dest = program_.expr(stmt.first);
        auto data = expression(stmt.second);
        if (dest.kind == ExprKind::Name) {
            auto& binding = slot(dest.text, dest.location);
            if (binding.readOnly) throw Error(dest.location, "cannot assign to read-only '" + dest.text + "'");
            binding.current = fit(data, binding.width, stmt.location, dest.text);
            binding.defined = {{0, binding.width}};
            return;
        }
        if (dest.kind != ExprKind::Bit && dest.kind != ExprKind::Slice && dest.kind != ExprKind::IndexedSlice)
            throw Error(dest.location, "invalid assignment destination");
        const auto& target = program_.expr(dest.operands[0]);
        if (target.kind != ExprKind::Name) throw Error(dest.location, "part-select assignment requires a named vector");
        auto& binding = slot(target.text, target.location);
        if (binding.readOnly) throw Error(target.location, "cannot assign to read-only '" + target.text + "'");
        binding.defined = definedHere(binding);
        if (!binding.current) binding.current = undef(target.text, binding.width, dest.location);
        auto part = selection(dest, binding.current);
        data = fit(data, value(part).type.width, stmt.location, target.text);
        binding.current = make("insert", target.text, {binding.width}, stmt.location, {part, data});
        if (auto range = selectedRange(part, binding.width)) define(binding, range->first, range->second);
        if (!covers(binding.defined, 0, binding.width)) partialDefinitions_[binding.current] = binding.defined;
    }

    void branch(const Stmt& stmt) {
        const auto condition = expression(stmt.first);
        if (auto test = constants_.get(condition)) {
            block(*test ? stmt.body : stmt.otherwise);
            return;
        }
        auto before = scopes_;
        const auto outer = guard_;
        auto arm = [&](const std::vector<Stmt>& statements, bool sense) {
            scopes_ = before;
            guard_ = outer;
            if (enter({condition, sense})) block(statements);
            return scopes_;
        };
        auto yes = arm(stmt.body, true);
        auto no = arm(stmt.otherwise, false);
        guard_ = outer;
        scopes_ = std::move(before);
        for (std::size_t i = 0; i < scopes_.size(); ++i) {
            for (auto& [name, binding] : scopes_[i]) {
                const auto& left = yes[i].at(name);
                const auto& right = no[i].at(name);
                if (left.current == right.current) {
                    binding = left;
                    continue;
                }
                binding.defined = intersection(left.defined, right.defined);
                const auto leftValue = left.current ? left.current : undef(name, binding.width, stmt.location);
                const auto rightValue = right.current ? right.current : undef(name, binding.width, stmt.location);
                binding.current = make("phi", name, {binding.width}, stmt.location,
                                       {condition, leftValue, rightValue});
                if (!covers(binding.defined, 0, binding.width)) partialDefinitions_[binding.current] = binding.defined;
            }
        }
    }

    void loop(const Stmt& stmt) {
        const auto start = constants_.integer(expression(stmt.first), stmt.location, "loop start");
        const auto end = constants_.integer(expression(stmt.second), stmt.location, "loop limit");
        const auto step = constants_.integer(expression(stmt.third), stmt.location, "loop step");
        if (step <= 0) throw Error(stmt.location, "loop step must be positive");
        constexpr auto low = std::numeric_limits<std::int32_t>::min();
        constexpr auto high = std::numeric_limits<std::int32_t>::max();
        if (start < low || start > high || end < low || end > high || step > high)
            throw Error(stmt.location, "loop bounds must fit a signed 32-bit integer");
        for (auto i = start; i < end; i += step) {
            if (++iterations_ > maxIterations) throw Error(stmt.location, "static loop expansion exceeds compiler limit");
            if (i + step > high) throw Error(stmt.location, "loop increment overflows its index");
            scopes_.emplace_back();
            const auto text = "32'sd" + std::to_string(static_cast<std::uint32_t>(i));
            const auto index = make("literal", text, {32, true}, stmt.location);
            bind(stmt.location, stmt.name, {32, index, true, {{0, 32}}});
            block(stmt.body);
            scopes_.pop_back();
        }
    }

    void block(const std::vector<Stmt>& statements) {
        scopes_.emplace_back();
        for (const auto& stmt : statements) {
            switch (stmt.kind) {
            case StmtKind::Declare: declare(stmt); break;
            case StmtKind::Assign: assign(stmt); break;
            case StmtKind::If: branch(stmt); break;
            case StmtKind::For: loop(stmt); break;
            case StmtKind::Update:
            case StmtKind::Yield: {
                const bool update = stmt.kind == StmtKind::Update;
                const auto data = fit(expression(stmt.first), 32, stmt.location, update ? "bitmap" : "write-value");
                effect(update ? "promise.update" : "promise.yield", stmt.location, {data});
                break;
            }
            case StmtKind::Effect: effectCall(program_.expr(stmt.first)); break;
            }
        }
        scopes_.pop_back();
    }

    const Program& program_;
    const Instruction& source_;
    Options options_;
    InstructionIR ir_;
    Constants constants_;
    std::vector<Scope> scopes_;
    Stage stage_ = Stage::Decode;
    Guard guard_;
    std::map<ValueId, ValueId> decodeCopies_;
    std::map<ValueId, Ranges> partialDefinitions_;
    bool active_ = true;
    unsigned iterations_ = 0;
    ValueId ins_ = 0;
    ValueId pc_ = 0;
};
} // namespace

ModuleIR lower(const Program& program, Options options) {
    ModuleIR module;
    std::set<std::string> names;
    for (const auto& instruction : program.instructions) {
        if (!names.insert(instruction.name).second)
            throw Error(instruction.location, "duplicate instruction '" + instruction.name + "'");
        module.instructions.push_back(Lowerer(program, instruction, options).run());
        module.readUpperBound = std::max(module.readUpperBound, module.instructions.back().readUpperBound);
    }
    return module;
}

} // namespace mdr

