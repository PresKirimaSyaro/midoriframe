#pragma once

#include "ir.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <tuple>

namespace mdr::detail {

using Bits = std::uint64_t;
using Static = std::optional<Bits>;

inline Bits mask(unsigned width) {
    return width >= 64 ? ~Bits{0} : (Bits{1} << width) - 1;
}

inline Bits resize(Bits bits, unsigned from, unsigned to, bool sign) {
    bits &= mask(from);
    if (sign && from > 0 && from <= 64 && (bits & (Bits{1} << (from - 1)))) bits |= ~mask(from);
    return bits & mask(to);
}

inline std::int64_t signedBits(Bits bits, unsigned width) {
    return std::bit_cast<std::int64_t>(resize(bits, width, 64, true));
}

inline bool isOneOf(std::string_view text, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), text) != choices.end();
}

inline bool comparison(std::string_view op) {
    return isOneOf(op, {"==", "!=", "===", "!==", "<", "<=", ">", ">="});
}

inline bool shift(std::string_view op) {
    return isOneOf(op, {"<<", ">>", "<<<", ">>>"});
}

// Constant evaluation supports compile-time integers and bit vectors up to 64 bits.
// Wide or four-state values remain in the IR as expressions.
class Constants {
public:
    explicit Constants(const InstructionIR& ir) : ir_(ir) {}

    Static get(ValueId id, unsigned context = 0, std::optional<bool> signedContext = {}) const {
        if (ir_.value(id).sources != 0) return {};
        const auto key = std::tuple{id, context, signedContext};
        if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
        const auto result = evaluate(id, context, signedContext);
        cache_.emplace(key, result);
        return result;
    }

    std::int64_t integer(ValueId id, Location at, std::string_view description) const;

private:
    Static evaluate(ValueId id, unsigned context, std::optional<bool> signedContext) const {
        const auto& n = ir_.value(id);
        const auto width = std::max(context, n.type.width);
        const bool sign = signedContext.value_or(n.type.isSigned);
        if (n.sources != 0 || width > 64) return {};
        const auto& a = n.operands;
        if (n.op == "literal") {
            auto lit = parseLiteral(n.detail, n.location);
            if (lit.bits) return resize(*lit.bits, lit.width, width, sign);
        } else if (n.op == "fit" || n.op == "signed") {
            const auto bits = get(a[0], n.op == "fit" ? n.type.width : 0);
            if (bits) return resize(*bits, n.type.width, width, sign);
        } else if (n.op == "select" || n.op == "phi") {
            const auto test = get(a[0]);
            if (test) return get(a[*test != 0 ? 1 : 2], width, sign);
        } else if (n.op == "binary") {
            return binary(n, width, sign);
        } else if (n.op == "unary") {
            return unary(n, width, sign);
        } else if (isOneOf(n.op, {"bit", "slice", "indexed"})) {
            auto data = get(a[0]);
            auto start = get(a[1]);
            if (!data || !start) return {};
            if (n.op == "slice" || n.detail == "-:") {
                if (*start < n.type.width - 1) return {};
                *start -= n.type.width - 1;
            }
            const auto baseWidth = ir_.value(a[0]).type.width;
            if (*start >= baseWidth || n.type.width > baseWidth - *start) return {};
            return (*data >> *start) & mask(n.type.width);
        } else if (n.op == "concat" || n.op == "repeat") {
            Bits result = 0;
            const auto first = n.op == "repeat" ? 1u : 0u;
            const auto times = first ? n.type.width / ir_.value(a[1]).type.width : 1;
            for (unsigned count = 0; count < times; ++count) {
                for (std::size_t index = first; index < a.size(); ++index) {
                    const auto data = get(a[index]);
                    if (!data) return {};
                    const auto partWidth = ir_.value(a[index]).type.width;
                    result = partWidth == 64 ? *data : (result << partWidth) | *data;
                }
            }
            return result;
        }
        return {};
    }

