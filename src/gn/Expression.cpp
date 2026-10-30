#include "gn/Expression.h"

#include "gn/Texture.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>

namespace lila::gn::expr
{
using D4 = glm::dvec4;

const char* typeName(Type t)
{
    switch (t)
    {
    case Type::Float: return "float";
    case Type::Int: return "int";
    case Type::Vector: return "vector";
    default: return "vector4";
    }
}

CompileError::CompileError(const std::string& message, int line, int column)
    : std::runtime_error("line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message), line(line), column(column)
{
}

namespace lang
{
constexpr size_t kMaxIterations = 1000000;

bool scalar(Type t) { return t == Type::Float || t == Type::Int; }

D4 fix(D4 v, Type t)
{
    switch (t)
    {
    case Type::Float: return {v.x, 0, 0, 0};
    case Type::Int: return {std::trunc(v.x), 0, 0, 0};
    case Type::Vector: return {v.x, v.y, v.z, 0};
    default: return v;
    }
}

D4 conv(const D4& v, Type from, Type to)
{
    if (from == to)
        return v;
    if (scalar(from))
    {
        switch (to)
        {
        case Type::Float: return {v.x, 0, 0, 0};
        case Type::Int: return {std::trunc(v.x), 0, 0, 0};
        case Type::Vector: return {v.x, v.x, v.x, 0};
        default: return D4(v.x);
        }
    }
    return fix(v, to);
}

bool truthy(const D4& v) { return v.x != 0.0 || v.y != 0.0 || v.z != 0.0 || v.w != 0.0; }

enum class Tok : uint8_t
{
    End,
    Number,
    Ident,
    String,
    Attr,
    Punct,
};

struct Token
{
    Tok kind = Tok::End;
    std::string text;
    double number = 0.0;
    bool isInt = false;
    char prefix = 0; // f, i, v or p before an @
    int line = 1, col = 1;
};

std::vector<Token> lex(std::string_view src)
{
    std::vector<Token> out;
    size_t i = 0;
    int line = 1, col = 1;
    auto advance = [&](size_t n) {
        for (size_t k = 0; k < n && i < src.size(); ++k, ++i)
        {
            if (src[i] == '\n')
            {
                ++line;
                col = 1;
            }
            else
                ++col;
        }
    };
    auto isIdent = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
    while (i < src.size())
    {
        char ch = src[i];
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            advance(1);
            continue;
        }
        if (ch == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            while (i < src.size() && src[i] != '\n')
                advance(1);
            continue;
        }
        if (ch == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            Token t{.line = line, .col = col};
            advance(2);
            while (i < src.size() && !(src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/'))
                advance(1);
            if (i >= src.size())
                throw CompileError("unterminated comment", t.line, t.col);
            advance(2);
            continue;
        }
        Token t{.line = line, .col = col};
        bool digitNext = i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1]));
        if (std::isdigit(static_cast<unsigned char>(ch)) || (ch == '.' && digitNext))
        {
            size_t j = i;
            bool isInt = true;
            while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j])))
                ++j;
            if (j < src.size() && src[j] == '.')
            {
                isInt = false;
                ++j;
                while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j])))
                    ++j;
            }
            if (j < src.size() && (src[j] == 'e' || src[j] == 'E'))
            {
                size_t k = j + 1;
                if (k < src.size() && (src[k] == '+' || src[k] == '-'))
                    ++k;
                if (k < src.size() && std::isdigit(static_cast<unsigned char>(src[k])))
                {
                    isInt = false;
                    j = k;
                    while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j])))
                        ++j;
                }
            }
            t.kind = Tok::Number;
            t.text = std::string(src.substr(i, j - i));
            t.number = std::strtod(t.text.c_str(), nullptr);
            t.isInt = isInt;
            advance(j - i);
            out.push_back(std::move(t));
            continue;
        }
        bool typedAttr = i + 1 < src.size() && src[i + 1] == '@' && (ch == 'f' || ch == 'i' || ch == 'v' || ch == 'p') &&
                         (i == 0 || !isIdent(src[i - 1]));
        if (ch == '@' || typedAttr)
        {
            if (typedAttr)
            {
                t.prefix = ch;
                advance(1);
            }
            advance(1);
            size_t j = i;
            while (j < src.size() && isIdent(src[j]))
                ++j;
            if (j == i)
                throw CompileError("expected an attribute name after @", t.line, t.col);
            t.kind = Tok::Attr;
            t.text = std::string(src.substr(i, j - i));
            advance(j - i);
            out.push_back(std::move(t));
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')
        {
            size_t j = i;
            while (j < src.size() && isIdent(src[j]))
                ++j;
            t.kind = Tok::Ident;
            t.text = std::string(src.substr(i, j - i));
            advance(j - i);
            out.push_back(std::move(t));
            continue;
        }
        if (ch == '"')
        {
            size_t j = i + 1;
            while (j < src.size() && src[j] != '"' && src[j] != '\n')
                ++j;
            if (j >= src.size() || src[j] != '"')
                throw CompileError("unterminated string", t.line, t.col);
            t.kind = Tok::String;
            t.text = std::string(src.substr(i + 1, j - i - 1));
            advance(j + 1 - i);
            out.push_back(std::move(t));
            continue;
        }
        static const char* twoChar[] = {"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "++", "--"};
        t.kind = Tok::Punct;
        for (const char* p : twoChar)
            if (src.substr(i, 2) == p)
                t.text = p;
        if (t.text.empty())
        {
            if (std::string_view("+-*/%^<>!?:=(){},;.").find(ch) == std::string_view::npos)
                throw CompileError(std::string("unexpected character '") + ch + "'", t.line, t.col);
            t.text = std::string(1, ch);
        }
        advance(t.text.size());
        out.push_back(std::move(t));
    }
    Token end{.line = line, .col = col};
    out.push_back(end);
    return out;
}

