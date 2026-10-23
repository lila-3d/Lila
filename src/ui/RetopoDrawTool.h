#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Plug.h"
#include "ui/ViewportTool.h"

#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QObject;

namespace lila::ui
{
/**
 * Retopo Draw: draws a new low-poly mesh on top of a live reference surface.
 *
 *   click                place a dot on the surface
 *   Shift hover/click    preview, then fill, a quad from the four dots/vertices around the cursor
 *   Shift drag           relax brush (Shift wheel resizes), points slide on the surface
 *   Ctrl drag            insert an edge loop across the strip under the cursor
 *   Tab drag             extend a strip of quads from a border edge
 *   Tab Shift drag       extrude the whole border loop the edge is on
 *   Tab Ctrl drag        extrude along the border until it turns more than the extrude angle (Tab Ctrl wheel adjusts)
 *   Ctrl Shift click     delete the face, vertex or dot under the cursor (highlighted while hovering)
 *   MMB or LMB drag      tweak a vertex or dot along the surface; dropping a vertex on another welds them
 *   Esc, Q/W/E/R         leave the tool
 *
 * Targets come from retopo::resolveRetopoDraw. The retopo mesh is created with the first dot, and keeps its reference
 * connected and its dots stored, so a later session resumes on it. Every edit, dots included, is a retopoDraw* command,
 * so it undoes and echoes like any other. Symmetry follows the retopo mesh's symmetry setting.
 */
class LILA_UI_API RetopoDrawTool : public ViewportTool
{
public:
    RetopoDrawTool(scene::NodeId reference, std::string retopoName);
    ~RetopoDrawTool() override;
    const char* name() const override { return "Retopo Draw"; }

    void activate() override;
    void deactivate() override;
    bool mousePress(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override;
    bool wheel(QWheelEvent* e, const ViewInfo& view) override;
    bool keyPress(QKeyEvent* e, const ViewInfo& view) override;
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override;
    std::optional<QCursor> cursor() const override;

    /** Tab is held; fed by the tool's application event filter since Tab never reaches the viewport. */
    void setTabDown(bool down)
    {
        mTabDown = down;
        viewportTools::requestRedraw();
    }
    /** A vertex drag is over another vertex and welds to it on release. */
    bool merging() const { return mMergeTarget != geo::kInvalid; }

    /** Tab Ctrl drag's turn limit, degrees; kept in the general prefs. */
    static double extrudeAngle();
    static void setExtrudeAngle(double degrees);

private:
    enum class Drag
    {
        None,
        Tweak,
        DotMove,
        Relax,
        Loop,
        Extend,
        ExtendRun,
        DeleteClick,
        FillClick,
        DotClick,
    };

    struct Hover
    {
        geo::Index point = geo::kInvalid;
        geo::Index edge = geo::kInvalid;
        geo::Index face = geo::kInvalid;
        int dot = -1;
    };

    /** The face a Shift click would fill: world corners in order, and the dots among them. */
    struct Fill
    {
        std::vector<glm::dvec3> points;
        std::vector<int> dots;
    };

    void refresh();
    bool surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const;
    glm::dvec3 onSurface(const glm::dvec3& p) const;
    std::vector<std::optional<QPointF>> projectPoints(const ViewInfo& view) const;
    bool visible(const glm::dvec3& p, const ViewInfo& view) const;
    Hover pick(QPointF px, const ViewInfo& view) const;
    glm::dvec3 pointWorld(geo::Index p) const;

    scene::Node* retopoNode() const;
    bool deleting() const;
    std::optional<Fill> findFill(QPointF px, const ViewInfo& view) const;
    bool setDots(const std::vector<glm::dvec3>& dots);
    void placeDot(QPointF px, const ViewInfo& view);
    void fillAt(QPointF px, const ViewInfo& view);
    void deleteAt(QPointF px, const ViewInfo& view);
    void relaxStep(QPointF px, const ViewInfo& view);
    void loopPreview(geo::Index edge, double fraction, std::vector<std::pair<glm::dvec3, glm::dvec3>>& out) const;
    double edgeFraction(geo::Index edge, QPointF px, const ViewInfo& view) const;
    void commitDrag(const ViewInfo& view);
    bool run(const char* command, const nlohmann::json& call, nlohmann::json* result = nullptr);
    /** Adds the retopo mesh, the reference to connect, and create=True. */
    nlohmann::json targetCall(nlohmann::json flags) const;
    /** The border run Tab Shift (whole loop) or Tab Ctrl (to the extrude angle) would extrude from edge. */
    retopo::BorderRun extrudeRun(geo::Index edge, bool wholeLoop) const;
    /** Starts a run extrude from the border edge: its run, and each run point's outward direction along the surface. */
    void beginRunExtrude(geo::Index edge, bool wholeLoop);
    void updateRunExtrude(QPointF px, const ViewInfo& view);
    void drawRun(render::FrameState& s, const retopo::BorderRun& run) const;
    void removeOwnDraw(render::FrameState& s) const;
    void drawDeleteTarget(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) const;

    scene::NodeId mReference = scene::kNoNode;
    std::string mRetopoName;
    scene::NodeId mRetopoId = scene::kNoNode;

    // Cached views of the scene, rebuilt when the meshes change.
    scene::MeshPtr mRefMesh;
    glm::dmat4 mRefWorld{1.0};
    geo::MeshBvh mRefBvh;
    scene::MeshPtr mRetopo; // base mesh of the retopo node; ids are what the commands take
    glm::dmat4 mRetopoWorld{1.0};
    std::optional<geo::SymmetryPlane> mSymmetry; // world space

    std::vector<glm::dvec3> mDots; // world space, read from the retopo mesh
    std::string mDotsText;
    glm::dmat4 mDotsWorld{1.0};
    Hover mHover;
    QPointF mCursor;
    std::string mCursorView; // camera of the viewport the cursor was last in; hover previews draw only there
    Qt::KeyboardModifiers mMods;
    bool mTabDown = false;
    double mBrush = 60.0;

    Drag mDrag = Drag::None;
    QPointF mPressPx;
    geo::Index mDragPoint = geo::kInvalid;
    int mDragDot = -1;
    std::optional<glm::dvec3> mDotTo;
    geo::Index mDragEdge = geo::kInvalid;
    double mLoopFraction = 0.5;
    glm::dvec3 mDragStartHit{0.0};
    std::map<geo::Index, glm::dvec3> mMoved; // tweak and relax preview positions
    std::array<glm::dvec3, 2> mExtendTo{};
    retopo::BorderRun mRun;
    std::vector<glm::dvec3> mRunOut;    // per run point: outward along the surface, scaled for even strip width
    std::vector<glm::dvec3> mRunTo;     // per run point: where its new point lands
    glm::dvec3 mRunPull{0.0};           // the grabbed edge's outward direction; the drag's distance is measured along it
    geo::Index mMergeTarget = geo::kInvalid;

    std::unique_ptr<QObject> mFilter;
};

/** Registers "retopo.draw". */
LILA_UI_API void registerRetopoActions();
} // namespace lila::ui
