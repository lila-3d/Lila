#include "curve/Cable.h"
#include "curve/EnvAttrs.h"
#include "curve/EnvironmentNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "math/MathCore.h"
#include "ops/CommandOptions.h"
#include "scene/MeshNodes.h"

#include <memory>

namespace lila::curve
{
using namespace scene;
using namespace env;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
constexpr int kColliders = 4;

std::string slot(const char* base, int k) { return base + std::to_string(k); }

/** Where a spline's cables hang from: its points, or a NURBS's edit points (where the curve passes). */
std::vector<D3> anchorsOf(const Spline& s)
{
    if (s.kind == Kind::Nurbs)
        return editPoints(s);
    return s.points;
}

struct Collider
{
    MeshPtr mesh;
    std::shared_ptr<geo::MeshBvh> bvh;
    glm::dmat4 world, toLocal;
    glm::dmat3 normalToWorld;
};

/** The closest point over every collider, world space. */
ClosestSurface closestOver(std::shared_ptr<std::vector<Collider>> colliders)
{
    return [colliders](const D3& p) -> std::optional<SurfacePoint> {
        std::optional<SurfacePoint> best;
        double bestDist = 1e300;
        for (const Collider& c : *colliders)
        {
            geo::BvhClosest hit;
            if (!c.bvh->closestPoint(D3(c.toLocal * glm::dvec4(p, 1.0)), hit) || !c.mesh->faceValid(hit.face))
                continue;
            D3 w(c.world * glm::dvec4(hit.point, 1.0));
            double dist = glm::distance(w, p);
            if (dist >= bestDist)
                continue;
            D3 n = c.normalToWorld * c.mesh->faceNormal(hit.face);
            double len = glm::length(n);
            bestDist = dist;
            best = SurfacePoint{w, len > 1e-12 ? n / len : D3(0, 1, 0)};
        }
        return best;
    };
}

/** Catenary cables between the anchor curve's points, optionally bundled and draped over the collider meshes. */
class CableNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int out = idx(*this, "outCurve");
        try
        {
            CableOptions o;
            o.mode = i(*this, "sagMode") == 1 ? SagMode::Depth : SagMode::Slack;
            o.slack = std::max(0.0, f(*this, "slack"));
            o.sag = std::max(0.0, f(*this, "sag"));
            o.gravity = v(*this, "gravity");
            o.samples = int(std::clamp<int64_t>(i(*this, "samples"), 4, 1024));
            o.tolerance = std::max(1e-4, f(*this, "tolerance"));
            o.count = int(std::clamp<int64_t>(i(*this, "count"), 1, 256));
            o.spread = std::max(0.0, f(*this, "spread"));
            o.sagVariation = std::max(0.0, f(*this, "sagVariation"));
            o.seed = uint32_t(i(*this, "seed"));
            o.drape = b(*this, "drape");
            o.clearance = std::max(0.0, f(*this, "clearance"));
            o.iterations = int(std::clamp<int64_t>(i(*this, "iterations"), 1, 1000));
            if (o.drape)
            {
                auto colliders = std::make_shared<std::vector<Collider>>();
                for (int k = 0; k < kColliders; ++k)
                    if (const MeshPtr& m = as<MeshPtr>(idx(*this, slot("collider", k).c_str())); m && m->numFaces() > 0)
                    {
                        glm::dmat4 w = as<glm::dmat4>(idx(*this, slot("colliderMatrix", k).c_str()));
                        colliders->push_back({m, std::make_shared<geo::MeshBvh>(*m), w, glm::inverse(w), glm::transpose(glm::inverse(glm::dmat3(w)))});
                    }
                if (!colliders->empty())
                    o.closest = closestOver(colliders);
            }
            Splines cables;
            std::string warning;
            for (const Spline& s : splinesOf(value(idx(*this, "inCurve"))))
            {
                CableResult r = makeCables(anchorsOf(s), s.closed, o);
                for (Spline& c : r.cables)
                    cables.push_back(std::move(c));
                for (const std::string& w : r.warnings)
                    warning += (warning.empty() ? "" : "; ") + w;
            }
            if (o.drape && !o.closest)
                warning += std::string(warning.empty() ? "" : "; ") + "drape is on but no collider mesh is connected";
            if (warning != as<std::string>(idx(*this, "warning")) && !warning.empty())
                log::warn("{}: {}", name(), warning);
            setOutput(idx(*this, "warning"), warning);
            setOutput(out, makeCurveData(std::move(cables)));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, DataPtr());
        }
    }
};

Node& meshNamed(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError("cable: " + name + " is not a mesh");
    return *n;
}