enum class Op : uint8_t
{
    Const,
    Local,
    Attr,
    Input,
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Pow,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Select,
    Component,
    Convert,
    Call,
    // Statements.
    Block,
    StoreLocal,
    StoreAttr,
    If,
    For,
    While,
    Break,
    Continue,
    Eval,
    Nop,
};

enum class Fn : uint8_t
{
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Sqrt,
    Pow,
    Exp,
    Log,
    Abs,
    Floor,
    Ceil,
    Frac,
    Sign,
    Radians,
    Degrees,
    Min,
    Max,
    Clamp,
    Lerp,
    Fit,
    Smooth,
    Length,
    Normalize,
    Dot,
    Cross,
    Distance,
    Rand,
    Noise,
    Set,
    ToInt,
    ToFloat,
};

struct Node
{
    Op op = Op::Nop;
    Type type = Type::Float;
    int a = -1, b = -1, c = -1, d = -1;
    int slot = -1;
    int comp = -1; // component read or written, -1 for the whole value
    Fn fn = Fn::Sin;
    D4 k{0.0};
    std::vector<int> args;
};

enum class Flow : uint8_t
{
    Normal,
    Break,
    Continue,
};

struct State
{
    std::vector<D4> locals;
    std::vector<D4> attrs;
    const Bindings* b = nullptr;
    size_t element = 0;
};
} // namespace lang

struct Program::Impl
{
    std::vector<lang::Node> nodes;
    std::vector<int> top;
    std::vector<AttrRef> attrs;
    int numLocals = 0;
    int result = -1; // the final Eval statement
    Type resultType = Type::Float;
    std::array<bool, 4> inputs{};

    D4 eval(int n, lang::State& s) const;
    lang::Flow exec(int n, lang::State& s) const;
};

namespace lang
{
double safeDiv(double a, double b) { return b == 0.0 ? 0.0 : a / b; }
double safeMod(double a, double b) { return b == 0.0 ? 0.0 : std::fmod(a, b); }

template <class F> D4 each(const D4& a, F f) { return {f(a.x), f(a.y), f(a.z), f(a.w)}; }
template <class F> D4 each(const D4& a, const D4& b, F f) { return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w)}; }

double length(const D4& v, Type t)
{
    if (scalar(t))
        return std::abs(v.x);
    return std::sqrt(glm::dot(v, v));
}

D4 callFn(const Node& n, const std::vector<D4>& a, const std::vector<Type>& types)
{
    const Type t = n.type;
    switch (n.fn)
    {
    case Fn::Sin: return fix(each(a[0], [](double x) { return std::sin(x); }), t);
    case Fn::Cos: return fix(each(a[0], [](double x) { return std::cos(x); }), t);
    case Fn::Tan: return fix(each(a[0], [](double x) { return std::tan(x); }), t);
    case Fn::Asin: return fix(each(a[0], [](double x) { return std::asin(std::clamp(x, -1.0, 1.0)); }), t);
    case Fn::Acos: return fix(each(a[0], [](double x) { return std::acos(std::clamp(x, -1.0, 1.0)); }), t);
    case Fn::Atan: return fix(each(a[0], [](double x) { return std::atan(x); }), t);
    case Fn::Atan2: return fix(each(a[0], a[1], [](double y, double x) { return std::atan2(y, x); }), t);
    case Fn::Sqrt: return fix(each(a[0], [](double x) { return x > 0.0 ? std::sqrt(x) : 0.0; }), t);
    case Fn::Pow: return fix(each(a[0], a[1], [](double x, double y) { return std::pow(x, y); }), t);
    case Fn::Exp: return fix(each(a[0], [](double x) { return std::exp(x); }), t);
    case Fn::Log: return fix(each(a[0], [](double x) { return x > 0.0 ? std::log(x) : 0.0; }), t);
    case Fn::Abs: return fix(each(a[0], [](double x) { return std::abs(x); }), t);
    case Fn::Floor: return fix(each(a[0], [](double x) { return std::floor(x); }), t);
    case Fn::Ceil: return fix(each(a[0], [](double x) { return std::ceil(x); }), t);
    case Fn::Frac: return fix(each(a[0], [](double x) { return x - std::floor(x); }), t);
    case Fn::Sign: return fix(each(a[0], [](double x) { return double((x > 0.0) - (x < 0.0)); }), t);
    case Fn::Radians: return fix(a[0] * (3.14159265358979323846 / 180.0), t);
    case Fn::Degrees: return fix(a[0] * (180.0 / 3.14159265358979323846), t);
    case Fn::Min: return fix(each(a[0], a[1], [](double x, double y) { return std::min(x, y); }), t);
    case Fn::Max: return fix(each(a[0], a[1], [](double x, double y) { return std::max(x, y); }), t);
    case Fn::Clamp:
    {
        D4 r;
        for (int k = 0; k < 4; ++k)
            r[k] = std::min(std::max(a[0][k], a[1][k]), a[2][k]);
        return fix(r, t);
    }
    case Fn::Lerp: return fix(a[0] + (a[1] - a[0]) * a[2], t);
    case Fn::Fit:
    {
        D4 r;
        for (int k = 0; k < 4; ++k)
        {
            double span = a[2][k] - a[1][k];
            double u = span == 0.0 ? 0.0 : std::clamp((a[0][k] - a[1][k]) / span, 0.0, 1.0);
            r[k] = a[3][k] + (a[4][k] - a[3][k]) * u;
        }
        return fix(r, t);
    }
    case Fn::Smooth:
    {
        D4 r;
        for (int k = 0; k < 4; ++k)
        {
            double span = a[1][k] - a[0][k];
            double u = span == 0.0 ? (a[2][k] < a[0][k] ? 0.0 : 1.0) : std::clamp((a[2][k] - a[0][k]) / span, 0.0, 1.0);
            r[k] = u * u * (3.0 - 2.0 * u);
        }
        return fix(r, t);
    }
    case Fn::Length: return {length(a[0], types[0]), 0, 0, 0};
    case Fn::Normalize:
    {
        double l = std::sqrt(glm::dot(a[0], a[0]));
        return l > 1e-300 ? a[0] / l : D4(0.0);
    }
    case Fn::Dot: return {glm::dot(a[0], a[1]), 0, 0, 0};
    case Fn::Cross: return D4(glm::cross(glm::dvec3(a[0]), glm::dvec3(a[1])), 0.0);
    case Fn::Distance:
    {
        D4 d = a[0] - a[1];
        return {std::sqrt(glm::dot(d, d)), 0, 0, 0};
    }
    case Fn::Rand:
    {
        const D4& v = a[0];
        switch (types[0])
        {
        case Type::Vector: return {tex::hashToFloat(v.x, v.y, v.z), 0, 0, 0};
        case Type::Vector4: return {tex::hashToFloat(v.x, v.y, v.z, v.w), 0, 0, 0};
        default: return {tex::hashToFloat(v.x), 0, 0, 0};
        }
    }
    case Fn::Noise: return {0.5 + 0.5 * tex::gradientNoise4(a[0]), 0, 0, 0};
    case Fn::Set:
        return t == Type::Vector ? D4(a[0].x, a[1].x, a[2].x, 0.0) : D4(a[0].x, a[1].x, a[2].x, a[3].x);
    case Fn::ToInt: return {std::trunc(a[0].x), 0, 0, 0};
    case Fn::ToFloat: return {a[0].x, 0, 0, 0};
    }
    return D4(0.0);
}

