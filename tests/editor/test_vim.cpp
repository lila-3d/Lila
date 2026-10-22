#include "ui/editor/VimEngine.h"

#include <doctest/doctest.h>

#include <vector>

using namespace lila::ui;
using vim::Mode;


namespace
{
/** A plain string with snapshot undo, standing in for the editor. */
struct TextBuffer : vim::Buffer
{
    QString t;
    int cur = 0;
    int depth = 0;
    QString groupStart;
    std::vector<QString> undos, redos;
    int selFrom = -1, selTo = -1;
    QString clip, lastMessage;
    int writes = 0, closes = 0;

    QString text() const override { return t; }
    int cursor() const override { return cur; }
    void setCursor(int pos) override { cur = pos; }
    void replace(int pos, int length, const QString& with) override { t.replace(pos, length, with); }
    void beginChange() override
    {
        if (depth++ == 0)
            groupStart = t;
    }
    void endChange() override
    {
        if (--depth == 0 && t != groupStart)
        {
            undos.push_back(groupStart);
            redos.clear();
        }
    }
    void undo() override
    {
        if (undos.empty())
            return;
        redos.push_back(t);
        t = undos.back();
        undos.pop_back();
        cur = std::min(cur, int(t.size()));
    }
    void redo() override
    {
        if (redos.empty())
            return;
        undos.push_back(t);
        t = redos.back();
        redos.pop_back();
        cur = std::min(cur, int(t.size()));
    }
    void showSelection(int from, int to) override
    {
        selFrom = from;
        selTo = to;
    }
    QString clipboard() const override { return clip; }
    void setClipboard(const QString& text) override { clip = text; }
    void write() override { ++writes; }
    void close() override { ++closes; }
    void message(const QString& text) override { lastMessage = text; }
};

/** The editor under test: '|' in the text marks the cursor. */
struct Vim
{
    TextBuffer buf;
    vim::Engine engine{buf};

    explicit Vim(const QString& marked)
    {
        buf.cur = int(marked.indexOf('|'));
        buf.t = marked;
        buf.t.remove(buf.cur, 1);
    }

    /** Keys as typed: <Esc>, <CR>, <BS>, <C-r> are one key each. Insert mode types into the buffer like the editor. */
    Vim& keys(const QString& seq)
    {
        for (int i = 0; i < seq.size();)
        {
            QString k;
            int close = seq[i] == '<' ? int(seq.indexOf('>', i)) : -1;
            if (close > i + 1)
                k = seq.mid(i, close - i + 1), i = close + 1;
            else
                k = seq.mid(i++, 1);
            if (engine.key(k))
                continue;
            REQUIRE(engine.mode() == Mode::Insert);
            if (k == "<CR>")
                type("\n");
            else if (k == "<BS>")
            {
                if (buf.cur > 0)
                    buf.t.remove(--buf.cur, 1);
            }
            else
                type(k);
        }
        return *this;
    }

    void type(const QString& s)
    {
        buf.t.insert(buf.cur, s);
        buf.cur += int(s.size());
    }

    std::string state() const
    {
        QString s = buf.t;
        s.insert(buf.cur, '|');
        return s.toStdString();
    }

    std::string text() const { return buf.t.toStdString(); }
};

std::string run(const QString& marked, const QString& keys)
{
    Vim v(marked);
    v.keys(keys);
    return v.state();
}
} // namespace

