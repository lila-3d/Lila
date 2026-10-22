#include "ui/editor/PythonHighlighter.h"

#include <doctest/doctest.h>

#include <QString>

using namespace lila::ui;

namespace
{
/** The kind covering column `col`, or Count if plain. Later tokens win, like setFormat. */
TokenKind kindAt(const std::vector<Token>& tokens, int col)
{
    TokenKind k = TokenKind::Count;
    for (const Token& t : tokens)
        if (col >= t.start && col < t.start + t.length)
            k = t.kind;
    return k;
}

std::vector<Token> lex(const QString& line, int state = 0, int* next = nullptr)
{
    std::vector<Token> out;
    int s = tokenizePython(line, state, out);
    if (next)
        *next = s;
    return out;
}
} // namespace

TEST_CASE("keywords, builtins, names")
{
    QString line = "def run(self, n=None): return len(n)";
    auto t = lex(line);
    CHECK(kindAt(t, 0) == TokenKind::Keyword);
    CHECK(kindAt(t, 4) == TokenKind::DefName);
    CHECK(kindAt(t, 8) == TokenKind::Self);
    CHECK(kindAt(t, int(line.indexOf("None"))) == TokenKind::Constant);
    CHECK(kindAt(t, int(line.indexOf("return"))) == TokenKind::Keyword);
    CHECK(kindAt(t, int(line.indexOf("len"))) == TokenKind::Builtin);
    CHECK(kindAt(t, int(line.indexOf("(n)")) + 1) == TokenKind::Count);
}

TEST_CASE("class names, decorators, attributes")
{
    auto t = lex("class Foo(Base):");
    CHECK(kindAt(t, 6) == TokenKind::ClassName);
    t = lex("@lila.command");
    CHECK(kindAt(t, 0) == TokenKind::Decorator);
    CHECK(kindAt(t, 8) == TokenKind::Decorator);
    // An attribute named like a builtin is not the builtin.
    t = lex("x.list");
    CHECK(kindAt(t, 2) == TokenKind::Count);
}

TEST_CASE("soft keywords stay names when used as names")
{
    auto t = lex("match = re.match(x)");
    CHECK(kindAt(t, 0) == TokenKind::Count);
    t = lex("match command:");
    CHECK(kindAt(t, 0) == TokenKind::Keyword);
}

TEST_CASE("numbers and comments")
{
    QString line = "x = 0x1F + 1_000.5e-3j  # done";
    auto t = lex(line);
    CHECK(kindAt(t, 4) == TokenKind::Number);
    CHECK(kindAt(t, int(line.indexOf("1_000"))) == TokenKind::Number);
    CHECK(kindAt(t, int(line.indexOf("j "))) == TokenKind::Number);
    CHECK(kindAt(t, int(line.indexOf("#")) + 2) == TokenKind::Comment);
    t = lex("# %% setup");
    CHECK(kindAt(t, 0) == TokenKind::CellMarker);
}

TEST_CASE("strings with escapes and prefixes")
{
    QString line = R"(s = r"\d" + b'\n' + "a#b")";
    auto t = lex(line);
    CHECK(kindAt(t, 4) == TokenKind::String);
    CHECK(kindAt(t, 6) == TokenKind::String); // raw: no escape
    int b = int(line.indexOf("b'"));
    CHECK(kindAt(t, b) == TokenKind::String);
    CHECK(kindAt(t, b + 2) == TokenKind::StringEscape);
    // A hash inside a string is not a comment.
    CHECK(kindAt(t, int(line.indexOf("a#b")) + 1) == TokenKind::String);
}

TEST_CASE("f-string fields are code")
{
    QString line = R"(f"x={len(items):>4} {{lit}}")";
    auto t = lex(line);
    CHECK(kindAt(t, 0) == TokenKind::String);
    int brace = int(line.indexOf('{'));
    CHECK(kindAt(t, brace) == TokenKind::FStringBrace);
    CHECK(kindAt(t, brace + 1) == TokenKind::Builtin);
    CHECK(kindAt(t, int(line.indexOf(":>4")) + 1) == TokenKind::String);
    CHECK(kindAt(t, int(line.indexOf("{{")) + 1) == TokenKind::String);
}

TEST_CASE("triple-quoted strings span lines")
{
    int state = 0;
    auto t = lex(R"(doc = """first)", 0, &state);
    CHECK(state != 0);
    CHECK(kindAt(t, 8) == TokenKind::String);
    int next = 0;
    t = lex("still inside def", state, &next);
    CHECK(next == state);
    CHECK(kindAt(t, 13) == TokenKind::String);
    t = lex(R"(end""" + len(x))", state, &next);
    CHECK(next == 0);
    CHECK(kindAt(t, 0) == TokenKind::String);
    CHECK(kindAt(t, 9) == TokenKind::Builtin);
}

TEST_CASE("triple-quoted f-string keeps fields as code across lines")
{
    int state = 0;
    lex(R"(msg = f'''head)", 0, &state);
    CHECK(state != 0);
    int next = 0;
    auto t = lex("{abs(v)} tail", state, &next);
    CHECK(next == state);
    CHECK(kindAt(t, 0) == TokenKind::FStringBrace);
    CHECK(kindAt(t, 1) == TokenKind::Builtin);
    CHECK(kindAt(t, 10) == TokenKind::String);
}

TEST_CASE("block data tells strings and comments apart")
{
    PythonBlockData data;
    tokenizePython(u"x = '(' # )", 0, data.tokens);
    CHECK(!data.inStringOrComment(1));
    CHECK(data.inStringOrComment(5));
    CHECK(data.inStringOrComment(10));
}