D4 arith(Op op, const D4& a, const D4& b, Type t)
{
    const bool integer = t == Type::Int;
    switch (op)
    {
    case Op::Add: return fix(a + b, t);
    case Op::Sub: return fix(a - b, t);
    case Op::Mul: return fix(a * b, t);
    case Op::Div: return fix(each(a, b, safeDiv), t);
    case Op::Mod: return fix(each(a, b, safeMod), t);
    default:
        return fix(each(a, b, [integer](double x, double y) { return integer ? std::round(std::pow(x, y)) : std::pow(x, y); }), t);
    }
}

bool compare(Op op, const D4& a, const D4& b)
{
    switch (op)
    {
    case Op::Eq: return a == b;
    case Op::Ne: return a != b;
    case Op::Lt: return a.x < b.x;
    case Op::Le: return a.x <= b.x;
    case Op::Gt: return a.x > b.x;
    default: return a.x >= b.x;
    }
}
} // namespace lang

D4 Program::Impl::eval(int i, lang::State& s) const
{
    using namespace lang;
    const Node& n = nodes[size_t(i)];
    switch (n.op)
    {
    case Op::Const: return n.k;
    case Op::Local: return s.locals[size_t(n.slot)];
    case Op::Attr: return s.attrs[size_t(n.slot)];
    case Op::Input:
    {
        const std::vector<D4>& in = s.b->inputs[size_t(n.slot)];
        return in.empty() ? D4(0.0) : in[std::min(s.element, in.size() - 1)];
    }
    case Op::Neg: return fix(-eval(n.a, s), n.type);
    case Op::Not: return {truthy(eval(n.a, s)) ? 0.0 : 1.0, 0, 0, 0};
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Mod:
    case Op::Pow: return arith(n.op, eval(n.a, s), eval(n.b, s), n.type);
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge: return {compare(n.op, eval(n.a, s), eval(n.b, s)) ? 1.0 : 0.0, 0, 0, 0};
    case Op::And: return {truthy(eval(n.a, s)) && truthy(eval(n.b, s)) ? 1.0 : 0.0, 0, 0, 0};
    case Op::Or: return {truthy(eval(n.a, s)) || truthy(eval(n.b, s)) ? 1.0 : 0.0, 0, 0, 0};
    case Op::Select: return truthy(eval(n.a, s)) ? eval(n.b, s) : eval(n.c, s);
    case Op::Component: return {eval(n.a, s)[n.comp], 0, 0, 0};
    case Op::Convert: return conv(eval(n.a, s), nodes[size_t(n.a)].type, n.type);
    case Op::Call:
    {
        std::vector<D4> args;
        std::vector<Type> types;
        args.reserve(n.args.size());
        types.reserve(n.args.size());
        for (int a : n.args)
        {
            args.push_back(eval(a, s));
            types.push_back(nodes[size_t(a)].type);
        }
        return callFn(n, args, types);
    }
    default: return D4(0.0);
    }
}

