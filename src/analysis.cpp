#include "constant.hpp"

#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace mdr {
namespace {
using detail::Constants;

std::optional<Guard> with(Guard guard, Predicate predicate) {
    for (const auto& term : guard) {
        if (term.value == predicate.value)
            return term.positive == predicate.positive ? std::optional{std::move(guard)} : std::nullopt;
    }
    guard.push_back(predicate);
    return guard;
}

class AddressClasses {
public:
    explicit AddressClasses(const InstructionIR& ir) : ir_(ir) {}

    unsigned classify(ValueId id) {
        if (auto found = classes_.find(id); found != classes_.end()) return found->second;
        const auto& node = ir_.value(id);
        std::vector<unsigned> operands;
        for (auto operand : node.operands) operands.push_back(classify(operand));
        const auto detail = node.op == "fit" || node.op == "phi" ? "" : node.detail;
        auto key = std::tuple{node.op, detail, node.type.width, node.type.isSigned, std::move(operands)};
        auto [entry, inserted] = keys_.try_emplace(std::move(key), static_cast<unsigned>(keys_.size()));
        (void)inserted;
        classes_.emplace(id, entry->second);
        return entry->second;
    }

private:
    using Key = std::tuple<std::string, std::string, unsigned, bool, std::vector<unsigned>>;
    std::map<Key, unsigned> keys_;
    std::map<ValueId, unsigned> classes_;
    const InstructionIR& ir_;
};

void analyzeReads(InstructionIR& ir, const Constants& constants) {
    AddressClasses classes(ir);
    std::map<std::pair<bool, unsigned>, Guard> addressGuards;
    for (const auto& read : ir.reads) {
        const auto index = constants.get(read.address);
        const auto key = std::pair{index.has_value(), index ? static_cast<unsigned>(*index) : classes.classify(read.address)};
        auto [entry, inserted] = addressGuards.emplace(key, read.decodeEnable);
        if (!inserted) {
            // A conjunction common to all uses is a safe upper bound on port usage.
            std::erase_if(entry->second, [&](auto term) {
                return std::find(read.decodeEnable.begin(), read.decodeEnable.end(), term) == read.decodeEnable.end();
            });
        }
    }
    std::map<ValueId, unsigned> tests;
    for (const auto& [address, guard] : addressGuards) {
        (void)address;
        for (auto term : guard) {
            const auto index = static_cast<unsigned>(tests.size());
            tests.try_emplace(term.value, index);
        }
    }
    unsigned upper = static_cast<unsigned>(addressGuards.size());
    if (tests.size() <= 12 && upper <= 256) {
        upper = 0;
        for (unsigned bits = 0; bits < (1u << tests.size()); ++bits) {
            unsigned active = 0;
            for (const auto& [address, guard] : addressGuards) {
                (void)address;
                if (std::all_of(guard.begin(), guard.end(), [&](auto term) {
                        return ((bits >> tests.at(term.value) & 1u) != 0) == term.positive;
                    })) ++active;
            }
            upper = std::max(upper, active);
        }
    }
    ir.readUpperBound = std::min(upper, 31u);
}

void analyzeCarries(InstructionIR& ir) {
    std::set<std::tuple<Stage, Stage, ValueId, bool>> carry;
    auto add = [&](ValueId id, Stage from, Stage use, bool refresh) {
        for (int i = static_cast<int>(from); i < static_cast<int>(use); ++i)
            carry.emplace(static_cast<Stage>(i), static_cast<Stage>(i + 1), id, refresh);
    };
    auto consume = [&](ValueId id, Stage stage) {
        const auto& item = ir.value(id);
        if (item.stage >= Stage::D && item.sources != 0)
            add(id, item.stage, stage, false);
    };
    // Only operations that feed effects are live. A logically authored RF read
    // is also retained so its consumption stage remains visible in the analysis.
    std::vector<bool> live(ir.values.size() + 1);
    for (const auto& effect : ir.effects) {
        for (auto arg : effect.operands) {
            live[arg] = true;
            consume(arg, effect.stage);
        }
        for (const auto& term : effect.guard) {
            live[term.value] = true;
            consume(term.value, effect.stage);
        }
    }
    for (const auto& read : ir.reads) {
        live[read.value] = true;
        for (const auto& term : read.guard) {
            live[term.value] = true;
            consume(term.value, read.use);
        }
        add(read.value, Stage::D, read.use, true);
    }
    live[ir.match] = true;
    for (auto id = static_cast<ValueId>(ir.values.size()); id > 0; --id) {
        if (!live[id]) continue;
        const auto& item = ir.value(id);
        for (auto arg : item.operands) {
            live[arg] = true;
            consume(arg, item.stage);
        }
    }
    for (auto [from, to, id, refresh] : carry) ir.carries.push_back({id, from, to, refresh});
}

std::uint32_t possibleMask(const InstructionIR& ir, const Constants& constants, ValueId id, unsigned context = 32) {
    const auto& node = ir.value(id);
    const auto width = std::max(context, node.type.width);
    if (auto bits = constants.get(id, width)) return static_cast<std::uint32_t>(*bits);
    const auto& a = node.operands;
    auto possible = [&](ValueId child) { return possibleMask(ir, constants, child, width); };
    if (node.op == "fit" && !ir.value(a[0]).type.isSigned) {
        const auto child = possibleMask(ir, constants, a[0], node.type.width);
        return child & static_cast<std::uint32_t>(detail::mask(node.type.width));
    }
    if ((node.op == "select" || node.op == "phi") &&
        !ir.value(a[1]).type.isSigned && !ir.value(a[2]).type.isSigned)
        return possible(a[1]) | possible(a[2]);
    if (node.op == "binary" && !ir.value(a[0]).type.isSigned && !ir.value(a[1]).type.isSigned) {
        if (node.detail == "|") return possible(a[0]) | possible(a[1]);
        if (node.detail == "&") return possible(a[0]) & possible(a[1]);
        if (node.detail == "<<" && constants.get(a[0], width) == 1 && ir.value(a[1]).type.width < 5)
            return static_cast<std::uint32_t>(detail::mask(1u << ir.value(a[1]).type.width));
    }
    return 0xffffffffu;
}

void analyzeWrites(InstructionIR& ir, const Constants& constants) {
    constexpr std::size_t maxPaths = 256;
    std::vector<WritePath> paths(1);
    bool seen = false;
    auto eventMask = [&](EffectId id) {
        return possibleMask(ir, constants, ir.effects[id - 1].operands.front()) & 0xfffffffeu;
    };
    for (EffectId id = 1; id <= ir.effects.size(); ++id) {
        const auto& effect = ir.effects[id - 1];
        const bool update = effect.op == "promise.update";
        if (!update && effect.op != "promise.yield") continue;
        seen = true;
        const auto mask = update ? eventMask(id) : 0;
        if (update) ir.initialMask |= mask;
        if (!ir.promisePathsComplete) continue;
        std::vector<WritePath> next;
        for (auto path : paths) {
            std::optional<Guard> enabled = path.guard;
            for (auto term : effect.guard) {
                if (!enabled) break;
                if (auto disabled = with(*enabled, {term.value, !term.positive})) {
                    auto skipped = path;
                    skipped.guard = std::move(*disabled);
                    next.push_back(std::move(skipped));
                }
                enabled = with(std::move(*enabled), term);
            }
            if (enabled) {
                path.guard = std::move(*enabled);
                if (update) {
                    if (path.updates.empty()) path.initialMask = mask;
                    path.updates.push_back(id);
                } else {
                    path.yield = id;
                }
                next.push_back(std::move(path));
            }
            if (next.size() > maxPaths) break;
        }
        if (next.size() > maxPaths) {
            ir.promisePathsComplete = false;
            paths.clear();
        } else {
            paths = std::move(next);
        }
    }
    if (!seen || !ir.promisePathsComplete) return;
    ir.initialMask = 0;
    for (auto& path : paths) {
        ir.initialMask |= path.initialMask;
        auto candidates = path.initialMask;
        for (auto update : path.updates) candidates &= eventMask(update);
        if (candidates != 0 && path.yield != 0) {
            path.readyAfter = std::max(ir.effects[path.updates.back() - 1].stage,
                                       ir.effects[path.yield - 1].stage);
            for (auto term : path.guard)
                path.readyAfter = std::max(path.readyAfter, ir.value(term.value).stage);
        }
    }
    ir.writes = std::move(paths);
}

} // namespace

void analyze(InstructionIR& ir) {
    const Constants constants(ir);
    analyzeReads(ir, constants);
    analyzeCarries(ir);
    analyzeWrites(ir, constants);
}

} // namespace mdr

