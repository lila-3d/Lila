#include "rigmath/Expressions.h"

#include "core/Profile.h"
#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <cctype>
#include <cmath>
#include <map>
#include <mutex>

namespace lila::scene::expressions
{
namespace
{
constexpr const char* kTag = "lilaExpression";

struct Token
{
    enum Kind : uint8_t
    {
        End,
        Number,
        Name, // identifiers, keywords and dotted plug paths
        Op,
    };
    Kind kind = End;
    std::string text;
    double number = 0.0;
    size_t at = 0;
};

std::vector<Token> tokenize(const std::string& s)
{
    std::vector<Token> out;
    size_t i = 0;
    auto nameChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':'; };
    while (i < s.size())
    {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }
        Token t;
        t.at = i;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))))
        {
            char* end = nullptr;
            t.number = std::strtod(s.c_str() + i, &end);
            const size_t n = size_t(end - (s.c_str() + i));
            t.kind = Token::Number;
            t.text = s.substr(i, n);
            i += n;
        }
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t j = i;
            // node.attr, node.attr[2], ns:node.attr: one token, split into a plug later.
            while (j < s.size() && (nameChar(s[j]) || (s[j] == '.' && j + 1 < s.size() && (std::isalpha(static_cast<unsigned char>(s[j + 1])) || s[j + 1] == '_')) ||
                                    (s[j] == '[' && s.find(']', j) != std::string::npos)))
            {
                if (s[j] == '[')
                    j = s.find(']', j);
                ++j;
            }
            t.kind = Token::Name;
            t.text = s.substr(i, j - i);
            i = j;
        }
        else
        {
            static const char* kTwo[] = {"**", "<=", ">=", "==", "!=", "//"};
            t.kind = Token::Op;
            t.text = std::string(1, c);
            for (const char* two : kTwo)
                if (s.compare(i, 2, two) == 0)
                    t.text = two;
            if (std::string("+-*/%()<>,").find(c) == std::string::npos && t.text.size() == 1)
                throw std::invalid_argument("can't read '" + std::string(1, c) + "' at " + std::to_string(i + 1));
            i += t.text.size();
        }
        out.push_back(std::move(t));
    }
    out.push_back(Token{Token::End, "", 0.0, s.size()});
    return out;
}

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens) : mTokens(std::move(tokens)) {}

    std::unique_ptr<Ast> parseAll()
    {
        auto e = ternary();
        if (peek().kind != Token::End)
            throw std::invalid_argument("unexpected '" + peek().text + "'");
        return e;
    }

