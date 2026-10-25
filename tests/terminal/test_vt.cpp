#include <doctest/doctest.h>

#include "ui/terminal/VtScreen.h"

using lila::term::VtScreen;
namespace term = lila::term;

namespace
{
std::string row(const VtScreen& s, int y)
{
    return s.lineText(y);
}
} // namespace

TEST_CASE("plain text, CR LF and wrapping")
{
    VtScreen s(10, 3, 100);
    s.feed("hello\r\nworld");
    CHECK(row(s, 0) == "hello");
    CHECK(row(s, 1) == "world");
    CHECK(s.cursorX() == 5);
    CHECK(s.cursorY() == 1);

    s.feed("\r\n0123456789AB");
    // Wrapping off the bottom row scrolls the first line into history.
    CHECK(row(s, 1) == "0123456789");
    CHECK(s.scrollbackSize() == 1);
    CHECK(s.lineText(-1) == "hello");
    CHECK(row(s, 2) == "AB");
}

TEST_CASE("pending wrap holds the cursor on the last column")
{
    VtScreen s(5, 2, 10);
    s.feed("abcde");
    CHECK(s.cursorX() == 4);
    CHECK(s.cursorY() == 0);
    s.feed("\r\n");
    CHECK(s.cursorY() == 1);
    CHECK(row(s, 1).empty());
}

TEST_CASE("CSI cursor movement")
{
    VtScreen s(20, 10, 0);
    s.feed("\x1b[5;7H");
    CHECK(s.cursorY() == 4);
    CHECK(s.cursorX() == 6);
    s.feed("\x1b[2A");
    CHECK(s.cursorY() == 2);
    s.feed("\x1b[3B");
    CHECK(s.cursorY() == 5);
    s.feed("\x1b[4C");
    CHECK(s.cursorX() == 10);
    s.feed("\x1b[20D");
    CHECK(s.cursorX() == 0);
    s.feed("\x1b[H");
    CHECK(s.cursorX() == 0);
    CHECK(s.cursorY() == 0);
    s.feed("\x1b[99;99H");
    CHECK(s.cursorX() == 19);
    CHECK(s.cursorY() == 9);
    s.feed("\x1b[3G");
    CHECK(s.cursorX() == 2);
    s.feed("\x1b[2d");
    CHECK(s.cursorY() == 1);
}

TEST_CASE("erase in line and display")
{
    VtScreen s(10, 3, 0);
    s.feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    s.feed("\x1b[2;5H\x1b[K");
    CHECK(row(s, 1) == "bbbb");
    s.feed("\x1b[1K");
    CHECK(row(s, 1).empty());
    s.feed("\x1b[1;3H\x1b[0J");
    CHECK(row(s, 0) == "aa");
    CHECK(row(s, 2).empty());
    s.feed("x\x1b[2J");
    for (int y = 0; y < 3; ++y)
        CHECK(row(s, y).empty());
}

TEST_CASE("ED 3 clears the scrollback")
{
    VtScreen s(10, 2, 100);
    s.feed("1\r\n2\r\n3\r\n4");
    CHECK(s.scrollbackSize() == 2);
    s.feed("\x1b[3J");
    CHECK(s.scrollbackSize() == 0);
    CHECK(s.droppedLines() == 2);
}

TEST_CASE("scrollback limit drops the oldest lines")
{
    VtScreen s(10, 2, 3);
    for (int i = 0; i < 10; ++i)
        s.feed(std::to_string(i) + "\r\n");
    CHECK(s.scrollbackSize() == 3);
    CHECK(s.lineText(-3) == "6");
    CHECK(s.droppedLines() == 6);
}

