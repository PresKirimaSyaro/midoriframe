#include "frontend.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <limits>
#include <utility>

namespace mdr {
namespace {

enum class TokenKind { Name, Number, Symbol, End };

struct Token {
    TokenKind kind;
    Location location;
    std::string text;
};

bool isAlpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }
bool isWord(char c) { return isAlpha(c) || isDigit(c) || c == '$'; }

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {
        if (source_.starts_with("\xef\xbb\xbf")) position_ = 3;
    }

    Token next() {
        skipSpace();
        Location start = location_;
        if (position_ == source_.size()) return {TokenKind::End, start, ""};

        std::size_t begin = position_;
        char c = take();
        if (isAlpha(c)) {
            while (isWord(peek())) take();
            return token(TokenKind::Name, start, begin);
        }
        if (isDigit(c) || c == '\'') {
            while (isWord(peek()) || peek() == '\'' || (peek() == '?' && numberHasBase(begin))) take();
            return token(TokenKind::Number, start, begin);
        }

        static constexpr std::array<std::string_view, 23> symbols{
            ">>>", "<<<", "===", "!==", "<<", ">>", "==", "!=", "<=", ">=",
            "&&", "||", "+:", "-:", "**", "~&", "~|", "~^", "^~",
            "+", "-", "*", "/"
        };
        const auto rest = source_.substr(begin);
        for (auto symbol : symbols) {
            if (rest.starts_with(symbol)) {
                for (std::size_t i = 1; i < symbol.size(); ++i) take();
                return token(TokenKind::Symbol, start, begin);
            }
        }
        if (std::string_view("{}[]():;,?=<>!~%&|^").find(c) != std::string_view::npos) {
            return token(TokenKind::Symbol, start, begin);
        }
        throw Error(start, "unexpected character '" + std::string(1, c) + "'");
    }

private:
    char peek() const { return position_ < source_.size() ? source_[position_] : '\0'; }

    char take() {
        char c = source_[position_++];
        if (c == '\n') {
            ++location_.line;
            location_.column = 1;
        } else {
            ++location_.column;
        }
        return c;
    }

    bool numberHasBase(std::size_t start) const {
        return source_.substr(start, position_ - start).find('\'') != std::string_view::npos;
    }

    Token token(TokenKind kind, Location location, std::size_t start) const {
        return {kind, location, std::string(source_.substr(start, position_ - start))};
    }

    void skipSpace() {
        for (;;) {
            if (std::isspace(static_cast<unsigned char>(peek()))) {
                take();
            } else if (source_.substr(position_).starts_with("//")) {
                while (position_ < source_.size() && peek() != '\n') take();
            } else if (source_.substr(position_).starts_with("/*")) {
                Location start = location_;
                take();
                take();
                while (position_ < source_.size() && !source_.substr(position_).starts_with("*/")) take();
                if (position_ == source_.size()) throw Error(start, "unterminated block comment");
                take();
                take();
            } else {
                return;
            }
        }
    }

    std::string_view source_;
    std::size_t position_ = 0;
    Location location_;
};

int precedence(std::string_view op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "|") return 3;
    if (op == "^" || op == "~^" || op == "^~") return 4;
    if (op == "&") return 5;
    if (op == "==" || op == "!=" || op == "===" || op == "!==") return 6;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 7;
    if (op == "<<" || op == ">>" || op == "<<<" || op == ">>>") return 8;
    if (op == "+" || op == "-") return 9;
    if (op == "*" || op == "/" || op == "%") return 10;
    if (op == "**") return 11;
    return 0;
}

bool unary(std::string_view op) {
    return op == "+" || op == "-" || op == "!" || op == "~" || op == "&" ||
        op == "|" || op == "^" || op == "~&" || op == "~|" || op == "~^" || op == "^~";
}

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), current_(lexer_.next()) {}

    Program run() {
        while (current_.kind != TokenKind::End) program_.instructions.push_back(instruction());
        if (program_.instructions.empty()) throw Error(current_.location, "expected an instruction definition");
        return std::move(program_);
    }