private:
    const Token& peek() const { return mTokens[mAt]; }
    bool accept(const char* text)
    {
        if ((peek().kind == Token::Op || peek().kind == Token::Name) && peek().text == text)
        {
            ++mAt;
            return true;
        }
        return false;
    }
    void expect(const char* text)
    {
        if (!accept(text))
            throw std::invalid_argument(std::string("expected '") + text + "'" + (peek().kind == Token::End ? " at the end" : " before '" + peek().text + "'"));
    }
    static std::unique_ptr<Ast> node(Ast::Kind k, std::string op, std::vector<std::unique_ptr<Ast>> args = {})
    {
        auto a = std::make_unique<Ast>();
        a->kind = k;
        a->op = std::move(op);
        a->args = std::move(args);
        return a;
    }
    template <class... A> static std::vector<std::unique_ptr<Ast>> list(A&&... a)
    {
        std::vector<std::unique_ptr<Ast>> v;
        (v.push_back(std::move(a)), ...);
        return v;
    }

    std::unique_ptr<Ast> ternary()
    {
        auto then = orExpr();
        if (!accept("if"))
            return then;
        auto cond = orExpr();
        expect("else");
        auto other = ternary();
        return node(Ast::Ternary, "", list(std::move(then), std::move(cond), std::move(other)));
    }
    std::unique_ptr<Ast> orExpr()
    {
        auto l = andExpr();
        while (accept("or"))
            l = node(Ast::Binary, "or", list(std::move(l), andExpr()));
        return l;
    }
    std::unique_ptr<Ast> andExpr()
    {
        auto l = notExpr();
        while (accept("and"))
            l = node(Ast::Binary, "and", list(std::move(l), notExpr()));
        return l;
    }
    std::unique_ptr<Ast> notExpr()
    {
        if (accept("not"))
            return node(Ast::Unary, "not", list(notExpr()));
        return comparison();
    }
    std::unique_ptr<Ast> comparison()
    {
        auto l = arith();
        for (const char* op : {"<=", ">=", "==", "!=", "<", ">"})
            if (accept(op))
            {
                auto c = node(Ast::Compare, op, list(std::move(l), arith()));
                for (const char* again : {"<=", ">=", "==", "!=", "<", ">"})
                    if (peek().kind == Token::Op && peek().text == again)
                        throw std::invalid_argument("chained comparisons run as Python");
                return c;
            }
        return l;
    }
    std::unique_ptr<Ast> arith()
    {
        auto l = term();
        while (true)
        {
            if (accept("+"))
                l = node(Ast::Binary, "+", list(std::move(l), term()));
            else if (accept("-"))
                l = node(Ast::Binary, "-", list(std::move(l), term()));
            else
                return l;
        }
    }
    std::unique_ptr<Ast> term()
    {
        auto l = factor();
        while (true)
        {
            if (accept("*"))
                l = node(Ast::Binary, "*", list(std::move(l), factor()));
            else if (accept("//"))
                throw std::invalid_argument("floor division runs as Python");
            else if (accept("/"))
                l = node(Ast::Binary, "/", list(std::move(l), factor()));
            else if (accept("%"))
                l = node(Ast::Binary, "%", list(std::move(l), factor()));
            else
                return l;
        }
    }
    std::unique_ptr<Ast> factor()
    {
        if (accept("-"))
            return node(Ast::Unary, "-", list(factor()));
        if (accept("+"))
            return factor();
        return power();
    }
    std::unique_ptr<Ast> power()
    {
        auto base = atom();
        if (accept("**"))
            return node(Ast::Binary, "**", list(std::move(base), factor()));
        return base;
    }
    std::unique_ptr<Ast> atom()
    {
        const Token t = peek();
        if (t.kind == Token::Number)
        {
            ++mAt;
            auto a = node(Ast::Number, "");
            a->number = t.number;
            return a;
        }
        if (accept("("))
        {
            auto e = ternary();
            expect(")");
            return e;
        }
        if (t.kind != Token::Name)
            throw std::invalid_argument(t.kind == Token::End ? "the expression ends early" : "unexpected '" + t.text + "'");
        ++mAt;
        static const std::map<std::string, double> kConstants = {{"pi", 3.14159265358979323846}, {"tau", 6.28318530717958647692},
                                                                 {"e", 2.71828182845904523536}, {"True", 1.0}, {"False", 0.0}};
        if (auto c = kConstants.find(t.text); c != kConstants.end())
        {
            auto a = node(Ast::Number, "");
            a->number = c->second;
            return a;
        }
        if (accept("("))
        {
            std::vector<std::unique_ptr<Ast>> args;
            if (!accept(")"))
            {
                do
                    args.push_back(ternary());
                while (accept(","));
                expect(")");
            }
            return node(Ast::Call, t.text, std::move(args));
        }
        if (t.text.find('.') == std::string::npos)
            throw std::invalid_argument("'" + t.text + "' is neither a plug (node.attr) nor a constant");
        return node(Ast::Plug, t.text);
    }

    std::vector<Token> mTokens;
    size_t mAt = 0;
};