TEST_CASE("SGR attributes and colors")
{
    VtScreen s(20, 2, 0);
    s.feed("\x1b[1;4;7mA\x1b[0mB\x1b[2mC\x1b[22mD");
    const term::Line& l = s.screenLine(0);
    CHECK(l[0].attrs == (term::Bold | term::Underline | term::Inverse));
    CHECK(l[1].attrs == 0);
    CHECK(l[2].attrs == term::Dim);
    CHECK(l[3].attrs == 0);

    s.feed("\x1b[31mR\x1b[92mG\x1b[38;5;21mI\x1b[38;2;255;255;255mW\x1b[48;2;0;0;0mK\x1b[39;49mN");
    CHECK(l[4].fg == 1);
    CHECK(l[5].fg == 10);
    CHECK(l[6].fg == 21);
    CHECK(l[7].fg == term::rgbColor(255, 255, 255));
    CHECK(l[8].bg == term::rgbColor(0, 0, 0));
    CHECK(l[9].fg == term::kDefaultColor);
    CHECK(l[9].bg == term::kDefaultColor);

    // Colon form with an empty color-space slot.
    s.feed("\x1b[38:2::0:0:0mX");
    CHECK(l[10].fg == term::rgbColor(0, 0, 0));

    CHECK(term::grayFromColor(l[4].fg) == term::grayFromIndex(1));
    CHECK(term::grayFromColor(l[7].fg) == 255);
    term::Rgb blue = term::rgbFromColor(21, nullptr);
    CHECK((blue.r == 0 && blue.g == 0 && blue.b == 255));
    term::Rgb ramp = term::rgbFromColor(232, nullptr);
    CHECK((ramp.r == 8 && ramp.g == 8 && ramp.b == 8));
}

TEST_CASE("gray mapping follows perceived lightness")
{
    CHECK(term::grayFromRgb(0, 0, 0) == 0);
    CHECK(term::grayFromRgb(255, 255, 255) == 255);
    CHECK(term::grayFromRgb(128, 128, 128) == 128);
    // Green reads far lighter than blue at the same intensity.
    CHECK(term::grayFromRgb(0, 200, 0) > term::grayFromRgb(0, 0, 200) + 60);
    CHECK(term::grayFromIndex(232) == 8);
    CHECK(term::grayFromIndex(255) == 238);
}

TEST_CASE("alternate screen keeps the main screen")
{
    VtScreen s(10, 3, 10);
    s.feed("main\r\nline");
    s.feed("\x1b[?1049h");
    CHECK(s.altScreen());
    CHECK(row(s, 0).empty());
    s.feed("\x1b[Halt");
    CHECK(row(s, 0) == "alt");
    s.feed("\x1b[?1049l");
    CHECK_FALSE(s.altScreen());
    CHECK(row(s, 0) == "main");
    CHECK(row(s, 1) == "line");
    CHECK(s.cursorX() == 4);
    CHECK(s.cursorY() == 1);
}

TEST_CASE("modes: cursor visibility, bracketed paste, app cursor keys")
{
    VtScreen s;
    s.feed("\x1b[?25l\x1b[?2004h\x1b[?1h");
    CHECK_FALSE(s.cursorVisible());
    CHECK(s.bracketedPaste());
    CHECK(s.appCursorKeys());
    s.feed("\x1b[?25h\x1b[?2004;1l");
    CHECK(s.cursorVisible());
    CHECK_FALSE(s.bracketedPaste());
    CHECK_FALSE(s.appCursorKeys());
}

TEST_CASE("OSC title is swallowed, with BEL or ST")
{
    VtScreen s(20, 2, 0);
    s.feed("\x1b]0;my title\x07" "a\x1b]2;other\x1b\\b");
    CHECK(row(s, 0) == "ab");
    CHECK(s.title() == "other");
}

TEST_CASE("UTF-8 decoding, split across feeds")
{
    VtScreen s(10, 2, 0);
    std::string text = "caf\xC3\xA9 \xE2\x94\x80";
    s.feed(text.substr(0, 4));
    s.feed(text.substr(4));
    CHECK(s.screenLine(0)[3].ch == char32_t(0xE9));
    CHECK(s.screenLine(0)[5].ch == char32_t(0x2500));
    CHECK(row(s, 0) == text);

    VtScreen bad(10, 2, 0);
    bad.feed("\xC3(");
    CHECK(bad.screenLine(0)[0].ch == char32_t(0xFFFD));
    CHECK(bad.screenLine(0)[1].ch == U'(');
}

TEST_CASE("wide chars take two cells")
{
    VtScreen s(6, 2, 0);
    s.feed("a\xE4\xB8\xAD" "b"); // a, U+4E2D, b
    const term::Line& l = s.screenLine(0);
    CHECK(l[1].width == 2);
    CHECK(l[2].width == 0);
    CHECK(l[3].ch == U'b');
    CHECK(s.cursorX() == 4);
    CHECK(row(s, 0) == "a\xE4\xB8\xAD" "b");

    // Overwriting the trailing half blanks the leading half.
    s.feed("\x1b[1;3Hx");
    CHECK(l[1].ch == U' ');
    CHECK(l[2].ch == U'x');

    // A wide char that doesn't fit on the last column wraps whole.
    VtScreen w(3, 2, 0);
    w.feed("ab\xE4\xB8\xAD");
    CHECK(w.screenLine(1)[0].width == 2);
    CHECK(w.cursorY() == 1);
}

