#pragma once

#include <QtCore/qnamespace.h>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace lila::ui
{
/** What a key asks of a tool's step history. */
enum class StepKey
{
    None,
    Undo,
    Redo,
};

/**
 * Undo and redo for a viewport tool's uncommitted steps (points placed but not yet made into anything). The tool keeps
 * the steps; this keeps what undo took off them so redo can put it back. Placing a new step, committing or cancelling
 * drops the redo.
 */
template <class T>
class StepHistory
{
public:
    /** Moves the last step onto the redo stack. False when there are none. */
    bool undo(std::vector<T>& steps)
    {
        if (steps.empty())
            return false;
        mRedo.push_back(std::move(steps.back()));
        steps.pop_back();
        return true;
    }
    /** Puts the step undo took last back on the end of steps. False when there is none. */
    bool redo(std::vector<T>& steps)
    {
        std::optional<T> s = take();
        if (!s)
            return false;
        steps.push_back(std::move(*s));
        return true;
    }

    /** For tools that keep their steps some other way: records a step undo removed. */
    void undone(T step) { mRedo.push_back(std::move(step)); }
    /** The step to redo, taken off the redo stack. */
    std::optional<T> take()
    {
        if (mRedo.empty())
            return std::nullopt;
        T s = std::move(mRedo.back());
        mRedo.pop_back();
        return s;
    }

    /**
     * What a hotkey does to the steps: edit.undo or Backspace steps back, edit.redo puts one back. A drawing tool owns
     * them while it is active, so they never reach the scene's history, even with nothing to step. None lets the
     * hotkey run as usual.
     */
    static StepKey route(int key, std::string_view action)
    {
        if (action == "edit.undo" || key == Qt::Key_Backspace)
            return StepKey::Undo;
        if (action == "edit.redo")
            return StepKey::Redo;
        return StepKey::None;
    }

    void clear() { mRedo.clear(); }
    bool canRedo() const { return !mRedo.empty(); }

private:
    std::vector<T> mRedo;
};
} // namespace lila::ui