struct FnInfo
{
    std::string rigmath;
    int args;
    int output = 0; // which output pin
};
const std::map<std::string, FnInfo>& fnTable()
{
    static const std::map<std::string, FnInfo> t = {
        {"abs", {"abs", 1}},       {"min", {"min", 2}},        {"max", {"max", 2}},       {"clamp", {"clamp", 3}},
        {"lerp", {"lerp", 3}},     {"sin", {"sin", 1}},        {"cos", {"cos", 1}},       {"tan", {"tan", 1}},
        {"asin", {"asin", 1}},     {"acos", {"acos", 1}},      {"atan", {"atan", 1}},     {"atan2", {"atan2", 2}},
        {"sqrt", {"sqrt", 1}},     {"exp", {"exp", 1}},        {"pow", {"pow", 2}},       {"log", {"loge", 1}},
        {"sign", {"sign", 1}},     {"fmod", {"fmod", 2, 1}},   {"hypot", {"hypotenuse", 2}}, {"degrees", {"radiansToDegrees", 1}},
        {"radians", {"degreesToRadians", 1}}, {"wrap", {"wrap", 3}}, {"mapRange", {"mapRangeClamped", 5}},
    };
    return t;
}

/** The overload of a rig math function whose inputs take kind (Float, or Bool for logic). */
const rigmath::Overload* overload(const std::string& fn, rigmath::Kind kind)
{
    for (const rigmath::Function& f : rigmath::functions())
        if (f.name == fn)
            for (const rigmath::Overload& o : f.overloads)
                if (!o.inputs.empty() && o.inputs[0].kind == kind)
                    return &o;
    return nullptr;
}

/** Why an AST doesn't compile to rig math (unknown function, wrong arity, missing plug), or empty. */
std::string uncompilable(const Ast& a, Graph& g)
{
    if (a.kind == Ast::Plug)
    {
        try
        {
            g.plug(a.op);
        }
        catch (const std::exception&)
        {
            return "no plug " + a.op;
        }
    }
    if (a.kind == Ast::Call)
    {
        auto it = fnTable().find(a.op);
        if (it == fnTable().end())
            return a.op + "() isn't a rig math function";
        if (int(a.args.size()) != it->second.args && !(a.op == "log" && a.args.size() == 2))
            return a.op + "() takes " + std::to_string(it->second.args) + " arguments";
    }
    for (const auto& c : a.args)
        if (std::string why = uncompilable(*c, g); !why.empty())
            return why;
    return {};
}

/** A value an expression builds: a plug to connect, or a constant to set. */
struct Value
{
    std::optional<PlugRef> plug;
    double constant = 0.0;
};

class Compiler
{
public:
    Compiler(Graph& g, std::string target, std::string label) : g(g), mTarget(std::move(target)), mLabel(std::move(label)) {}

    Value build(const Ast& a)
    {
        switch (a.kind)
        {
        case Ast::Number: return {std::nullopt, a.number};
        case Ast::Plug: return {g.plug(a.op), 0.0};
        case Ast::Unary:
            if (a.op == "-")
                return op("negate", rigmath::Kind::Float, {build(*a.args[0])});
            return op("not", rigmath::Kind::Bool, {build(*a.args[0])});
        case Ast::Binary:
        {
            static const std::map<std::string, std::pair<const char*, rigmath::Kind>> kOps = {
                {"+", {"add", rigmath::Kind::Float}},      {"-", {"subtract", rigmath::Kind::Float}}, {"*", {"multiply", rigmath::Kind::Float}},
                {"/", {"divide", rigmath::Kind::Float}},   {"**", {"pow", rigmath::Kind::Float}},     {"and", {"and", rigmath::Kind::Bool}},
                {"or", {"or", rigmath::Kind::Bool}}};
            if (a.op == "%")
                return op("fmod", rigmath::Kind::Float, {build(*a.args[0]), build(*a.args[1])}, 1);
            const auto& [fn, kind] = kOps.at(a.op);
            return op(fn, kind, {build(*a.args[0]), build(*a.args[1])});
        }
        case Ast::Compare:
        {
            static const std::map<std::string, const char*> kCmp = {{"<", "less"},     {"<=", "lessEqual"}, {">", "greater"},
                                                                    {">=", "greaterEqual"}, {"==", "equal"}, {"!=", "notEqual"}};
            return op(kCmp.at(a.op), rigmath::Kind::Float, {build(*a.args[0]), build(*a.args[1])});
        }
        case Ast::Ternary:
            // select's pins are a, b, pickA.
            return op("select", rigmath::Kind::Float, {build(*a.args[0]), build(*a.args[2]), build(*a.args[1])});
        case Ast::Call:
        {
            const FnInfo& f = fnTable().at(a.op);
            std::vector<Value> args;
            for (const auto& c : a.args)
                args.push_back(build(*c));
            if (a.op == "log" && args.size() == 2)
                return op("log", rigmath::Kind::Float, std::move(args));
            return op(f.rigmath, rigmath::Kind::Float, std::move(args), f.output);
        }
        }
        throw std::invalid_argument("unknown expression part");
    }