TEST_CASE("vim motions")
{
    CHECK(run("|foo bar.baz qux", "w") == "foo |bar.baz qux");
    CHECK(run("|foo bar.baz qux", "ww") == "foo bar|.baz qux");
    CHECK(run("|foo bar.baz qux", "2W") == "foo bar.baz |qux");
    CHECK(run("|foo bar.baz qux", "e") == "fo|o bar.baz qux");
    CHECK(run("foo bar.baz |qux", "b") == "foo bar.|baz qux");
    CHECK(run("foo bar.baz |qux", "B") == "foo |bar.baz qux");
    CHECK(run("foo bar.baz |qux", "ge") == "foo bar.ba|z qux");
    CHECK(run("  foo |bar", "0") == "|  foo bar");
    CHECK(run("  foo |bar", "^") == "  |foo bar");
    CHECK(run("|  foo bar", "$") == "  foo ba|r");
    CHECK(run("a|bc\nd\nefg", "jj") == "abc\nd\ne|fg");
    CHECK(run("abc\nd\ne|fg", "kk") == "a|bc\nd\nefg");
    CHECK(run("a|bc\n  d\nefg", "G") == "abc\n  d\n|efg");
    CHECK(run("abc\n  d\ne|fg", "gg") == "|abc\n  d\nefg");
    CHECK(run("abc\n  d\ne|fg", "2G") == "abc\n  |d\nefg");
    CHECK(run("|a,b,c,d", "f,") == "a|,b,c,d");
    CHECK(run("|a,b,c,d", "2f,") == "a,b|,c,d");
    CHECK(run("|a,b,c,d", "f,;;") == "a,b,c|,d");
    CHECK(run("|a,b,c,d", "f,;,") == "a|,b,c,d");
    CHECK(run("|a,b,c,d", "t,") == "|a,b,c,d");
    CHECK(run("|a,b,c,d", "t,;") == "a,|b,c,d");
    CHECK(run("a,b,c,|d", "F,") == "a,b,c|,d");
    CHECK(run("|f(a, (b))", "%") == "f(a, (b)|)");
    CHECK(run("f(a, (b)|)", "%") == "f|(a, (b))");
    CHECK(run("|a\nb\n\nc\nd", "}") == "a\nb\n|\nc\nd");
    CHECK(run("|a\n\nb", "w") == "a\n|\nb");
    CHECK(run("|foo\nbar", "3l") == "fo|o\nbar");
}

TEST_CASE("vim vertical motion keeps the column")
{
    CHECK(run("abc|d\nx\nabcdef", "jj") == "abcd\nx\nabc|def");
    CHECK(run("a|b\nabcdef", "$j") == "ab\nabcde|f");
}

TEST_CASE("vim operators with motions")
{
    CHECK(run("|foo bar baz", "dw") == "|bar baz");
    CHECK(run("|foo bar baz", "d2w") == "|baz");
    CHECK(run("|foo bar baz", "2dw") == "|baz");
    CHECK(run("foo |bar\nbaz", "dw") == "foo| \nbaz"); // the last word on a line keeps the newline
    CHECK(run("|foo bar", "cwnew<Esc>") == "ne|w bar");
    CHECK(run("fo|o bar", "cwX<Esc>") == "fo|X bar");
    CHECK(run("|foo bar", "de") == "| bar");
    CHECK(run("foo |bar", "db") == "|bar");
    CHECK(run("foo b|ar", "d$") == "foo |b");
    CHECK(run("foo b|ar", "D") == "foo |b");
    CHECK(run("foo b|ar", "d0") == "|ar");
    CHECK(run("|a,b,c", "dt,") == "|,b,c");
    CHECK(run("|a,b,c", "df,") == "|b,c");
    CHECK(run("foo b|ar", "C!<Esc>") == "foo b|!");
    CHECK(run("|abc", "x") == "|bc");
    CHECK(run("|abcd", "3x") == "|d");
    CHECK(run("ab|c", "X") == "a|c");
    CHECK(run("|abc", "sZ<Esc>") == "|Zbc");
    CHECK(run("|aBc", "g~$") == "|AbC");
    CHECK(run("|abc def", "gUiw") == "|ABC def");
    CHECK(run("|abc", "~~") == "AB|c");
    CHECK(run("|abc", "rx") == "|xbc");
    CHECK(run("|abc", "2rx") == "x|xc");
}

TEST_CASE("vim linewise operators and put")
{
    CHECK(run("a\n|b\nc", "dd") == "a\n|c");
    CHECK(run("a\nb\n|c", "dd") == "a\n|b");
    CHECK(run("|a\nb\nc\nd", "2dd") == "|c\nd");
    CHECK(run("|a\nb\nc\nd", "d2d") == "|c\nd");
    CHECK(run("|a\nb\nc", "dj") == "|c");
    CHECK(run("a\n|b\nc", "ddp") == "a\nc\n|b");
    CHECK(run("a\n|b\nc", "ddP") == "a\n|b\nc");
    CHECK(run("|a\nb", "yyp") == "a\n|a\nb");
    CHECK(run("|a\nb", "yy2p") == "a\n|a\na\nb");
    CHECK(run("|a\nb", "Yjp") == "a\nb\n|a");
    CHECK(run("  |a\nb", "ccx<Esc>") == "  |x\nb");
    CHECK(run("|a\nb\nc", "yjGp") == "a\nb\nc\n|a\nb");
    CHECK(run("|abc", "ylp") == "a|abc");
    CHECK(run("|abc", "xp") == "b|ac");
    CHECK(run("|abc", "xP") == "|abc");
    CHECK(run("|a\n  b\nc", "J") == "a| b\nc");
    CHECK(run("|a\nb\nc", "3J") == "a b| c");
    CHECK(run("|a\nb", ">>") == "    |a\nb");
    CHECK(run("    |a\nb", "<<") == "|a\nb");
    CHECK(run("|a\nb\nc", ">j") == "    |a\n    b\nc");
}

