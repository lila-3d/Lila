#include "ui/editor/Format.h"
#include "ui/editor/Ruff.h"

#include <doctest/doctest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace lila::ui;

namespace
{
QString applyEdits(const QString& before, const std::vector<format::Edit>& edits)
{
    QString out = before;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it)
        out.replace(it->start, it->end - it->start, it->text);
    return out;
}

void write(const QString& path, const QByteArray& text)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(text);
}
} // namespace

TEST_CASE("format settings default to the reference editors' behaviour")
{
    format::Settings s;
    CHECK_FALSE(s.onSave);
    CHECK_FALSE(s.onType);
    CHECK_FALSE(s.onPaste);
    CHECK_FALSE(s.onFocusLoss);
    CHECK_FALSE(s.organizeImportsOnSave);
    CHECK_FALSE(s.fixAllOnSave);
    CHECK(s.trimTrailingWhitespace);
    CHECK(s.finalNewline);
    CHECK(s.useProjectConfig);
    CHECK(s.lineLength == 88);
    CHECK(s.quoteStyle == "double");
    CHECK(s.indentWidth == 4);
}

TEST_CASE("style args defer to a project config")
{
    format::Settings s;
    s.lineLength = 100;
    s.quoteStyle = "single";
    CHECK(format::styleArgs(s, true, false).isEmpty());
    QStringList args = format::styleArgs(s, false, false);
    CHECK(args.mid(0, 2) == QStringList{"--line-length", "100"});
    CHECK(args.contains("format.quote-style = 'single'"));
    CHECK(args.contains("indent-width = 4"));
    CHECK(format::styleArgs(s, false, true) == QStringList{"--line-length", "100"});
    s.useProjectConfig = false;
    CHECK_FALSE(format::styleArgs(s, true, false).isEmpty());
}

TEST_CASE("project config discovery walks up like ruff")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    QDir root(tmp.path());
    root.mkpath("a/b");
    const QString file = root.filePath("a/b/x.py");
    CHECK(format::findProjectConfig(file).isEmpty());

    write(root.filePath("pyproject.toml"), "[project]\nname = 'x'\n");
    CHECK(format::findProjectConfig(file).isEmpty());

    write(root.filePath("pyproject.toml"), "[tool.ruff]\nline-length = 100\n");
    CHECK(QDir::cleanPath(format::findProjectConfig(file)) == QDir::cleanPath(root.filePath("pyproject.toml")));

    write(root.filePath("a/ruff.toml"), "line-length = 90\n");
    CHECK(QDir::cleanPath(format::findProjectConfig(file)) == QDir::cleanPath(root.filePath("a/ruff.toml")));
}

TEST_CASE("range arg counts code points from 1")
{
    QStringList lines{"x = 1", QString::fromUtf8("s = '\xF0\x9F\x98\x80' + y")};
    CHECK(format::rangeArg(lines, 0, 0, 1, 10) == "--range=1:1-2:10");
}

TEST_CASE("diff yields minimal edits per changed run")
{
    const QString before = "import os\nx=foo( 1,2 )\n\n\ndef f():\n    return  1\n";
    const QString after = "import os\nx = foo(1, 2)\n\n\ndef f():\n    return 1\n";
    auto edits = format::diff(before, after);
    CHECK(applyEdits(before, edits) == after);
    REQUIRE(edits.size() == 2);
    CHECK(edits[0].start >= before.indexOf("x="));
    CHECK(edits[1].start > before.indexOf("return"));

    CHECK(format::diff(before, before).empty());
    CHECK(applyEdits("", format::diff("", "a\n")) == "a\n");
    CHECK(applyEdits("a\nb\nc", format::diff("a\nb\nc", "c")) == "c");
    const QString moved = "b\na\nc\nd\n";
    CHECK(applyEdits("a\nb\nc\nd\n", format::diff("a\nb\nc\nd\n", moved)) == moved);
}

TEST_CASE("caret maps through formatting edits")
{
    const QString before = "x=foo( 1,2 )\ny = 3\n";
    const QString after = "x = foo(1, 2)\ny = 3\n";
    auto edits = format::diff(before, after);

    // After the closing bracket.
    int pos = int(before.indexOf(')')) + 1;
    CHECK(format::mapPosition(pos, before, edits) == after.indexOf(')') + 1);
    // On the next line, shifted by the change before it.
    int y = int(before.indexOf('y')) + 2;
    CHECK(format::mapPosition(y, before, edits) == after.indexOf('y') + 2);
    // Right after the `2`, which moved.
    int two = int(before.indexOf('2')) + 1;
    CHECK(format::mapPosition(two, before, edits) == after.indexOf('2') + 1);
    // Before every change.
    CHECK(format::mapPosition(0, before, edits) == 0);
}