private:
    Token take() { return std::exchange(current_, lexer_.next()); }
    bool accept(std::string_view text) {
        if (current_.text != text) return false;
        take();
        return true;
    }
    void expect(std::string_view text) {
        if (!accept(text)) throw Error(current_.location, "expected '" + std::string(text) + "'");
    }
    Token name() {
        if (current_.kind != TokenKind::Name) throw Error(current_.location, "expected identifier");
        return take();
    }
    ExprId add(ExprKind kind, const Token& token, std::vector<ExprId> operands = {}) {
        program_.expressions.push_back({kind, token.location, token.text, std::move(operands)});
        return static_cast<ExprId>(program_.expressions.size());
    }

    Instruction instruction() {
        auto token = name();
        Instruction result{token.text, token.location, noExpr, {}, {}};
        expect("{");
        Stage previous = Stage::Decode;
        while (!accept("}")) {
            auto item = name();
            if (item.text == "match") {
                if (result.match != noExpr) throw Error(item.location, "duplicate match");
                expect("(");
                result.match = expression();
                expect(")");
                expect(";");
            } else if (item.text == "D" || item.text == "E" || item.text == "M") {
                auto stage = item.text == "D" ? Stage::D : item.text == "E" ? Stage::E : Stage::M;
                if (stage <= previous) throw Error(item.location, "expected stage blocks in D, E, M order, at most once each");
                previous = stage;
                result.stages.push_back({stage, block()});
            } else {
                if (previous != Stage::Decode || current_.text != ":")
                    throw Error(item.location, "expected match, outer declaration, or stage block");
                result.declarations.push_back(declaration(item));
            }
        }
        if (result.match == noExpr) throw Error(result.location, "instruction requires one match");
        return result;
    }

    Stmt declaration(const Token& token) {
        expect(":");
        Stmt result{StmtKind::Declare, token.location, token.text, expression(), noExpr, noExpr, {}, {}};
        if (accept("=")) result.second = expression();
        expect(";");
        return result;
    }

    std::vector<Stmt> block() {
        expect("{");
        std::vector<Stmt> statements;
        while (!accept("}")) statements.push_back(statement());
        return statements;
    }

    std::vector<Stmt> controlBody() {
        if (current_.text == "{") return block();
        std::vector<Stmt> statements;
        statements.push_back(statement());
        return statements;
    }

    Stmt statement() {
        auto token = name();
        Stmt result{StmtKind::Effect, token.location, token.text, noExpr, noExpr, noExpr, {}, {}};
        if (current_.text == ":") return declaration(token);
        if (token.text == "if") {
            result.kind = StmtKind::If;
            expect("(");
            result.first = expression();
            expect(")");
            result.body = controlBody();
            if (accept("else")) result.otherwise = controlBody();
        } else if (token.text == "for") {
            result.kind = StmtKind::For;
            expect("(");
            result.name = name().text;
            expect("=");
            result.first = expression();
            expect(";");
            expect(result.name);
            expect("<");
            result.second = expression();
            expect(";");
            expect(result.name);
            expect("=");
            expect(result.name);
            expect("+");
            result.third = expression();
            expect(")");
            result.body = controlBody();
        } else if (token.text == "update" || token.text == "yield") {
            result.kind = token.text == "update" ? StmtKind::Update : StmtKind::Yield;
            result.first = expression();
            expect(";");
        } else {
            auto value = postfix(token);
            if (accept("=")) {
                result.kind = StmtKind::Assign;
                result.first = value;
                result.second = expression();
            } else if (program_.expr(value).kind == ExprKind::Call) {
                result.first = value;
            } else {
                throw Error(token.location, "expected an assignment or effect");
            }
            expect(";");
        }
        return result;
    }

    ExprId expression(int min = 1) {
        auto left = primary();
        for (;;) {
            const int level = precedence(current_.text);
            if (level < min) break;
            auto op = take();
            auto right = expression(level + 1);
            left = add(ExprKind::Binary, op, {left, right});
        }
        if (min == 1 && current_.text == "?") {
            auto op = take();
            auto yes = expression();
            expect(":");
            auto no = expression();
            left = add(ExprKind::Select, op, {left, yes, no});
        }
        return left;
    }

    ExprId primary() {
        auto token = take();
        if (unary(token.text)) return add(ExprKind::Unary, token, {primary()});
        if (token.kind == TokenKind::Name) return postfix(token);
        if (token.kind == TokenKind::Number) {
            parseLiteral(token.text, token.location);
            return slices(add(ExprKind::Literal, token));
        }
        if (token.text == "(") {
            auto result = expression();
            expect(")");
            return slices(result);
        }
        if (token.text == "{") {
            auto first = expression();
            if (current_.text == "{") {
                auto part = primary();
                expect("}");
                return slices(add(ExprKind::Repeat, token, {first, part}));
            }
            std::vector<ExprId> parts{first};
            while (accept(",")) parts.push_back(expression());
            expect("}");
            return slices(add(ExprKind::Concat, token, std::move(parts)));
        }
        throw Error(token.location, "expected expression");
    }

    ExprId postfix(const Token& token) {
        if (!accept("(")) return slices(add(ExprKind::Name, token));
        std::vector<ExprId> arguments;
        if (!accept(")")) {
            do { arguments.push_back(expression()); } while (accept(","));
            expect(")");
        }
        return slices(add(ExprKind::Call, token, std::move(arguments)));
    }

    ExprId slices(ExprId value) {
        while (current_.text == "[") {
            auto token = take();
            auto start = expression();
            if (current_.text == ":" || current_.text == "+:" || current_.text == "-:") {
                auto op = take();
                auto end = expression();
                auto kind = op.text == ":" ? ExprKind::Slice : ExprKind::IndexedSlice;
                value = add(kind, op, {value, start, end});
            } else {
                value = add(ExprKind::Bit, token, {value, start});
            }
            expect("]");
        }
        return value;
    }

    Lexer lexer_;
    Token current_;
    Program program_;
};

} // namespace