TEST_CASE("vim text objects")
{
    CHECK(run("foo b|ar baz", "diw") == "foo | baz");
    CHECK(run("foo b|ar baz", "daw") == "foo |baz");
    CHECK(run("foo b|ar", "daw") == "fo|o");
    CHECK(run("foo b|ar baz", "ciwX<Esc>") == "foo |X baz");
    CHECK(run("x = \"he|llo\" + y", "di\"") == "x = \"|\" + y");
    CHECK(run("x = \"he|llo\" + y", "da\"") == "x = |+ y");
    CHECK(run("|x = 'a' + 'b'", "ci'Q<Esc>") == "x = '|Q' + 'b'");
    CHECK(run("f(a, g(|b), c)", "di(") == "f(a, g(|), c)");
    CHECK(run("f(a, g(|b), c)", "d2i(") == "f(|)");
    CHECK(run("f(a, g(|b), c)", "da(") == "f(a, g|, c)");
    CHECK(run("f(a, g(b), |c)", "dib") == "f(|)");
    CHECK(run("x = [1, |2]", "ci]9<Esc>") == "x = [|9]");
    CHECK(run("if {\n    fo|o\n}", "di{") == "if {\n|}");
    CHECK(run("a\n|b\nc\n\nd", "dap") == "|d");
    CHECK(run("a\n|b\nc\n\nd", "yipP") == "|a\nb\nc\na\nb\nc\n\nd");
}

TEST_CASE("vim insert entries and counts")
{
    CHECK(run("a|bc", "iX<Esc>") == "a|Xbc");
    CHECK(run("a|bc", "aX<Esc>") == "ab|Xc");
    CHECK(run("  a|bc", "IX<Esc>") == "  |Xabc");
    CHECK(run("a|bc", "AX<Esc>") == "abc|X");
    CHECK(run("a|bc", "3ix<Esc>") == "axx|xbc");
    CHECK(run("|def f():", "opass<Esc>") == "def f():\n    pas|s");
    CHECK(run("    |a", "Ob<Esc>") == "    |b\n    a");
    CHECK(run("|a", "2oz<Esc>") == "a\nz\n|z");
    CHECK(run("|ab", "ixy<BS>z<Esc>") == "x|zab");
}

TEST_CASE("vim undo, redo and repeat")
{
    Vim v("|one two three");
    v.keys("dw");
    CHECK(v.state() == "|two three");
    v.keys("u");
    CHECK(v.text() == "one two three");
    v.keys("<C-r>");
    CHECK(v.text() == "two three");
    v.keys(".");
    CHECK(v.state() == "|three");

    Vim c("|aa bb cc");
    c.keys("cwxx<Esc>w.");
    CHECK(c.state() == "xx x|x cc");
    c.keys("u");
    CHECK(c.text() == "xx bb cc"); // the whole change, typing included, is one undo step
    c.keys("u");
    CHECK(c.text() == "aa bb cc");

    CHECK(run("|a\nb\nc\nd", "dd.") == "|c\nd");
    CHECK(run("|abcdef", "x3.") == "|ef");
    CHECK(run("|a\nb", "A;<Esc>j.") == "a;\nb|;");
    CHECK(run("|x\ny", "ox<Esc>j.") == "x\nx\ny\n|x");
    CHECK(run("|abc", "yl.") == "|abc"); // yanks don't repeat
}

