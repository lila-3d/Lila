#pragma once

#include "core/Export.h"

#include "render/Renderer.h"
#include "scene/ComponentPick.h"
#include "scene/Isolation.h"
#include "scene/Plug.h"
#include "ui/Camera.h"
#include "ui/CameraHistory.h"
#include "ui/Gizmo.h"
#include "ui/Overlay.h"
#include "ui/PaintSelect.h"
#include "ui/SelectionHandlers.h"
#include "ui/Tools.h"
#include "ui/ViewportState.h"

#include <QWidget>

class QMimeData;
class QTimer;

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>
#include <string>
#include <vector>

namespace lila::ui
{
/** Native child window the render thread presents into. Owns the camera and Maya-style navigation. */
class LILA_UI_API ViewportWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override;

    QPaintEngine* paintEngine() const override { return nullptr; }

    Camera& camera() { return mCamera; }
    const Camera& camera() const { return mCamera; }
    void requestFrame();
    void frameSelection(bool all);
    /**
     * "persp", "top", "bottom", "front", "back", "left" or "right", or a camera node's name to look through it. The
     * perspective camera is kept while away. Unknown names are ignored.
     */
    void setView(const std::string& name);
    /**
     * Maya's Look Through: the view takes the camera node's pose and lens and its name, and navigating moves the node
     * (on this view's camera undo, not the scene's) unless it is locked. The view goes back to where it was if the camera is deleted.
     */
    void lookThrough(scene::NodeId camera);
    /** The camera node this view looks through, or kNoNode. */
    scene::NodeId lookThroughCamera() const { return mLookThrough; }
    /** Back to the view shown before looking through a camera node. */
    void stopLookingThrough() { setView(mReturnView); }
    /** The built-in view shown (the one to return to while looking through) and its cameras, as scenes save them. */
    nlohmann::json cameraState() const;
    /** Takes what cameraState wrote: shows that view with those poses. Leaves any look-through first. */
    void restoreCameraState(const nlohmann::json& state);
    /**
     * Shows only the current selection in this view; again to show everything. Selected components of a mesh show only
     * the faces they touch. Hidden things can't be picked, hovered or snapped to here.
     */
    void toggleIsolate();
    /** Adds the selection (objects or components) to this view's isolation, starting one if none is on. */
    void addToIsolation();
    void removeFromIsolation();
    bool isolating() const { return mIsolation.active(); }
    const scene::isolation::Set& isolation() const { return mIsolation; }
    void setIsolation(scene::isolation::Set set);
    /** The gizmo in use for the current tool, or null when none applies. */
    const Gizmo* gizmo() const { return const_cast<ViewportWidget*>(this)->currentGizmo(); }
    /** Steps this view's camera back or forward through its own history ([ and ]); the scene's undo is untouched. */
    void cameraUndo();
    void cameraRedo();
    const std::string& viewName() const { return mViewName; }
    /** Which viewport panel this is, in creation order; keys its heads-up display prefs. */
    int hudPanel() const { return mHudPanel; }

signals:
    /** The view (camera) or isolate state changed. */
    void viewStateChanged();

protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    enum class Nav
    {
        None,
        Tumble,
        Track,
        Dolly
    };

    void submit();
    ViewInfo viewInfo() const;
    /**
     * The gizmo that takes input for the current tool: the one dragging, else the active provider's, else the default
     * provider's when the active one doesn't support the mode. A viewport tool's name is the mode when a gizmo
     * supports it; otherwise the transform tool's is, so a press a tool declines still reaches the gizmo.
     */
    Gizmo* currentGizmo();
    /** The gizmo to draw: as currentGizmo(), but none while a viewport tool no gizmo supports is active. */
    Gizmo* drawnGizmo();
    /** Makes the held instances match gizmos::activeId(); drops any whose provider is gone, cancelling its drag. */
    void syncGizmos();
    /** What a click at px hits for the selection handler: the object, plus the face and point on a mesh. */
    selectionHandlers::Hit selectionHit(QPointF px);
    /** The active selection handler's hover highlight under the cursor, recomputed on preselect. */
    void updateHandlerHover(QPointF at);
    void drawHandlerHover(render::FrameState& s);
    /** This view for callbacks that shouldn't see the widget, with the cursor at cursor. */
    ViewportState state(QPointF cursor);
    /** Wraps the cursor to the opposite edge during manipulator drags, so a drag never runs out of screen. */
    void wrapCursor(QMouseEvent* e);
    void updateCursor();
    /** Widget-space pixel of a world point; nullopt behind the camera. */
    std::optional<QPointF> project(const glm::dvec3& world) const;
    /** Names under a click point (nearest only) or inside a box (the lasso instead while mLassoPick is set). */
    std::vector<std::string> hitTest(QPointF a, QPointF b);
    /** A drag selection's test for a projected point: inside the lasso being picked, else inside box. */
    bool inSelectRegion(const QRectF& box, QPointF p) const;
    /** Selects with the lasso drawn in mLasso, or clicks at the press when it is too small to enclose anything. */
    void pickLasso(Qt::KeyboardModifiers mods);
    /** Whether what a Tab press at the point would land on is selected; nullopt over nothing. */
    std::optional<bool> selectedUnder(QPointF at);
    /** Refreshes mTabUnder for the cursor badge while Tab is held. */
    void updateTabHover(QPointF at);
    /** The skinned mesh under the cursor, or empty. */
    std::string skinnedMeshAt(QPointF at);
    void pick(QPointF a, QPointF b, Qt::KeyboardModifiers mods);
    /** Component-mode picking on the hilited meshes. */
    void pickComponents(QPointF a, QPointF b, Qt::KeyboardModifiers mods);
    /** Double-click in a component mode: the loop through what is under the cursor, or with Shift the path to the last click. */
    bool selectLoopAt(QPointF at, Qt::KeyboardModifiers mods);
    void drawComponents(render::FrameState& s);
    /** What a click at the point would select in the current component mode: the nearest visible component. */
    scene::componentPick::Hover componentAt(QPointF at);
    /** Re-picks under the cursor for the preselection highlight; bursts of moves coalesce into one pick. */
    void queuePreselect(QPointF at);
    void updatePreselect();
    void clearPreselect();
    void drawPreselect(render::FrameState& s);
    QSize pixelSize() const;
    double aspect() const;
    CameraView cameraView() const;
    void applyCameraView(const CameraView& v);
    void recordCamera(const CameraView& before, uint64_t gesture = 0);
    /** Copies the look-through camera's pose and lens onto mCamera; drops back to the old view if it is gone. */
    void syncLookThrough();
    /**
     * Navigation edits on the look-through camera, kept off the scene's undo stack like any camera move: begin saves
     * the node, preview poses it, commit marks the scene changed and keys it when Auto Key is on. Begin returns false
     * (with a message) when the camera is locked.
     */
    bool beginCameraEdit();
    void previewCameraEdit();
    void commitCameraEdit();
    /** The first camera node named in a drag from the outliner, or kNoNode. */
    scene::NodeId droppedCamera(const QMimeData* mime) const;

