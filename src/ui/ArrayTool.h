#pragma once

#include "core/Export.h"

#include "geo/ArrayTransform.h"
#include "scene/Plug.h"
#include "ui/ViewportTool.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::ui
{
/** The array the tool is building, in the terms the array command takes: offset, axis and spacing in the array's space. */
struct ArrayToolSettings
{
    geo::ArrayMode mode = geo::ArrayMode::Linear;
    int count = 5;
    glm::dvec3 offset{100.0, 0.0, 0.0};
    int axis = 1;
    double angle = 360.0;
    double rise = 0.0;
    glm::ivec3 gridCount{3, 1, 3};
    glm::dvec3 spacing{100.0};
    /** World point the copies turn about: the array's pivot. */
    glm::dvec3 center{0.0};
};

/**
 * The interactive Array tool. With objects selected: drag a direction for a row, or click a centre for a ring (3 makes a
 * grid of the drag instead), then drag the handles for spacing, count, angle and rise; 1, 2, 3 switch between linear,
 * radial and grid, + and - change the count. Enter makes the live array (one array command, adjustable afterwards), Esc
 * leaves without changing anything. press/drag/release take widget pixels, so tests drive it headless.
 */
class LILA_UI_API ArrayTool final : public ViewportTool
{
public:
    enum class Handle
    {
        None,
        Spacing,  // linear step, or grid spacing along the first in-plane axis
        Count,    // linear count, or the grid count along the first in-plane axis
        SpacingB, // grid spacing along the second in-plane axis
        CountB,   // grid count along the second in-plane axis
        Angle,    // radial: the angle the copies span
        Step,     // radial: the first copy, dragged round to set the count
        Rise,     // radial: height of the last copy along the axis
    };

    const char* name() const override { return "Array"; }

    /** Takes the selected objects as the sources. False when nothing is selected. */
    bool begin();
    const std::vector<scene::NodeId>& sources() const { return mSources; }
    /** True once a direction or centre is placed and the handles are live. */
    bool placed() const { return mPlaced; }

    const ArrayToolSettings& settings() const { return mSettings; }
    void setMode(geo::ArrayMode mode);
    void setCount(int count);

    bool press(QPointF px, const ViewInfo& view);
    void drag(QPointF px, const ViewInfo& view);
    void release(QPointF px, const ViewInfo& view);
    /** A key the tool takes: 1/2/3, +/-, Up/Down, Enter, Esc. */
    bool key(int key);

    /** The array call Enter would run. */
    nlohmann::json call() const;
    /** Runs it, as a UI call so the adjust popup offers it. Returns the array's path, empty when it failed. */
    std::string finish();
    /** Where a handle sits on screen, nullopt when it is not shown in the current mode. */
    std::optional<QPointF> handlePos(Handle h, const ViewInfo& view) const;
    /** World transforms of every copy, the originals (identity) first, as the array will make them. */
    std::vector<glm::dmat4> copyTransforms() const;

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    bool mousePress(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override;
    bool keyPress(QKeyEvent* e, const ViewInfo& view) override;
    bool hotkey(const QKeyEvent& e, std::string_view action) override;
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override;

private:
    enum class Drag
    {
        None,
        Place,
        Handle,
    };

    glm::dmat3 axes() const;
    glm::dmat4 arrayWorld() const;
    glm::dvec3 worldAxis(int local) const;
    /** The pivot of copy `cell` (grid) or copy k (linear, radial), world space. */
    glm::dvec3 copyPivot(int k) const;
    glm::dvec3 gridPivot(int a, int b) const;
    /** The array's in-plane axes for a grid: the two local axes other than the one facing the view. */
    std::pair<int, int> planeAxes() const;
    double ringRadius() const;
    glm::dvec3 ringStart() const;
    std::optional<glm::dvec3> planeHit(QPointF px, const ViewInfo& view) const;
    void choosePlane(QPointF px, const ViewInfo& view);
    void placeFrom(const glm::dvec3& from, const glm::dvec3& to);
    void dragHandle(QPointF px, const ViewInfo& view);
    Handle handleAt(QPointF px, const ViewInfo& view) const;
    void exit();

    std::vector<scene::NodeId> mSources;
    glm::dmat4 mParentWorld{1.0};
    glm::dvec3 mPivot{0.0};
    glm::dvec3 mNormal{0.0, 1.0, 0.0};
    ArrayToolSettings mSettings;
    bool mPlaced = false;
    bool mRadialSet = false;
    Drag mDrag = Drag::None;
    Handle mHandle = Handle::None;
    Handle mHover = Handle::None;
    QPointF mDownPx;
    glm::dvec3 mDownWorld{0.0};
    bool mMoved = false;
    double mDragAngle = 0.0;
    std::string mHoverView;
};

LILA_UI_API void registerArrayToolActions();
} // namespace lila::ui
