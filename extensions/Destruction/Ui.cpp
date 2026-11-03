// The Effects menu and the impact tool: pick a material, click where the selected mesh is hit, and breakApart builds the
// destruction there; or click a destruction to set off another explosion at the current frame.

#include "Destruction.h"
#include "PieceTransform.h"
#include "RigidState.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QKeyEvent>
#include <QMouseEvent>

#include <memory>
#include <string>
#include <vector>

using namespace lila;
using namespace lila::ui;

namespace destruction
{
using json = nlohmann::json;

namespace
{
/** What the next impact click makes; set by the action that starts the tool. */
std::string gMaterial = "concrete";
bool gSimulate = true;
bool gExplode = false;

class ImpactTool final : public ViewportTool
{
public:
    const char* name() const override { return gExplode ? "Add Explosion" : "Break Apart"; }

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
            geo::Mesh world = *mesh;
            const glm::dmat4 m = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            for (geo::Index p = 0; p < world.pointCapacity(); ++p)
                if (world.pointValid(p))
                    world.setPosition(p, glm::dvec3(m * glm::dvec4(world.position(p), 1.0)));
            Target t;
            t.path = n->path();
            t.bvh = std::make_unique<geo::MeshBvh>();
            t.bvh->build(world);
            mTargets.push_back(std::move(t));
        }
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        const Target* best = nullptr;
        geo::BvhHit bestHit;
        for (const Target& t : mTargets)
        {
            geo::BvhHit hit;
            if (t.bvh->raycast(o, d, hit) && hit.t < bestHit.t)
            {
                bestHit = hit;
                best = &t;
            }
        }
        if (!best)
            return true;
        const glm::dvec3 at = bestHit.point;
        try
        {
            if (gExplode)
                ops::execute("addExplosion", {{"args", {best->path}}, {"position", json::array({at.x, at.y, at.z})}}, CallSource::UI);
            else
            {
                // Everything selected breaks together, hit where the click landed.
                json paths = json::array();
                for (const Target& t : mTargets)
                    paths.push_back(t.path);
                ops::execute("breakApart",
                             {{"args", paths}, {"material", gMaterial}, {"impact", json::array({at.x, at.y, at.z})}, {"simulate", gSimulate}},
                             CallSource::UI);
            }
        }
        catch (const std::exception& ex)
        {
            log::error("{}", ex.what());
        }
        viewportTools::set(nullptr);
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape)
            return false;
        viewportTools::set(nullptr);
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        ToolStatus status{.tool = name(), .keys = {{"Esc", "cancel"}}};
        if (gExplode)
            status.prompt = mTargets.empty() ? "select a destruction" : "click where it explodes, on this frame";
        else
        {
            status.mode = QString::fromStdString(gMaterial);
            status.prompt = mTargets.empty() ? "select a mesh" : "click where it is hit";
        }
        drawToolStatus(p, view, status);
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    struct Target
    {
        std::string path;
        std::unique_ptr<geo::MeshBvh> bvh;
    };
    std::vector<Target> mTargets;
};

bool gShowLinks = true;

/** A heat ramp for link strength (a data display): weak links dark red, strong ones pale yellow. */
glm::vec4 strengthColour(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return {float(0.55 + 0.45 * t), float(0.1 + 0.8 * t), float(0.1 + 0.5 * t * t), 0.9f};
}

/**
 * The links of the selected meshes' destruction, where the simulation has carried them on this frame: a line per intact
 * link from its anchor on one piece to the other, coloured by strength. Reads only cached frames, so drawing never
 * simulates.
 */
void drawLinks(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    if (!gShowLinks)
        return;
    scene::Graph& g = scene::Graph::current();
    const int64_t frame = scene::sim::frameOf(g.time());
    for (scene::NodeId id : g.selection())
    {
        for (scene::NodeId m : scene::modifiers::stack(g, id))
        {
            scene::Node* mod = g.node(m);
            if (!mod || !scene::sim::isSimulation(*mod))
                continue;
            std::shared_ptr<const gn::SimInputs> in = gn::simulationInputs(m);
            if (!in)
                continue;
            gn::GeometryPtr links = in->geometry("links"), pieces = in->geometry("pieces");
            if (!links->curves || !pieces->instances)
                continue;
            const std::vector<uint8_t> bytes = gn::cachedSimulationState(g, m, frame);
            const StateView state(bytes);
            const gn::Instances& inst = *pieces->instances;
            std::vector<glm::dmat4> delta(inst.size(), glm::dmat4(1.0));
            if (state.valid() && state.header().bodies == inst.size())
                for (size_t k = 0; k < inst.size(); ++k)
                {
                    delta[k] = compose(state.position(k), state.rotation(k), decompose(inst.transforms[k]).scale) * glm::inverse(inst.transforms[k]);
                }
            const glm::dmat4 world = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            const gn::CurveSet& c = *links->curves;
            std::span<const int32_t> pa = c.curveAttrs.get<int32_t>("pieceA"), pb = c.curveAttrs.get<int32_t>("pieceB");
            std::span<const float> strength = c.curveAttrs.get<float>("strength");
            double strongest = 1e-9;
            for (float v : strength)
                strongest = std::max(strongest, double(v));
            size_t link = 0;
            for (size_t k = 0; k < c.numCurves(); ++k)
            {
                if (c.curveSize(k) == 0)
                    continue;
                const size_t record = link++;
                if (state.valid() && record < state.header().links && state.link(record).brokenStep >= 0)
                    continue;
                const glm::dvec3 anchor = c.curvePoints(k)[0];
                auto placed = [&](int piece) {
                    const glm::dmat4& d = piece >= 0 && size_t(piece) < delta.size() ? delta[size_t(piece)] : glm::dmat4(1.0);
                    return glm::dvec3(world * d * glm::dvec4(anchor, 1.0));
                };
                const int a = k < pa.size() ? pa[k] : -1, b = k < pb.size() ? pb[k] : -1;
                const glm::dvec3 from = placed(a), to = b >= 0 ? placed(b) : glm::dvec3(world * glm::dvec4(anchor, 1.0));
                auto sa = view.project(from), sb = view.project(to);
                if (!sa || !sb)
                    continue;
                const glm::vec4 colour = strengthColour(k < strength.size() ? strength[k] / strongest : 1.0);
                if (glm::length(from - to) < 1e-3)
                    p.line(*sa - QPointF(2, 0), *sa + QPointF(2, 0), 3.0f, colour);
                else
                    p.line(*sa, *sb, 2.0f, colour);
            }
        }
    }
}
} // namespace