TEST_CASE("scroll region, insert and delete lines")
{
    VtScreen s(5, 5, 100);
    s.feed("1\r\n2\r\n3\r\n4\r\n5");
    s.feed("\x1b[2;4r"); // rows 2..4
    s.feed("\x1b[4;1H\n");
    CHECK(row(s, 0) == "1");
    CHECK(row(s, 1) == "3");
    CHECK(row(s, 2) == "4");
    CHECK(row(s, 3).empty());
    CHECK(row(s, 4) == "5");
    // Scrolling inside a region never feeds history.
    CHECK(s.scrollbackSize() == 0);

    s.feed("\x1b[r\x1b[2;1H\x1b[L");
    CHECK(row(s, 1).empty());
    CHECK(row(s, 2) == "3");
    s.feed("\x1b[2M");
    CHECK(row(s, 1) == "4");
}

TEST_CASE("insert, delete and erase characters")
{
    VtScreen s(8, 1, 0);
    s.feed("abcdef\x1b[1;3H\x1b[2@");
    CHECK(row(s, 0) == "ab  cdef");
    s.feed("\x1b[3P");
    CHECK(row(s, 0) == "abdef");
    s.feed("\x1b[1;1H\x1b[2X");
    CHECK(row(s, 0) == "  def");
}

TEST_CASE("reverse index at the top scrolls down")
{
    VtScreen s(5, 3, 0);
    s.feed("a\r\nb\r\nc\x1b[H\x1bM");
    CHECK(row(s, 0).empty());
    CHECK(row(s, 1) == "a");
    CHECK(row(s, 2) == "b");
}

TEST_CASE("save and restore cursor")
{
    VtScreen s(10, 5, 0);
    s.feed("\x1b[3;4H\x1b" "7\x1b[H\x1b" "8");
    CHECK(s.cursorX() == 3);
    CHECK(s.cursorY() == 2);
    s.feed("\x1b[1;1H\x1b[s\x1b[5;5H\x1b[u");
    CHECK(s.cursorX() == 0);
    CHECK(s.cursorY() == 0);
}

TEST_CASE("tabs and backspace")
{
    VtScreen s(20, 1, 0);
    s.feed("a\tb\x08" "c");
    CHECK(row(s, 0) == "a       c");
}

TEST_CASE("DSR and DA replies")
{
    VtScreen s(10, 5, 0);
    s.feed("\x1b[2;3H\x1b[6n\x1b[c");
    CHECK(s.takeResponses() == "\x1b[2;3R\x1b[?1;2c");
    CHECK(s.takeResponses().empty());
}

TEST_CASE("DEC line drawing charset")
{
    VtScreen s(5, 1, 0);
    s.feed("\x1b(0qx\x1b(Bq");
    CHECK(s.screenLine(0)[0].ch == char32_t(0x2500));
    CHECK(s.screenLine(0)[1].ch == char32_t(0x2502));
    CHECK(s.screenLine(0)[2].ch == U'q');
}

TEST_CASE("resize keeps the cursor line visible")
{
    VtScreen s(10, 5, 100);
    s.feed("1\r\n2\r\n3\r\n4\r\n5");
    s.resize(6, 3);
    CHECK(s.rows() == 3);
    CHECK(s.cols() == 6);
    CHECK(s.cursorY() == 2);
    CHECK(row(s, 2) == "5");
    CHECK(s.scrollbackSize() == 2);
    CHECK(s.lineText(-2) == "1");
    s.resize(12, 6);
    CHECK(s.screenLine(5).size() == 12);
}

TEST_CASE("text extraction across history and screen")
{
    VtScreen s(10, 2, 10);
    s.feed("first\r\nsecond\r\nthird");
    CHECK(s.text(-1, 2, 1, 3) == "rst\nsecond\nthi");
}

TEST_CASE("unknown sequences are ignored without corrupting output")
{
    VtScreen s(20, 1, 0);
    s.feed("a\x1b[?9001h\x1b[>4;1m\x1b[5 q\x1bP1$r\x1b\\b\x1b[12;34;56tc");
    CHECK(row(s, 0) == "abc");
}
