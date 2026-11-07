#pragma once

#include "ui/Theme.h"
#include "ui/UvEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Topology.h"
#include "scene/ComponentPick.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/CursorBadges.h"
#include "ui/Deletion.h"
#include "ui/FixedRowLayout.h"
#include "ui/Icons.h"
#include "ui/MarkingMenu.h"
#include "ui/MenuButtons.h"
#include "ui/PaintSelect.h"
#include "ui/TabSelectLogic.h"
#include "ui/UvLive.h"
#include "ui/UvTexturePaint.h"
#include "ui/UvTools.h"
#include "ui/UvPackPanel.h"
#include "ui/UvProjectTool.h"
#include "ui/ViewportTool.h"
#include "uv/LiveSession.h"
#include "uv/SymmetryUv.h"
#include "uv/Uv.h"
#include "uv/UvCommands.h"
#include "uv/UvEdit.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_set>

namespace lila::ui
{
using json = nlohmann::json;
using scene::ComponentMode;
using geo::Index;

/** The 2D uv canvas. */
class UvView : public QWidget
{
public:
    enum class Tool
    {
        Select,
        Move,
        Rotate,
        Scale,
        Brush
    };
    enum class Display
    {
        Plain,
        Checker,
        Distortion,
        Stretch,
        Islands
    };

    static QColor gray(int v, int a = 255);
    static json safeRun(const char* name, const json& call, CallSource src);
    static std::vector<scene::NodeId> shownMeshes();

    explicit UvView(QWidget* parent);

    UvTexturePaint& texturePaint() { return mTexPaint; }

    ~UvView() override;

    /** The canvas a borrowed uvTools tool paints on. */
    uvTools::Canvas canvas() const;

    void setTool(Tool t);
    Tool tool() const { return mTool; }
    void setDisplay(Display d);
    void setShowUdim(bool on);
    void setShowStacks(bool on);
    /** Shows the shown meshes' islands as if packed together into 0-1; view and select only, the uvs are untouched. */
    void setPackPreview(bool on);
    bool packPreview() const { return mPackPreview; }
    void invalidate() { update(); }
    std::function<void(Tool)> onToolChanged;

    void frameAll();

    void frameSelection();

protected:
    void resizeEvent(QResizeEvent*) override;

    void paintEvent(QPaintEvent*) override;

    void wheelEvent(QWheelEvent* e) override;

    void mousePressEvent(QMouseEvent* e) override;

    void mouseMoveEvent(QMouseEvent* e) override;

    void leaveEvent(QEvent* e) override;

    void mouseReleaseEvent(QMouseEvent* e) override;

    void mouseDoubleClickEvent(QMouseEvent* e) override;

    bool event(QEvent* e) override;

    void keyPressEvent(QKeyEvent* e) override;

    void keyReleaseEvent(QKeyEvent* e) override;

private:
    struct Segment
    {
        uint32_t a, b; // entries
        Index edge;
        bool seam;
    };

    /** One mesh's uvs, flattened per face corner. Rebuilt when the mesh or uv set changes. */
    struct MeshUv
    {
        scene::NodeId node = scene::kNoNode;
        std::string name;
        scene::MeshPtr mesh;
        std::string set;
        std::vector<Index> only;         // the faces shown, sorted; empty shows them all
        std::vector<Index> faces;
        std::vector<uint32_t> faceStart; // per face, plus one
        std::vector<Index> corner;       // per entry
        std::vector<glm::dvec2> uv;      // per entry
        std::vector<uint32_t> entryOf;   // per corner capacity
        std::vector<Segment> segments;
        std::vector<float> distortion; // per face (local)
        bool distortionValid = false;
        std::vector<uint8_t> pinned;   // per entry
        std::vector<int> island;       // per face, for the stretch and island displays (empty until needed)
        std::vector<double> area3;     // per face
        std::vector<double> islandRatio;
        std::vector<int> pickIsland; // per face, its island as island select sees it (empty until needed)
        uint64_t liveGen = 0, liveEpoch = 0;
        // Symmetry caches, built on demand for the axis they were made for (-2: not yet).
        mutable int symAxis = -2;
        mutable std::shared_ptr<const geo::SymmetryMap> sym;
        mutable std::shared_ptr<const uv::UvMirror> mirror;
    };

    enum class Nav
    {
        None,
        Pan,
        Zoom
    };
    enum class Handle
    {
        None,
        Free,
        U,
        V,
        Ring,
    };

    // View mapping: v points up.
    QPointF toScreen(const glm::dvec2& uv) const { return QPointF(mPan.x + uv.x * mZoom, mPan.y - uv.y * mZoom); }
    glm::dvec2 toUv(const QPointF& s) const { return {(s.x() - mPan.x) / mZoom, (mPan.y - s.y()) / mZoom}; }
    UvTexturePaint::ToUv uvOf() const;

    void frame(glm::dvec2 lo, glm::dvec2 hi);
    /** Frames every shown mesh's uvs and the 0-1 square, from what is already synced. */
    void frameShown();
    /** Whether any shown uv lies inside the view. */
    bool anyShownInView() const;

