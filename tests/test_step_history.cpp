#include <doctest/doctest.h>

#include "ui/StepHistory.h"

using lila::ui::StepHistory;
using lila::ui::StepKey;

TEST_CASE("step history: undo, redo, and a new step drops the redo")
{
    std::vector<int> steps = {1, 2, 3};
    StepHistory<int> h;
    CHECK(h.undo(steps));
    CHECK(h.undo(steps));
    CHECK(steps == std::vector<int>{1});
    CHECK(h.redo(steps));
    CHECK(steps == std::vector<int>{1, 2});
    CHECK(h.canRedo());

    steps.push_back(9);
    h.clear();
    CHECK_FALSE(h.redo(steps));
    CHECK(steps == std::vector<int>{1, 2, 9});

    CHECK(h.undo(steps));
    CHECK(h.undo(steps));
    CHECK(h.undo(steps));
    CHECK_FALSE(h.undo(steps));
    CHECK(h.redo(steps));
    CHECK(h.redo(steps));
    CHECK(h.redo(steps));
    CHECK(steps == std::vector<int>{1, 2, 9});
}

TEST_CASE("step history: steps kept elsewhere")
{
    StepHistory<int> h;
    h.undone(4);
    h.undone(5);
    CHECK(h.take() == 5);
    CHECK(h.take() == 4);
    CHECK_FALSE(h.take());
}

TEST_CASE("step history: routing hotkeys")
{
    // Undo, Backspace and redo belong to the tool, with or without points to step; other keys pass.
    CHECK(StepHistory<int>::route(Qt::Key_Z, "edit.undo") == StepKey::Undo);
    CHECK(StepHistory<int>::route(Qt::Key_Backspace, "edit.delete") == StepKey::Undo);
    CHECK(StepHistory<int>::route(Qt::Key_Backspace, "") == StepKey::Undo);
    CHECK(StepHistory<int>::route(Qt::Key_Z, "edit.redo") == StepKey::Redo);
    CHECK(StepHistory<int>::route(Qt::Key_Delete, "edit.delete") == StepKey::None);
    CHECK(StepHistory<int>::route(Qt::Key_G, "edit.group") == StepKey::None);

    // Stepping with nothing to step does nothing, and keeps what redo has.
    StepHistory<int> h;
    std::vector<int> steps = {1};
    CHECK(h.undo(steps));
    CHECK_FALSE(h.undo(steps));
    CHECK(h.canRedo());
    CHECK(h.redo(steps));
    CHECK_FALSE(h.redo(steps));
    CHECK(steps == std::vector<int>{1});
}
