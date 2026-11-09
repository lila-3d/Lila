#include "layout/SoftNet.h"

#include "core/Profile.h"
#include "deform/SoftNet.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "layout/LayoutNode.h"
#include "layout/Path.h"
#include "layout/Resolve.h"
#include "ops/Command.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"
#include "scene/Node.h"

#include <cmath>
#include <map>
#include <mutex>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;
namespace sn = lila::deform::softnet;

/** What a bind fixed: the point count, the knots, the mean edge length, the reach it picks, and every curve sample. */
struct Bind
{
    size_t points = 0;
    size_t knots = 0;
    double edge = 1.0;
    double autoReach = 1.0;
    std::vector<sn::Sample> samples;
};

json bindJson(const Bind& b)
{
    json samples = json::array();
    for (const sn::Sample& s : b.samples)
        samples.push_back({s.points[0], s.points[1], s.points[2], s.bary.x, s.bary.y, s.bary.z, s.knot[0], s.knot[1], s.weight[0], s.weight[1]});
    return {{"points", b.points}, {"knots", b.knots}, {"edge", b.edge}, {"autoReach", b.autoReach}, {"samples", samples}};
}

Bind bindFrom(const std::string& text)
{
    Bind b;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        return b;
    b.points = j.value("points", size_t(0));
    b.knots = j.value("knots", size_t(0));
    b.edge = j.value("edge", 1.0);
    b.autoReach = j.value("autoReach", 1.0);
    for (const json& s : j.value("samples", json::array()))
        if (s.is_array() && s.size() == 10)
            b.samples.push_back({.points = {s[0].get<uint32_t>(), s[1].get<uint32_t>(), s[2].get<uint32_t>()},
                                 .bary = {s[3].get<double>(), s[4].get<double>(), s[5].get<double>()},
                                 .knot = {s[6].get<int>(), s[7].get<int>()},
                                 .weight = {s[8].get<double>(), s[9].get<double>()}});
    return b;
}

bool nearIdentity(const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(m[c][r] - (c == r ? 1.0 : 0.0)) > 1e-12)
                return false;
    return true;
}

class SoftNetNode final : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();
        const NodeType& t = type();
        // Knots at rest leave the surface exactly as it came in.
        std::map<int, glm::dmat4> controls;
        for (const auto& [i, v] : elements(t.attrIndex("knotMatrix")))
            if (const glm::dmat4 m = std::get<glm::dmat4>(v); !nearIdentity(m))
                controls[i] = m;
        if (controls.empty())
            return;

        std::lock_guard lock(mMutex);
        const std::string text = as<std::string>(t.attrIndex("binding"));
        const double reach = as<double>(t.attrIndex("reach")), stiffness = as<double>(t.attrIndex("stiffness"));
        if (text != mText || reach != mReach || stiffness != mStiffness)
        {
            mText = text;
            mReach = reach;
            mStiffness = stiffness;
            mBind = bindFrom(text);
            mSolver = sn::Solver();
            if (mBind.points == points.size())
                mSolver.prepare(in, mBind.samples, mBind.edge, reach > 0.0 ? reach : mBind.autoReach, stiffness);
        }
        if (mBind.points != points.size())
            throw std::runtime_error("the mesh changed since the Soft Net was bound; make it again");
        if (!mSolver.ready())
            return;

        std::map<int, glm::dmat4> frames;
        for (const auto& [i, v] : elements(t.attrIndex("knotFrame")))
            frames[i] = std::get<glm::dmat4>(v);
        const glm::dmat4 toObject = glm::inverse(as<glm::dmat4>(t.attrIndex("meshMatrix")));
        // Each knot's control moves, turns and scales in its frame on the surface; in object space that is F C F^-1.
        std::vector<glm::dmat4> turn(mBind.knots, glm::dmat4(1.0));
        for (const auto& [k, c] : controls)
            if (k >= 0 && size_t(k) < turn.size())
            {
                const auto f = frames.find(k);
                const glm::dmat4 frame = toObject * (f != frames.end() ? f->second : glm::dmat4(1.0));
                turn[size_t(k)] = frame * c * glm::inverse(frame);
            }
        std::vector<glm::dvec3> deltas(mBind.samples.size());
        for (size_t s = 0; s < mBind.samples.size(); ++s)
        {
            const sn::Sample& smp = mBind.samples[s];
            const glm::dvec3 p = sn::at(smp, points);
            glm::dvec3 q(0.0);
            for (int e = 0; e < 2; ++e)
                if (smp.knot[size_t(e)] >= 0 && size_t(smp.knot[size_t(e)]) < turn.size())
                    q += smp.weight[size_t(e)] * glm::dvec3(turn[size_t(smp.knot[size_t(e)])] * glm::dvec4(p, 1.0));
                else
                    q += smp.weight[size_t(e)] * p;
            deltas[s] = q - p;
        }
        const std::vector<glm::dvec3> moved = mSolver.solve(deltas);
        for (size_t i = 0; i < points.size() && i < moved.size(); ++i)
            points[i] += moved[i];
    }