lang::Flow Program::Impl::exec(int i, lang::State& s) const
{
    using namespace lang;
    const Node& n = nodes[size_t(i)];
    switch (n.op)
    {
    case Op::Block:
        for (int st : n.args)
            if (Flow f = exec(st, s); f != Flow::Normal)
                return f;
        return Flow::Normal;
    case Op::StoreLocal:
    case Op::StoreAttr:
    {
        D4 v = eval(n.a, s);
        D4& target = n.op == Op::StoreLocal ? s.locals[size_t(n.slot)] : s.attrs[size_t(n.slot)];
        if (n.comp < 0)
            target = v;
        else
            target[n.comp] = v.x;
        return Flow::Normal;
    }
    case Op::If:
        if (truthy(eval(n.a, s)))
            return exec(n.b, s);
        return n.c >= 0 ? exec(n.c, s) : Flow::Normal;
    case Op::For:
    case Op::While:
    {
        if (n.a >= 0)
            exec(n.a, s);
        size_t iterations = 0;
        while (n.b < 0 || truthy(eval(n.b, s)))
        {
            if (++iterations > kMaxIterations)
                throw std::runtime_error("a loop ran more than " + std::to_string(kMaxIterations) + " times");
            if (exec(n.d, s) == Flow::Break)
                break;
            if (n.c >= 0)
                exec(n.c, s);
        }
        return Flow::Normal;
    }
    case Op::Break: return Flow::Break;
    case Op::Continue: return Flow::Continue;
    case Op::Eval: eval(n.a, s); return Flow::Normal;
    default: return Flow::Normal;
    }
}

namespace lang
{
struct Builtin
{
    const char* name;
    Type type;
    AttrKind kind;
    bool readOnly;
};

constexpr Builtin kBuiltins[] = {
    {"P", Type::Vector, AttrKind::Attribute, false},   {"N", Type::Vector, AttrKind::Attribute, true},
    {"Cd", Type::Vector, AttrKind::Attribute, false},  {"v", Type::Vector, AttrKind::Attribute, false},
    {"up", Type::Vector, AttrKind::Attribute, false},  {"id", Type::Int, AttrKind::Attribute, false},
    {"ptnum", Type::Int, AttrKind::Index, true},       {"elemnum", Type::Int, AttrKind::Index, true},
    {"numpt", Type::Int, AttrKind::Count, true},       {"numelem", Type::Int, AttrKind::Count, true},
};

std::optional<Type> prefixType(char p)
{
    switch (p)
    {
    case 'f': return Type::Float;
    case 'i': return Type::Int;
    case 'v': return Type::Vector;
    case 'p': return Type::Vector4;
    default: return std::nullopt;
    }
}

struct FnInfo
{
    const char* name;
    Fn fn;
    int arity; // -1: three or four (set)
};

constexpr FnInfo kFunctions[] = {
    {"sin", Fn::Sin, 1},          {"cos", Fn::Cos, 1},         {"tan", Fn::Tan, 1},          {"asin", Fn::Asin, 1},
    {"acos", Fn::Acos, 1},        {"atan", Fn::Atan, 1},       {"atan2", Fn::Atan2, 2},      {"sqrt", Fn::Sqrt, 1},
    {"pow", Fn::Pow, 2},          {"exp", Fn::Exp, 1},         {"log", Fn::Log, 1},          {"abs", Fn::Abs, 1},
    {"floor", Fn::Floor, 1},      {"ceil", Fn::Ceil, 1},       {"frac", Fn::Frac, 1},        {"sign", Fn::Sign, 1},
    {"radians", Fn::Radians, 1},  {"degrees", Fn::Degrees, 1}, {"min", Fn::Min, 2},          {"max", Fn::Max, 2},
    {"clamp", Fn::Clamp, 3},      {"lerp", Fn::Lerp, 3},       {"fit", Fn::Fit, 5},          {"smooth", Fn::Smooth, 3},
    {"length", Fn::Length, 1},    {"normalize", Fn::Normalize, 1}, {"dot", Fn::Dot, 2},      {"cross", Fn::Cross, 2},
    {"distance", Fn::Distance, 2}, {"rand", Fn::Rand, 1},      {"noise", Fn::Noise, 1},      {"set", Fn::Set, -1},
    {"int", Fn::ToInt, 1},        {"float", Fn::ToFloat, 1},
};

int componentIndex(const std::string& s)
{
    if (s.size() != 1)
        return -1;
    switch (s[0])
    {
    case 'x':
    case 'r': return 0;
    case 'y':
    case 'g': return 1;
    case 'z':
    case 'b': return 2;
    case 'w':
    case 'a': return 3;
    default: return -1;
    }
}

/** Arithmetic result of two operand types: vectors win, int only when both are int. */
Type unify(Type a, Type b)
{
    if (a == Type::Vector4 || b == Type::Vector4)
        return Type::Vector4;
    if (a == Type::Vector || b == Type::Vector)
        return Type::Vector;
    return a == Type::Int && b == Type::Int ? Type::Int : Type::Float;
}

class Parser
{
public:
    Parser(std::vector<Token> tokens, const Options& o, Program::Impl& p) : mTok(std::move(tokens)), mOpt(o), mP(p) { prescan(); }