    void zoomAbout(QPointF at, double f);

    void changeTool(Tool t);

    void sync();

    /** Shows the live solver's latest uvs and pins in place of the mesh's while a session matches it. */
    void applyLive();

    /** Packs copies of the shown meshes together with the last uvPack options and shows their uvs in place of the real ones. */
    void packShown();

    /** In face mode with faces selected, the selected faces of each mesh (none for a mesh without any); empty otherwise. */
    static std::vector<std::vector<Index>> shownFaces(const std::vector<scene::NodeId>& meshes);
    static MeshUv build(scene::NodeId id, const std::string& name, scene::MeshPtr mesh, const std::string& set,
                        std::vector<Index> only = {});

    /** Entries the selection moves: wedges of selected faces, or corners of selected points and edge ends. */
    std::vector<uint8_t> selectedEntries(const MeshUv& mu) const;

    /** The mesh's mirror pairs while its modeling symmetry is on, else null. */
    const geo::SymmetryMap* symmetryOf(const MeshUv& mu) const;

    /** Its uv corner pairs while symmetry is on, else null. */
    const uv::UvMirror* mirrorOf(const MeshUv& mu) const;

    /** ids plus their mirror images while the mesh's symmetry is on. */
    std::vector<Index> withMirror(scene::NodeId node, ComponentMode mode, const std::vector<Index>& ids) const;

    bool hasSelection() const;

    /** With drivingSide, a symmetric mesh counts only the side its selection mostly is on (the side edits follow). */
    bool selectionBounds(glm::dvec2& lo, glm::dvec2& hi, bool drivingSide = false) const;

    /** Where an entry is drawn, with the live drag applied (and mirrored onto its partner with symmetry on). */
    glm::dvec2 drawnUv(size_t mesh, size_t entry) const;

    glm::dvec2 dragTransform(const glm::dvec2& p) const;

    void drawBackground(QPainter& p);

    void drawFaces(QPainter& p);

    void drawEdges(QPainter& p);

    void drawPoints(QPainter& p);

    QPointF pivotScreen() const { return toScreen(mDrag != Handle::None ? mPivot + mDragMove : currentPivot()); }

    glm::dvec2 currentPivot() const;

    void drawManipulator(QPainter& p);

    /** A "x3" badge on every stack of identical islands, so stacking from uvPack stays visible. */
    void drawStacks(QPainter& p);

    void drawHud(QPainter& p);

    Handle handleAt(QPointF s) const;

    void beginDrag(Handle h);

    /** Per entry, the entry whose drag it follows and through what map, as uvTransform will mirror it; empty without symmetry. */
    std::vector<std::pair<uint32_t, glm::dmat2>> dragFollow(const MeshUv& mu, const std::vector<uint8_t>& mask) const;

    void updateDrag(Qt::KeyboardModifiers mods);

    void commitDrag();

    struct Hit
    {
        size_t mesh = 0;
        Index id = geo::kInvalid;
    };

    static bool insidePoly(const std::vector<QPointF>& poly, QPointF p);

    static double segDistance(QPointF p, QPointF a, QPointF b);

    ComponentMode pickMode() const;

    Hit faceAt(QPointF s) const;

    /** What a click at s would select. */
    Hit hitAt(QPointF s, ComponentMode mode) const;

    scene::componentPick::Hover hoverAt(QPointF s) const;

    void setPreselect(const scene::componentPick::Hover& h);

    /** The hovered component, light gray and heavier than the selection's white. */
    void drawPreselect(QPainter& p);

    /** Per shown face, the index of its island under islandOptions; built once per mesh. */
    const std::vector<int>& pickIslands(size_t mi);

    void pickAt(QPointF s, Qt::KeyboardModifiers mods);

    void pickRect(QRectF r, Qt::KeyboardModifiers mods);

    static uv::ChartOptions islandOptions(const MeshUv& mu);

    void selectIslandAt(QPointF s, Qt::KeyboardModifiers mods);

    void selectLoopAt(QPointF s, Qt::KeyboardModifiers mods);

    /** Tools that own the left button keep it while Tab is held. */
    bool tabSelectable() const { return mTool != Tool::Brush && !uvTools::active() && !mTexPaint.painting(); }

    /** Whether the component a click at s would pick is selected; nullopt over nothing. */
    std::optional<bool> selectedUnder(QPointF s) const;

    /** Components of the pick mode within radiusPx of screen point s; faces count when s is inside or near an edge. */
    std::map<scene::NodeId, scene::components::Ids> componentsUnder(QPointF s, double radiusPx);

    /** The viewport's Tab select or deselect badge beside the cursor, and its help line. */
    void drawTabBadges(QPainter& p);

    /** Brushes the Tab stroke along the segment from a to b (widget pixels). */
    void tabPaint(QPointF a, QPointF b);

    /** Same modifiers as the viewport: shift toggles, ctrl removes, both add. */
    void apply(ComponentMode mode, const std::map<scene::NodeId, std::vector<Index>>& hits, Qt::KeyboardModifiers mods);