TEST_CASE("line scan tracks brackets, strings and continuations")
{
    QStringList lines{"x = (1,", "     2)", "s = '''a", "b)'''", "y = 1 + \\", "    2", "t = 'x(' # ("};
    auto st = format::scan(lines);
    REQUIRE(st.size() == lines.size() + 1);
    CHECK(st[1].depth == 1);
    CHECK(st[2].depth == 0);
    CHECK(st[3].inString);
    CHECK_FALSE(st[4].inString);
    CHECK(st[5].continued);
    CHECK_FALSE(st[6].continued);
    CHECK(st[7].depth == 0);
}

TEST_CASE("statement range for on-type formatting")
{
    QStringList lines{"x = foo(a,", "        b)", "y = [1,", "", "if x==1:", "    pass", "# note"};
    auto r = format::statementAt(lines, 1);
    REQUIRE(r);
    CHECK(r->first == 0);
    CHECK(r->last == 1);
    CHECK_FALSE(format::statementAt(lines, 0)); // still open
    CHECK_FALSE(format::statementAt(lines, 2)); // still open
    CHECK_FALSE(format::statementAt(lines, 3)); // blank
    CHECK_FALSE(format::statementAt(lines, 6)); // comment
    CHECK_FALSE(format::statementAt(lines, 4)); // the open `[` above swallows the rest

    QStringList body{"if x==1:", "    pass"};
    r = format::statementAt(body, 0);
    REQUIRE(r);
    CHECK(r->first == 0);
    CHECK_FALSE(format::needsPlaceholder(body, *r)); // has its body

    QStringList header{"def f():", "    if x==1:", "    ", "y = 2"};
    r = format::statementAt(header, 1);
    REQUIRE(r);
    CHECK(format::needsPlaceholder(header, *r));
    CHECK_FALSE(format::needsPlaceholder(header, {3, 3}));

    const QString ph = format::placeholderLine(header[1]);
    CHECK(ph.startsWith(QString(8, ' ')));
    QStringList with = header;
    with.insert(2, ph);
    CHECK(format::removePlaceholder(with.join('\n')) == header.join('\n'));
}

TEST_CASE("cleanup on save trims outside strings and ends with a newline")
{
    const QString text = "x = 1   \ns = '''a  \nb'''  \n    \ny = 2";
    CHECK(format::cleanupForSave(text, true, true) == "x = 1\ns = '''a  \nb'''\n\ny = 2\n");
    CHECK(format::cleanupForSave(text, true, true, 3) == "x = 1\ns = '''a  \nb'''\n    \ny = 2\n");
    CHECK(format::cleanupForSave(text, false, false) == text);
    CHECK(format::cleanupForSave("", true, true).isEmpty());
}

TEST_CASE("else, elif, except and finally dedent to their block")
{
    QStringList lines{"if a:", "    b()", "    else:"};
    CHECK(format::keywordDedent(lines, 2) == 0);

    QStringList nested{"if a:", "    if b:", "        x", "    else:"};
    CHECK_FALSE(format::keywordDedent(nested, 3)); // already belongs to `if b`

    QStringList deep{"for i in r:", "    if c:", "        d()", "        else:"};
    CHECK(format::keywordDedent(deep, 3) == 4);

    QStringList tryBlock{"try:", "    a()", "    except ValueError:"};
    CHECK(format::keywordDedent(tryBlock, 2) == 0);

    QStringList noColon{"if a:", "    b()", "    else"};
    CHECK_FALSE(format::keywordDedent(noColon, 2));
    QStringList other{"x = 1", "    else:"};
    CHECK_FALSE(format::keywordDedent(other, 1));
}

TEST_CASE("ruff check json carries quick fixes")
{
    const QByteArray json = R"([{"code": "F401", "message": "`os` imported but unused",
        "location": {"row": 1, "column": 8}, "end_location": {"row": 1, "column": 10},
        "fix": {"applicability": "safe", "message": "Remove unused import: `os`",
                "edits": [{"content": "", "location": {"row": 1, "column": 1}, "end_location": {"row": 2, "column": 1}}]}},
        {"code": "E741", "message": "Ambiguous variable name", "location": {"row": 2, "column": 1},
         "end_location": {"row": 2, "column": 2}, "fix": null}])";
    auto list = ruff::parseCheck(json, {"import os", "l = 1"});
    REQUIRE(list.size() == 2);
    CHECK(list[0].fixTitle == "Remove unused import: `os`");
    REQUIRE(list[0].fix.size() == 1);
    CHECK(list[0].fix[0].endLine == 1);
    CHECK(list[0].fix[0].endCol == 0);
    CHECK_FALSE(list[0].fixUnsafe);
    CHECK(list[1].fixTitle.isEmpty());
}