    Static unary(const Value& n, unsigned width, bool sign) const {
        const auto id = n.operands[0];
        const auto& op = n.detail;
        const auto bits = isOneOf(op, {"+", "-", "~"}) ? get(id, width, sign) : get(id);
        if (!bits) return {};
        if (op == "+") return bits;
        if (op == "-") return (Bits{0} - *bits) & mask(width);
        if (op == "~") return ~*bits & mask(width);
        if (op == "!") return *bits == 0;
        const bool reduced = op.ends_with('&') ? *bits == mask(ir_.value(id).type.width) :
            op.ends_with('|') ? *bits != 0 : std::popcount(*bits) % 2 != 0;
        return reduced != (op.find('~') != std::string::npos);
    }

    Static binary(const Value& n, unsigned width, bool sign) const {
        const auto& op = n.detail;
        const auto x = n.operands[0];
        const auto y = n.operands[1];
        if (op == "&&" || op == "||") {
            const auto a = get(x);
            const auto b = get(y);
            if (a && b) return op == "&&" ? *a && *b : *a || *b;
            return {};
        }
        if (comparison(op)) {
            const auto left = ir_.value(x).type;
            const auto right = ir_.value(y).type;
            width = std::max(left.width, right.width);
            sign = left.isSigned && right.isSigned;
        }
        if (width > 64) return {};
        const auto a = get(x, width, sign);
        const auto b = shift(op) || op == "**" ? get(y) : get(y, width, sign);
        if (!a || !b) return {};
        if (op == "+") return (*a + *b) & mask(width);
        if (op == "-") return (*a - *b) & mask(width);
        if (op == "*") return (*a * *b) & mask(width);
        if (op == "&") return *a & *b;
        if (op == "|") return *a | *b;
        if (op == "^") return *a ^ *b;
        if (op == "~^" || op == "^~") return ~(*a ^ *b) & mask(width);
        if (op == "==" || op == "===") return *a == *b;
        if (op == "!=" || op == "!==") return *a != *b;
        if (op == "<<" || op == "<<<") return *b >= width ? 0 : (*a << *b) & mask(width);
        if (op == ">>" || (op == ">>>" && !sign)) return *b >= width ? 0 : *a >> *b;
        if (op == ">>>")
            return static_cast<Bits>(signedBits(*a, width) >> std::min(*b, Bits{63})) & mask(width);
        if (op == "**") {
            const auto type = ir_.value(y).type;
            if (type.isSigned && signedBits(*b, type.width) < 0) return {};
            Bits result = 1;
            Bits base = *a;
            for (Bits exponent = *b; exponent != 0; exponent >>= 1) {
                if (exponent & 1) result *= base;
                base *= base;
            }
            return result & mask(width);
        }
        const auto sa = signedBits(*a, width);
        const auto sb = signedBits(*b, width);
        if (op == "<") return sign ? sa < sb : *a < *b;
        if (op == "<=") return sign ? sa <= sb : *a <= *b;
        if (op == ">") return sign ? sa > sb : *a > *b;
        if (op == ">=") return sign ? sa >= sb : *a >= *b;
        if ((op == "/" || op == "%") && *b != 0) {
            if (!sign) return op == "/" ? *a / *b : *a % *b;
            if (sa == std::numeric_limits<std::int64_t>::min() && sb == -1) return op == "/" ? *a : 0;
            return static_cast<Bits>(op == "/" ? sa / sb : sa % sb) & mask(width);
        }
        return {};
    }

    const InstructionIR& ir_;
    mutable std::map<std::tuple<ValueId, unsigned, std::optional<bool>>, Static> cache_;
};

inline std::int64_t Constants::integer(ValueId id, Location at, std::string_view description) const {
        const auto bits = get(id);
        if (!bits) throw Error(at, std::string(description) + " must be a supported compile-time integer");
        const auto type = ir_.value(id).type;
        if (type.isSigned) return signedBits(*bits, type.width);
        if (*bits <= static_cast<Bits>(std::numeric_limits<std::int64_t>::max()))
            return static_cast<std::int64_t>(*bits);
        throw Error(at, std::string(description) + " is out of range");
    }

} // namespace mdr::detail