const char* stageName(Stage stage) {
    switch (stage) {
    case Stage::Decode: return "decode";
    case Stage::D: return "D";
    case Stage::E: return "E";
    case Stage::M: return "M";
    case Stage::W: return "W";
    }
    return "?";
}

Literal parseLiteral(std::string_view text, Location location) {
    auto fail = [location, text] { throw Error(location, "invalid Verilog literal '" + std::string(text) + "'"); };
    const auto apostrophe = text.find('\'');
    unsigned width = 32;
    bool isSigned = true;
    unsigned base = 10;
    auto digits = text;

    const bool sized = apostrophe != 0 && apostrophe != std::string_view::npos;
    if (apostrophe != std::string_view::npos) {
        width = sized ? 0 : 32;
        for (char c : text.substr(0, apostrophe)) {
            if (c == '_') continue;
            if (!isDigit(c) || width > (std::numeric_limits<unsigned>::max() - 9) / 10) fail();
            width = width * 10 + static_cast<unsigned>(c - '0');
        }
        if (width == 0) fail();
        digits = text.substr(apostrophe + 1);
        isSigned = !digits.empty() && (digits.front() == 's' || digits.front() == 'S');
        if (isSigned) digits.remove_prefix(1);
        if (digits.empty()) fail();
        switch (static_cast<char>(std::tolower(static_cast<unsigned char>(digits.front())))) {
        case 'b': base = 2; break;
        case 'o': base = 8; break;
        case 'd': base = 10; break;
        case 'h': base = 16; break;
        default: fail();
        }
        digits.remove_prefix(1);
    }

    bool known = true;
    unsigned digitCount = 0;
    bool overflow = false;
    std::uint64_t bits = 0;
    for (char c : digits) {
        if (c == '_') continue;
        ++digitCount;
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (apostrophe != std::string_view::npos && (c == 'x' || c == 'z' || c == '?')) {
            known = false;
            continue;
        }
        const unsigned value = isDigit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
        if (value >= base) fail();
        overflow |= bits > (std::numeric_limits<std::uint64_t>::max() - value) / base;
        bits = bits * base + value;
    }
    if (digitCount == 0) fail();
    if (!sized) {
        if (base == 10) {
            if (overflow) throw Error(location, "large decimal literals require an explicit width");
            const auto magnitude = static_cast<unsigned>(std::bit_width(bits));
            const auto sign = apostrophe == std::string_view::npos ? 1u : 0u;
            width = std::max(32u, magnitude + sign);
        } else {
            const auto digitWidth = base == 2 ? 1u : base == 8 ? 3u : 4u;
            width = std::max(32u, digitWidth * digitCount);
        }
    }
    if (width != 0 && width < 64) bits &= (std::uint64_t{1} << width) - 1;
    return {width, isSigned, known && (width <= 64 && width != 0) ? std::optional{bits} : std::nullopt};
}

Program parse(std::string_view source) { return Parser(source).run(); }

} // namespace mdr

