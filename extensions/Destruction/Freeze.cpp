// freezeDestruction: a simulation's pieces where they came to rest (or on a chosen frame), turned into plain static meshes
// for level dressing, each with its own UCX_ collision hull, ready to send to Unreal.

#include "Destruction.h"
#include "PieceTransform.h"
#include "RigidState.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>

#include <algorithm>
#include <format>
#include <map>

using namespace lila;

namespace destruction
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;

/** One frozen piece: its mesh in its own space and where it sits in the destruction's space. */
struct Item
{
    std::shared_ptr<const geo::Mesh> mesh;
    glm::dmat4 transform{1.0};
};

void collectInstances(const gn::Geometry& g, const glm::dmat4& parent, std::vector<Item>& out)
{
    if (!g.instances)
        return;
    const gn::Instances& inst = *g.instances;
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        if (r < 0 || size_t(r) >= inst.references.size() || !inst.references[size_t(r)])
            continue;
        const gn::Geometry& ref = *inst.references[size_t(r)];
        const glm::dmat4 m = parent * inst.transforms[k];
        if (ref.mesh && ref.mesh->numFaces() > 0)
            out.push_back({ref.mesh, m});
        collectInstances(ref, m, out);
    }
}

/** The modifier running the mesh's destruction (a simulation), else its top geometry-nodes modifier. */
Node* modifierOf(Graph& g, Node& target)
{
    if (gn::isGnModifier(target))
        return &target;
    Node* top = nullptr;
    for (scene::NodeId m : scene::modifiers::stack(g, target.id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
        {
            if (scene::sim::isSimulation(*mod))
                return mod;
            top = mod;
        }
    return top;
}

/** Every active piece moving slower than this is at rest: cm/s and rad/s. */
constexpr double kRestSpeed = 2.0;
constexpr double kRestSpin = 0.1;

bool atRest(Graph& g, scene::NodeId modifier, int64_t frame)
{
    const std::vector<uint8_t> bytes = gn::simulationState(g, modifier, frame);
    const StateView v(bytes);
    if (!v.valid())
        return false;
    for (size_t k = 0; k < v.header().bodies; ++k)
    {
        const BodyRecord b = v.body(k);
        if (!b.active)
            continue;
        if (glm::length(glm::dvec3(b.velocity[0], b.velocity[1], b.velocity[2])) * 100.0 > kRestSpeed ||
            glm::length(glm::dvec3(b.angularVelocity[0], b.angularVelocity[1], b.angularVelocity[2])) > kRestSpin)
            return false;
    }
    return true;
}

/**
 * The frame a simulation comes to rest on: the start of the still run that ends the range, or, when the range ends in
 * motion, the first still frame up to ten seconds past it. Second: whether it did come to rest.
 */
std::pair<int64_t, bool> restFrame(Graph& g, scene::NodeId modifier, int64_t first, int64_t last)
{
    if (atRest(g, modifier, last))
    {
        int64_t f = last;
        while (f > first && atRest(g, modifier, f - 1))
            --f;
        return {f, true};
    }
    const int64_t cap = last + int64_t(std::llround(scene::sim::framesPerSecond() * 10.0));
    for (int64_t f = last + 1; f <= cap; ++f)
        if (atRest(g, modifier, f))
            return {f, true};
    return {cap, false};
}

/** The pieces from the solver's own inputs and state, for a destruction whose output is realized. */
std::vector<Item> solverPieces(Graph& g, scene::NodeId modifier, int64_t frame)
{
    std::vector<Item> out;
    std::shared_ptr<const gn::SimInputs> in = gn::simulationInputs(modifier);
    if (!in)
        return out;
    gn::GeometryPtr pieces = in->geometry("pieces");
    if (!pieces->instances)
        return out;
    const gn::Instances& inst = *pieces->instances;
    const std::vector<uint8_t> bytes = gn::simulationState(g, modifier, frame);
    const StateView v(bytes);
    const bool moved = v.valid() && v.header().bodies == inst.size();
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        if (r < 0 || size_t(r) >= inst.references.size() || !inst.references[size_t(r)])
            continue;
        const gn::Geometry ref = gn::realize(*inst.references[size_t(r)]);
        if (!ref.mesh || ref.mesh->numFaces() == 0)
            continue;
        const glm::dmat4 m = moved ? compose(v.position(k), v.rotation(k), decompose(inst.transforms[k]).scale) : inst.transforms[k];
        out.push_back({ref.mesh, m});
    }
    return out;
}

double sizeOf(const geo::Mesh& m, const glm::dmat4& t)
{
    const geo::Bounds b = m.bounds();
    if (!b.valid())
        return 0.0;
    const glm::dvec3 s(glm::length(glm::dvec3(t[0])), glm::length(glm::dvec3(t[1])), glm::length(glm::dvec3(t[2])));
    return glm::length(b.size() * s);
}

std::vector<glm::dvec3> pointsOf(const geo::Mesh& m)
{
    std::vector<glm::dvec3> pts;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            pts.push_back(m.position(p));
    return pts;
}
} // namespace

