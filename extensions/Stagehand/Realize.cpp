// instancesToObjects: the instances a geometry-nodes modifier makes (Fracture pieces, scatters, arrangements) as scene
// objects, so physics, the Push tool and Unreal placements work on each one.

#include "Stagehand.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <algorithm>
#include <map>

using namespace lila;

namespace stagehand
{
namespace
{
using RealizeArg = ArgType;

/** One referenced mesh and where each of its instances sits, in the modifier's (the mesh node's) space. */
struct Piece
{
    std::shared_ptr<const geo::Mesh> mesh;
    std::vector<glm::dmat4> placed;
};

void collectPieces(const gn::Instances& inst, const glm::dmat4& xf, std::vector<Piece>& out, std::map<const geo::Mesh*, size_t>& index, int depth)
{
    // Instances nest but never cycle; the cap only guards a malformed geometry.
    if (depth > 32)
        return;
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        if (r < 0 || size_t(r) >= inst.references.size() || !inst.references[size_t(r)])
            continue;
        const gn::Geometry& ref = *inst.references[size_t(r)];
        const glm::dmat4 m = xf * inst.transforms[k];
        if (ref.mesh && ref.mesh->numFaces() > 0)
        {
            auto [it, added] = index.try_emplace(ref.mesh.get(), out.size());
            if (added)
                out.push_back({ref.mesh, {}});
            out[it->second].placed.push_back(m);
        }
        if (ref.instances)
            collectPieces(*ref.instances, m, out, index, depth + 1);
    }
}

/** The mesh as a scene mesh holds it: compact, without the anonymous attributes fields leave behind. */
scene::MeshPtr cleanedPiece(const std::shared_ptr<const geo::Mesh>& m)
{
    geo::Mesh copy = *m;
    gn::stripAnonymous(copy);
    copy.compact();
    return std::make_shared<const geo::Mesh>(std::move(copy));
}

/** The geometry-nodes modifier at the top of a mesh's stack (or the modifier itself) and the mesh it feeds. */
std::pair<Node*, Node*> gnModifierAndMesh(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError("instancesToObjects: no node named '" + name + "'");
    if (gn::isGnModifier(*n))
        return {n, g.node(scene::modifiers::meshOf(g, n->id()))};
    if (!n->type().isA("mesh"))
        throw CommandError("instancesToObjects: " + n->path() + " is neither a mesh nor a geometry-nodes modifier");
    const std::vector<NodeId> stack = scene::modifiers::stack(g, n->id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
            return {m, n};
    throw CommandError("instancesToObjects: " + n->path() + " has no geometry-nodes modifier");
}

std::string madePath(const json& made) { return made.is_array() ? made.at(0).get<std::string>() : made.get<std::string>(); }
} // namespace

void registerRealizeCommands(ext::Context& ctx)
{
    std::vector<std::string> roles = {"dynamic", "static", "kinematic", "off"};
    std::vector<std::string> mats;
    for (const Material& m : materials())
        mats.emplace_back(m.name);

    ctx.registerCommand({
        .name = "instancesToObjects",
        .doc = "Turn the instances a geometry-nodes modifier makes (Fracture pieces, scattered props, an arrangement) into "
               "objects in the scene, one per instance, under a new group, where they were.\n\n"
               "mode instance makes the copies of each referenced mesh scene instances of one another (one mesh, many "
               "placements, sent to Unreal as instances); unique gives every object its own mesh. role= and material= "
               "give each a stagehandBody, so the pieces can be settled, pushed and dropped. The source mesh is hidden. "
               "One undo step. Returns group, objects, meshes (distinct referenced meshes) and count.",
        .positionalDoc = "mesh with a geometry-nodes modifier, or the modifier (default the selection)",
        .flags = {{"mode", "m", RealizeArg::String, "instance", "instance: copies of one mesh share it. unique: each object gets its own.", {"instance", "unique"}},
                  {"name", "n", RealizeArg::String, nullptr, "Name of the group (default <source>_Objects)."},
                  {"role", "r", RealizeArg::String, nullptr, "Give each object a body with this role.", roles},
                  {"material", "ma", RealizeArg::String, nullptr, "Give each object a body of this material (role dynamic unless role= says).", mats},
                  {"hide", "hi", RealizeArg::Bool, true, "Hide the source mesh."},
                  {"limit", "li", RealizeArg::Int, int64_t(100000), "Refuse to make more objects than this.", {}, {}, 1.0}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("instancesToObjects: give one mesh or modifier");
            g.evaluate();
            auto [modifier, mesh] = gnModifierAndMesh(g, names[0]);
            if (!mesh)
                throw CommandError("instancesToObjects: " + modifier->path() + " feeds no mesh");
            const gn::GeometryPtr geometry = gn::modifierGeometry(modifier->id());
            if (!geometry || !geometry->instances || geometry->instances->size() == 0)
                throw CommandError("instancesToObjects: " + modifier->path() + " makes no instances");
            std::vector<Piece> pieces;
            std::map<const geo::Mesh*, size_t> index;
            collectPieces(*geometry->instances, glm::dmat4(1.0), pieces, index, 0);
            size_t count = 0;
            for (const Piece& p : pieces)
                count += p.placed.size();
            if (count == 0)
                throw CommandError("instancesToObjects: " + modifier->path() + "'s instances hold no meshes");
            if (int64_t(count) > a.get<int64_t>("limit", 100000))
                throw CommandError("instancesToObjects: " + std::to_string(count) + " instances, more than limit=" + std::to_string(a.get<int64_t>("limit", 100000)));
            const std::string mode = a.get<std::string>("mode", "instance");

            const glm::dmat4 world = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            const std::string meshPath = mesh->path();
            const json group = ops::execute("group", {{"empty", true}, {"name", a.get<std::string>("name", mesh->name() + "_Objects")}});
            const NodeId groupId = g.find(madePath(group))->id();
            const std::string base = mesh->name() + "_p";
            json objects = json::array();
            for (size_t k = 0; k < pieces.size(); ++k)
            {
                const Piece& p = pieces[k];
                const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", base + std::to_string(k + 1) + "_1"}});
                Node* first = g.find(madePath(made));
                g.setValue({first->id(), scene::MeshNode::kInMesh, -1}, cleanedPiece(p.mesh));
                ops::execute("parent", {{"args", {first->path(), g.node(groupId)->path()}}});
                if (a.has("role") || a.has("material"))
                {
                    json body = {{"args", {first->path()}}, {"role", a.get<std::string>("role", "dynamic")}};
                    if (a.has("material"))
                        body["material"] = a.get<std::string>("material");
                    ops::execute("physicsBody", body);
                }
                std::vector<NodeId> members = {first->id()};
                // Copies come in runs: duplicate takes at most 10000 at a time.
                for (size_t done = 1; done < p.placed.size();)
                {
                    const size_t n = std::min<size_t>(p.placed.size() - done, 10000);
                    const json copies = ops::execute("duplicate", {{"args", {g.node(members.front())->path()}},
                                                                   {"count", int64_t(n)},
                                                                   {"mode", mode == "unique" ? "copy" : "instance"}});
                    for (const json& c : copies)
                        members.push_back(g.find(c.get<std::string>())->id());
                    done += n;
                }
                for (size_t i = 0; i < members.size() && i < p.placed.size(); ++i)
                {
                    setWorld(g, members[i], world * p.placed[i]);
                    objects.push_back(g.node(members[i])->path());
                }
            }
            if (a.get<bool>("hide", true))
                ops::execute("setAttr", {{"args", {meshPath + ".visibility", false}}});
            g.select({groupId});
            return {{"group", g.node(groupId)->path()}, {"objects", objects}, {"meshes", int64_t(pieces.size())}, {"count", int64_t(count)}};
        },
        .examples = {"r = instancesToObjects('Wall', role='dynamic', material='stone')\nsettle(r['objects'])",
                     "instancesToObjects('Scatter', mode='unique')"},
    });
}
} // namespace stagehand