    void parseProgram()
    {
        mScopes.emplace_back();
        while (peek().kind != Tok::End)
            mP.top.push_back(statement());
        if (!mP.top.empty() && mP.nodes[size_t(mP.top.back())].op == Op::Eval)
        {
            int e = mP.nodes[size_t(mP.top.back())].a;
            mP.result = e;
            mP.resultType = mP.nodes[size_t(e)].type;
            mP.top.pop_back();
        }
        if (mOpt.needResult && mP.result < 0)
            throw CompileError("the code must end with a value, such as a * 2", peek().line, peek().col);
    }

private:
    std::vector<Token> mTok;
    size_t mPos = 0;
    Options mOpt;
    Program::Impl& mP;
    std::vector<std::map<std::string, std::pair<int, Type>>> mScopes;
    std::map<std::string, Type> mTyped;
    int mLoopDepth = 0;

    const Token& peek(size_t ahead = 0) const { return mTok[std::min(mPos + ahead, mTok.size() - 1)]; }
    const Token& next() { return mTok[std::min(mPos++, mTok.size() - 1)]; }
    bool isPunct(const char* p, size_t ahead = 0) const { return peek(ahead).kind == Tok::Punct && peek(ahead).text == p; }
    bool isIdent(const char* p, size_t ahead = 0) const { return peek(ahead).kind == Tok::Ident && peek(ahead).text == p; }
    [[noreturn]] void fail(const std::string& msg, const Token& t) const { throw CompileError(msg, t.line, t.col); }
    void expect(const char* p)
    {
        if (!isPunct(p))
            fail(std::string("expected '") + p + "'" + (peek().kind == Tok::End ? " at the end" : " before '" + peek().text + "'"), peek());
        ++mPos;
    }

    /** Typed references fix an attribute's type wherever it appears, so @name before v@name reads as a vector. */
    void prescan()
    {
        for (const Token& t : mTok)
        {
            if (t.kind != Tok::Attr || !t.prefix)
                continue;
            Type ty = *prefixType(t.prefix);
            auto [it, added] = mTyped.emplace(t.text, ty);
            if (!added && it->second != ty)
                fail("@" + t.text + " is used as both " + typeName(it->second) + " and " + typeName(ty), t);
        }
    }

    int add(Node n)
    {
        mP.nodes.push_back(std::move(n));
        return int(mP.nodes.size()) - 1;
    }
    Type typeOf(int n) const { return mP.nodes[size_t(n)].type; }

    int convert(int n, Type to)
    {
        Type from = typeOf(n);
        if (from == to)
            return n;
        const Node& src = mP.nodes[size_t(n)];
        if (src.op == Op::Const)
            return add({.op = Op::Const, .type = to, .k = conv(src.k, from, to)});
        return add({.op = Op::Convert, .type = to, .a = n});
    }

    int constant(Type t, D4 v) { return add({.op = Op::Const, .type = t, .k = fix(v, t)}); }

    std::optional<std::pair<int, Type>> local(const std::string& name) const
    {
        for (auto s = mScopes.rbegin(); s != mScopes.rend(); ++s)
            if (auto it = s->find(name); it != s->end())
                return it->second;
        return std::nullopt;
    }

    int declare(const std::string& name, Type t, const Token& at)
    {
        if (mScopes.back().count(name))
            fail("'" + name + "' is already declared here", at);
        int slot = mP.numLocals++;
        mScopes.back()[name] = {slot, t};
        return slot;
    }

    int attribute(const Token& t)
    {
        for (size_t k = 0; k < mP.attrs.size(); ++k)
            if (mP.attrs[k].name == t.text)
                return int(k);
        AttrRef r;
        r.name = t.text;
        for (const Builtin& b : kBuiltins)
            if (t.text == b.name)
            {
                r.type = b.type;
                r.kind = b.kind;
                r.readOnly = b.readOnly;
            }
        if (auto it = mTyped.find(t.text); it != mTyped.end())
        {
            if (r.kind != AttrKind::Attribute && it->second != r.type)
                fail("@" + t.text + " is " + typeName(r.type), t);
            r.type = it->second;
        }
        mP.attrs.push_back(r);
        return int(mP.attrs.size()) - 1;
    }

    std::optional<Type> typeKeyword(size_t ahead = 0) const
    {
        const Token& t = peek(ahead);
        if (t.kind != Tok::Ident)
            return std::nullopt;
        if (t.text == "float")
            return Type::Float;
        if (t.text == "int")
            return Type::Int;
        if (t.text == "vector")
            return Type::Vector;
        if (t.text == "vector4")
            return Type::Vector4;
        return std::nullopt;
    }

    static bool isAssignOp(const Token& t)
    {
        return t.kind == Tok::Punct && (t.text == "=" || t.text == "+=" || t.text == "-=" || t.text == "*=" || t.text == "/=" || t.text == "++" || t.text == "--");
    }

    int block(std::vector<int> stmts) { return add({.op = Op::Block, .args = std::move(stmts)}); }

