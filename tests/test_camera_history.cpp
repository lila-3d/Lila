#include <doctest/doctest.h>

#include "ui/CameraHistory.h"
#include "ui/OutputTail.h"

using namespace lila;
using namespace lila::ui;

namespace
{
CameraView at(double yaw, const char* view = "persp")
{
    CameraView v;
    v.view = view;
    v.yaw = yaw;
    v.distance = 600.0;
    return v;
}
} // namespace

TEST_CASE("camera history undoes and redoes view changes in order")
{
    CameraHistory h;
    h.record(at(0), at(10), 0, 0.0);
    h.record(at(10), at(20), 0, 10.0);
    auto back = h.undo(at(20));
    REQUIRE(back);
    CHECK(back->yaw == 10.0);
    back = h.undo(*back);
    REQUIRE(back);
    CHECK(back->yaw == 0.0);
    CHECK_FALSE(h.undo(*back));
    auto fwd = h.redo(at(0));
    REQUIRE(fwd);
    CHECK(fwd->yaw == 10.0);
    fwd = h.redo(*fwd);
    REQUIRE(fwd);
    CHECK(fwd->yaw == 20.0);
    CHECK_FALSE(h.redo(*fwd));
}

TEST_CASE("camera history coalesces a gesture's burst into one step")
{
    CameraHistory h(100, 600.0);
    // Wheel clicks 100 ms apart share gesture 1: one step back to before the first.
    h.record(at(0), at(1), 1, 0.0);
    h.record(at(1), at(2), 1, 100.0);
    h.record(at(2), at(3), 1, 200.0);
    CHECK(h.undoCount() == 1);
    // A pause longer than the window starts a new step.
    h.record(at(3), at(4), 1, 1000.0);
    CHECK(h.undoCount() == 2);
    // A gesture of 0 (a drag, a frame, a view switch) never merges.
    h.record(at(4), at(5), 0, 1001.0);
    h.record(at(5), at(6), 0, 1002.0);
    CHECK(h.undoCount() == 4);
    auto v = h.undo(at(6));
    v = h.undo(*v);
    v = h.undo(*v);
    v = h.undo(*v);
    REQUIRE(v);
    CHECK(v->yaw == 0.0);
}

TEST_CASE("camera history skips no-op changes, clears redo on a new change and stays within its limit")
{
    CameraHistory h(3);
    h.record(at(5), at(5), 0, 0.0);
    CHECK(h.undoCount() == 0);
    for (int i = 0; i < 5; ++i)
        h.record(at(i), at(i + 1), 0, i * 1000.0);
    CHECK(h.undoCount() == 3);
    auto v = h.undo(at(5));
    REQUIRE(v);
    CHECK(h.redoCount() == 1);
    h.record(*v, at(42), 0, 9000.0);
    CHECK(h.redoCount() == 0);
}

TEST_CASE("camera history does not merge a gesture across an undo")
{
    CameraHistory h;
    h.record(at(0), at(1), 1, 0.0);
    auto v = h.undo(at(1));
    REQUIRE(v);
    h.record(*v, at(2), 1, 50.0);
    CHECK(h.undoCount() == 1);
    v = h.undo(at(2));
    REQUIRE(v);
    CHECK(v->yaw == 0.0);
}

TEST_CASE("camera views compare by view name and placement")
{
    CHECK(at(10).same(at(10)));
    CHECK_FALSE(at(10).same(at(10, "top")));
    CHECK_FALSE(at(10).same(at(10.5)));
}

TEST_CASE("output tail keeps the last lines, joining stdout pieces and dropping debug and blank lines")
{
    OutputTail t(3);
    CHECK_FALSE(t.add(LogLevel::Debug, "noise"));
    CHECK_FALSE(t.add(LogLevel::Output, "hel"));
    CHECK(t.lines().empty());
    CHECK(t.add(LogLevel::Output, "lo\nwor"));
    REQUIRE(t.lines().size() == 1);
    CHECK(t.lines().back().text == "hello");
    // Another level flushes the unfinished stdout line before its own.
    CHECK(t.add(LogLevel::Warning, "careful\n"));
    REQUIRE(t.lines().size() == 3);
    CHECK(t.lines()[1].text == "wor");
    CHECK(t.lines()[1].level == LogLevel::Output);
    CHECK(t.lines().back().text == "careful");
    CHECK(t.lines().back().level == LogLevel::Warning);
    CHECK_FALSE(t.add(LogLevel::Output, "\n"));
    CHECK_FALSE(t.add(LogLevel::Info, "   \n\n"));
    // A traceback is several lines; the exception message ends up last.
    CHECK(t.add(LogLevel::Error, "Traceback (most recent call last):\n  File \"<prompt>\", line 1\nNameError: name 'x' is not defined\n"));
    REQUIRE(t.lines().size() == 3);
    CHECK(t.lines().back().text == "NameError: name 'x' is not defined");
    CHECK(t.lines().back().level == LogLevel::Error);
}

TEST_CASE("camera history of a camera node restores its transform")
{
    auto node = [](double tx) {
        CameraView v;
        v.view = "#7";
        v.node = true;
        v.translate = {tx, 0, 0};
        return v;
    };
    CameraHistory h;
    h.record(node(0), node(0), 0, 0.0);
    CHECK(h.undoCount() == 0);
    h.record(node(0), node(5), 0, 0.0);
    auto back = h.undo(node(5));
    REQUIRE(back);
    CHECK(back->node);
    CHECK(back->translate.x == 0.0);
    CHECK(!node(1).same(node(2)));
}
