#pragma once

#include "geo/Bvh.h"
#include "geo/Symmetry.h"
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
 * Maya-style Quad Draw: draws a new low-poly mesh on top of a live reference surface.
 *
 *   click                place a dot on the surface
 *   Shift hover/click    preview, then fill, a quad from the four dots/vertices around the cursor
 *   Shift drag           relax brush (Shift wheel resizes), points slide on the surface
 *   Ctrl drag            insert an edge loop across the strip under the cursor
 *   Tab drag             extend a strip of quads from a border edge
 *   Ctrl Shift click     delete the face, vertex or dot under the cursor (highlighted while hovering)
 *   MMB or LMB drag      tweak a vertex or dot along the surface; dropping a vertex on another welds them
 *   Esc, Q/W/E/R         leave the tool
 *
 * Targets come from retopo::resolveQuadDraw. The retopo mesh is created with the first dot, and keeps its reference
 * connected and its dots stored, so a later session resumes on it. Every edit, dots included, is a quadDraw* command,
 * so it undoes and echoes like any other. Symmetry follows the retopo mesh's symmetry setting.
 */
class QuadDrawTool : public ViewportTool
{
public:
    QuadDrawTool(scene::NodeId reference, std::string retopoName);
    ~QuadDrawTool() override;
    const char* name() const override { return "Quad Draw"; }

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
    void setTabDown(bool down) { mTabDown = down; }

private:
    enum class Drag
    {
        None,
        Tweak,
        DotMove,
        Relax,
        Loop,
        Extend,
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

    std::unique_ptr<QObject> mFilter;
};

/** Registers "retopo.quadDraw". */
void registerRetopoActions();
} // namespace lila::ui