    int statement()
    {
        const Token& t = peek();
        if (isPunct("{"))
        {
            ++mPos;
            mScopes.emplace_back();
            std::vector<int> stmts;
            while (!isPunct("}"))
            {
                if (peek().kind == Tok::End)
                    fail("missing '}' for the block opened here", t);
                stmts.push_back(statement());
            }
            ++mPos;
            mScopes.pop_back();
            return block(std::move(stmts));
        }
        if (isPunct(";"))
        {
            ++mPos;
            return add({.op = Op::Nop});
        }
        if (isIdent("if"))
        {
            ++mPos;
            expect("(");
            int cond = expression();
            expect(")");
            int then = scopedStatement();
            int otherwise = -1;
            if (isIdent("else"))
            {
                ++mPos;
                otherwise = scopedStatement();
            }
            return add({.op = Op::If, .a = cond, .b = then, .c = otherwise});
        }
        if (isIdent("for"))
        {
            ++mPos;
            expect("(");
            mScopes.emplace_back();
            int init = isPunct(";") ? -1 : simple();
            expect(";");
            int cond = isPunct(";") ? -1 : expression();
            expect(";");
            int step = isPunct(")") ? -1 : simple();
            expect(")");
            ++mLoopDepth;
            int body = scopedStatement();
            --mLoopDepth;
            mScopes.pop_back();
            return add({.op = Op::For, .a = init, .b = cond, .c = step, .d = body});
        }
        if (isIdent("while"))
        {
            ++mPos;
            expect("(");
            int cond = expression();
            expect(")");
            ++mLoopDepth;
            int body = scopedStatement();
            --mLoopDepth;
            return add({.op = Op::While, .b = cond, .d = body});
        }
        if (isIdent("break") || isIdent("continue"))
        {
            bool brk = isIdent("break");
            if (mLoopDepth == 0)
                fail(std::string(brk ? "break" : "continue") + " outside a loop", t);
            ++mPos;
            expect(";");
            return add({.op = brk ? Op::Break : Op::Continue});
        }
        int s = simple();
        if (peek().kind == Tok::End && mP.nodes[size_t(s)].op == Op::Eval)
            return s;
        expect(";");
        return s;
    }

    /** A statement that gets its own scope even without braces, so a declaration in it ends with it. */
    int scopedStatement()
    {
        mScopes.emplace_back();
        int s = statement();
        mScopes.pop_back();
        return s;
    }

    /** A declaration, an assignment, ++/--, or an expression evaluated for nothing (or the result, when last). */
    int simple()
    {
        if (auto ty = typeKeyword(); ty && peek(1).kind == Tok::Ident)
        {
            ++mPos;
            std::vector<int> stores;
            for (;;)
            {
                const Token& name = next();
                if (name.kind != Tok::Ident)
                    fail("expected a variable name", name);
                int value = -1;
                if (isPunct("="))
                {
                    ++mPos;
                    value = convert(expression(), *ty);
                }
                else
                    value = constant(*ty, D4(0.0));
                int slot = declare(name.text, *ty, name);
                stores.push_back(add({.op = Op::StoreLocal, .type = *ty, .a = value, .slot = slot}));
                if (!isPunct(","))
                    break;
                ++mPos;
            }
            return stores.size() == 1 ? stores[0] : block(std::move(stores));
        }
        bool target = peek().kind == Tok::Ident || peek().kind == Tok::Attr;
        size_t opAt = isPunct(".", 1) && peek(2).kind == Tok::Ident ? 3 : 1;
        if (target && isAssignOp(peek(opAt)))
            return assignment(opAt == 3);
        int e = expression();
        return add({.op = Op::Eval, .type = typeOf(e), .a = e});
    }

    int assignment(bool hasComponent)
    {
        const Token name = next();
        int comp = -1;
        if (hasComponent)
        {
            ++mPos;
            const Token& c = next();
            comp = componentIndex(c.text);
            if (comp < 0)
                fail("'" + c.text + "' is not a component (x, y, z, w)", c);
        }
        const Token op = next();

        bool isAttr = name.kind == Tok::Attr;
        int slot = -1;
        Type ty = Type::Float;
        bool declared = true;
        if (isAttr)
        {
            if (!mOpt.allowWrites)
                fail("an Expression can't write attributes; use a Wrangle node", name);
            slot = attribute(name);
            AttrRef& r = mP.attrs[size_t(slot)];
            if (r.readOnly)
                fail("@" + name.text + " is read-only", name);
            r.written = true;
            ty = r.type;
        }
        else if (auto l = local(name.text))
        {
            slot = l->first;
            ty = l->second;
        }
        else
        {
            if (op.text != "=" || comp >= 0)
                fail("unknown name '" + name.text + "'", name);
            if (inputIndex(name.text) >= 0)
                fail("'" + name.text + "' is an input and read-only; declare a variable to reuse the name", name);
            declared = false;
        }
        if (comp >= 0 && (scalar(ty) || (ty == Type::Vector && comp == 3)))
            fail("'" + name.text + "' has no component " + "xyzw"[comp], name);

        auto current = [&]() {
            int whole = add({.op = isAttr ? Op::Attr : Op::Local, .type = ty, .slot = slot});
            return comp < 0 ? whole : add({.op = Op::Component, .type = Type::Float, .a = whole, .comp = comp});
        };
        Type lane = comp >= 0 ? Type::Float : ty;
        int value = -1;
        if (op.text == "++" || op.text == "--")
        {
            int one = constant(lane == Type::Int ? Type::Int : Type::Float, D4(1.0));
            value = binary(op.text == "++" ? Op::Add : Op::Sub, current(), one, op);
        }
        else
        {
            int rhs = expression();
            if (op.text == "=")
                value = rhs;
            else
            {
                Op bop = op.text == "+=" ? Op::Add : op.text == "-=" ? Op::Sub : op.text == "*=" ? Op::Mul : Op::Div;
                value = binary(bop, current(), rhs, op);
            }
        }
        if (!declared)
        {
            ty = lane = typeOf(value);
            slot = declare(name.text, ty, name);
        }
        value = convert(value, lane);
        return add({.op = isAttr ? Op::StoreAttr : Op::StoreLocal, .type = lane, .a = value, .slot = slot, .comp = comp});
    }