private:
    mutable std::mutex mMutex;
    mutable std::string mText;
    mutable double mReach = -1.0, mStiffness = -1.0;
    mutable Bind mBind;
    mutable sn::Solver mSolver;
};

/** The binding of the layout's lines on mesh (the deformer's input as it is now): curve samples and knots by junction. */
Bind bindLines(const Resolved& resolved, const geo::Mesh& mesh, double spacing, std::vector<uint32_t>& knotJunctions, std::vector<glm::dvec3>& knotPositions)
{
    LILA_ZONE();
    Bind b;
    b.points = mesh.pointCapacity();
    const geo::MeshBvh bvh(mesh);
    const geo::Triangulation& tris = bvh.triangles();
    auto sampleAt = [&](const glm::dvec3& p, sn::Sample& out) {
        geo::BvhClosest hit;
        if (!bvh.closestPoint(p, hit) || hit.triangle == geo::kInvalid)
            return false;
        for (int k = 0; k < 3; ++k)
            out.points[size_t(k)] = uint32_t(mesh.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + size_t(k)]));
        out.bary = hit.bary;
        return true;
    };
    std::map<uint32_t, int> knotOf;
    for (const ResolvedJunction& j : resolved.junctions())
    {
        knotOf[j.id] = int(knotJunctions.size());
        knotJunctions.push_back(j.id);
        knotPositions.push_back(j.position);
        sn::Sample s{.knot = {knotOf[j.id], knotOf[j.id]}, .weight = {1.0, 0.0}};
        if (sampleAt(j.position, s))
            b.samples.push_back(s);
    }
    b.knots = knotJunctions.size();
    const double step = spacing > 0.0 ? spacing : std::max(resolved.spacing() * 2.0, 1e-6);
    for (const ResolvedRun& r : resolved.runs())
    {
        const auto ka = knotOf.find(r.a), kb = knotOf.find(r.b);
        if (ka == knotOf.end() || kb == knotOf.end() || r.path.points.size() < 2)
            continue;
        const double total = polylineLength(r.path.points);
        const std::vector<glm::dvec3> along = resample(r.path.points, step);
        double s = 0.0;
        for (size_t i = 0; i < along.size(); ++i)
        {
            if (i > 0)
                s += glm::length(along[i] - along[i - 1]);
            // The ends are the knots' own samples.
            if (i == 0 || i + 1 == along.size())
                continue;
            const std::array<double, 2> w = sn::knotWeights(total > 0.0 ? s / total : 0.0);
            sn::Sample smp{.knot = {ka->second, kb->second}, .weight = w};
            if (sampleAt(along[i], smp))
                b.samples.push_back(smp);
        }
    }
    double sum = 0.0;
    size_t count = 0;
    for (geo::Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e))
        {
            sum += glm::length(mesh.position(mesh.edgePoint(e, 1)) - mesh.position(mesh.edgePoint(e, 0)));
            ++count;
        }
    b.edge = count ? sum / double(count) : 1.0;
    const geo::Bounds box = mesh.bounds();
    b.autoReach = std::max(glm::length(box.max - box.min) / 6.0, b.edge);
    return b;
}
} // namespace