std::vector<D3> pointsFlag(const Args& a)
{
    std::vector<D3> out;
    for (const json& p : a.flags["points"])
    {
        if (!p.is_array() || p.size() != 3)
            throw CommandError("cable: points: expected [[x, y, z], ...]");
        out.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    }
    return out;
}

void registerOptionLayout()
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = {"sagMode", "slack", "sag", "count", "spread", "sagVariation", "seed", "drape", "clearance", "pipe", "pipeRadius", "pipeSides"};
    o.hidden = {"points", "name", "colliders"};
    o.sections = {{"count", "Bundle"},   {"spread", "Bundle"},   {"sagVariation", "Bundle"}, {"seed", "Bundle"},
                  {"drape", "Drape"},    {"clearance", "Drape"}, {"iterations", "Drape"},    {"pipe", "Pipe"},
                  {"pipeRadius", "Pipe"}, {"pipeSides", "Pipe"},  {"samples", "Advanced"},    {"tolerance", "Advanced"},
                  {"gravity", "Advanced"}};
    o.ranges["slack"] = {0.0, 10.0, 0.01, ""};
    o.ranges["sag"] = {0.0, 1e5, 1.0, " cm"};
    o.ranges["spread"] = {0.0, 1e4, 0.5, " cm"};
    o.ranges["clearance"] = {0.0, 1e4, 0.1, " cm"};
    o.ranges["pipeRadius"] = {0.01, 1e4, 0.1, " cm"};
    o.labels = {{"slack", "Slack (fraction of the span)"}, {"sag", "Sag Depth"}, {"count", "Cables per Span"}, {"pipe", "Build Pipe Geometry"}};
    opt::setOverride("cable", std::move(o));
}
} // namespace

void registerCableType()
{
    if (types::find("curveCable"))
        return;
    NodeType t;
    t.name = "curveCable";
    t.icon = "line-dashed";
    t.doc = "Cables hanging between the points of anchor curves: true catenaries by slack or sag depth under gravity, bundles with "
            "seeded variation, optionally draped over collider meshes.";
    t.ownAttrs.push_back(dataIn("inCurve", "Anchor curves, world space: a cable spans each pair of neighbouring points."));
    for (int k = 0; k < kColliders; ++k)
    {
        t.ownAttrs.push_back(meshIn(slot("collider", k), "A mesh the cables drape over."));
        t.ownAttrs.push_back(matrixIn(slot("colliderMatrix", k), "That mesh's world matrix."));
    }
    for (AttrDef a : {eAttr("sagMode", {"slack", "depth"}, 0, "How much the cables hang: slack (extra length) or sag (depth below the chord)."),
                      fAttr("slack", 0.1, "Extra length over the straight span, as a fraction of it (0.1 is 10% longer).", 0.0),
                      fAttr("sag", 20.0, "How far the cable dips below the straight span, cm, along gravity.", 0.0),
                      vAttr("gravity", D3(0, -1, 0), "Which way is down."),
                      iAttr("count", 1, "Cables per span: above 1 makes a bundle.", 1, 256.0),
                      fAttr("spread", 2.0, "Bundle: how far each cable's ends stray from the anchors, cm.", 0.0),
                      fAttr("sagVariation", 0.15, "Bundle: how much each cable's slack or sag varies, as a fraction of it.", 0.0),
                      iAttr("seed", 1, "Bundle: the random variation's seed."),
                      bAttr("drape", false, "Let the cables settle onto the collider meshes where they would pass into them."),
                      fAttr("clearance", 0.5, "Drape: how far the centreline stays off surfaces (the cable's radius), cm.", 0.0),
                      iAttr("iterations", 60, "Drape: settling steps.", 1, 1000.0),
                      iAttr("samples", 32, "Points along each span before fitting.", 4),
                      fAttr("tolerance", 0.05, "Fitting tolerance, cm.", 1e-4)})
        t.ownAttrs.push_back(std::move(a));
    t.ownAttrs.push_back(dataOut("outCurve", "The cable curves, world space, one per cable per span."));
    t.ownAttrs.push_back(stringOut("warning", "Problems with the last build."));
    t.create = [] { return std::make_unique<CableNode>(); };
    types::registerType(std::move(t));
}