    std::vector<NodeId> nodes;

private:
    Value op(const std::string& fn, rigmath::Kind kind, std::vector<Value> args, int output = 0)
    {
        const rigmath::Overload* o = overload(fn, kind);
        if (!o)
            throw std::invalid_argument("no rig math " + fn);
        const NodeType* type = types::find(o->nodeType);
        if (!type)
            throw std::invalid_argument("rig math node " + o->nodeType + " isn't registered");
        Node* n = g.createNode(*type, mLabel + "_" + fn);
        nodes.push_back(n->id());
        for (size_t i = 0; i < args.size() && i < o->inputs.size(); ++i)
        {
            const int pin = n->type().attrIndex(o->inputs[i].name);
            if (args[i].plug)
                g.connect(*args[i].plug, {n->id(), pin});
            else
            {
                const AttrDef& d = n->type().attrs[size_t(pin)];
                g.setValue({n->id(), pin}, d.type == PlugType::Bool ? PlugValue(args[i].constant != 0.0)
                                           : d.type == PlugType::Int ? PlugValue(int64_t(std::llround(args[i].constant)))
                                                                     : PlugValue(args[i].constant));
            }
        }
        return {PlugRef{n->id(), n->type().attrIndex(o->outputs[size_t(output)].name)}, 0.0};
    }

    Graph& g;
    std::string mTarget;
    std::string mLabel;
};

/** Plug paths in text as (offset, length), for Python: dotted names that resolve, outside strings. */
std::vector<std::pair<size_t, size_t>> plugSpans(Graph& g, const std::string& s)
{
    std::vector<std::pair<size_t, size_t>> out;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_') || (i > 0 && (s[i - 1] == '.' || std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_')))
            continue;
        size_t j = i;
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == ':' || s[j] == '.' || s[j] == '[' || s[j] == ']'))
            ++j;
        const std::string name = s.substr(i, j - i);
        if (name.find('.') != std::string::npos)
            try
            {
                g.plug(name);
                out.emplace_back(i, j - i);
            }
            catch (const std::exception&)
            {
            }
        i = j - 1;
    }
    return out;
}

PythonEvaluator& evaluator()
{
    static PythonEvaluator fn;
    return fn;
}
std::mutex& evaluatorLock()
{
    static std::mutex m;
    return m;
}

class PythonNode final : public Node
{
public:
    enum Attr
    {
        kCode,
        kInputs,
        kOutput,
        kError,
    };
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        std::vector<double> values;
        for (const auto& [index, v] : elements(kInputs))
        {
            if (size_t(index) >= values.size())
                values.resize(size_t(index) + 1, 0.0);
            values[size_t(index)] = std::get<double>(v);
        }
        PythonEvaluator fn;
        {
            std::lock_guard lock(evaluatorLock());
            fn = evaluator();
        }
        std::string error;
        std::optional<double> r = fn ? fn(as<std::string>(kCode), values, &error) : std::nullopt;
        if (!fn)
            error = "Python isn't running";
        setOutput(kOutput, r.value_or(0.0));
        setOutput(kError, r ? std::string() : error);
    }
};