    // Live editing: pins, the stretch display, drags that steer the solver, brushes and sewing by dragging.

    void drawPins(QPainter& p);

    /** Light = stretched (bigger in uv than on the surface), dark = compressed, relative to the face's island. */
    static QColor stretchColor(double v, bool twoColour);

    /** Per face: log2 of its uv-to-surface area ratio over its island's, clamped to -1..1; folded faces read -1. */
    std::vector<double> faceStretch(size_t mi);

    /** Pins or unpins the uv vertex nearest the cursor (P, or a click with the pin brushes). */
    void togglePinAt(QPointF s);

    struct SewDrag
    {
        bool armed = false;
        bool dragging = false;
        size_t mesh = 0;
        size_t segment = 0;
    };

    /** The other sides of a split edge: segments of the same mesh edge drawn elsewhere. */
    std::vector<size_t> partners(size_t mi, size_t si) const;

    /** In edge mode a press on a border edge that has a partner arms sewing by dragging it onto the partner. */
    bool armSewDrag(QPointF s);

    QPointF segmentMid(size_t mi, size_t si) const;

    void drawSewDrag(QPainter& p);

    /** Released over the partner side: Move and Sew that edge (the smaller island travels). */
    void finishSewDrag(QPointF s);

    struct LiveDrag
    {
        bool armed = false;
        bool started = false;
        size_t mesh = 0;
        ComponentMode mode = ComponentMode::Vertex;
        Index id = geo::kInvalid;
        bool island = false;
        scene::NodeId node = scene::kNoNode;
        glm::dvec2 pressUv{0.0};
        std::vector<std::tuple<int, Index, glm::dvec2>> wedges; // island, vertex, start
        std::vector<int> islands;
    };

    /** In live mode a press on a uv component arms a drag of it; it starts once the cursor moves. */
    bool armLiveDrag(QPointF s);

    /** The grabbed uvs become soft pins that follow the cursor; the islands relax around them. */
    void startLiveDrag();

    void updateLiveDrag();

    /** Release lets the soft pins go (Ctrl keeps them as pins where they are). */
    void endLiveDrag(Qt::KeyboardModifiers mods);

    enum class Adjust
    {
        None,
        Radius,
        Strength
    };

    struct BrushStroke
    {
        bool active = false;
        bool started = false;
        std::vector<scene::NodeId> nodes;
        QPointF last;
        glm::dvec2 lastUv{0.0}, fromUv{0.0};
        uv::LiveBrush brush = uv::LiveBrush::Relax;
        bool invert = false;
    };

    /** B or F held + drag sizes the brush, M or Shift+F + drag sets its strength, [ ] resize; as in the sculpt tool. */
    bool brushKey(QKeyEvent* e, bool down);

    void brushPress(QMouseEvent* e);

    void brushMove(QMouseEvent* e);

    void dabAt(QPointF px, bool first);

    void brushRelease(QMouseEvent* e);

    void drawBrush(QPainter& p);

    SewDrag mSew;
    LiveDrag mLiveDrag;
    BrushStroke mStroke;
    Adjust mArmed = Adjust::None;
    Adjust mAdjusting = Adjust::None;
    int mArmedKey = 0;
    bool mArmedUsed = false;
    QPointF mAdjustStart;
    double mAdjustFrom = 0.0;

    std::vector<MeshUv> mMeshes;
    bool mFramed = false;
    /** The bounds last framed, kept framed through resizes until the view is panned or zoomed. */
    std::optional<std::pair<glm::dvec2, glm::dvec2>> mFit;
    glm::dvec2 mPan{0.0};
    double mZoom = 300.0;
    UvTexturePaint mTexPaint;
    Tool mTool = Tool::Select;
    int mUvToolListener = 0;
    bool mUvToolDrag = false;
    Display mDisplay = Display::Checker;
    bool mUdim = false;
    bool mShowStacks = false;
    bool mPackPreview = false;
    bool mPackDirty = false;
    bool mPackFailed = false;
    /** Stacked islands per shown mesh: a badge position (uv) and how many islands share it. */
    struct StackBadges
    {
        scene::MeshPtr mesh;
        std::string set;
        std::vector<std::pair<glm::dvec2, int>> badges;
    };
    std::vector<StackBadges> mStacks;

    Nav mNav = Nav::None;
    bool mMarquee = false;
    bool mTabHeld = false;
    bool mCursorInside = false;
    paintSelect::Stroke mTabStroke;
    Qt::KeyboardModifiers mMods;
    QPointF mPressPos, mMousePos, mLastPos;
    Handle mHover = Handle::None;
    scene::componentPick::Hover mPreselect;
    Handle mDrag = Handle::None;
    int mAxisLock = 0; // a Shift free move: 1 along U, 2 along V
    glm::dvec2 mPivot{0.5}, mPressUv{0.0}, mDragMove{0.0}, mDragScale{1.0};
    double mDragAngle = 0.0;
    std::vector<std::vector<uint8_t>> mDragMask;
    std::vector<std::vector<std::pair<uint32_t, glm::dmat2>>> mDragFollow;
};
} // namespace lila::ui
