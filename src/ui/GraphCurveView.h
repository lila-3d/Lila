#pragma once

#include "ui/GraphEditorCommon.h"
#include "ui/GraphMotion.h"

#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace lila::ui
{
class GraphEditor;

namespace graphed
{
enum class ViewMode : uint8_t
{
    Absolute,
    Stacked,
    Normalized,
};

enum class Tool : uint8_t
{
    Move,
    Rotate, // turns each selected key's tangents about the key
    Scale,
};

enum class Pivot : uint8_t
{
    Selection,
    Click,
    CurrentFrame,
    FirstKey,
    LastKey,
};

enum class Quantity : uint8_t
{
    Channels, // each shown curve's rate of change
    WorldLinear, // cm/s of the selected transforms
    WorldAngular, // degrees/s of the selected transforms
};

enum class RotationDisplay : uint8_t
{
    Euler,
    Quaternion,
    Both,
};

/** A curve as it was when snapshotted, drawn behind the live one and swapped back in on request. */
struct Buffer
{
    bool rotation = false;
    anim::Curve curve;
    anim::RotationCurve rotationCurve;
};

/** What the view shows and how its tools behave; the editor's toolbar edits it and it persists in prefs. */
struct ViewSettings
{
    ViewMode view = ViewMode::Absolute;
    Tool tool = Tool::Move;
    Pivot pivot = Pivot::Selection;
    bool falloff = false;
    double falloffFrames = 6.0;
    bool speedStrip = false;
    bool accelStrip = false;
    Quantity quantity = Quantity::Channels;
    double targetSpeed = 0.0; // 0 = none
    RotationDisplay rotation = RotationDisplay::Euler;
    bool showBuffer = true;
};

class CurveView : public QWidget
{
public:
    explicit CurveView(GraphEditor* editor);
    ~CurveView() override;

    ViewSettings settings;
    std::vector<Channel> channels;
    /** Keyed by channel id; a rotation channel's holds its derived keys. */
    std::map<NodeId, anim::Curve> curves;
    /** Rotation curves by their own node id, loop applied. */
    std::map<NodeId, anim::RotationCurve> rotations;
    /** Channels whose curve has a loop key. */
    std::set<NodeId> loopCurves;
    /** By the curve node's own id. */
    std::map<NodeId, Buffer> buffers;
    std::vector<KeySel> sel;
    NodeId activeCurve = scene::kNoNode;

    /** What commands call a channel: its curve node, or for a rotation channel its plug. Empty when read only. */
    std::string commandTarget(NodeId id) const;
    const Channel* channelOf(NodeId curve) const;
    bool isSelected(NodeId curve, double t) const;
    /** Curves edits apply to: those with selected keys, else the clicked one, else every shown one. */
    std::vector<NodeId> targetCurves() const;
    void dropMissing();
    void frame(bool selectedOnly);
    /** Recomputes each channel's display map for the view mode. */
    void updateMaps();
    /** The selection as a keys argument: [{curve, times}], read-only channels left out. */
    nlohmann::json keysArgument() const;
    /** The curves edits apply to as command objects (curve nodes, or plugs for rotation channels). */
    nlohmann::json targetNames() const;
    void loopKeys(bool on);
    void invalidateMotion() { mWorld.invalidate(); }

    /** The tween slider: a live preview while dragged, one blendKeys on release. */
    void beginTween(anim::BlendMode mode);
    void setTween(double amount);
    void endTween();

    /**
     * W, E or R pressed: the tool turns on, and while the key stays down, moving the mouse over the view drags the
     * selected keys with it. A tap (released quickly, or before the mouse travels) only switches the tool.
     */
    void toolKeyDown(Tool tool, bool holdToDrag);
    void toolKeyUp();
    /** B held: LMB drag sets the falloff radius, MMB drag sets it from zero. Returns whether the radius was dragged. */
    void falloffKeyDown();
    bool falloffKeyUp();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    enum class Drag : uint8_t
    {
        None,
        Pan,
        Zoom,
        Box,
        Move,
        Scale,
        Region,
        Tangent,
        TangentRotate,
        Time,
    };

    double stripHeight() const;
    double plotBottom() const { return height() - stripHeight(); }
    double xOf(double t) const { return (t - mT0) * mSx; }
    double yOf(double display) const { return plotBottom() - (display - mV0) * mSy; }
    double timeAt(double x) const { return mT0 + x / mSx; }
    double displayAt(double y) const { return mV0 + (plotBottom() - y) / mSy; }
    double yOfKey(const Channel& ch, double v) const { return yOf(ch.display(v)); }

    std::vector<NodeId> activeSet() const;
    std::vector<double> selectedTimes(NodeId id) const;
    /** The channel's value over time as drawn, with any edit in progress and the curve's modifiers applied. */
    std::function<double(double)> sampler(NodeId id, const anim::Curve& c) const;
    double defaultFor(const Channel& ch) const;

    /** The move or scale in progress for one channel. */
    anim::KeyTransform transformFor(NodeId id) const;
    bool transforming() const { return (mDrag == Drag::Move || mDrag == Drag::Scale || mDrag == Drag::Region) && mMoved; }
    /** During a key drag the selection still holds the old times; maps a previewed key back. */
    double keyTimeBeforeDrag(NodeId curve, double t) const;
    anim::RotationCurve previewRotation(NodeId realId) const;
    /** The channel's keys with every edit in progress applied. */
    anim::Curve preview(NodeId id) const;
    /** Weight (0..1) the falloff gives an unselected key during a drag; 0 when falloff is off. */
    double falloffWeightAt(NodeId id, double t) const;

    QPointF handlePoint(const anim::Curve& c, size_t i, bool out, const Channel& ch) const;
    void drawCurve(QPainter& p, const anim::Curve& c, const Channel& ch, bool active, const std::function<double(double)>& eval,
                   const QColor* ghost = nullptr);
    void drawGrid(QPainter& p);
    void drawStrips(QPainter& p, const std::map<NodeId, anim::Curve>& shown);
    /** The selected keys' bounds on screen; empty when fewer than two keys are selected. */
    QRectF regionRect() const;
    /** 0..7 (corners then edges, clockwise from top left) when pos is on a region handle, else -1. */
    int hitRegionHandle(QPointF pos) const;

    std::optional<KeySel> hitKey(QPointF pos) const;
    bool hitHandle(QPointF pos);
    NodeId hitCurve(QPointF pos) const;
    double angleFor(QPointF pos, double* weight) const;
    double screenAngleOf(const Channel& ch, double slope) const;
    double slopeOfScreenAngle(const Channel& ch, double degrees) const;
    void scrubTo(double x);

    void beginTransform(Drag kind);
    /** Pivots for a scale: the time pivot and each channel's value pivot, from the pivot setting. */
    void setPivots(QPointF press);
    void finishBox();
    void commitTransform();
    void commitTangent();
    void commitTangentRotate();
    void selectionChanged();
    void showMenu(QPoint at);

    GraphEditor* mEditor;
    double mT0 = -0.2; // time at the left edge
    double mV0 = -5.0; // display value at the bottom of the plot
    double mSx = 200.0; // pixels per second
    double mSy = 10.0; // pixels per display unit
    bool mFramed = false;
    bool mAxisColors = true;
    int mPrefListener = 0;

    Drag mDrag = Drag::None;
    QPointF mPress;
    QPointF mLast;
    bool mMoved = false;
    int mBoxMode = 0; // 1 add, -1 remove, 0 replace
    NodeId mPressCurve = scene::kNoNode;
    double mZoomAnchorT = 0.0, mZoomAnchorV = 0.0, mZoomSx = 1.0, mZoomSy = 1.0;

    // Move, scale and region drags: time parts shared, value offset in display units, value pivots per channel.
    anim::KeyTransform mXform;
    double mDisplayOffset = 0.0;
    std::map<NodeId, double> mValuePivots;
    int mRegionHandle = -1;
    QRectF mRegionStart;

    KeySel mTangent;
    int mTangentSide = 1;
    double mTangentAngle = std::nan("");
    double mTangentWeight = std::nan("");
    double mRotateDelta = 0.0; // screen degrees, the rotate tool

    bool mTweening = false;
    anim::BlendMode mTweenMode = anim::BlendMode::Neighbour;
    double mTweenAmount = 0.0;

    WorldMotion mWorld;

    /** Mirrors the drag in progress into the scene so the viewport follows it; cleared before every commit. */
    void pushPreviews();
    void dropPreviews();
    void endHold();

    struct Hold
    {
        bool active = false;
        bool dragging = false;
        Drag kind = Drag::Move;
        QPointF anchor;
        qint64 since = 0; // ms
    } mHold;
    bool mRadiusKey = false;
    bool mRadiusUsed = false;
    bool mRadiusDrag = false;
    double mRadiusStart = 0.0;
    QPointF mRadiusPress;
    QPointF mCursor;
    bool mHoldPose = false; // the ruler's MMB scrub

    friend class lila::ui::GraphEditor;
};
} // namespace graphed
} // namespace lila::ui
