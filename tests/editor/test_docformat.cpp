#include "ui/editor/DocFormat.h"

#include <doctest/doctest.h>

using namespace lila::ui;

namespace
{
doc::Theme testTheme()
{
    doc::Theme t;
    t.background = QColor("#282828");
    t.text = QColor("#c6c6c6");
    t.dim = QColor("#8c8c8c");
    t.border = QColor("#4a4a4a");
    t.codeBackground = QColor("#1e1e1e");
    t.error = QColor("#f14c4c");
    t.warning = QColor("#cca700");
    t.info = QColor("#3794ff");
    t.monoFamily = "JetBrains Mono";
    t.charWidth = 8;
    QTextCharFormat string;
    string.setForeground(QColor("#ce9178"));
    t.syntax.tokens[size_t(TokenKind::String)] = string;
    QTextCharFormat keyword;
    keyword.setForeground(QColor("#569cd6"));
    keyword.setFontWeight(QFont::Bold);
    t.syntax.tokens[size_t(TokenKind::Keyword)] = keyword;
    t.syntax.tokens[size_t(TokenKind::Constant)] = keyword;
    t.syntax.control = keyword;
    return t;
}

const char* kDoc = "Create a Cube mesh. Returns [mesh, generator].\n"
                   "\n"
                   "Args:\n"
                   "    name: Name of the mesh; made unique. Short form n.\n"
                   "    width: Size along X, cm. Default 100.0.\n"
                   "    upAxis: Direction the primitive's up points. One of 'x', 'y'.\n"
                   "    query: Return a value instead. Example: `xform(\"c\", query=True)`\n"
                   "        Continued on an indented line.\n"
                   "\n"
                   "Examples:\n"
                   "    cube = polyCube(n=\"Box\")\n"
                   "\n"
                   "    if cube:\n"
                   "        pass\n";
} // namespace

TEST_CASE("code is coloured by token")
{
    doc::Theme t = testTheme();
    QString html = doc::codeHtml("x = 'a' if None else 1", t);
    CHECK(html.contains("<span style=\"color:#ce9178\">'a'</span>"));
    CHECK(html.contains("color:#569cd6; font-weight:bold\">None</span>"));
    CHECK(html.contains("color:#c6c6c6\">x = </span>"));

    // The active parameter is emphasised; newlines become breaks.
    QString sig = doc::codeHtml("(a: int, b: str)", t, 9, 15);
    CHECK(sig.contains("font-weight:bold; text-decoration:underline\">b: str</span>"));
    CHECK(doc::codeHtml("a\n    b", t).contains("<br>"));
}

TEST_CASE("annotations come from the signature without the None default")
{
    auto a = doc::parameterAnnotations("(function) def polyCube(\n    name: str | None = None,\n    size: Sequence[float] | None = None,\n"
                                       "    *objects: Any,\n    count: int = 3,\n    maybe: int | None = 2\n) -> Any");
    CHECK(a.value("name") == "str");
    CHECK(a.value("size") == "Sequence[float]");
    CHECK_FALSE(a.contains("objects"));
    CHECK(a.value("count") == "int");
    CHECK(a.value("maybe") == "int | None");
}

TEST_CASE("docstring sections render as paragraphs, an argument list and code")
{
    doc::Theme t = testTheme();
    QHash<QString, QString> annotations{{"name", "str"}, {"width", "float"}};
    QString html = doc::docstringHtml(kDoc, t, annotations);

    CHECK(html.contains("Create a Cube mesh. Returns [mesh, generator].</p>"));
    CHECK(html.contains("font-weight:bold\">Args</p>"));
    CHECK(html.contains("font-weight:bold\">Examples</p>"));

    // One table row per argument, the name in bold monospace and its annotation beside the description.
    CHECK(html.count("<tr><td nowrap") == 4);
    CHECK(html.contains("<span style=\"font-family:'JetBrains Mono'; font-weight:bold\">name</span>"));
    CHECK(html.contains("color:#8c8c8c\">str</span>&nbsp;&nbsp;Name of the mesh; made unique."));
    // The flag metadata keeps its values as code, on the same entry.
    CHECK(html.contains("Short form </span><span style=\"font-family:'JetBrains Mono'; background-color:#1e1e1e\">"));
    CHECK(html.contains(">100.0</span>"));
    // The Args list keeps the docstring's indent.
    CHECK(html.contains("<table cellspacing=\"0\" cellpadding=\"2\" style=\"margin-left:16px"));
    // Continuation lines join their entry instead of starting a paragraph.
    CHECK(html.contains("Continued on an indented line.</td></tr>"));
    // Inline backticks become code.
    CHECK(html.contains("background-color:#1e1e1e\"><span style=\"color:#c6c6c6\">xform(</span>"));

    // Examples: one code block, blank line and indentation kept, dedented by the section indent.
    CHECK(html.count("white-space:pre-wrap") == 1);
    CHECK(html.contains("cube = polyCube(n=</span>"));
    CHECK(html.contains("<br><br><span style=\"color:#569cd6; font-weight:bold\">if</span>"));
    CHECK(html.contains("<br><span style=\"color:#c6c6c6\">    </span><span style=\"color:#569cd6; font-weight:bold\">pass</span>"));
}

TEST_CASE("plain paragraphs reflow and keep relative indent")
{
    doc::Theme t = testTheme();
    QString html = doc::docstringHtml("First line\nwraps here.\n\n    Indented block\n    goes on.\n- a bullet\n- another", t);
    CHECK(html.contains("margin-left:0px\">First line wraps here.</p>"));
    CHECK(html.contains("margin-left:16px\">Indented block goes on.</p>"));
    CHECK(html.contains(">- a bullet</p>"));
    CHECK(html.contains(">- another</p>"));
}

TEST_CASE("hover puts diagnostics in a severity strip above the docs")
{
    doc::Theme t = testTheme();
    Diagnostic d;
    d.severity = Severity::Error;
    d.message = "Undefined name `xform`";
    d.source = "ruff";
    d.code = "F821";
    QString html = doc::hoverHtml({d}, "(function) def xform(\n    *objects: Any\n) -> Any\n\nQuery or set.", t);

    qsizetype strip = html.indexOf("Error</span>");
    qsizetype sig = html.indexOf(">def</span>");
    REQUIRE(strip >= 0);
    REQUIRE(sig >= 0);
    CHECK(strip < sig);
    CHECK(html.contains("color:#f14c4c; font-weight:bold\">Error</span>"));
    CHECK(html.contains("ruff F821"));
    CHECK(html.contains(">xform</span></span>"));
    // The "(function)" tag is dimmed on its own line, not part of the code.
    CHECK(html.contains("color:#8c8c8c\">function</p>"));
    CHECK(html.contains("Query or set.</p>"));
    // Full-width strips are marked so the popup can measure its content without them.
    CHECK(html.count(doc::kFullWidth) == 3);
}
