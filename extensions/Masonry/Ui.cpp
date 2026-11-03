// Level > Masonry, whose Masonry Wall builds a wall along the selected mesh's loose edges, the Ruin Brush that paints damage
// onto a wall or building, and the Level Toolkit's Masonry section.

#include "Masonry.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QKeyEvent>
#include <QMouseEvent>

#include <memory>

using namespace lila;
using namespace lila::ui;

namespace masonry
{
namespace
{
/** Brush radius, cm, kept between uses. */
double gRadius = 0.0;

/** Drag over a masonry wall or building to knock its stones out (Shift holds them up); the stroke goes to ruinPaint on release. */
class RuinBrush final : public ViewportTool
{
public:
    const char* name() const override { return "Ruin Brush"; }

    void activate() override
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        for (scene::NodeId id : g.selection())
        {
            scene::Node* n = g.node(id);
            const int out = n ? n->type().attrIndex("outMesh") : -1;
            if (out < 0)
                continue;
            const auto mesh = n->as<scene::MeshPtr>(out);
            if (!mesh || mesh->numFaces() == 0)
                continue;
            mWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            geo::Mesh world = *mesh;
            for (glm::dvec3& q : world.positions())
                q = glm::dvec3(mWorld * glm::dvec4(q, 1.0));
            mBvh = std::make_unique<geo::MeshBvh>(world);
            mPath = n->path();
            if (gRadius <= 0.0)
                gRadius = std::max(10.0, glm::length(world.bounds().size()) * 0.05);
            break;
        }
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || !mBvh)
            return false;
        mStroke.clear();
        mHold = e->modifiers().testFlag(Qt::ShiftModifier);
        sample(e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!(e->buttons() & Qt::LeftButton) || !mBvh)
            return false;
        sample(e, view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mStroke.empty())
            return false;
        nlohmann::json pts = nlohmann::json::array();
        const glm::dmat4 toLocal = glm::inverse(mWorld);
        for (const glm::dvec3& q : mStroke)
        {
            const glm::dvec3 l(toLocal * glm::dvec4(q, 1.0));
            pts.push_back({l.x, l.y, l.z});
        }
        try
        {
            ops::execute("ruinPaint", {{"args", {mPath}}, {"points", pts}, {"radius", gRadius}, {"strength", mHold ? -1.0 : 1.0}}, CallSource::UI);
        }
        catch (const std::exception& ex)
        {
            log::write(LogLevel::Warning, ex.what());
        }
        mStroke.clear();
        // The wall changed under the brush.
        activate();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight)
        {
            gRadius = std::max(1.0, gRadius * (e->key() == Qt::Key_BracketRight ? 1.25 : 0.8));
            viewportTools::requestRedraw();
            return true;
        }
        if (e->key() != Qt::Key_Escape)
            return false;
        viewportTools::set(nullptr);
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        const QString text = mBvh ? QString("Drag to knock stones out, Shift to hold them, [ ] radius %1 cm (Esc to finish)").arg(gRadius, 0, 'f', 0)
                                  : QString("Select a masonry wall or building first (Esc to cancel)");
        drawToolMessage(p, view, text);
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    void sample(QMouseEvent* e, const ViewInfo& view)
    {
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        geo::BvhHit hit;
        if (!mBvh->raycast(o, d, hit))
            return;
        if (mStroke.empty() || glm::length(hit.point - mStroke.back()) > gRadius * 0.25)
            mStroke.push_back(hit.point);
    }

    std::unique_ptr<geo::MeshBvh> mBvh;
    glm::dmat4 mWorld{1.0};
    std::string mPath;
    std::vector<glm::dvec3> mStroke;
    bool mHold = false;
};
} // namespace

void registerUi(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = "masonry.ruinBrush", .label = "Ruin Brush", .icon = "wall-off", .create = [] {
                                 return std::unique_ptr<ViewportTool>(std::make_unique<RuinBrush>());
                             }});
    actions::add(ctx, {.id = "masonry.brush",
                       .label = "Ruin Brush",
                       .category = "Masonry",
                       .run = [] { viewportTools::activate("masonry.ruinBrush"); },
                       .doc = "Drag over the selected wall or building to knock stones out; Shift holds them up. Agents call ruinPaint.",
                       .python = "ruinPaint('Wall', points=[[0, 200, 0], [300, 180, 0]], radius=80)"});
    actions::add(ctx, {.id = "masonry.wall",
                       .label = "Masonry Wall",
                       .category = "Create",
                       .run =
                           [] {
                               nlohmann::json names = nlohmann::json::array();
                               scene::Graph& g = scene::Graph::current();
                               for (scene::NodeId id : g.selection())
                                   if (scene::Node* n = g.node(id))
                                       names.push_back(n->path());
                               try
                               {
                                   for (const nlohmann::json& n : names)
                                       ops::execute("masonryWall", {{"args", {n}}}, CallSource::UI);
                               }
                               catch (const std::exception& e)
                               {
                                   log::write(LogLevel::Warning, e.what());
                               }
                           },
                       .doc = "Lay a brick wall along each selected mesh's loose edges: courses, bonded corners and a mortar core.",
                       .python = "masonryWall('Footprint', bond='running', height=200)"});
    actions::add(ctx, {.id = "masonry.ship", .label = "Send Masonry to Unreal", .category = "Masonry",
                       .doc = "Send the selected wall to Unreal's open level as instanced stones per ground chunk.", .python = "masonryShip('Wall')",
                       .command = "masonryShip", .call = nlohmann::json::object()});

    actions::add(ctx, {.id = "masonry.ruin", .label = "Ruin Masonry", .category = "Masonry",
                       .doc = "Ruin the selected wall or building to half its stone, or the selected stones in place; change it with ruin().",
                       .python = "ruin('Wall', left=0.5)", .command = "ruin", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "masonry.age", .label = "Age Masonry", .category = "Masonry",
                       .doc = "Weather the selected wall or building: moss, wet and wear for its material.", .python = "age('Wall', age=0.7)",
                       .command = "age", .call = nlohmann::json::object()});

    levelToolkit::add(ctx, {.id = "masonry",
                            .title = "Masonry",
                            .items = {{"masonry.wall", "wall", "Wall", "Wall"}, {"masonry.ruin", "wall-off", "Ruin"}, {"masonry.brush", "brush", "Brush"}, {"masonry.age", "leaf", "Age"}, {"masonry.ship", "send", "Unreal"}},
                            .order = 30,
                            .docs = "masonry"});
    menus::addItem(ctx, {.id = "masonry.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Masonry",
                         .before = levelToolkit::kDressSection});
    for (const char* a : {"masonry.wall", "masonry.ruin", "masonry.brush", "masonry.age", "masonry.ship"})
        menus::addItem(ctx, {.id = std::string("masonry.menu.") + a, .menu = "Level/Masonry", .action = a});
}
} // namespace masonry
