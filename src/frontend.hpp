#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mdr {

struct Location {
    unsigned line = 1;
    unsigned column = 1;
};

struct Error : std::runtime_error {
    Location location;
    Error(Location where, const std::string& message)
        : std::runtime_error(message), location(where) {}
};

using ExprId = unsigned;
inline constexpr ExprId noExpr = 0;

enum class ExprKind {
    Literal, Name, Unary, Binary, Select, Call, Bit, Slice, IndexedSlice, Concat, Repeat
};

struct Expr {
    ExprKind kind;
    Location location;
    std::string text;
    std::vector<ExprId> operands;
};

enum class StmtKind { Declare, Assign, If, For, Update, Yield, Effect };

enum class LoopComparison { Less, LessEqual, Greater, GreaterEqual };

struct Stmt {
    StmtKind kind;
    Location location;
    std::string name;
    ExprId first = noExpr;
    ExprId second = noExpr;
    ExprId third = noExpr;
    std::vector<Stmt> body;
    std::vector<Stmt> otherwise;
    LoopComparison loopComparison = LoopComparison::Less;
    bool subtractStep = false;
};

enum class Stage : int { Decode = -1, D, E, M, W };
const char* stageName(Stage stage);

struct StageBlock {
    std::size_t outerDeclarationCount;
    Stage stage;
    std::vector<Stmt> statements;
};

struct Instruction {
    std::string name;
    Location location;
    ExprId match = noExpr;
    std::vector<Stmt> declarations;
    std::vector<StageBlock> stages;
};

struct Program {
    std::vector<Expr> expressions;
    std::vector<Instruction> instructions;
    const Expr& expr(ExprId id) const { return expressions.at(id - 1); }
};

struct Literal {
    unsigned width;
    bool isSigned;
    std::optional<std::uint64_t> bits;
};

Literal parseLiteral(std::string_view text, Location location);
Program parse(std::string_view source);

} // namespace mdr

