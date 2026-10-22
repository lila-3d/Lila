#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace lila::ui
{
/**
 * Where a view's camera looks: the navigation state camera undo restores. A built-in view orbits a target; a camera
 * node looked through is its own local translate, rotate and scale.
 */
struct CameraView
{
    std::string view = "persp";
    glm::dvec3 target{0.0};
    double distance = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
    double orthoWidth = 0.0;
    bool node = false;
    glm::dvec3 translate{0.0};
    glm::dvec3 rotate{0.0};
    glm::dvec3 scale{1.0};

    bool same(const CameraView& o) const
    {
        auto eq = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)}); };
        auto eq3 = [&](const glm::dvec3& a, const glm::dvec3& b) { return eq(a.x, b.x) && eq(a.y, b.y) && eq(a.z, b.z); };
        return view == o.view && node == o.node && eq3(target, o.target) && eq(distance, o.distance) && eq(yaw, o.yaw) &&
               eq(pitch, o.pitch) && eq(orthoWidth, o.orthoWidth) && eq3(translate, o.translate) && eq3(rotate, o.rotate) &&
               eq3(scale, o.scale);
    }
};

/**
 * Maya's camera undo and redo ([ and ]): one camera's moves in one viewport, kept apart from the scene's undo stack. Each
 * change records the view before it. Changes with the same non-zero gesture id inside the coalesce window merge into
 * one step, so a burst of wheel clicks undoes at once; a drag records once, on release.
 */
class CameraHistory
{
public:
    explicit CameraHistory(size_t limit = 100, double coalesceMs = 600.0) : mLimit(limit), mCoalesceMs(coalesceMs) {}

    void record(const CameraView& before, const CameraView& after, uint64_t gesture, double nowMs)
    {
        if (before.same(after))
            return;
        mRedo.clear();
        bool merge = gesture != 0 && gesture == mLastGesture && nowMs - mLastMs <= mCoalesceMs && !mUndo.empty();
        mLastGesture = gesture;
        mLastMs = nowMs;
        if (merge)
            return;
        mUndo.push_back(before);
        if (mUndo.size() > mLimit)
            mUndo.pop_front();
    }

    /** The view to go back to, given the one showing now; nullopt when there is nothing to undo. */
    std::optional<CameraView> undo(const CameraView& current) { return step(mUndo, mRedo, current); }
    std::optional<CameraView> redo(const CameraView& current) { return step(mRedo, mUndo, current); }

    size_t undoCount() const { return mUndo.size(); }
    size_t redoCount() const { return mRedo.size(); }

private:
    std::optional<CameraView> step(std::deque<CameraView>& from, std::deque<CameraView>& to, const CameraView& current)
    {
        if (from.empty())
            return std::nullopt;
        CameraView v = from.back();
        from.pop_back();
        to.push_back(current);
        mLastGesture = 0;
        return v;
    }

    std::deque<CameraView> mUndo;
    std::deque<CameraView> mRedo;
    size_t mLimit;
    double mCoalesceMs;
    uint64_t mLastGesture = 0;
    double mLastMs = 0.0;
};
} // namespace lila::ui