json tagOf(const Node& n)
{
    const int i = n.type().attrIndex(kTag);
    if (i < 0)
        return nullptr;
    json j = json::parse(n.as<std::string>(i), nullptr, false);
    return j.is_object() ? j : json();
}

void tag(Graph& g, NodeId id, const json& value)
{
    const int i = g.addAttr(id, AttrDef{.name = kTag, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrHidden,
                                         .doc = "The expression this node belongs to."});
    g.setValue({id, i}, value.dump());
}

/** Nodes of the expression on target (its full plug name), the root (the one with the text) first. */
std::vector<NodeId> expressionNodes(Graph& g, const std::string& target)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (json t = tagOf(*g.node(id)); t.is_object() && t.value("target", "") == target)
        {
            if (t.contains("text"))
                out.insert(out.begin(), id);
            else
                out.push_back(id);
        }
    return out;
}
} // namespace

std::unique_ptr<Ast> parse(const std::string& text) { return Parser(tokenize(text)).parseAll(); }

const std::vector<std::pair<std::string, std::string>>& functions()
{
    static const std::vector<std::pair<std::string, std::string>> list = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [name, f] : fnTable())
            out.emplace_back(name, f.rigmath);
        return out;
    }();
    return list;
}

bool compiles(const std::string& text, std::string* why)
{
    try
    {
        std::unique_ptr<Ast> a = parse(text);
        std::string reason = uncompilable(*a, Graph::current());
        if (why)
            *why = reason;
        return reason.empty();
    }
    catch (const std::invalid_argument& e)
    {
        if (why)
            *why = e.what();
        return false;
    }
}

void setPythonEvaluator(PythonEvaluator fn)
{
    std::lock_guard lock(evaluatorLock());
    evaluator() = std::move(fn);
}

