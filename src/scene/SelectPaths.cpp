#include "scene/Components.h"

#include "geo/ComponentPaths.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/Modifiers.h"

#include <string>

namespace lila::scene::components
{
namespace
{
struct Target
{
    NodeId node = kNoNode;
    MeshPtr mesh; // the cage, which component ids index
};

Target meshArg(const Args& a, const char* cmd)
{
    auto objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": no mesh");
    Graph& g = Graph::current();
    Node* n = g.find(objs.back());
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + objs.back() + "'");
    if (!n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + objs.back() + " is not a mesh");
    g.evaluate();
    Target t{n->id(), modifiers::cage(g, n->id())};
    if (!t.mesh)
        throw CommandError(std::string(cmd) + ": " + objs.back() + " has no mesh");
    return t;
}

Op opArg(const Args& a, Op fallback)
{
    if (a.get<bool>("add"))
        return Op::Add;
    if (a.get<bool>("deselect"))
        return Op::Remove;
    if (a.get<bool>("toggle"))
        return Op::Toggle;
    return fallback;
}

void selectAndShow(NodeId node, ComponentMode kind, const Ids& ids, Op op)
{
    select(node, kind, ids, op);
    setMode(kind);
}
} // namespace

void registerPathCommands()
{
    using T = ArgType;
    const FlagDef opFlags[] = {
        {"add", "", T::Bool, false, "Add to the selection."},
        {"deselect", "", T::Bool, false, "Remove from the selection."},
        {"toggle", "", T::Bool, false, "Toggle."},
    };

    std::vector<FlagDef> loopFlags = {
        {"edge", "", T::Int, nullptr, "The edge the loop runs through."},
        {"ring", "", T::Bool, false, "The edge ring instead of the loop."},
        {"faces", "", T::Bool, false, "The faces of the edge's ring: the face loop crossing the edge."},
    };
    loopFlags.insert(loopFlags.end(), std::begin(opFlags), std::end(opFlags));
    ops::registerCommand({
        .name = "selectLoopThrough",
        .doc = "Select the edge loop (or ring) through one edge of a mesh, or with faces=True the face loop crossing it. "
               "Without add/deselect/toggle it replaces the selection of that kind. Returns the ids.",
        .positionalDoc = "mesh",
        .flags = loopFlags,
        .run = [](const Args& a) -> json {
            Target t = meshArg(a, "selectLoopThrough");
            if (!a.has("edge"))
                throw CommandError("selectLoopThrough: give edge=");
            geo::Index e = geo::Index(a.get<int64_t>("edge"));
            if (!t.mesh->edgeValid(e))
                throw CommandError("selectLoopThrough: no edge " + std::to_string(e));
            bool faces = a.get<bool>("faces");
            const bool ring = !faces && a.get<bool>("ring");
            Ids ids = faces ? geo::faceLoop(*t.mesh, e) : ring ? geo::edgeRing(*t.mesh, e) : geo::edgeLoop(*t.mesh, e);
            ids = isolation::clipLoop(*t.mesh, isolation::activeVisibility(t.node, *t.mesh), ids, e, faces ? ComponentMode::Face : ComponentMode::Edge, ring);
            UndoChunk chunk("Select Loop");
            selectAndShow(t.node, faces ? ComponentMode::Face : ComponentMode::Edge, ids, opArg(a, Op::Replace));
            return ids;
        },
    });

    ops::registerCommand({
        .name = "selectPathBetween",
        .doc = "Select the components between two of a mesh's vertices, edges or faces: the shorter way along a loop or ring "
               "they share, else the shortest path across the surface. Adds to the selection unless replace or deselect. Returns the ids in order.",
        .positionalDoc = "mesh",
        .flags = {
            {"kind", "", T::String, "edge", "vertex | edge | face", {"vertex", "edge", "face"}},
            {"from", "", T::Int, nullptr, "Id the path starts at."},
            {"to", "", T::Int, nullptr, "Id the path ends at."},
            {"replace", "", T::Bool, false, "Replace the selection of that kind."},
            {"deselect", "", T::Bool, false, "Remove the path from the selection."},
        },
        .run = [](const Args& a) -> json {
            Target t = meshArg(a, "selectPathBetween");
            if (!a.has("from") || !a.has("to"))
                throw CommandError("selectPathBetween: give from= and to=");
            ComponentMode kind = ComponentMode::Edge;
            try
            {
                kind = modeFromName(a.get<std::string>("kind", "edge"));
            }
            catch (const std::invalid_argument&)
            {
                throw CommandError("selectPathBetween: kind must be vertex, edge or face");
            }
            geo::Index from = geo::Index(a.get<int64_t>("from")), to = geo::Index(a.get<int64_t>("to"));
            Ids path = kind == ComponentMode::Vertex ? geo::pointPathBetween(*t.mesh, from, to)
                       : kind == ComponentMode::Edge ? geo::edgePathBetween(*t.mesh, from, to)
                       : kind == ComponentMode::Face ? geo::facePathBetween(*t.mesh, from, to)
                                                     : throw CommandError("selectPathBetween: kind must be vertex, edge or face");
            if (path.empty())
                throw CommandError("selectPathBetween: no path between " + std::to_string(from) + " and " + std::to_string(to));
            Op op = a.get<bool>("deselect") ? Op::Remove : a.get<bool>("replace") ? Op::Replace : Op::Add;
            UndoChunk chunk("Select Path");
            selectAndShow(t.node, kind, path, op);
            return path;
        },
    });

    ops::registerCommand({
        .name = "selectContainedFaces",
        .doc = "Select the faces enclosed by the selected edges: a flood fill that stops at selected edges and mesh borders. "
               "On each piece of a mesh the smaller regions are kept (every one but the largest by area), or with larger=True "
               "only the largest. Switches to face mode.",
        .flags = {{"larger", "", T::Bool, false, "Keep the largest region instead of the smaller ones."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            bool larger = a.get<bool>("larger");
            std::map<NodeId, Ids> faces;
            json out = json::object();
            for (const auto& [node, edges] : selected(ComponentMode::Edge))
            {
                MeshPtr m = modifiers::cage(g, node);
                Node* n = g.node(node);
                if (!m || !n || edges.empty())
                    continue;
                if (Ids ids = geo::containedFaces(*m, edges, !larger); !ids.empty())
                {
                    out[n->path()] = ids;
                    faces[node] = std::move(ids);
                }
            }
            if (faces.empty())
                throw CommandError("selectContainedFaces: select edges that close off a region");
            setAll({}, selected(ComponentMode::Edge), std::move(faces));
            setMode(ComponentMode::Face);
            return out;
        },
    });
}
} // namespace lila::scene::components