    static int inputIndex(const std::string& s)
    {
        if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'd')
            return s[0] - 'a';
        return -1;
    }

    int binary(Op op, int a, int b, const Token& at)
    {
        Type ta = typeOf(a), tb = typeOf(b);
        if (op == Op::And || op == Op::Or)
            return add({.op = op, .type = Type::Int, .a = a, .b = b});
        if (op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge)
        {
            if (!scalar(ta) || !scalar(tb))
                fail("vectors can't be ordered; compare a component or length()", at);
            Type t = unify(ta, tb);
            return add({.op = op, .type = Type::Int, .a = convert(a, t), .b = convert(b, t)});
        }
        Type t = unify(ta, tb);
        int ca = convert(a, t), cb = convert(b, t);
        if (op == Op::Eq || op == Op::Ne)
            return add({.op = op, .type = Type::Int, .a = ca, .b = cb});
        return add({.op = op, .type = t, .a = ca, .b = cb});
    }

    int expression() { return ternary(); }

    int ternary()
    {
        int cond = logicOr();
        if (!isPunct("?"))
            return cond;
        const Token& at = next();
        int a = expression();
        expect(":");
        int b = expression();
        Type t = unify(typeOf(a), typeOf(b));
        (void)at;
        return add({.op = Op::Select, .type = t, .a = cond, .b = convert(a, t), .c = convert(b, t)});
    }

    int logicOr()
    {
        int l = logicAnd();
        while (isPunct("||"))
        {
            const Token at = next();
            l = binary(Op::Or, l, logicAnd(), at);
        }
        return l;
    }

    int logicAnd()
    {
        int l = equality();
        while (isPunct("&&"))
        {
            const Token at = next();
            l = binary(Op::And, l, equality(), at);
        }
        return l;
    }

    int equality()
    {
        int l = relational();
        while (isPunct("==") || isPunct("!="))
        {
            const Token at = next();
            l = binary(at.text == "==" ? Op::Eq : Op::Ne, l, relational(), at);
        }
        return l;
    }

    int relational()
    {
        int l = additive();
        while (isPunct("<") || isPunct("<=") || isPunct(">") || isPunct(">="))
        {
            const Token at = next();
            Op op = at.text == "<" ? Op::Lt : at.text == "<=" ? Op::Le : at.text == ">" ? Op::Gt : Op::Ge;
            l = binary(op, l, additive(), at);
        }
        return l;
    }

    int additive()
    {
        int l = multiplicative();
        while (isPunct("+") || isPunct("-"))
        {
            const Token at = next();
            l = binary(at.text == "+" ? Op::Add : Op::Sub, l, multiplicative(), at);
        }
        return l;
    }

    int multiplicative()
    {
        int l = unary();
        while (isPunct("*") || isPunct("/") || isPunct("%"))
        {
            const Token at = next();
            Op op = at.text == "*" ? Op::Mul : at.text == "/" ? Op::Div : Op::Mod;
            l = binary(op, l, unary(), at);
        }
        return l;
    }

    int unary()
    {
        if (isPunct("-"))
        {
            ++mPos;
            int a = unary();
            return add({.op = Op::Neg, .type = typeOf(a), .a = a});
        }
        if (isPunct("+"))
        {
            ++mPos;
            return unary();
        }
        if (isPunct("!"))
        {
            ++mPos;
            int a = unary();
            return add({.op = Op::Not, .type = Type::Int, .a = a});
        }
        return power();
    }

    /** ^ is power, binding tighter than unary minus and to the right: -2^2 is -4, 2^3^2 is 512. */
    int power()
    {
        int base = postfix();
        if (!isPunct("^"))
            return base;
        const Token at = next();
        return binary(Op::Pow, base, unary(), at);
    }

    int postfix()
    {
        int e = primary();
        while (isPunct("."))
        {
            ++mPos;
            const Token& c = next();
            int comp = c.kind == Tok::Ident ? componentIndex(c.text) : -1;
            if (comp < 0)
                fail("expected a component (x, y, z, w) after '.'", c);
            Type t = typeOf(e);
            if (scalar(t) || (t == Type::Vector && comp == 3))
                fail(std::string("a ") + typeName(t) + " has no component " + "xyzw"[comp], c);
            e = add({.op = Op::Component, .type = Type::Float, .a = e, .comp = comp});
        }
        return e;
    }

    int input(int k)
    {
        mP.inputs[size_t(k)] = true;
        return add({.op = Op::Input, .type = k == 3 ? Type::Vector : Type::Float, .slot = k});
    }

    int primary()
    {
        const Token t = next();
        switch (t.kind)
        {
        case Tok::Number: return constant(t.isInt ? Type::Int : Type::Float, D4(t.number, 0, 0, 0));
        case Tok::Attr:
        {
            int slot = attribute(t);
            return add({.op = Op::Attr, .type = mP.attrs[size_t(slot)].type, .slot = slot});
        }
        case Tok::Punct:
            if (t.text == "(")
            {
                int e = expression();
                expect(")");
                return e;
            }
            fail("unexpected '" + t.text + "'", t);
        case Tok::String: fail("strings are only used in ch(\"name\")", t);
        case Tok::End: fail("the code ends in the middle of an expression", t);
        default: break;
        }
        if (isPunct("("))
            return call(t);
        if (auto l = local(t.text))
            return add({.op = Op::Local, .type = l->second, .slot = l->first});
        if (int k = inputIndex(t.text); k >= 0)
            return input(k);
        if (t.text == "PI" || t.text == "M_PI")
            return constant(Type::Float, D4(3.14159265358979323846, 0, 0, 0));
        fail("unknown name '" + t.text + "'", t);
    }