TEST_CASE("vim visual modes")
{
    Vim v("|foo bar baz");
    v.keys("v");
    CHECK(v.engine.mode() == Mode::Visual);
    v.keys("e");
    CHECK(v.buf.selFrom == 0);
    CHECK(v.buf.selTo == 3);
    v.keys("d");
    CHECK(v.engine.mode() == Mode::Normal);
    CHECK(v.state() == "| bar baz");
    CHECK(v.buf.selFrom < 0);

    CHECK(run("|a\nb\nc", "Vjd") == "|c");
    CHECK(run("|a\nb\nc", "VjyGp") == "a\nb\nc\n|a\nb");
    CHECK(run("foo b|ar baz", "viwcX<Esc>") == "foo |X baz");
    CHECK(run("|abc", "vlU") == "|ABc");
    CHECK(run("|ab\ncd", "VjJ") == "ab| cd");
    CHECK(run("|a\nb", "Vj>") == "    |a\n    b");
    CHECK(run("ab|c", "vhho") == "ab|c"); // o swaps ends: the cursor goes back to the anchor
    CHECK(run("|one two", "yiwwviwp") == "one on|e");

    Vim e("|abc");
    e.keys("vl<Esc>");
    CHECK(e.engine.mode() == Mode::Normal);
    CHECK(e.buf.selFrom < 0);
    CHECK(e.text() == "abc");
}

TEST_CASE("vim registers")
{
    Vim v("|one two");
    v.keys("\"ayiww\"byiw\"aP");
    CHECK(v.state() == "one on|etwo");
    CHECK(v.engine.registerText('b').toStdString() == "two");
    v.keys("\"+yiw");
    CHECK(v.buf.clip.toStdString() == "onetwo");
    v.buf.clip = "Z";
    v.keys("\"+p");
    CHECK(v.buf.t.contains("Z"));
}

TEST_CASE("vim search and command line")
{
    CHECK(run("|a foo b foo", "/foo<CR>") == "a |foo b foo");
    CHECK(run("|a foo b foo", "/foo<CR>n") == "a foo b |foo");
    CHECK(run("|a foo b foo", "/foo<CR>nn") == "a |foo b foo"); // wraps
    CHECK(run("a foo b |foo", "?foo<CR>") == "a |foo b foo");
    CHECK(run("|a foo b foo", "/foo<CR>N") == "a foo b |foo");
    CHECK(run("|foo x foo", "*") == "foo x |foo");
    CHECK(run("foo x |foo", "#") == "|foo x foo");
    CHECK(run("|a(b", "/(<CR>") == "a|(b");      // a bare ( is literal

    Vim v("|a\nb\nc");
    v.keys(":3<CR>");
    CHECK(v.state() == "a\nb\n|c");
    CHECK(v.engine.mode() == Mode::Normal);
    v.keys(":w<CR>");
    CHECK(v.buf.writes == 1);
    v.keys(":q<CR>");
    CHECK(v.buf.closes == 1);
    v.keys(":wq<CR>");
    CHECK(v.buf.writes == 2);
    CHECK(v.buf.closes == 2);
    v.keys(":bogus<CR>");
    CHECK(v.buf.lastMessage.contains("bogus"));

    Vim s("|foo foo\nfoo");
    s.keys(":s/foo/bar/<CR>");
    CHECK(s.text() == "bar foo\nfoo");
    s.keys("u:%s/fo\\(o\\)/x\\1&/g<CR>");
    CHECK(s.text() == "xofoo xofoo\nxofoo");
    s.keys("u");
    CHECK(s.text() == "foo foo\nfoo");

    Vim esc("|abc");
    esc.keys(":wq<Esc>");
    CHECK(esc.engine.mode() == Mode::Normal);
    CHECK(esc.buf.writes == 0);
}

TEST_CASE("vim status and passthrough")
{
    Vim v("|abc");
    CHECK(v.engine.status().toStdString() == "NORMAL");
    v.keys("2d");
    CHECK(v.engine.status().toStdString() == "NORMAL  2d");
    v.keys("<Esc>i");
    CHECK(v.engine.status().toStdString() == "INSERT");
    CHECK(!v.engine.key("x")); // typed by the editor
    v.keys("<Esc>V");
    CHECK(v.engine.status().toStdString() == "V-LINE");
    v.keys("<Esc>:w");
    CHECK(v.engine.status().toStdString() == ":w");
    v.keys("<Esc>");
    CHECK(!v.engine.key("<C-s>")); // host chords go through
    CHECK(v.engine.key("<Tab>"));  // never typed in normal mode
    CHECK(v.text() == "abc");
}
