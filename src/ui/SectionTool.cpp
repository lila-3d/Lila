#include "ui/CurveUi.h"
#include "ui/CurveToolsCommon.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/Contour.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Isolate.h"
#include "ui/ViewportTool.h"

namespace lila::ui
{
namespace
{
using namespace curveTools;

constexpr size_t kMaxMeshes = 8;

/** The selected meshes, or every visible one when none is selected (as many as a section takes). */
std::vector<scene::NodeId> sectionTargets()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<scene::NodeId> out;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && out.size() < kMaxMeshes)
            out.push_back(id);
    if (!out.empty())
        return out;
    const scene::isolation::Set* iso = scene::isolation::active();
    for (scene::NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && scene::effectivelyVisible(g, id) && isolate::shows(iso, id) && out.size() < kMaxMeshes)
            out.push_back(id);
    return out;
}

/**
 * Section: drag a line across the view and the meshes are cut by the plane through it, edge on to the view, previewed as
 * you drag; releasing makes the section curves with live history. Cuts the selected meshes, or every visible one.
 */
class SectionTool final : public ViewportTool
{
public:
    const char* name() const override { return "Section"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void activate() override { log::info("Section: drag a line across the meshes to cut them along it"); }
    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mTargets = sectionTargets();
        if (mTargets.empty())
        {
            log::warn("Section: no mesh to cut");
            return true;
        }
        mFrom = mTo = e->position();
        mDragging = true;
        mView = view.camera;
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDragging)
            return false;
        mTo = e->position();
        updatePlane(view);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        mTo = e->position();
        if (distPx(mFrom, mTo) >= 4.0 && updatePlane(view))
        {
            json meshes = json::array();
            for (scene::NodeId id : mTargets)
                if (const scene::Node* n = scene::Graph::current().node(id))
                    meshes.push_back(n->path());
            try
            {
                ops::execute("section", {{"args", meshes}, {"point", {mOrigin.x, mOrigin.y, mOrigin.z}}, {"normal", {mNormal.x, mNormal.y, mNormal.z}}},
                             CallSource::UI);
            }
            catch (const CommandError& err)
            {
                log::error("{}", err.what());
            }
        }
        mPreview.clear();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape)
            return false;
        if (!mDragging)
            exitTool();
        mDragging = false;
        mPreview.clear();
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        if (mDragging && view.camera == mView)
        {
            p.line(mFrom, mTo, 1.5f, kPreview);
            for (const curve::Spline& s : mPreview)
                drawSpline(f, s, kPreview);
        }
        drawToolMessage(p, view, "Section: drag a line across the meshes to cut them along it (the selected meshes, or all visible), Esc: exit");
    }

private:
    /** The plane through the dragged line, edge on to the view, and the preview cut through it. */
    bool updatePlane(const ViewInfo& view)
    {
        LILA_ZONE();

        D3 o1, d1, o2, d2;
        view.ray(mFrom, o1, d1);
        view.ray(mTo, o2, d2);
        // Points on both rays at the same depth: with the ray direction they span the plane, perspective or orthographic.
        const D3 p1 = o1 + d1 * 100.0, p2 = o2 + d2 * 100.0;
        D3 n = glm::cross(p2 - p1, d1);
        if (glm::length(n) < 1e-12)
            return false;
        mNormal = glm::normalize(n);
        mOrigin = p1;
        scene::Graph& g = scene::Graph::current();
        std::vector<scene::MeshPtr> keep;
        std::vector<geo::SectionMesh> meshes;
        for (scene::NodeId id : mTargets)
            if (scene::Node* node = g.node(id))
                if (scene::MeshPtr m = node->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
                {
                    keep.push_back(m);
                    meshes.push_back({m.get(), node->as<glm::dmat4>(scene::Transform::kWorldMatrix)});
                }
        mPreview = curve::sectionCurves(meshes, mOrigin, mNormal);
        return true;
    }

    std::vector<scene::NodeId> mTargets;
    QPointF mFrom, mTo;
    bool mDragging = false;
    std::string mView;
    D3 mOrigin{0.0}, mNormal{0, 1, 0};
    curve::Splines mPreview;
};
} // namespace

void registerSectionTool()
{
    if (!viewportTools::registry().find("curve.sectionTool"))
        viewportTools::add({.id = "curve.sectionTool", .label = "Section Tool", .icon = "slice", .create = [] { return std::make_unique<SectionTool>(); }});
    actions::add({.id = "curve.sectionTool",
                  .label = "Section Tool",
                  .category = "Curves",
                  .run = [] { viewportTools::activate("curve.sectionTool"); },
                  .doc = "Cuts the selected meshes (or every visible one) into curves along a line you drag across the view, kept live.",
                  .python = "lila.ui.run_action(\"curve.sectionTool\")"});
    actions::add({.id = "curve.contour",
                  .label = "Contour",
                  .category = "Curves",
                  .doc = "Slices the selected meshes into curves at evenly spaced planes along an axis (spacing or count in the option "
                         "box), kept live.",
                  .python = ops::toPython("contour", json::object()),
                  .command = "contour",
                  .call = json::object()});
}
} // namespace lila::ui