    int call(const Token& name)
    {
        expect("(");
        if (name.text == "ch")
        {
            const Token& s = next();
            int k = s.kind == Tok::String ? inputIndex(s.text) : -1;
            if (k < 0)
                fail("ch() reads the inputs \"a\", \"b\", \"c\" or \"d\"", s);
            expect(")");
            return input(k);
        }
        const FnInfo* info = nullptr;
        for (const FnInfo& f : kFunctions)
            if (name.text == f.name)
                info = &f;
        if (!info)
            fail("unknown function '" + name.text + "'", name);
        std::vector<int> args;
        if (!isPunct(")"))
            for (;;)
            {
                args.push_back(expression());
                if (!isPunct(","))
                    break;
                ++mPos;
            }
        expect(")");
        int n = int(args.size());
        if (info->arity >= 0 && n != info->arity)
            fail(name.text + "() takes " + std::to_string(info->arity) + " argument" + (info->arity == 1 ? "" : "s"), name);
        if (info->arity < 0 && n != 3 && n != 4)
            fail("set() takes three or four values", name);

        Node node{.op = Op::Call, .fn = info->fn};
        auto unifyAll = [&](bool keepInt) {
            Type t = typeOf(args[0]);
            for (int a : args)
                t = unify(t, typeOf(a));
            if (t == Type::Int && !keepInt)
                t = Type::Float;
            for (int& a : args)
                a = convert(a, t);
            return t;
        };
        switch (info->fn)
        {
        case Fn::Abs:
        case Fn::Floor:
        case Fn::Ceil:
        case Fn::Sign:
        case Fn::Min:
        case Fn::Max:
        case Fn::Clamp: node.type = unifyAll(true); break;
        case Fn::Length:
        case Fn::Dot:
        case Fn::Distance:
            if (info->fn != Fn::Length)
                unifyAll(false);
            node.type = Type::Float;
            break;
        case Fn::Normalize:
            if (scalar(typeOf(args[0])))
                fail("normalize() takes a vector", name);
            node.type = typeOf(args[0]);
            break;
        case Fn::Cross:
            for (int& a : args)
                a = convert(a, Type::Vector);
            node.type = Type::Vector;
            break;
        case Fn::Rand:
        case Fn::Noise:
            if (typeOf(args[0]) == Type::Int)
                args[0] = convert(args[0], Type::Float);
            node.type = Type::Float;
            break;
        case Fn::Set:
            for (int& a : args)
                a = convert(a, Type::Float);
            node.type = n == 3 ? Type::Vector : Type::Vector4;
            break;
        case Fn::ToInt: node.type = Type::Int; break;
        case Fn::ToFloat: node.type = Type::Float; break;
        default: node.type = unifyAll(false); break;
        }
        node.args = std::move(args);
        return add(std::move(node));
    }
};
} // namespace lang

const std::vector<AttrRef>& Program::attributes() const { return mImpl->attrs; }
bool Program::usesInput(int k) const { return k >= 0 && k < 4 && mImpl->inputs[size_t(k)]; }
bool Program::hasResult() const { return mImpl->result >= 0; }
Type Program::resultType() const { return mImpl->resultType; }

void Program::run(Bindings& b) const
{
    const Impl& p = *mImpl;
    const size_t n = b.count;
    b.attrs.resize(p.attrs.size());
    for (size_t a = 0; a < p.attrs.size(); ++a)
        if (p.attrs[a].kind == AttrKind::Attribute && b.attrs[a].size() != n)
            b.attrs[a].resize(n, D4(0.0));
    b.result.assign(p.result >= 0 ? n : 0, D4(0.0));

    auto body = [&](const tbb::blocked_range<size_t>& r) {
        lang::State s;
        s.b = &b;
        s.attrs.resize(p.attrs.size());
        for (size_t k = r.begin(); k != r.end(); ++k)
        {
            if (!b.selection.empty() && !b.selection[std::min(k, b.selection.size() - 1)])
                continue;
            s.locals.assign(size_t(p.numLocals), D4(0.0));
            for (size_t a = 0; a < p.attrs.size(); ++a)
                switch (p.attrs[a].kind)
                {
                case AttrKind::Index: s.attrs[a] = D4(double(k), 0, 0, 0); break;
                case AttrKind::Count: s.attrs[a] = D4(double(n), 0, 0, 0); break;
                default: s.attrs[a] = b.attrs[a][k]; break;
                }
            s.element = k;
            for (int st : p.top)
                p.exec(st, s);
            if (p.result >= 0)
                b.result[k] = p.eval(p.result, s);
            for (size_t a = 0; a < p.attrs.size(); ++a)
                if (p.attrs[a].written)
                    b.attrs[a][k] = s.attrs[a];
        }
    };
    if (n > 1024)
        tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 256), body);
    else
        body(tbb::blocked_range<size_t>(0, n));
}

ProgramPtr compile(std::string_view code, const Options& options)
{
    auto impl = std::make_shared<Program::Impl>();
    lang::Parser parser(lang::lex(code), options, *impl);
    parser.parseProgram();
    return std::make_shared<const Program>(std::move(impl));
}
} // namespace lila::gn::expr