void registerTypes()
{
    if (types::find("pythonExpression"))
        return;
    NodeType t;
    t.name = "pythonExpression";
    t.icon = "brand-python";
    t.doc = "An expression that runs as Python each time its inputs change: slower than rig math nodes and Lila only.";
    t.ownAttrs = {
        {.name = "code", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrCode,
         .doc = "Python returning a number; v0, v1 ... are the inputs, math's functions are in scope."},
        {.name = "inputs", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrArray, .doc = "The plugs the code reads."},
        {.name = "output", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "The result."},
        {.name = "error", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Why the last run failed."},
    };
    t.create = [] { return std::make_unique<PythonNode>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "expression",
        .doc = "Drive a plug with an expression in Python syntax: expression('box.ty', string='ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)'). "
               "Numbers, other plugs by name, + - * / % **, comparisons, and/or/not, `a if c else b` and abs, min, max, clamp, lerp, "
               "sin, cos, tan, asin, acos, atan, atan2, sqrt, exp, pow, log, sign, fmod, hypot, degrees, radians, wrap and mapRange "
               "compile to rig math nodes: as fast as nodes, shown in the node editor. Anything else runs as Python (slower, Lila only). "
               "Returns {native, nodes, reason}.",
        .positionalDoc = "plug",
        .flags = {{"string", "s", T::String, nullptr, "The expression; a leading = is optional."},
                  {"python", "py", T::Bool, false, "Run it as Python even when it compiles to nodes."},
                  {"query", "q", T::Bool, false, "Return {text, native, nodes} for the plug's expression, or None."},
                  {"delete", "d", T::Bool, false, "Remove the plug's expression and its nodes."}},
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1)
                throw CommandError("expression: expected a plug");
            Graph& g = Graph::current();
            PlugRef target;
            try
            {
                target = g.plug(a.positional[0].get<std::string>());
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(e.what());
            }
            const std::string key = g.fullPlugName(target);
            std::vector<NodeId> existing = expressionNodes(g, key);
            if (a.get<bool>("query"))
            {
                if (existing.empty())
                    return nullptr;
                json root = tagOf(*g.node(existing[0]));
                json nodes = json::array();
                for (NodeId id : existing)
                    nodes.push_back(g.node(id)->path());
                return {{"text", root.value("text", "")}, {"native", root.value("native", true)}, {"nodes", nodes}};
            }
            for (NodeId id : existing)
                if (g.node(id))
                    g.deleteNode(id);
            if (a.get<bool>("delete"))
                return nullptr;
            std::string text = a.get<std::string>("string", "");
            while (!text.empty() && (text.front() == '=' || std::isspace(static_cast<unsigned char>(text.front()))))
                text.erase(text.begin());
            if (text.empty())
                throw CommandError("expression: give string, the expression");
            const AttrDef& def = g.attr(target);
            const bool scalar = target.component >= 0 || def.type == PlugType::Float || def.type == PlugType::Int || def.type == PlugType::Bool ||
                                def.type == PlugType::Enum;
            if (!scalar)
                throw CommandError("expression: " + g.plugName(target) + " isn't a number; drive one of its components");
            const std::string label = "expr_" + g.node(target.node)->name();
            std::string why;
            const bool native = !a.get<bool>("python") && compiles(text, &why);
            // Unbalanced or unfinished text is a mistake in Python too, not a reason to run it there.
            if (!native && (why.rfind("the expression ends early", 0) == 0 || why.rfind("expected '", 0) == 0))
                throw CommandError("expression: " + why);
            json nodes = json::array();
            try
            {
                if (native)
                {
                    std::unique_ptr<Ast> ast = parse(text);
                    Compiler c(g, key, label);
                    Value v = c.build(*ast);
                    if (c.nodes.empty())
                    {
                        // A constant or a lone plug: a node still holds it, so the expression has somewhere to keep its text.
                        const rigmath::Overload* o = overload("add", rigmath::Kind::Float);
                        Node* n = g.createNode(*types::find(o->nodeType), label + "_value");
                        c.nodes.push_back(n->id());
                        const int in = n->type().attrIndex(o->inputs[0].name);
                        if (v.plug)
                            g.connect(*v.plug, {n->id(), in});
                        else
                            g.setValue({n->id(), in}, v.constant);
                        g.setValue({n->id(), n->type().attrIndex(o->inputs[1].name)}, 0.0);
                        v.plug = PlugRef{n->id(), n->type().attrIndex(o->outputs[0].name)};
                    }
                    g.connect(*v.plug, target, true);
                    const NodeId root = v.plug->node;
                    for (NodeId id : c.nodes)
                    {
                        json t = {{"target", key}};
                        if (id == root)
                            t.update({{"text", text}, {"native", true}});
                        tag(g, id, t);
                        nodes.push_back(g.node(id)->path());
                    }
                }
                else
                {
                    Node* n = g.createNode(*types::find("pythonExpression"), label + "_py");
                    std::string code;
                    size_t at = 0;
                    int index = 0;
                    for (auto [start, length] : plugSpans(g, text))
                    {
                        code += text.substr(at, start - at) + "v" + std::to_string(index);
                        g.connect(g.plug(text.substr(start, length)), {n->id(), PythonNode::kInputs, -1, index});
                        at = start + length;
                        ++index;
                    }
                    code += text.substr(at);
                    g.setValue({n->id(), PythonNode::kCode}, code);
                    g.connect({n->id(), PythonNode::kOutput}, target, true);
                    tag(g, n->id(), {{"target", key}, {"text", text}, {"native", false}});
                    nodes.push_back(n->path());
                }
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("expression: ") + e.what());
            }
            json out = {{"native", native}, {"nodes", nodes}};
            if (!native)
                out["reason"] = a.get<bool>("python") ? std::string("asked for Python") : why;
            return out;
        },
    });
}
} // namespace lila::scene::expressions
