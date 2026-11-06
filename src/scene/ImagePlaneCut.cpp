#include "scene/ImagePlane.h"

#include "core/Profile.h"
#include "curve/RegionSplit.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/SceneIndex.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

namespace lila::scene
{
namespace
{
namespace ip = imagePlane;
using curve::RegionLoop;

/** How far off its plane a cut point may lie and still be on it, cm in the plane's local units. */
double planeTolerance(const Node& n)
{
    return std::max(0.5, 0.01 * std::max(n.as<double>(ImagePlane::kWidth), n.as<double>(ImagePlane::kHeight)));
}


glm::dvec3 planeLocal(const Node& n, const glm::dvec3& world)
{
    return glm::dvec3(glm::inverse(n.as<glm::dmat4>(Transform::kWorldMatrix)) * glm::dvec4(world, 1.0));
}

int depthOf(const Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        ++d;
    return d;
}

bool attached(const Node& n)
{
    return !ip::cameraName(n.as<std::string>(ImagePlane::kCamera)).empty();
}

/** The piece a cut works on: among candidates, the deepest one whose region holds the joint point. */
Node* pieceAt(Graph& g, const std::vector<Node*>& candidates, const glm::dvec3& joint, double aspect)
{
    Node* best = nullptr;
    int bestDepth = -1;
    for (Node* n : candidates)
    {
        if (attached(*n) || !effectivelyVisible(g, n->id()))
            continue;
        glm::dvec3 p = planeLocal(*n, joint);
        if (std::abs(p.z) > planeTolerance(*n) || !curve::regionContains(ip::shownRegion(*n, aspect), {p.x, p.y}))
            continue;
        int d = depthOf(g, n->id());
        if (d >= bestDepth)
            best = n, bestDepth = d;
    }
    return best;
}

/**
 * Cuts one piece along a world polyline. The limb (the side of the joint away from the piece's pivot) becomes a new image
 * plane under it, pivoting at the joint; the rest stays on the piece. Returns the new piece, or kNoNode.
 */
NodeId cutPiece(Graph& g, Node& n, const std::vector<glm::dvec3>& points, const glm::dvec3& joint, double aspect)
{
    RegionLoop cut;
    for (const glm::dvec3& w : points)
    {
        glm::dvec3 p = planeLocal(n, w);
        cut.emplace_back(p.x, p.y);
    }
    const glm::dvec3 pivot = n.as<glm::dvec3>(Transform::kRotatePivot);
    curve::RegionCut parts = curve::cutRegion(ip::shownRegion(n, aspect), cut, {pivot.x, pivot.y});
    if (parts.limb.empty() || parts.rest.empty())
        return kNoNode;

    const NodeId id = n.id();
    const glm::dvec3 j = planeLocal(n, joint);
    const glm::dvec3 jointLocal(j.x, j.y, 0.0);
    // Pieces are named after the plane the chain started from: imagePlane1Piece1, imagePlane1Piece2, ...
    NodeId root = id;
    for (NodeId p = g.parent(root); p != kNoNode && g.node(p)->type().isA("imagePlane"); p = g.parent(p))
        root = p;
    const std::string stem = g.node(root)->name() + "Piece1";
    const NodeId pid = g.createNode(n.type(), stem, id)->id();
    for (int attr = ImagePlane::kImageName; attr < ImagePlane::kCount; ++attr)
        if (attr != ImagePlane::kMedia && attr != ImagePlane::kRegion && attr != ImagePlane::kDrawOrder && attr != ImagePlane::kGuides)
            g.setValue({pid, attr, -1}, g.node(id)->value(attr));
    g.setValue({pid, ImagePlane::kRegion, -1}, ip::regionString(parts.limb));
    g.setValue({pid, Transform::kRotatePivot, -1}, jointLocal);
    g.setValue({pid, Transform::kScalePivot, -1}, jointLocal);
    g.setValue({id, ImagePlane::kRegion, -1}, ip::regionString(parts.rest));
    return pid;
}

/** The image plane a chain of cut pieces started from. */
NodeId rootPlane(const Graph& g, NodeId id)
{
    for (NodeId p = g.parent(id); p != kNoNode && g.node(p)->type().isA("imagePlane"); p = g.parent(p))
        id = p;
    return id;
}

/**
 * Drives the mirrored piece from the first: translate, rotate, scale, pivots and pivot orient, mirrored across the world
 * axis. Both chains grow from one plane, so at every level the mirror is the same reflection in that plane's frame: across
 * whichever of its axes the world axis lies along, at the plane's own coordinate of the world mirror plane. Offsets
 * (translate, pivot compensation) negate that component, pivots reflect across that coordinate, rotations negate the other
 * two components. Skipped when the world axis lies along neither of the plane's X and Y (a plane turned off the mirror).
 */
void linkMirror(Graph& g, NodeId driver, NodeId driven, int worldAxis)
{
    const glm::dmat4 root = g.node(rootPlane(g, driver))->as<glm::dmat4>(Transform::kWorldMatrix);
    int k = -1;
    for (int axis = 0; axis < 2; ++axis)
        if (glm::length(glm::dvec3(root[axis])) > 0.0 && std::abs(glm::normalize(glm::dvec3(root[axis]))[worldAxis]) > 0.999)
            k = axis;
    const std::string from = g.node(driver)->path(), to = g.node(driven)->path();
    // Expressions name plugs without '|', so only unique names can be linked.
    if (k < 0 || !ops::find("expression") || from.find('|') != std::string::npos || to.find('|') != std::string::npos)
        return;
    // Where the world mirror plane crosses the root plane's axis k, in its local units.
    const double across = -root[3][worldAxis] / root[k][worldAxis];
    const char* const xyz[] = {"x", "y", "z"};
    auto link = [&](const char* attr, bool rotation, bool point) {
        for (int c = 0; c < 3; ++c)
        {
            const std::string plug = std::string(attr) + xyz[c];
            if ((c == k) == rotation)
                ops::execute("connectAttr", {{"args", {from + "." + plug, to + "." + plug}}});
            else if (point && std::abs(across) > 1e-9)
                ops::execute("expression", {{"args", {to + "." + plug}}, {"string", std::format("{} - {}.{}", 2.0 * across, from, plug)}});
            else
                ops::execute("expression", {{"args", {to + "." + plug}}, {"string", "-" + from + "." + plug}});
        }
    };
    for (const char* attr : {"t", "rpt", "spt"})
        link(attr, false, false);
    for (const char* attr : {"rp", "sp"})
        link(attr, false, true);
    for (const char* attr : {"r", "po"})
        link(attr, true, false);
    for (int c = 0; c < 3; ++c)
        ops::execute("connectAttr", {{"args", {from + ".s" + xyz[c], to + ".s" + xyz[c]}}});
}

bool within(const Graph& g, NodeId id, NodeId root)
{
    for (NodeId p = id; p != kNoNode; p = g.parent(p))
        if (p == root)
            return true;
    return false;
}

std::vector<glm::dvec3> cutPointsArg(const Args& a)
{
    if (!a.has("points"))
        throw CommandError("imagePlaneCut: points is required");
    const json& j = a.flags["points"];
    std::vector<glm::dvec3> out;
    if (j.is_array())
        for (const json& p : j)
        {
            if (!p.is_array() || p.size() != 3 || !std::ranges::all_of(p, [](const json& v) { return v.is_number(); }))
                throw CommandError("imagePlaneCut: each point is [x, y, z]");
            out.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
        }
    if (out.size() < 2)
        throw CommandError("imagePlaneCut: points needs at least two points");
    return out;
}

/** The nodes named and every image plane under them, or every image plane. */
std::vector<Node*> candidatesOf(Graph& g, const Args& a)
{
    if (a.positional.empty())
        return sceneIndex::ofType(g, "imagePlane");
    std::vector<Node*> out;
    std::function<void(NodeId)> add = [&](NodeId id) {
        Node* n = g.node(id);
        if (n->type().isA("imagePlane"))
            out.push_back(n);
        for (NodeId c : g.children(id))
            add(c);
    };
    for (const json& v : a.positional)
    {
        std::string name = v.get<std::string>();
        Node* n = g.find(name);
        if (!n || !n->type().isA("imagePlane"))
            throw CommandError("imagePlaneCut: " + name + " is not an image plane");
        if (attached(*n))
            throw CommandError("imagePlaneCut: " + name + " follows a camera; free it first (camera='') to cut it");
        add(n->id());
    }
    return out;
}
} // namespace

void registerImagePlaneCutCommand()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "imagePlaneGuide",
        .doc = "Add, remove or clear guide lines drawn over an image plane, for lining limbs up with an angle while reposing: "
               "imagePlaneGuide('front', lines=[[x0, y0, x1, y1]]). Each runs from its start through its end to the picture's "
               "edge, in the plane's own XY (local cm). Returns how many guides it has.",
        .positionalDoc = "image plane",
        .flags = {{"lines", "l", T::Any, nullptr, "Guides to add: [[x0, y0, x1, y1], ...] in the plane's local cm."},
                  {"remove", "rm", T::Int, nullptr, "Index of a guide to remove."},
                  {"clear", "cl", T::Bool, false, "Remove every guide."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() != 1)
                throw CommandError("imagePlaneGuide: name one image plane");
            const std::string name = a.positional[0].get<std::string>();
            Node* n = g.find(name);
            if (!n || !n->type().isA("imagePlane"))
                throw CommandError("imagePlaneGuide: " + name + " is not an image plane");
            std::vector<glm::dvec4> lines = a.get<bool>("clear", false) ? std::vector<glm::dvec4>{} : ip::guides(*n);
            if (a.has("remove"))
            {
                const int64_t i = a.get<int64_t>("remove");
                if (i < 0 || i >= int64_t(lines.size()))
                    throw CommandError("imagePlaneGuide: " + name + " has no guide " + std::to_string(i));
                lines.erase(lines.begin() + i);
            }
            if (a.has("lines"))
                for (const json& l : a.flags["lines"])
                {
                    if (!l.is_array() || l.size() != 4 || !std::ranges::all_of(l, [](const json& v) { return v.is_number(); }))
                        throw CommandError("imagePlaneGuide: each line is [x0, y0, x1, y1]");
                    lines.emplace_back(l[0].get<double>(), l[1].get<double>(), l[2].get<double>(), l[3].get<double>());
                }
            json out = json::array();
            for (const glm::dvec4& l : lines)
                out.push_back({l.x, l.y, l.z, l.w});
            g.setValue({n->id(), ImagePlane::kGuides, -1}, lines.empty() ? std::string() : out.dump());
            return lines.size();
        },
    });

    ops::registerCommand({
        .name = "imagePlaneCut",
        .doc = "Cut an image plane into pieces to repose the picture, e.g. arms drawn at a poor angle: "
               "imagePlaneCut(points=[[x, y, z], ...], symmetry='x'). The first two points span the joint and the piece their "
               "midpoint lies on is cut. More points go around the limb and close back to the first, a lasso: the limb is "
               "what lies inside. With only two, the limb is the side away from the piece's pivot, each end running on to "
               "the edge fanned 30 degrees out from straight across. The limb becomes a new image plane parented under the "
               "piece, pivoting at the joint, so turning its rotateZ swings it. Cut shoulder, then elbow, then wrist for a "
               "chain. Returns the new pieces.",
        .positionalDoc = "image planes to cut, with the pieces under them (default: whichever the cut lies on)",
        .flags = {{"points", "p", T::Any, nullptr, "The cut in world space: [[x, y, z], ...], two or more points on the plane."},
                  {"symmetry", "sym", T::String, "off", "Also make the mirror cut across a world plane: off, x, y or z.", {"off", "x", "y", "z"}},
                  {"mirrorTransform", "mt", T::Bool, true, "With symmetry: the mirrored piece follows the first one's transform and pivots, mirrored."},
                  {"keyBackground", "kb", T::Bool, true, "Clear the background of the plane cut and its pieces, so a turned piece doesn't cover its neighbours."},
                  {"aspect", "a", T::Float, 0.0, "The picture's width over height, so an uncut plane is cut at the picture's edge; 0 runs cuts past it.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            g.evaluate();
            std::vector<glm::dvec3> points = cutPointsArg(a);
            const std::string sym = a.get<std::string>("symmetry", "off");
            const int axis = sym == "x" ? 0 : sym == "y" ? 1 : sym == "z" ? 2 : sym == "off" ? -1 : -2;
            if (axis == -2)
                throw CommandError("imagePlaneCut: symmetry must be off, x, y or z");

            const double aspect = a.get<double>("aspect", 0.0);
            UndoChunk chunk("Cut Image Plane");
            const glm::dvec3 joint = (points[0] + points[1]) * 0.5;
            Node* target = pieceAt(g, candidatesOf(g, a), joint, aspect);
            if (!target)
                throw CommandError("imagePlaneCut: the cut's first segment doesn't lie on a free image plane");
            const NodeId targetId = target->id();
            const bool key = a.get<bool>("keyBackground", true);
            auto keyed = [&](NodeId cut, const std::vector<NodeId>& pieces) {
                if (!key)
                    return;
                g.setValue({cut, ImagePlane::kKeyBackground, -1}, true);
                for (NodeId id : pieces)
                    g.setValue({id, ImagePlane::kKeyBackground, -1}, true);
            };
            const NodeId cutId = cutPiece(g, *target, points, joint, aspect);
            const std::vector<NodeId> made = cutId == kNoNode ? std::vector<NodeId>{} : std::vector<NodeId>{cutId};
            if (made.empty())
                throw CommandError("imagePlaneCut: the cut doesn't part " + target->path() + "; draw it across the piece");

            keyed(targetId, made);
            json out = json::array();
            for (NodeId id : made)
                out.push_back(g.node(id)->path());

            // A cut that crosses the mirror plane is its own mirror image.
            const bool crosses = axis >= 0 && std::ranges::any_of(points, [&](const glm::dvec3& p) { return p[axis] > 0.0; }) &&
                                 std::ranges::any_of(points, [&](const glm::dvec3& p) { return p[axis] < 0.0; });
            if (axis >= 0 && !crosses)
            {
                std::vector<glm::dvec3> mirrored = points;
                for (glm::dvec3& p : mirrored)
                    p[axis] = -p[axis];
                g.evaluate();
                const glm::dvec3 mjoint = (mirrored[0] + mirrored[1]) * 0.5;
                // The mirror cut belongs on the other side: never on what this cut just made, and on the piece this cut was
                // made on only when nothing else is there (the body both arms come off).
                std::vector<Node*> candidates = candidatesOf(g, a);
                std::erase_if(candidates, [&](Node* n) { return within(g, n->id(), cutId); });
                std::vector<Node*> others = candidates;
                std::erase_if(others, [&](Node* n) { return n->id() == targetId; });
                Node* other = pieceAt(g, others, mjoint, aspect);
                if (!other)
                    other = pieceAt(g, candidates, mjoint, aspect);
                const NodeId mirrorId = other ? cutPiece(g, *other, mirrored, mjoint, aspect) : kNoNode;
                const std::vector<NodeId> mirror = mirrorId == kNoNode ? std::vector<NodeId>{} : std::vector<NodeId>{mirrorId};
                if (!mirror.empty())
                    keyed(other->id(), mirror);
                for (NodeId id : mirror)
                    out.push_back(g.node(id)->path());

                if (a.get<bool>("mirrorTransform", true) && !mirror.empty())
                {
                    g.evaluate();
                    linkMirror(g, made[0], mirror[0], axis);
                }
            }
            g.select({made.front()});
            return out;
        },
    });
}
} // namespace lila::scene