void registerUi(ext::Context& ctx)
{
    viewportHooks::addDrawer(ctx, "destruction.links", drawLinks);
    actions::add(ctx, {.id = "destruction.showLinks",
                       .label = "Show Destruction Links",
                       .category = "Effects",
                       .run =
                           [] {
                               gShowLinks = !gShowLinks;
                               viewportTools::requestRedraw();
                           },
                       .doc = "Draw the links holding the selected destruction's pieces together, coloured by strength; broken links disappear.",
                       .checked = [] { return gShowLinks; }});
    viewportTools::add(ctx, {.id = "destruction.impact", .label = "Break Apart", .icon = "bolt", .create = [] {
                                 return std::unique_ptr<ViewportTool>(std::make_unique<ImpactTool>());
                             }});
    auto breakAction = [&](const std::string& material, const char* label, bool simulate, const char* doc) {
        const std::string id = std::string(simulate ? "destruction.break." : "destruction.fracture.") + material;
        actions::add(ctx, {.id = id,
                           .label = label,
                           .category = "Effects",
                           .run =
                               [material, simulate] {
                                   gMaterial = material;
                                   gSimulate = simulate;
                                   gExplode = false;
                                   viewportTools::activate("destruction.impact");
                               },
                           .doc = doc,
                           .python = "breakApart(material='" + material + "', impact=[x, y, z]" + (simulate ? "" : ", simulate=False") + ")"});
        return id;
    };
    const std::vector<std::string> items = {
        breakAction("concrete", "Break Apart: Concrete", true,
                    "Click where the selected mesh is hit: it cracks into rough chunks like concrete and falls, anchored where it meets the ground."),
        breakAction("glass", "Break Apart: Glass", true, "Click where the selected pane is hit: it shatters into shards radiating from there."),
        breakAction("wood", "Break Apart: Wood", true, "Click where the selected mesh is hit: it splinters along the grain."),
        "",
        breakAction("concrete", "Fracture: Concrete", false, "Click where the selected mesh is hit: it's cut into pieces like concrete, without simulating."),
        breakAction("glass", "Fracture: Glass", false, "Click where the selected pane is hit: it's cut into shards, without simulating."),
        breakAction("wood", "Fracture: Wood", false, "Click where the selected mesh is hit: it's cut into splinters, without simulating."),
        "",
        "destruction.addExplosion",
        "destruction.showLinks",
        "",
        "animated.unreal.vat",
        "animated.sendOptions",
    };
    actions::add(ctx, {.id = "destruction.addExplosion",
                       .label = "Add Explosion",
                       .category = "Effects",
                       .run =
                           [] {
                               gExplode = true;
                               viewportTools::activate("destruction.impact");
                           },
                       .doc = "Click the selected destruction where another explosion goes off, on the current frame.",
                       .python = "addExplosion(position=[x, y, z], frame=f)"});
    menus::addMenu(ctx, {.id = "Effects", .title = "Effects", .before = "Rig", .docs = "destruction", .sets = {"Animation", "Level"}});
    menus::addItem(ctx, {.id = "destruction.menu", .menu = "Effects", .kind = menus::Kind::Submenu, .label = "Destruction"});
    int sep = 0;
    for (const std::string& a : items)
    {
        if (!a.empty())
            menus::addItem(ctx, {.id = "destruction.menu." + a, .menu = "Effects/Destruction", .action = a});
        else
            menus::addItem(ctx, {.id = "destruction.menu.sep" + std::to_string(sep++), .menu = "Effects/Destruction", .kind = menus::Kind::Separator});
    }
}
} // namespace destruction