    render::ViewportId mId = 0;
    Camera mCamera;
    Camera mPerspCamera;
    std::string mViewName = "persp";
    scene::NodeId mLookThrough = scene::kNoNode;
    std::string mReturnView = "persp"; // the view to go back to when the look-through camera goes away
    struct CameraEdit
    {
        scene::NodeId id = scene::kNoNode;
        std::vector<std::pair<int, scene::PlugValue>> saved;
    };
    std::optional<CameraEdit> mCameraEdit;
    QTimer* mWheelCommit = nullptr; // a run of wheel clicks on a camera node commits once it stops
    double mLockedWarnMs = 0.0;
    scene::isolation::Set mIsolation;
    void drawHud(OverlayPainter& p);
    void drawViewIndicator(OverlayPainter& p);
    /** Resolution and film gates with the mask outside them, Maya style. */
    void drawGate(OverlayPainter& p);
    /** Copies the scene's settings for this view's camera (film fit, overscan, gate) onto mCamera. */
    void syncCameraSettings();
    Nav mNav = Nav::None;
    /** Camera undo per camera: built-in views by name, camera nodes by id, so [ steps through the shown camera's moves. */
    std::map<std::string, CameraHistory> mCameraHistories;
    std::string historyKey() const;
    CameraView mNavStart;
    bool mCameraRestoring = false;
    QPointF mLastPos;
    QPointF mPressPos;
    bool mSelecting = false;
    bool mMarquee = false;
    /** Tab held over this view with a select or transform tool: LMB drags paint the selection (mTabStroke). */
    bool mTabHeld = false;
    paintSelect::Stroke mTabStroke;
    std::optional<bool> mTabUnder; // selectedUnder at the cursor while Tab is held
    /** The drag under way draws a lasso (mLasso, widget pixels) rather than a marquee. */
    bool mLassoDrag = false;
    std::vector<glm::dvec2> mLasso;
    bool mLassoPick = false; // hitTest and pickComponents test against mLasso
    bool mSwallowRelease = false; // the release ending a double-click that already selected
    /** The last two components clicked, newest first: Shift+double-click selects the path between them. */
    scene::componentPick::Hover mClicked[2];
    bool mFrameQueued = false;
    int mGraphListener = 0;
    int mPrefListener = 0;
    int mToolListener = 0;
    int mComponentListener = 0;
    int mPlaybackListener = 0;
    int mViewportToolListener = 0;
    int mRenderSettingsListener = 0;
    struct GizmoSlot
    {
        std::string id;
        std::unique_ptr<Gizmo> gizmo;
    };
    GizmoSlot mGizmo;        // the active provider's
    GizmoSlot mDefaultGizmo; // gizmos::kDefault's, when it isn't the active one
    Gizmo* mGizmoDrag = nullptr; // took the press; null when no gizmo drag is under way
    int mGizmoListener = 0;
    int mHandlerListener = 0;
    int mArrangeListener = 0;
    std::optional<selectionHandlers::Highlight> mHandlerHover;
    bool mChannelDrag = false; // middle drag driving the channel box's highlighted channels
    bool mToolDrag = false;
    bool mSoftDrag = false;
    double mSoftStartRadius = 0.0;
    scene::componentPick::Hover mPreselect;
    std::optional<QPointF> mPreselectAt; // cursor awaiting a pick
    bool mPreselectQueued = false;
    double mPreselectCostMs = 0.0;
    int mHudPanel = 0;
    std::vector<float> mBackdropBright; // OverlayPainter::classifyBackdrop state, so overlay colors switch with hysteresis
    std::deque<std::pair<double, uint64_t>> mFrameSamples; // (ms, frames the renderer drew) over the last second, for the HUD
    bool mHudRefreshQueued = false;
};
} // namespace lila::ui