void registerCableCommands()
{
    std::vector<FlagDef> flags = attrFlags("curveCable");
    for (FlagDef fd : std::vector<FlagDef>{
             {"points", "p", T::List, nullptr, "[[x, y, z], ...] world anchor points, made into an anchor curve, instead of anchor curves."},
             {"closed", "c", T::Bool, false, "With points: a cable from the last point back to the first too."},
             {"colliders", "col", T::List, nullptr, "Meshes to drape over (default: the selected meshes), at most 4."},
             {"pipe", "", T::Bool, false, "Also build pipe geometry along the cables (the cable pipe preset, live)."},
             {"pipeRadius", "pr", T::Float, 0.6, "Pipe radius, cm.", {}, {}, 0.0},
             {"pipeSides", "ps", T::Int, 6, "Pipe sides.", {}, {}, 3.0},
             {"constructionHistory", "ch", T::Bool, true, "Keep the cable node so moving anchors or editing slack updates the cables."},
             {"name", "n", T::String, nullptr, "Cable curve name; made unique."}})
        flags.push_back(std::move(fd));

    ops::registerCommand({
        .name = "cable",
        .doc = "Hang cables between anchor points: true catenaries by slack (extra length) or sag depth under gravity, bundles of "
               "several with seeded spread and sag variation, optionally draped over meshes, optionally piped. Live: moving the "
               "anchor curve's points or editing the settings updates them. Returns [cable curve, curveCable, anchor curve, pipe "
               "mesh or None] per anchor curve.",
        .positionalDoc = "anchor curves (default: the selected curves)",
        .flags = flags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> anchors;
            std::vector<Node*> colliders;
            if (a.has("colliders"))
                for (const json& c : a.flags["colliders"])
                    colliders.push_back(&meshNamed(g, c.get<std::string>()));
            if (a.has("points"))
            {
                Spline s;
                s.kind = Kind::Poly;
                s.points = pointsFlag(a);
                s.closed = a.get<bool>("closed", false) && s.size() >= 3;
                if (s.size() < 2)
                    throw CommandError("cable: give at least two points");
                s.normalize();
                Node* c = g.createNode(*types::find("curve"), "CableAnchors", kNoNode, true);
                g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData({s}));
                anchors.push_back(c);
            }
            for (const json& p : a.positional)
            {
                Node* n = g.find(p.get<std::string>());
                if (!n || !n->type().isA("curve"))
                    throw CommandError("cable: " + p.get<std::string>() + " is not a curve");
                anchors.push_back(n);
            }
            if (anchors.empty())
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("curve"))
                        anchors.push_back(n);
            if (!a.has("colliders"))
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("mesh"))
                        colliders.push_back(n);
            if (anchors.empty())
                throw CommandError("cable: select anchor curves, or give points=");
            if (colliders.size() > size_t(kColliders))
                throw CommandError("cable: at most " + std::to_string(kColliders) + " collider meshes");

            json out = json::array();
            std::vector<NodeId> made;
            for (Node* anchor : anchors)
            {
                Node* op = g.createNode(*types::find("curveCable"), {}, kNoNode, true);
                if (math::upAxis() == math::UpAxis::Z)
                    g.setValue({op->id(), idx(*op, "gravity"), -1}, D3(0, 0, -1));
                applyFlags(g, *op, a, "cable");
                g.connect({anchor->id(), CurveShape::kWorldSpace, -1}, {op->id(), idx(*op, "inCurve"), -1});
                for (size_t k = 0; k < colliders.size(); ++k)
                {
                    g.connect({colliders[k]->id(), MeshNode::kOutMesh, -1}, {op->id(), idx(*op, slot("collider", int(k)).c_str()), -1});
                    g.connect({colliders[k]->id(), Transform::kWorldMatrix, -1}, {op->id(), idx(*op, slot("colliderMatrix", int(k)).c_str()), -1});
                }
                Node* curve = g.createNode(*types::find("curve"), a.get<std::string>("name", "Cable"), kNoNode, true);
                g.connect({op->id(), idx(*op, "outCurve"), -1}, {curve->id(), CurveShape::kCreate, -1});
                json opName = op->path();
                if (!a.get<bool>("constructionHistory", true))
                {
                    bake(g, *op, {curve->id(), CurveShape::kCreate, -1});
                    opName = nullptr;
                }
                g.evaluate();
                if (splinesOf(g.value({curve->id(), CurveShape::kLocal, -1})).empty())
                    log::warn("cable: no cables made from {}", anchor->path());
                json pipeMesh = nullptr;
                if (a.get<bool>("pipe", false))
                {
                    json r = ops::execute("pipe", {{"args", {curve->path()}},
                                                   {"preset", "cable"},
                                                   {"radius", a.get<double>("pipeRadius", 0.6)},
                                                   {"sides", a.get<int>("pipeSides", 6)},
                                                   {"name", curve->name() + "Pipe"}});
                    if (r.is_array() && !r.empty())
                        pipeMesh = r[0];
                }
                out.push_back(json::array({curve->path(), opName, anchor->path(), pipeMesh}));
                made.push_back(curve->id());
            }
            g.select(made);
            return out;
        },
    });
    registerOptionLayout();
}
} // namespace lila::curve