void registerFreeze(ext::Context& ctx)
{
    using AT = ArgType;
    ctx.registerCommand({
        .name = "freezeDestruction",
        .doc = "Turn a destruction into static level dressing: its pieces where they came to rest (or on a given frame) become "
               "separate meshes under one group, each with a UCX_ collision hull, ready to place in Unreal. Instances share one "
               "mesh per distinct piece (debris made from a few shapes), objects give each piece a mesh of its own. Works on any "
               "geometry-nodes result made of instances too, at the current frame. The destruction is hidden, not deleted. "
               "Returns {group, pieces, meshes, frame, settled}.",
        .positionalDoc = "destruction mesh or its modifier (default the selection)",
        .flags = {{"frame", "f", AT::Int, nullptr, "Frame to freeze. Default: where the simulation comes to rest."},
                  {"end", "e", AT::Int, nullptr, "Last frame to look for rest in before going past it. Default: the end of the playback range."},
                  {"meshes", "", AT::String, "instances", "instances: pieces of the same shape share a mesh; objects: every piece its own.",
                   {"instances", "objects"}},
                  {"collision", "c", AT::Bool, true, "A UCX_ convex hull under each piece."},
                  {"hullPoints", "", AT::Int, 32, "Most points in a collision hull.", {}, {}, 4.0, 255.0},
                  {"smallest", "", AT::Float, 0.0, "Pieces smaller across than this are left out, cm.", {}, {}, 0.0},
                  {"debris", "", AT::String, "with", "with: pieces and addDebris chunks; only: just the chunks (a group of their own); without: just the pieces.",
                   {"with", "only", "without"}},
                  {"name", "n", AT::String, nullptr, "The group's name. Default: the mesh's plus _Frozen."},
                  {"hide", "", AT::Bool, true, "Hide the destruction afterwards."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("freezeDestruction: give one destruction");
            Node* target = g.find(names[0]);
            if (!target)
                throw CommandError("freezeDestruction: no node named '" + names[0] + "'");
            g.evaluate();
            Node* mod = modifierOf(g, *target);
            if (!mod)
                throw CommandError("freezeDestruction: " + target->path() + " has no geometry-nodes modifier");
            const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id()) != scene::kNoNode ? scene::modifiers::meshOf(g, mod->id()) : target->id();
            Node* mesh = g.node(meshId);
            const bool simulated = scene::sim::isSimulation(*mod);
            const double fps = scene::sim::framesPerSecond();
            const double before = g.time();

            int64_t frame = scene::sim::frameOf(before);
            bool settled = true;
            if (a.has("frame"))
                frame = a.get<int64_t>("frame");
            else if (simulated)
            {
                const int64_t first = scene::sim::frameOf(scene::anim::playback().minTime);
                const int64_t last = a.has("end") ? a.get<int64_t>("end") : scene::sim::frameOf(scene::anim::playback().maxTime);
                std::tie(frame, settled) = restFrame(g, mod->id(), first, std::max(first, last));
            }

            std::vector<Item> items;
            {
                if (simulated)
                    gn::simulationState(g, mod->id(), frame);
                g.setTime(double(frame) / fps);
                for (int k = 0; k < 3; ++k)
                {
                    g.evaluate();
                    if (!simulated || gn::simulationStateReady(mod->id(), g.time()))
                        break;
                }
                if (gn::GeometryPtr result = gn::modifierGeometry(mod->id()))
                    collectInstances(*result, glm::dmat4(1.0), items);
                if (items.empty() && simulated)
                    items = solverPieces(g, mod->id(), frame);
                g.setTime(before);
                g.evaluate();
            }
            if (items.empty())
                throw CommandError("freezeDestruction: " + mesh->path() + " has no pieces to freeze");

            const bool share = a.get<std::string>("meshes", "instances") == "instances";
            const bool collision = a.get<bool>("collision", true);
            const int hullPoints = int(std::clamp<int64_t>(a.get<int64_t>("hullPoints", 32), 4, 255));
            const double smallest = std::max(0.0, a.get<double>("smallest", 0.0));
            const std::string debris = a.get<std::string>("debris", "with");
            const std::string base = a.get<std::string>("name", mesh->name() + (debris == "only" ? "_Debris" : "_Frozen"));

            Graph::Batch batch(g);
            Node* group = g.createNode(*scene::types::find("transform"), base);
            scene::Transform::setLocalMatrix(g, group->id(), mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            const std::string pieceBase = mesh->name() + (debris == "only" ? "_Chunk" : "_Piece");
            std::map<const geo::Mesh*, std::pair<scene::NodeId, scene::NodeId>> firstOf; // mesh, its collision
            std::map<const geo::Mesh*, scene::MeshPtr> hulls;
            int64_t made = 0;
            for (const Item& it : items)
            {
                const geo::Attribute* flag = it.mesh->pointAttrs().find("debris");
                const bool chunk = flag && flag->type() == geo::AttrType::Int && flag->size() > 0 && flag->values<int32_t>()[0] != 0;
                if ((debris == "only" && !chunk) || (debris == "without" && chunk))
                    continue;
                if (smallest > 0.0 && sizeOf(*it.mesh, it.transform) < smallest)
                    continue;
                Node* n = g.createNode(*scene::types::find("mesh"), std::format("{}_{:03}", pieceBase, made + 1), group->id());
                auto first = firstOf.find(it.mesh.get());
                if (share && first != firstOf.end())
                    scene::instancing::connectInstance(g, first->second.first, n->id());
                else
                    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(it.mesh));
                scene::Transform::setLocalMatrix(g, n->id(), it.transform);
                scene::NodeId hullId = scene::kNoNode;
                if (collision)
                {
                    Node* c = g.createNode(*scene::types::find("mesh"), "UCX_" + n->name(), n->id());
                    hullId = c->id();
                    if (share && first != firstOf.end() && first->second.second != scene::kNoNode)
                        scene::instancing::connectInstance(g, first->second.second, c->id());
                    else
                    {
                        scene::MeshPtr& hull = hulls[it.mesh.get()];
                        if (!hull)
                        {
                            const std::vector<glm::dvec3> pts = pointsOf(*it.mesh);
                            hull = std::make_shared<geo::Mesh>(geo::convexHullMesh(pts, hullPoints));
                        }
                        g.setValue({c->id(), scene::MeshNode::kInMesh, -1}, hull);
                    }
                }
                if (first == firstOf.end())
                    firstOf[it.mesh.get()] = {n->id(), hullId};
                ++made;
            }
            if (a.get<bool>("hide", true))
                ops::execute("hide", {{"args", json::array({mesh->path()})}});
            g.select({group->id()});
            return {{"group", group->path()}, {"pieces", made}, {"meshes", int64_t(firstOf.size())}, {"frame", frame}, {"settled", settled}};
        },
        .examples = {"freezeDestruction('Wall')", "freezeDestruction('Wall', frame=60, meshes='objects', smallest=5)"},
    });
}
} // namespace destruction
