#include <doctest/doctest.h>

#include "ui/terminal/VtInput.h"
#include "ui/terminal/VtScreen.h"

#include <string>

using lila::term::VtScreen;
namespace term = lila::term;

TEST_CASE("line feeds at the bottom margin fill the scrollback")
{
    VtScreen s(20, 5, 10000);
    for (int i = 1; i <= 300; ++i)
        s.feed("line " + std::to_string(i) + "\r\n");
    CHECK(s.scrollbackSize() == 296);
    CHECK(s.lineText(-296) == "line 1");
    CHECK(s.lineText(-1) == "line 296");
    CHECK(s.lineText(0) == "line 297");
    CHECK(s.lineText(3) == "line 300");
}

TEST_CASE("full-screen scroll region and CSI S push to the scrollback; inner regions don't")
{
    VtScreen s(10, 4, 100);
    s.feed("a\r\nb\r\nc\r\nd");
    s.feed("\x1b[1;4r\x1b[4;1H\n");
    CHECK(s.scrollbackSize() == 1);
    CHECK(s.lineText(-1) == "a");
    s.feed("\x1b[2S");
    CHECK(s.scrollbackSize() == 3);
    CHECK(s.lineText(-1) == "c");

    VtScreen inner(10, 4, 100);
    inner.feed("a\r\nb\r\nc\r\nd");
    inner.feed("\x1b[2;4r\x1b[4;1H\n\x1b[S");
    CHECK(inner.scrollbackSize() == 0);
    CHECK(inner.lineText(0) == "a");
}

TEST_CASE("ED 2 on the main screen moves its contents into the scrollback")
{
    VtScreen s(10, 4, 100);
    s.feed("one\r\ntwo\r\n");
    s.feed("\x1b[2J");
    CHECK(s.scrollbackSize() == 2); // the blank rows below the content stay out
    CHECK(s.lineText(-2) == "one");
    CHECK(s.lineText(-1) == "two");
    for (int y = 0; y < 4; ++y)
        CHECK(s.lineText(y).empty());

    // A blank screen adds nothing: ConPTY opens with ED 2.
    s.feed("\x1b[2J");
    CHECK(s.scrollbackSize() == 2);

    // A redraw that clears the history as well, like cls or Ink's clearTerminal.
    s.feed("x\x1b[2J\x1b[3J\x1b[H");
    CHECK(s.scrollbackSize() == 0);
}

TEST_CASE("ED 2 on the alternate screen leaves the scrollback alone")
{
    VtScreen s(10, 3, 100);
    s.feed("\x1b[?1049hfull\x1b[2J");
    CHECK(s.scrollbackSize() == 0);
    s.feed("\x1b[?1049l");
    CHECK_FALSE(s.altScreen());
}

TEST_CASE("mouse tracking, encoding, alternate scroll, focus and synchronized output modes")
{
    VtScreen s(80, 24, 0);
    CHECK(s.mouseTracking() == term::MouseTracking::Off);
    CHECK(s.mouseEncoding() == term::MouseEncoding::Default);
    CHECK(s.alternateScroll());
    CHECK_FALSE(s.synchronizedOutput());
    CHECK_FALSE(s.focusReporting());

    // What Claude Code sends through ConPTY when it starts.
    s.feed("\x1b[?1049h\x1b[2J\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h\x1b[?2026h");
    CHECK(s.mouseTracking() == term::MouseTracking::Any);
    CHECK(s.mouseEncoding() == term::MouseEncoding::Sgr);
    CHECK(s.synchronizedOutput());
    s.feed("\x1b[?2026l\x1b[?1004h");
    CHECK_FALSE(s.synchronizedOutput());
    CHECK(s.focusReporting());

    // Resetting a mode that isn't the current one keeps the current one.
    s.feed("\x1b[?1000l");
    CHECK(s.mouseTracking() == term::MouseTracking::Any);
    s.feed("\x1b[?1003l\x1b[?1006l\x1b[?1007l");
    CHECK(s.mouseTracking() == term::MouseTracking::Off);
    CHECK(s.mouseEncoding() == term::MouseEncoding::Default);
    CHECK_FALSE(s.alternateScroll());

    s.feed("\x1b[?2026h\x1b[!p");
    CHECK_FALSE(s.synchronizedOutput());
}

TEST_CASE("wheel: scrollback on the main screen, mouse reports when tracked, keys on the alternate screen")
{
    VtScreen s(80, 24, 100);
    CHECK(term::wheelAction(s, false) == term::WheelAction::ScrollHistory);
    CHECK(term::wheelAction(s, true) == term::WheelAction::ScrollHistory);

    s.feed("\x1b[?1049h");
    CHECK(term::wheelAction(s, false) == term::WheelAction::SendKeys);
    CHECK(term::wheelKeys(s, true) == "\x1b[A");
    s.feed("\x1b[?1h");
    CHECK(term::wheelKeys(s, false) == "\x1bOB");
    s.feed("\x1b[?1007l");
    CHECK(term::wheelAction(s, false) == term::WheelAction::Ignore);

    s.feed("\x1b[?1000h\x1b[?1006h");
    CHECK(term::wheelAction(s, false) == term::WheelAction::SendMouse);
    CHECK(term::wheelAction(s, true) == term::WheelAction::Ignore);

    // Mouse tracking on the main screen also takes the wheel; Shift still reaches the scrollback.
    s.feed("\x1b[?1049l");
    CHECK(term::wheelAction(s, false) == term::WheelAction::SendMouse);
    CHECK(term::wheelAction(s, true) == term::WheelAction::ScrollHistory);
}

