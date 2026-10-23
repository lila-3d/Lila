#include "ui/editor/TextUtil.h"

#include <doctest/doctest.h>

using namespace lila::ui;

TEST_CASE("code point and UTF-16 columns convert both ways")
{
    QString line = QString::fromUtf8("a\xF0\x9F\x98\x80" "b"); // a, an emoji (surrogate pair), b
    CHECK(text::codepointToUtf16(line, 0) == 0);
    CHECK(text::codepointToUtf16(line, 1) == 1);
    CHECK(text::codepointToUtf16(line, 2) == 3);
    CHECK(text::codepointToUtf16(line, 3) == 4);
    CHECK(text::utf16ToCodepoint(line, 3) == 2);
    CHECK(text::utf16ToCodepoint(line, 4) == 3);
    CHECK(text::codepointToUtf16(u"abc", 10) == 3);
}

TEST_CASE("indent after a line")
{
    CHECK(text::indentAfter(u"def f(x):") == 4);
    CHECK(text::indentAfter(u"    if x:  # note") == 8);
    CHECK(text::indentAfter(u"    return x") == 0);
    CHECK(text::indentAfter(u"        pass") == 4);
    CHECK(text::indentAfter(u"    x = foo(") == 8);
    CHECK(text::indentAfter(u"    s = 'a:'") == 4);
    CHECK(text::indentAfter(u"    returned = 1") == 4);
}

TEST_CASE("dedent strips the common indent")
{
    CHECK(text::dedent("    a\n      b\n\n    c") == "a\n  b\n\nc");
    CHECK(text::dedent("a\n  b") == "a\n  b");
}

TEST_CASE("toggle comment")
{
    QStringList in{"    a = 1", "", "    if a:", "        b()"};
    QStringList commented = text::toggleComment(in);
    CHECK(commented == QStringList{"    # a = 1", "", "    # if a:", "    #     b()"});
    CHECK(text::toggleComment(commented) == in);
    CHECK(text::toggleComment({"#x", "#  y"}) == QStringList{"x", " y"});
}

TEST_CASE("cells split on # %%")
{
    QStringList lines{"import x", "# %% one", "a = 1", "b = 2", "# %% two", "c = 3"};
    auto r = text::cellAt(lines, 0);
    CHECK(r.first == 0);
    CHECK(r.last == 0);
    r = text::cellAt(lines, 3);
    CHECK(r.first == 1);
    CHECK(r.last == 3);
    r = text::cellAt(lines, 5);
    CHECK(r.first == 4);
    CHECK(r.last == 5);
}

TEST_CASE("traceback references")
{
    auto r = text::findTraceRef(u"  File \"C:\\scripts\\a.py\", line 12, in <module>");
    REQUIRE(r);
    CHECK(r->file == "C:\\scripts\\a.py");
    CHECK(r->line == 12);
    CHECK(r->start == 2);
    CHECK(!text::findTraceRef(u"nothing here"));
    auto u = text::findTraceRef(u"File \"<Untitled-1>\", line 3");
    REQUIRE(u);
    CHECK(u->file == "<Untitled-1>");
}

TEST_CASE("dotted prefixes and symbols")
{
    auto p = text::dottedPrefixAt(u"x = lila.ops.po", 16);
    CHECK(p.base == "lila.ops");
    CHECK(p.prefix == "po");
    p = text::dottedPrefixAt(u"lila.", 5);
    CHECK(p.base == "lila");
    CHECK(p.prefix.isEmpty());
    p = text::dottedPrefixAt(u"pri", 3);
    CHECK(p.base.isEmpty());
    CHECK(p.prefix == "pri");
    CHECK(text::dottedSymbolAt(u"ops.polyCube(w=1)", 7) == "ops.polyCube");
    CHECK(text::isDottedName(u"a.b_c.d1"));
    CHECK(!text::isDottedName(u"a().b"));
    CHECK(!text::isDottedName(u"1a"));
}

TEST_CASE("match score ranks prefixes ahead of subsequences")
{
    CHECK(text::matchScore(u"polyCube", u"po") >= 0);
    CHECK(text::matchScore(u"polyCube", u"pC") >= 0);
    CHECK(text::matchScore(u"polyCube", u"po") < text::matchScore(u"PolyCube", u"po"));
    CHECK(text::matchScore(u"PolyCube", u"po") < text::matchScore(u"spoly", u"po"));
    CHECK(text::matchScore(u"polyCube", u"xz") == -1);
}

TEST_CASE("file URIs round-trip")
{
    QString uri = text::pathToUri("X:/Lila/my script.py");
    CHECK(uri.startsWith("file:///"));
    CHECK(text::uriToPath(uri) == "X:/Lila/my script.py");
    // basedpyright reports drive colons percent-encoded and lowercase.
    CHECK(text::sameUri(uri, "file:///x%3A/Lila/my%20script.py"));
}