void registerSoftNetTypes()
{
    if (types::find(kSoftNetType))
        return;
    const uint32_t keyable = AttrInput | AttrStorable | AttrKeyable;
    const uint32_t hiddenArray = AttrInput | AttrHidden | AttrArray;
    deformers::registerDeformer(
        {kSoftNetType, "topology-ring",
         "Soft Net: a Panel Layout's lines as curves that shape the surface after the skin. Each line sample holds to the mesh "
         "as it is (never cut); move, turn or scale a knot control and the lines through it follow, the surface following "
         "the lines and fading over the reach.",
         {{.name = "reach", .type = PlugType::Float, .defaultValue = 0.0, .flags = keyable,
           .doc = "How far from a line the surface follows it, cm; 0 is a sixth of the mesh's size.", .minValue = 0.0},
          {.name = "stiffness", .type = PlugType::Float, .defaultValue = 1.0, .flags = keyable,
           .doc = "How closely the surface meets the lines against keeping smooth.", .minValue = 0.0},
          {.name = "knotFrame", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenArray,
           .doc = "Each knot's frame on the surface under the net (its surface pin's world matrix)."},
          {.name = "knotMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenArray,
           .doc = "Each knot control's local matrix: its move, turn and scale in its frame."},
          {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
           .doc = "The mesh's world matrix at bind, which the knot frames are placed by."},
          {.name = "binding", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
           .doc = "The bind: every line sample's triangle and weights, and its knots."}},
         [] { return std::make_unique<SoftNetNode>(); }});
}

void registerSoftNetCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "softNet",
        .doc = "Make a Soft Net on a mesh from its Panel Layout: the layout's lines become curves bound to the surface as it is "
               "(no cuts), every junction a knot with a control riding the surface under the net. Added on top of the stack, so "
               "it shapes the skinned mesh. Move, turn, scale or key the knot controls. Returns {softNet, knots, samples}.",
        .positionalDoc = "mesh",
        .flags = {{"layout", "ly", T::String, nullptr, "The Panel Layout whose lines make the net; default the mesh's topmost."},
                  {"reach", "r", T::Float, 0.0, "How far from a line the surface follows it, cm; 0 is a sixth of the mesh's size.", {}, {}, 0.0},
                  {"stiffness", "st", T::Float, 1.0, "How closely the surface meets the lines.", {}, {}, 0.0},
                  {"spacing", "sp", T::Float, 0.0, "Distance between line samples, cm; 0 is twice the layout's.", {}, {}, 0.0},
                  {"name", "n", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.objects().empty())
                throw CommandError("softNet: give a mesh");
            Node& mesh = nodeNamed(a.objects().front(), "softNet");
            if (!mesh.type().isA("mesh"))
                throw CommandError("softNet: " + mesh.path() + " is not a mesh");
            Node* layoutNode = findLayoutNode(g, a.has("layout") ? nodeNamed(a.get<std::string>("layout"), "softNet").id() : mesh.id());
            if (!layoutNode)
                throw CommandError("softNet: " + mesh.path() + " has no Panel Layout; draw its lines with the Layout Pencil first");
            const std::shared_ptr<const Resolved> resolved = resolvedLayout(g, layoutNode->id());
            if (!resolved || resolved->runs().empty())
                throw CommandError("softNet: the Panel Layout has no lines");

            const std::string name = a.has("name") ? a.get<std::string>("name") : std::string("SoftNet");
            const std::vector<NodeId> made = deformers::addDeformer(g, kSoftNetType, {mesh.id()}, name);
            if (made.empty())
                throw CommandError("softNet: could not add the deformer");
            Node& d = *g.node(made.front());
            g.evaluate();
            const MeshPtr in = d.as<MeshPtr>(DeformerNode::kInMesh);
            if (!in)
                throw CommandError("softNet: " + mesh.path() + " has no mesh to bind");
            std::vector<uint32_t> junctions;
            std::vector<glm::dvec3> positions;
            const Bind b = bindLines(*resolved, *in, a.get<double>("spacing", 0.0), junctions, positions);
            if (b.samples.empty())
                throw CommandError("softNet: no line sample lies on " + mesh.path());
            g.setValue({d.id(), d.type().attrIndex("binding"), -1}, bindJson(b).dump());
            g.setValue({d.id(), d.type().attrIndex("reach"), -1}, a.get<double>("reach", 0.0));
            g.setValue({d.id(), d.type().attrIndex("stiffness"), -1}, a.get<double>("stiffness", 1.0));
            // The mesh's matrix at bind, held rather than connected: the mesh's output depends on the net, so nothing on the
            // mesh may feed the net or its knots' frames.
            const glm::dmat4 toWorld = worldOf(mesh);
            g.setValue({d.id(), d.type().attrIndex("meshMatrix"), -1}, toWorld);

            // Each knot's frame rides the surface the net shapes, not its result, so a knot moves its lines once.
            const std::optional<PlugRef> upstream = g.source({d.id(), DeformerNode::kInMesh, -1});
            if (!upstream || upstream->node == mesh.id())
                throw CommandError("softNet: " + mesh.path() + " has nothing under the net to ride; add it after a skin, a modifier or the Panel Layout");
            json knots = json::array();
            for (size_t k = 0; k < junctions.size(); ++k)
            {
                const glm::dvec3 at = glm::dvec3(toWorld * glm::dvec4(positions[k], 1.0));
                const std::string base = d.name() + "_knot" + std::to_string(k + 1);
                const json pins = ops::execute("pin", {{"args", {mesh.path()}},
                                                       {"source", "surface"},
                                                       {"surfaceMode", "nearest"},
                                                       {"position", {at.x, at.y, at.z}},
                                                       {"name", base + "_frame"}});
                const json first = pins.is_array() && !pins.empty() ? pins[0] : pins;
                Node* locator = first.is_string() ? g.find(first.get<std::string>()) : nullptr;
                if (!locator)
                    throw CommandError("softNet: could not make knot " + std::to_string(k + 1) + "'s frame");
                const std::optional<PlugRef> pinPlug = g.source({locator->id(), Transform::kTranslate, -1});
                Node* pin = pinPlug ? g.node(pinPlug->node) : nullptr;
                if (!pin)
                    throw CommandError("softNet: knot " + std::to_string(k + 1) + "'s frame has no surface pin");
                const int pinMatrix = pin->type().attrIndex("meshMatrix");
                g.disconnect({pin->id(), pinMatrix, -1});
                g.setValue({pin->id(), pinMatrix, -1}, toWorld);
                g.connect(*upstream, {pin->id(), pin->type().attrIndex("inMesh"), -1}, true);
                Node* control = g.createNode(*types::find("transform"), base, locator->id());
                g.connect({control->id(), Transform::kMatrix, -1}, {d.id(), d.type().attrIndex("knotMatrix"), -1, int(k)});
                g.connect({pin->id(), pin->type().attrIndex("outMatrix"), -1}, {d.id(), d.type().attrIndex("knotFrame"), -1, int(k)});
                knots.push_back(control->path());
            }
            return {{"softNet", d.path()}, {"knots", knots}, {"samples", b.samples.size()}};
        },
        .examples = {"softNet(\"Mochi\", reach=4.0)"},
    });
}
} // namespace lila::layout