TEST_CASE("mouse reports in each encoding")
{
    using enum term::MouseEventType;
    using B = term::MouseButton;
    VtScreen s(300, 24, 0);
    CHECK(term::encodeMouse(s, Press, B::Left, 0, 0).empty());

    s.feed("\x1b[?1000h\x1b[?1006h");
    CHECK(term::encodeMouse(s, Press, B::WheelUp, 19, 9) == "\x1b[<64;20;10M");
    CHECK(term::encodeMouse(s, Press, B::WheelDown, 19, 9) == "\x1b[<65;20;10M");
    CHECK(term::encodeMouse(s, Press, B::Left, 2, 3) == "\x1b[<0;3;4M");
    CHECK(term::encodeMouse(s, Release, B::Left, 2, 3) == "\x1b[<0;3;4m");
    CHECK(term::encodeMouse(s, Press, B::Right, 0, 0, {.shift = false, .alt = true, .ctrl = true}) == "\x1b[<26;1;1M");
    CHECK(term::encodeMouse(s, Release, B::WheelUp, 0, 0).empty());
    CHECK(term::encodeMouse(s, Move, B::Left, 0, 0).empty()); // 1000 has no motion

    s.feed("\x1b[?1002h");
    CHECK(term::encodeMouse(s, Move, B::Left, 4, 0) == "\x1b[<32;5;1M");
    CHECK(term::encodeMouse(s, Move, B::None, 4, 0).empty());
    s.feed("\x1b[?1003h");
    CHECK(term::encodeMouse(s, Move, B::None, 4, 0) == "\x1b[<35;5;1M");

    s.feed("\x1b[?1006l");
    CHECK(term::encodeMouse(s, Press, B::Left, 0, 0) == std::string("\x1b[M !!"));
    CHECK(term::encodeMouse(s, Release, B::Middle, 0, 0) == std::string("\x1b[M#!!"));
    CHECK(term::encodeMouse(s, Press, B::WheelUp, 1, 2) == std::string("\x1b[M`\"#"));
    CHECK(term::encodeMouse(s, Press, B::Left, 250, 0).empty()); // past what a byte can carry

    s.feed("\x1b[?1015h");
    CHECK(term::encodeMouse(s, Press, B::Left, 250, 0) == "\x1b[32;251;1M");
    s.feed("\x1b[?1005h");
    CHECK(term::encodeMouse(s, Press, B::Left, 250, 0) == std::string("\x1b[M \xC4\x9B!"));

    s.feed("\x1b[?9h");
    CHECK(term::encodeMouse(s, Press, B::Left, 0, 0, {.shift = true}) == std::string("\x1b[M !!"));
    CHECK(term::encodeMouse(s, Release, B::Left, 0, 0).empty());
    CHECK(term::encodeMouse(s, Press, B::WheelUp, 0, 0).empty());
}

TEST_CASE("find walks the screen and scrollback in either direction")
{
    VtScreen s(20, 3, 100);
    s.feed("alpha Needle\r\nbeta\r\nneedle gamma\r\ndelta\r\nlast");
    // history: -2 "alpha Needle", -1 "beta"; screen: 0 "needle gamma", 1 "delta", 2 "last"
    auto m = s.find("needle", 3, 0, true);
    REQUIRE(m);
    CHECK(m->line == 0);
    CHECK(m->col == 0);
    CHECK(m->length == 6);

    m = s.find("needle", m->line, m->col, true);
    REQUIRE(m);
    CHECK(m->line == -2);
    CHECK(m->col == 6);

    CHECK_FALSE(s.find("needle", -2, 6, true));
    CHECK_FALSE(s.find("needle", 0, 0, true, true)); // case-sensitive skips "Needle"
    m = s.find("Needle", 3, 0, true, true);
    REQUIRE(m);
    CHECK(m->line == -2);

    m = s.find("needle", -2, 6, false);
    REQUIRE(m);
    CHECK(m->line == 0);
    CHECK_FALSE(s.find("zzz", 3, 0, true));
    CHECK_FALSE(s.find("", 3, 0, true));
}

TEST_CASE("find measures wide characters in cells")
{
    VtScreen s(20, 2, 0);
    s.feed("\xE4\xB8\xAD\xE6\x96\x87 ok");
    auto m = s.find("ok", 2, 0, true);
    REQUIRE(m);
    CHECK(m->col == 5);
    m = s.find("\xE6\x96\x87", 2, 0, true);
    REQUIRE(m);
    CHECK(m->col == 2);
    CHECK(m->length == 2);
}
