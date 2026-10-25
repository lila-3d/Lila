#include "retopo/RetopoCommands.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "retopo/Retopo.h"
#include "retopo/TopologyCommands.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <map>
#include <optional>

namespace lila::retopo
{
namespace
{
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::Node;
using scene::PlugRef;
using scene::Transform;

Graph& graph() { return Graph::current(); }

Node& meshArg(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

geo::Mesh baseMesh(const PlugRef& p)
{
    graph().evaluate();
    const MeshPtr& m = std::get<MeshPtr>(graph().storedValue(p));
    return m ? *m : geo::Mesh();
}

glm::dmat4 worldOf(const Node& n) { return n.as<glm::dmat4>(Transform::kWorldMatrix); }

/** The mesh's drawn result, moved into another node's object space. */
geo::Mesh meshInSpaceOf(const Node& src, const Node& space)
{
    graph().evaluate();
    MeshPtr m = src.as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        return {};
    geo::Mesh out = *m;
    out.compact();
    glm::dmat4 x = glm::inverse(worldOf(space)) * worldOf(src);
    for (glm::dvec3& p : out.positions())
        p = glm::dvec3(x * glm::dvec4(p, 1.0));
    return out;
}

std::optional<geo::SymmetryPlane> symmetryPlane(const Node& n)
{
    int64_t axis = n.as<int64_t>(MeshNode::kSymmetry);
    if (axis <= 0 || axis > 3)
        return std::nullopt;
    return geo::axisPlane(int(axis - 1), symmetryOffset(n, int(axis - 1)));
}

double symmetryTolerance(const geo::Mesh& m) { return std::max(1e-4, glm::length(m.bounds().size()) * 1e-5); }

std::vector<FlagDef> flagsWith(std::vector<FlagDef> base, std::vector<FlagDef> more)
{
    base.insert(base.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    return base;
}

std::vector<FlagDef> repairFlags()
{
    using T = ArgType;
    std::vector<FlagDef> flags = {
        {"mode", "", T::String, "repair",
         "select: select the problems instead of fixing them. Points, edges and faces are each selected in their own "
         "component mode, and the mode switches to whichever kind has the most; the log lists the counts per check. "
         "repair: fix them.",
         {"select", "repair"}},
    };
    static const char* docs[kIssueCount] = {
        "Points closer than the tolerance to another (repair welds them).",
        "Edges shorter than the tolerance (repair collapses them).",
        "Faces built on exactly the points of another face (repair deletes the extra copy).",
        "Faces thinner than the tolerance (repair merges each into the face across its longest edge).",
        "Edges shared by three or more faces (repair splits the extra faces off).",
        "Vertices where separate fans of faces meet, like a bowtie (repair gives each fan its own vertex).",
        "Edges on an open border (repair fills each hole with a face). Off by default: open meshes are often meant to be.",
        "Faces with more than four sides (repair triangulates them and joins the triangles back into convex quads where they fit).",
        "Faces with a corner bending inward (repair cuts them into convex quads and triangles).",
        "Faces with no area in the first uv set. Repair only reports them: they need a new unwrap.",
        "Faces wound against the rest of their shell, or closed shells facing inward (repair turns them).",
    };
    RepairOptions defaults;
    for (size_t i = 0; i < kIssueCount; ++i)
        flags.push_back({issueName(MeshIssue(i)), "", T::Bool, defaults.checks[i], docs[i]});
    flags.push_back({"tolerance", "", T::Float, -1.0, "Distance under which points coincide and edges and faces count as zero size, cm; negative scales with the mesh."});
    flags.push_back({"removeLoose", "", T::Bool, true, "Repair also removes points and edges left without faces."});
    return flags;
}

/** Replaces the component selection with the issues found and shows the kind with the most of them (faces win ties). */
void selectIssues(Graph& g, const std::vector<scene::NodeId>& meshes, std::map<scene::NodeId, scene::components::Ids> sets[3])
{
    std::vector<scene::NodeId> unselected;
    for (scene::NodeId id : meshes)
        if (std::find(g.selection().begin(), g.selection().end(), id) == g.selection().end())
            unselected.push_back(id);
    if (!unselected.empty())
        g.select(unselected, scene::SelectMode::Add);
    size_t total[3] = {};
    for (int k = 0; k < 3; ++k)
        for (auto& [id, ids] : sets[k])
            total[k] += ids.size();
    using scene::ComponentMode;
    ComponentMode mode = ComponentMode::Face;
    if (total[1] > total[2] && total[1] >= total[0])
        mode = ComponentMode::Edge;
    else if (total[0] > total[2] && total[0] > total[1])
        mode = ComponentMode::Vertex;
    scene::components::setAll(std::move(sets[0]), std::move(sets[1]), std::move(sets[2]));
    if (total[0] + total[1] + total[2] > 0)
        scene::components::setMode(mode);
}

json metricsJson(const QualityMetrics& q)
{
    return {{"faces", q.faces},
            {"quads", q.quads},
            {"triangles", q.triangles},
            {"ngons", q.ngons},
            {"quadPercent", q.quadPercent},
            {"valence", q.valence},
            {"irregular", q.irregular},
            {"maxAngleDeviation", q.maxAngleDeviation},
            {"meanAngleDeviation", q.meanAngleDeviation},
            {"maxDistance", q.maxDistance},
            {"meanDistance", q.meanDistance}};
}

std::vector<glm::dvec3> vecList(const json& j, const char* cmd)
{
    std::vector<glm::dvec3> out;
    if (!j.is_array())
        throw CommandError(std::string(cmd) + ": expected a list of [x, y, z]");
    for (const json& v : j)
    {
        if (!v.is_array() || v.size() != 3)
            throw CommandError(std::string(cmd) + ": expected a list of [x, y, z]");
        out.emplace_back(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    }
    return out;
}

/** Guide polylines in the source's object space: edge chains of mesh nodes, or point lists given directly in world space. */
std::vector<std::vector<glm::dvec3>> guidesFor(const Args& a, const Node& src)
{
    std::vector<std::vector<glm::dvec3>> out;
    glm::dmat4 toLocal = glm::inverse(worldOf(src));
    if (a.has("guides"))
        for (const json& g : a.flags["guides"])
        {
            if (!g.is_string())
                continue;
            Node& n = meshArg(g.get<std::string>(), "quadRemesh");
            geo::Mesh m = meshInSpaceOf(n, src);
            std::vector<Index> edges;
            for (Index e = 0; e < m.edgeCapacity(); ++e)
                if (m.edgeValid(e))
                    edges.push_back(e);
            for (const geo::EdgePath& path : geo::chainEdges(m, edges))
            {
                std::vector<glm::dvec3> line;
                for (Index p : path.points)
                    line.push_back(m.position(p));
                if (path.closed && !path.points.empty())
                    line.push_back(m.position(path.points.front()));
                if (line.size() >= 2)
                    out.push_back(std::move(line));
            }
        }
    if (a.has("guidePoints"))
        for (const json& g : a.flags["guidePoints"])
        {
            std::vector<glm::dvec3> line;
            for (const glm::dvec3& p : vecList(g, "quadRemesh"))
                line.push_back(glm::dvec3(toLocal * glm::dvec4(p, 1.0)));
            if (line.size() >= 2)
                out.push_back(std::move(line));
        }
    return out;
}

/** Poles from a list of {position: world [x, y, z], valence}, in the source's object space. */
std::vector<Pole> polesFor(const json& list, const Node& src, const char* cmd)
{
    glm::dmat4 toLocal = glm::inverse(worldOf(src));
    std::vector<Pole> out;
    for (const json& j : list)
    {
        if (!j.is_object() || !j.contains("position"))
            throw CommandError(std::string(cmd) + ": each pole is {position: [x, y, z], valence: 3 or 5}");
        std::vector<glm::dvec3> at = vecList(json::array({j["position"]}), cmd);
        int valence = j.value("valence", 3);
        if (valence < 2 || valence > 8)
            throw CommandError(std::string(cmd) + ": a pole's valence is 2 to 8");
        out.push_back({glm::dvec3(toLocal * glm::dvec4(at.front(), 1.0)), valence});
    }
    return out;
}

json polesJson(const std::vector<Pole>& poles, const Node& src)
{
    glm::dmat4 world = worldOf(src);
    json out = json::array();
    for (const Pole& p : poles)
    {
        glm::dvec3 w = glm::dvec3(world * glm::dvec4(p.position, 1.0));
        out.push_back({{"position", {w.x, w.y, w.z}}, {"valence", p.valence}});
    }
    return out;
}

std::vector<FlagDef> remeshFlags()
{
    using T = ArgType;
    return {{"targetFaces", "", T::Int, 500, "Approximate face count."},
            {"timeout", "", T::Float, 5.0, "Seconds before giving up with an error saying how far it got, leaving the scene as it was; 0 waits forever."},
            {"targetEdgeLength", "", T::Float, 0.0, "Edge length, cm; overrides targetFaces when positive."},
            {"adaptivity", "", T::Float, 0.25, "0 uniform, 1 much denser where curved; painted point 'density' multiplies."},
            {"useHardEdges", "", T::Bool, true, "Follow edges marked hard."},
            {"sharpAngle", "", T::Float, 40.0, "Also follow creases sharper than this, degrees; negative disables."},
            {"symmetry", "", T::String, "off", "off, x, y or z: remesh half and mirror, exactly symmetric."},
            {"guides", "", T::List, nullptr, "Mesh nodes whose edges are guide curves."},
            {"guidePoints", "", T::List, nullptr, "Guide polylines as lists of world [x, y, z]."},
            {"pureQuads", "", T::Bool, true, "All quads; false allows a few triangles and pentagons."},
            {"resolveGaps", "", T::Bool, true, "Finer where the surface comes close to itself across a gap (a ball in a pocket), so the gap isn't bridged; costs faces."},
            {"seamless", "", T::Bool, false, "Experimental: also try a global layout that only turns at the poles, kept when it has fewer irregular points."},
            {"poles", "", T::List, nullptr,
             "The poles the edge flow turns at, as {position: world [x, y, z], valence: 3 or 5}, and nowhere else. Start from "
             "retopoPoles; on a closed shell (4 - valence) must sum to what retopoPoles reports."}};
}

QuadRemeshOptions remeshOptions(const Args& a, const Node& src, const char* cmd)
{
    QuadRemeshOptions o;
    o.targetFaces = std::max(8, a.get<int>("targetFaces", 500));
    o.timeout = std::max(0.0, a.get<double>("timeout", 5.0));
    o.targetEdgeLength = a.get<double>("targetEdgeLength", 0.0);
    o.adaptivity = std::clamp(a.get<double>("adaptivity", 0.25), 0.0, 1.0);
    o.useHardEdges = a.get<bool>("useHardEdges", true);
    o.sharpAngle = a.get<double>("sharpAngle", 40.0);
    std::string sym = a.get<std::string>("symmetry", "off");
    if (sym != "off" && (sym.size() != 1 || sym[0] < 'x' || sym[0] > 'z'))
        throw CommandError(std::string(cmd) + ": symmetry must be off, x, y or z");
    o.symmetryAxis = sym == "off" ? -1 : sym[0] - 'x';
    o.guides = guidesFor(a, src);
    o.pureQuads = a.get<bool>("pureQuads", true);
    o.seamless = a.get<bool>("seamless", false);
    o.resolveGaps = a.get<bool>("resolveGaps", true);
    if (a.has("poles") && !a.flags["poles"].is_null())
        o.poles = polesFor(a.flags["poles"], src, cmd);
    return o;
}

void copyTransform(Graph& g, const Node& from, const Node& to)
{
    for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                     Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
        g.setValue({to.id(), attr, -1}, g.storedValue({from.id(), attr, -1}));
}

/** Points of the component selection on a node, as a mask over the base mesh's point capacity; empty when none. */
std::vector<uint8_t> selectedPointMask(scene::NodeId node, const geo::Mesh& base)
{
    if (scene::components::mode() == scene::ComponentMode::Object)
        return {};
    scene::components::Ids pts = scene::components::pointsOf(node);
    if (pts.empty())
        return {};
    return geo::makeMask(base.pointCapacity(), pts);
}

// Retopo Draw edits

/** Existing point within tol of p (object space), or a new one. */
Index pointAt(geo::Mesh& m, const glm::dvec3& p, double tol)
{
    Index best = geo::kInvalid;
    double bestD = tol * tol;
    for (Index q = 0; q < m.pointCapacity(); ++q)
        if (m.pointValid(q))
        {
            glm::dvec3 d = m.position(q) - p;
            double d2 = glm::dot(d, d);
            if (d2 <= bestD)
            {
                bestD = d2;
                best = q;
            }
        }
    return best != geo::kInvalid ? best : m.addPoint(p);
}

/** Adds a face, flipped if needed so it winds against the faces it shares edges with. */
Index addOrientedFace(geo::Mesh& m, std::vector<Index> pts)
{
    std::vector<Index> sorted = pts;
    std::sort(sorted.begin(), sorted.end());
    if (pts.size() < 3 || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return geo::kInvalid;
    bool sameWay = false;
    for (size_t k = 0; k < pts.size(); ++k)
    {
        Index a = pts[k], b = pts[(k + 1) % pts.size()];
        Index e = m.findEdge(a, b);
        if (e == geo::kInvalid)
            continue;
        if (m.edgeFaceCount(e) >= 2)
            return geo::kInvalid;
        m.forEachEdgeCorner(e, [&](Index c) { sameWay |= m.cornerPoint(c) == a; });
    }
    if (sameWay)
        std::reverse(pts.begin(), pts.end());
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceSize(f) == pts.size())
        {
            std::vector<Index> fp = m.facePoints(f);
            std::sort(fp.begin(), fp.end());
            if (fp == sorted)
                return geo::kInvalid;
        }
    return m.addFace(pts);
}

/** The retopo mesh, created on demand. With reference=, that mesh's outMesh is connected as its Retopo Draw reference. */
Node& retopoDrawTarget(const Args& a, const char* cmd, bool create)
{
    std::string name = a.get<std::string>("mesh");
    if (name.empty())
        throw CommandError(std::string(cmd) + ": give mesh=");
    Graph& g = graph();
    Node* ref = a.has("reference") ? &meshArg(a.get<std::string>("reference"), cmd) : nullptr;
    Node* n = g.find(name);
    bool created = false;
    if (!n && create)
    {
        n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
        g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>()));
        created = true;
    }
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    if (ref)
    {
        if (ref->id() == n->id())
            throw CommandError(std::string(cmd) + ": the reference can't be the retopo mesh itself");
        PlugRef dst{n->id(), MeshNode::kRetopoDrawReference, -1};
        if (auto src = g.source(dst); !src || src->node != ref->id())
            g.connect({ref->id(), MeshNode::kOutMesh, -1}, dst, true);
        if (created)
            g.setValue({n->id(), MeshNode::kSymmetry, -1}, ref->as<int64_t>(MeshNode::kSymmetry));
    }
    return *n;
}
} // namespace

scene::Node& retopoTargetNode(const Args& a, const char* cmd, bool create) { return retopoDrawTarget(a, cmd, create); }

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "quadRemesh",
        .doc = "Auto quad remesh: a new all-quad mesh whose edge flow follows curvature, hard edges and guide curves, at about "
               "the target face count, with UVs (and optionally normals) transferred from the source. Open borders, sharp "
               "creases, crease corners and cone tips are kept. Where the shape is too thin or tight for the requested size "
               "(a strip narrower than the edge length) it retries finer, so the face count can come out higher. The kept source "
               "is hidden. Runs off the UI thread with a cancellable progress bar, and gives up after timeout= seconds with "
               "an error saying how far it got, changing nothing. Returns {name, metrics}.",
        .positionalDoc = "source",
        .flags = flagsWith(remeshFlags(), {{"relaxIterations", "", T::Int, 6, "Tangential relax passes on the surface."},
                  {"transferUvs", "", T::Bool, true, "Copy UV sets from the source."},
                  {"transferNormals", "", T::Bool, false, "Copy the source's normals as custom normals."},
                  {"keepSource", "", T::Bool, true, "Keep the source mesh; false deletes it."},
                  {"hideSource", "", T::Bool, true, "Hide the kept source mesh."},
                  {"name", "", T::String, nullptr, "Name of the new mesh."}}),
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("quadRemesh: give a source mesh");
            Graph& g = graph();
            Node& src = meshArg(objs.front(), "quadRemesh");
            g.evaluate();
            MeshPtr srcMesh = src.as<MeshPtr>(MeshNode::kOutMesh);
            if (!srcMesh || srcMesh->numFaces() == 0)
                throw CommandError("quadRemesh: " + src.path() + " has no faces");

            QuadRemeshOptions o = remeshOptions(a, src, "quadRemesh");
            o.relaxIterations = std::clamp(a.get<int>("relaxIterations"), 0, 100);

            QuadRemeshResult r;
            {
                progress::Scope scope("Quad remesh " + src.name(), 1.0, progress::Cancellable);
                progress::runOnWorker([&] { r = quadRemesh(*srcMesh, o); });
            }
            if (!r.ok())
                throw CommandError("quadRemesh: " + r.error + (r.error.rfind("gave up", 0) == 0 ? "; raise timeout= or lower targetFaces" : ""));
            double total = 0.0;
            for (auto& [stage, sec] : r.timings)
                total += sec;
            log::info("quadRemesh {}: {} ({:.2f} s)", src.path(), r.metrics.summary(), total);

            std::string name = a.get<std::string>("name", src.name() + "_remesh");
            Node* out = g.createNode(*scene::types::find("mesh"), name, g.parent(src.id()), true);
            copyTransform(g, src, *out);
            if (o.symmetryAxis >= 0)
                g.setValue({out->id(), MeshNode::kSymmetry, -1}, int64_t(o.symmetryAxis + 1));
            g.setValue({out->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));

            bool uvs = a.get<bool>("transferUvs") && srcMesh->cornerAttrs().has(geo::attr::uvName(0));
            bool normals = a.get<bool>("transferNormals");
            if (uvs || normals)
                ops::execute("transferAttributes",
                              {{"args", json::array({src.path(), out->path()})}, {"uvs", uvs}, {"normals", normals}, {"sampleSpace", "object"}});
            std::string outName = out->path();
            if (!a.get<bool>("keepSource"))
                g.deleteNode(src.id());
            else if (a.get<bool>("hideSource", true))
                g.setValue({src.id(), Transform::kVisibility, -1}, false);
            g.select({out->id()});
            return {{"name", outName}, {"metrics", metricsJson(r.metrics)}};
        },
    });

    ops::registerCommand({
        .name = "retopoPoles",
        .doc = "The poles quadRemesh's edge flow would turn at with these options, without remeshing: where each one is and "
               "its valence (3 or 5). Move, add or remove them and pass them back as quadRemesh(poles=...). Also returns, per "
               "closed shell, what (4 - valence) over its poles has to sum to.",
        .positionalDoc = "source",
        .flags = remeshFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("retopoPoles: give a source mesh");
            Graph& g = graph();
            Node& src = meshArg(objs.front(), "retopoPoles");
            g.evaluate();
            MeshPtr srcMesh = src.as<MeshPtr>(MeshNode::kOutMesh);
            if (!srcMesh || srcMesh->numFaces() == 0)
                throw CommandError("retopoPoles: " + src.path() + " has no faces");
            QuadRemeshOptions o = remeshOptions(a, src, "retopoPoles");
            PoleResult r;
            {
                progress::Scope scope("Finding poles on " + src.name(), 1.0, progress::Cancellable);
                progress::runOnWorker([&] { r = findPoles(*srcMesh, o); });
            }
            if (!r.ok())
                throw CommandError("retopoPoles: " + r.error);
            json totals = json::array();
            for (const std::optional<int>& t : r.shellTotals)
                totals.push_back(t ? json(*t) : json(nullptr));
            return {{"poles", polesJson(r.poles, src)}, {"shellTotals", totals}};
        },
    });

    ops::registerCommand({
        .name = "meshRepair",
        .doc = "Find or fix broken geometry. mode='select' selects what each enabled check finds and reports the counts; "
               "mode='repair' fixes it: welds coincident points, collapses zero-length edges, drops lamina copies, merges "
               "zero-area slivers into a neighbour, splits non-manifold edges and vertices apart, fills holes, cuts n-gons "
               "and concave faces into quads and triangles, conforms winding, removes loose points and edges.",
        .positionalDoc = "objects",
        .flags = repairFlags(),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::string mode = a.get<std::string>("mode", "repair");
            if (mode != "select" && mode != "repair")
                throw CommandError("meshRepair: mode must be select or repair");
            RepairOptions ro;
            for (size_t i = 0; i < kIssueCount; ++i)
                ro.checks[i] = a.get<bool>(issueName(MeshIssue(i)), ro.checks[i]);
            ro.tolerance = a.get<double>("tolerance", -1.0);
            ro.removeLoose = a.get<bool>("removeLoose", true);

            json out = json::object();
            UndoChunk chunk(mode == "select" ? "Select Mesh Issues" : "Mesh Repair");
            if (mode == "repair")
            {
                for (const std::string& name : a.objects())
                {
                    Node& n = meshArg(name, "meshRepair");
                    PlugRef base = scene::modifiers::editPlug(g, n.id());
                    geo::Mesh m = baseMesh(base);
                    std::string report = repairMesh(m, ro).summary();
                    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                    log::info("meshRepair {}: {}", name, report);
                    out[name] = report;
                }
                scene::components::clear();
                return out;
            }

            std::map<scene::NodeId, scene::components::Ids> sets[3];
            std::vector<scene::NodeId> meshes;
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "meshRepair");
                g.evaluate();
                MeshPtr cage = scene::modifiers::cage(g, n.id());
                if (!cage)
                    continue;
                MeshIssues found = findIssues(*cage, ro);
                json counts = json::object();
                for (size_t i = 0; i < kIssueCount; ++i)
                    if (ro.checks[i])
                        counts[issueName(MeshIssue(i))] = found.count(MeshIssue(i));
                for (int k = 0; k < 3; ++k)
                    if (std::vector<Index> ids = found.elements(IssueElement(k)); !ids.empty())
                        sets[k][n.id()] = std::move(ids);
                meshes.push_back(n.id());
                log::info("meshRepair {}: {}", name, found.summary());
                out[name] = counts;
            }
            selectIssues(g, meshes, std::move(sets));
            return out;
        },
    });
    ops::setAdjustable("meshRepair");

    ops::registerCommand({
        .name = "voxelRemesh",
        .doc = "Rebuild meshes as one closed, manifold quad surface from a voxel grid, whatever state they are in: closes holes, "
               "fuses intersecting and inner shells. For dirty scans and kitbashed blockouts; UVs are not kept.",
        .positionalDoc = "objects",
        .flags = {{"resolution", "", T::Int, 128, "Voxels along the longest side of the bounds, 8 to 1024."},
                  {"project", "", T::Bool, true, "Pull the result onto the input surface where it lies close."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::object();
            UndoChunk chunk("Voxel Remesh");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "voxelRemesh");
                PlugRef base = scene::modifiers::editPlug(g, n.id());
                VoxelOptions vo;
                vo.resolution = std::clamp(a.get<int>("resolution"), 8, 1024);
                vo.project = a.get<bool>("project");
                geo::Mesh m = voxelRemesh(baseMesh(base), vo);
                out[name] = m.numFaces();
                log::info("voxelRemesh {}: {} faces", name, m.numFaces());
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            scene::components::clear();
            return out;
        },
    });
    ops::setAdjustable("voxelRemesh");

    ops::registerCommand({
        .name = "relaxMesh",
        .doc = "Even out point spacing while keeping the form: points slide over the mesh's own starting surface (or a target "
               "mesh), points on sharp creases and hard edges slide only along them, and crease corners and cone tips stay. "
               "No face is allowed to fold over. Moves only the selected components when there is a component selection. "
               "Honours the mesh's symmetry.",
        .positionalDoc = "objects",
        .flags = {{"iterations", "", T::Int, 10, "Smoothing passes, 1 to 1000."},
                  {"strength", "", T::Float, 0.5, "Step toward the neighbour average per pass, 0 to 1."},
                  {"target", "", T::String, nullptr, "Mesh to stay on; default is the mesh itself."},
                  {"project", "", T::Bool, true, "Keep points on the surface. Off is plain smoothing, which shrinks and rounds the shape."},
                  {"pinBoundary", "", T::Bool, true, "Keep border points fixed; off lets them slide along the border, keeping its outline."},
                  {"sharpAngle", "", T::Float, 12.0, "Edges whose faces meet sharper than this are creases that keep their line, degrees; negative turns it off."},
                  {"keepHardEdges", "", T::Bool, true, "Hard edges keep their line like creases do."},
                  {"pinCreases", "", T::Bool, false, "Keep points on creased edges fixed."},
                  {"pinHard", "", T::Bool, false, "Keep points on hard edges fixed."},
                  {"pinSeams", "", T::Bool, false, "Keep points on UV seams fixed."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Relax");
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(name, "relaxMesh");
                if (a.has("target") && a.get<std::string>("target") == name)
                    continue;
                PlugRef base = scene::modifiers::editPlug(g, n.id());
                geo::Mesh m = baseMesh(base);
                RelaxOptions ro;
                ro.iterations = std::clamp(a.get<int>("iterations"), 1, 1000);
                ro.strength = std::clamp(a.get<double>("strength"), 0.0, 1.0);
                ro.pinBoundary = a.get<bool>("pinBoundary");
                ro.project = a.get<bool>("project");
                ro.sharpAngle = a.get<double>("sharpAngle");
                ro.hardEdgesAreCreases = a.get<bool>("keepHardEdges");
                ro.active = selectedPointMask(n.id(), m);
                FeatureOptions fo;
                fo.boundary = false;
                fo.creases = a.get<bool>("pinCreases");
                fo.hardEdges = a.get<bool>("pinHard");
                fo.uvSeams = a.get<bool>("pinSeams");
                if (fo.creases || fo.hardEdges || fo.uvSeams)
                    ro.pinned = featurePoints(m, fo);
                std::optional<geo::SymmetryMap> map;
                std::vector<Index> activeIds;
                if (int64_t axis = n.as<int64_t>(MeshNode::kSymmetry); axis >= 1 && axis <= 3)
                {
                    map = geo::SymmetryMap::persistent(m, int(axis - 1), symmetryTolerance(m), nullptr, symmetryOffset(n, int(axis - 1)));
                    for (Index p = 0; p < ro.active.size(); ++p)
                        if (ro.active[p])
                            activeIds.push_back(p);
                    for (Index p : map->expandPoints(activeIds))
                        ro.active[p] = 1;
                }
                std::optional<geo::MeshBvh> target;
                if (a.has("target"))
                    target.emplace(meshInSpaceOf(meshArg(a.get<std::string>("target"), "relaxMesh"), n));
                relaxMesh(m, target ? &*target : nullptr, ro);
                if (map)
                    map->symmetrize(m, activeIds.empty() ? 1 : map->majoritySide(activeIds));
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            return nullptr;
        },
    });
    ops::setAdjustable("relaxMesh");

    ops::registerCommand({
        .name = "shrinkwrap",
        .doc = "Reproject meshes onto a target surface: to the closest point, along each point's normal, or along a fixed "
               "direction, optionally held off the surface by an offset; then relax them on it. Moves only the selected "
               "components when there is a component selection.",
        .positionalDoc = "objects",
        .flags = {{"target", "", T::String, nullptr, "Mesh to wrap onto."},
                  {"iterations", "", T::Int, 3, "Relax passes after projecting."},
                  {"method", "", T::String, "closest", "closest, normal or direction.", {"closest", "normal", "direction"}},
                  {"direction", "", T::Vec3, json::array({0.0, 0.0, -1.0}), "World direction for method='direction'."},
                  {"offset", "", T::Float, 0.0, "Distance kept off the target along its normal, cm; negative goes inside."},
                  {"maxDistance", "", T::Float, -1.0, "Leave points farther than this, cm; negative is unlimited."}},
        .run = [](const Args& a) -> json {
            if (!a.has("target"))
                throw CommandError("shrinkwrap: give target=");
            Graph& g = graph();
            std::string targetName = a.get<std::string>("target");
            Node& targetNode = meshArg(targetName, "shrinkwrap");
            UndoChunk chunk("Shrinkwrap");
            for (const std::string& name : a.objects())
            {
                if (name == targetName)
                    continue;
                Node& n = meshArg(name, "shrinkwrap");
                PlugRef base = scene::modifiers::editPlug(g, n.id());
                geo::Mesh m = baseMesh(base);
                geo::Mesh targetMesh = meshInSpaceOf(targetNode, n);
                geo::MeshBvh bvh(targetMesh);
                std::string method = a.get<std::string>("method", "closest");
                ProjectOptions po;
                po.method = method == "normal" ? ProjectMethod::Normal : method == "direction" ? ProjectMethod::Direction : ProjectMethod::Closest;
                if (method != "closest" && method != "normal" && method != "direction")
                    throw CommandError("shrinkwrap: method must be closest, normal or direction");
                if (a.has("direction"))
                {
                    json d = a.flags["direction"];
                    glm::dvec3 world(d[0].get<double>(), d[1].get<double>(), d[2].get<double>());
                    po.direction = glm::dvec3(glm::inverse(worldOf(n)) * glm::dvec4(world, 0.0));
                }
                po.maxDistance = a.get<double>("maxDistance");
                po.active = selectedPointMask(n.id(), m);
                projectMesh(m, targetMesh, bvh, po);
                if (int it = std::clamp(a.get<int>("iterations"), 0, 1000); it > 0)
                {
                    RelaxOptions ro;
                    ro.iterations = it;
                    ro.pinBoundary = false;
                    ro.active = po.active;
                    relaxMesh(m, &bvh, ro);
                }
                if (double offset = a.get<double>("offset"); offset != 0.0)
                {
                    ProjectOptions off;
                    off.offset = offset;
                    off.active = po.active;
                    projectMesh(m, targetMesh, bvh, off);
                }
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "retopoDrawAddFace",
        .doc = "Retopo Draw: add a face through world points, reusing existing points within weld distance (and its mirror when "
               "the mesh has symmetry). faces= adds several in one step, skipping any that already exist or would make a "
               "non-manifold edge. Creates the mesh when create=True. Returns the mesh name.",
        .flags = {{"mesh", "", T::String, nullptr, "Retopo mesh."},
                  {"points", "", T::List, nullptr, "3 or 4 world [x, y, z], in order around the face."},
                  {"faces", "", T::List, nullptr, "Several faces, each a list of 3 or 4 world [x, y, z]."},
                  {"weld", "", T::Float, 0.01, "Reuse points closer than this, cm."},
                  {"reference", "", T::String, nullptr, "Reference mesh, connected so Retopo Draw resumes on this pair later."},
                  {"create", "", T::Bool, false, "Create the mesh if it doesn't exist (taking the reference's symmetry)."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node& n = retopoDrawTarget(a, "retopoDrawAddFace", a.get<bool>("create"));
            const bool many = a.flags.contains("faces") && !a.flags["faces"].is_null();
            std::vector<std::vector<glm::dvec3>> worldFaces;
            if (many)
                for (const json& face : a.flags["faces"])
                    worldFaces.push_back(vecList(face, "retopoDrawAddFace"));
            else
                worldFaces.push_back(vecList(a.flags.value("points", json::array()), "retopoDrawAddFace"));
            if (worldFaces.empty())
                throw CommandError("retopoDrawAddFace: give points= or faces=");
            for (const std::vector<glm::dvec3>& world : worldFaces)
                if (world.size() < 3 || world.size() > 4)
                    throw CommandError("retopoDrawAddFace: give 3 or 4 points per face");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            glm::dmat4 toLocal = glm::inverse(worldOf(n));
            double weld = std::max(0.0, a.get<double>("weld"));
            auto sym = symmetryPlane(n);
            double planeTol = std::max(weld, symmetryTolerance(m));
            auto build = [&](const std::vector<glm::dvec3>& ps) {
                std::vector<Index> ids;
                for (const glm::dvec3& p : ps)
                    ids.push_back(pointAt(m, p, weld));
                return addOrientedFace(m, ids);
            };
            int added = 0;
            for (const std::vector<glm::dvec3>& world : worldFaces)
            {
                std::vector<glm::dvec3> local;
                for (const glm::dvec3& p : world)
                    local.push_back(glm::dvec3(toLocal * glm::dvec4(p, 1.0)));
                if (sym)
                    for (glm::dvec3& p : local)
                        if (std::abs(sym->distance(p)) < planeTol)
                            p = sym->project(p);
                Index f = build(local);
                if (sym)
                {
                    std::vector<glm::dvec3> mirrored;
                    for (auto it = local.rbegin(); it != local.rend(); ++it)
                        mirrored.push_back(sym->reflect(*it));
                    build(mirrored);
                }
                added += f != geo::kInvalid;
            }
            if (added == 0)
                throw CommandError(many ? "retopoDrawAddFace: those faces already exist or would make non-manifold edges"
                                        : "retopoDrawAddFace: that face already exists or would make a non-manifold edge");
            m.removeAllLoose();
            m.compact();
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            return n.path();
        },
    });

    ops::registerCommand({
        .name = "retopoDrawSetDots",
        .doc = "Retopo Draw: replace the retopo mesh's dots (the loose points a fill turns into a face) with world points. "
               "Creates the mesh when create=True. Returns the mesh name.",
        .flags = {{"mesh", "", T::String, nullptr, "Retopo mesh."},
                  {"dots", "", T::List, json::array(), "World [x, y, z] per dot."},
                  {"reference", "", T::String, nullptr, "Reference mesh, connected so Retopo Draw resumes on this pair later."},
                  {"create", "", T::Bool, false, "Create the mesh if it doesn't exist (taking the reference's symmetry)."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node& n = retopoDrawTarget(a, "retopoDrawSetDots", a.get<bool>("create"));
            std::vector<glm::dvec3> world = vecList(a.flags["dots"], "retopoDrawSetDots");
            g.evaluate();
            glm::dmat4 toLocal = glm::inverse(worldOf(n));
            json local = json::array();
            for (const glm::dvec3& p : world)
            {
                glm::dvec3 l(toLocal * glm::dvec4(p, 1.0));
                local.push_back({l.x, l.y, l.z});
            }
            std::string text = local.empty() ? std::string() : local.dump();
            if (n.as<std::string>(MeshNode::kRetopoDrawDots) != text)
                g.setValue({n.id(), MeshNode::kRetopoDrawDots, -1}, text);
            return n.path();
        },
    });

    ops::registerCommand({
        .name = "retopoDrawSetReference",
        .doc = "Retopo Draw: connect a different reference mesh to the retopo mesh, so drawing continues on that surface and "
               "resumes on it later. Returns the mesh name.",
        .flags = {{"mesh", "", T::String, nullptr, "Retopo mesh."},
                  {"reference", "", T::String, nullptr, "The new reference mesh."}},
        .run = [](const Args& a) -> json {
            if (!a.has("reference"))
                throw CommandError("retopoDrawSetReference: give reference=");
            return retopoDrawTarget(a, "retopoDrawSetReference", false).path();
        },
    });

    ops::registerCommand({
        .name = "retopoDrawDelete",
        .doc = "Retopo Draw: delete faces and points (base mesh ids) of the retopo mesh, mirrored with symmetry.",
        .flags = {{"mesh", "", T::String, nullptr, "Retopo mesh."},
                  {"faces", "", T::List, json::array(), "Face ids."},
                  {"points", "", T::List, json::array(), "Point ids."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node& n = retopoDrawTarget(a, "retopoDrawDelete", false);
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            std::optional<geo::SymmetryMap> map;
            if (auto plane = symmetryPlane(n))
                map = geo::SymmetryMap::build(m, *plane, symmetryTolerance(m));
            std::vector<Index> faces = a.flags["faces"].get<std::vector<Index>>();
            std::vector<Index> points = a.flags["points"].get<std::vector<Index>>();
            geo::deleteFaces(m, faces, map ? &*map : nullptr);
            geo::deletePoints(m, points, map ? &*map : nullptr);
            m.compact();
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            scene::components::clear();
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "retopoDrawMove",
        .doc = "Retopo Draw: move points (base mesh ids) to world positions; mirror partners follow with symmetry.",
        .flags = {{"mesh", "", T::String, nullptr, "Retopo mesh."},
                  {"points", "", T::List, json::array(), "Point ids."},
                  {"positions", "", T::List, json::array(), "World [x, y, z] per point."},
                  {"weld", "", T::Float, 0.0, "Merge a moved point into another point closer than this, cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node& n = retopoDrawTarget(a, "retopoDrawMove", false);
            std::vector<Index> points = a.flags["points"].get<std::vector<Index>>();
            std::vector<glm::dvec3> world = vecList(a.flags["positions"], "retopoDrawMove");
            if (points.size() != world.size())
                throw CommandError("retopoDrawMove: points and positions differ in length");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            std::optional<geo::SymmetryMap> map;
            if (auto plane = symmetryPlane(n))
                map = geo::SymmetryMap::build(m, *plane, symmetryTolerance(m));
            glm::dmat4 toLocal = glm::inverse(worldOf(n));
            for (size_t i = 0; i < points.size(); ++i)
            {
                Index p = points[i];
                if (!m.pointValid(p))
                    continue;
                glm::dvec3 at(toLocal * glm::dvec4(world[i], 1.0));
                if (map && map->onPlane(p))
                    at = map->plane().project(at);
                m.setPosition(p, at);
                if (map)
                    if (Index q = map->mirrorPoint(p); q != geo::kInvalid && q != p)
                        m.setPosition(q, map->plane().reflect(at));
            }
            if (double weld = a.get<double>("weld"); weld > 0.0)
            {
                // Dropping a point onto another merges them, like Retopo Draw's auto-weld.
                std::vector<Index> target(m.pointCapacity(), geo::kInvalid);
                bool any = false;
                for (Index p : points)
                {
                    if (!m.pointValid(p))
                        continue;
                    for (Index q = 0; q < m.pointCapacity(); ++q)
                        if (q != p && m.pointValid(q) && target[q] == geo::kInvalid && glm::length(m.position(q) - m.position(p)) <= weld)
                        {
                            target[p] = q;
                            any = true;
                            break;
                        }
                }
                if (any)
                {
                    geo::weldPoints(m, target);
                    m.compact();
                }
            }
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "retopoDrawInsertLoop",
        .doc = "Retopo Draw: insert an edge loop across the ring through an edge (base mesh id), mirrored with symmetry.",
        .flags = {{"mesh", "", T::String, nullptr, "Retopo mesh."},
                  {"edge", "", T::Int, nullptr, "An edge the loop crosses."},
                  {"fraction", "", T::Float, 0.5, "Where along the ring edges, 0 to 1."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node& n = retopoDrawTarget(a, "retopoDrawInsertLoop", false);
            if (!a.has("edge"))
                throw CommandError("retopoDrawInsertLoop: give edge=");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            Index e = Index(a.get<int>("edge"));
            if (!m.edgeValid(e))
                throw CommandError("retopoDrawInsertLoop: no such edge");
            Index mirrorEdge = geo::kInvalid;
            std::vector<Index> ring = geo::edgeRing(m, e);
            if (auto plane = symmetryPlane(n))
            {
                geo::SymmetryMap map = geo::SymmetryMap::build(m, *plane, symmetryTolerance(m));
                Index me = map.mirrorEdge(m, e);
                if (me != geo::kInvalid && std::find(ring.begin(), ring.end(), me) == ring.end())
                    mirrorEdge = me;
            }
            double fraction = std::clamp(a.get<double>("fraction"), 0.01, 0.99);
            // The mirror ring's fraction runs from the mirrored start point.
            glm::dvec3 mirrorStart{0.0};
            if (mirrorEdge != geo::kInvalid)
                mirrorStart = symmetryPlane(n)->reflect(m.position(m.edgePoint(e, 0)));
            geo::EdgeLoopOptions lo;
            lo.fraction = fraction;
            geo::insertEdgeLoop(m, e, lo);
            if (mirrorEdge != geo::kInvalid && m.edgeValid(mirrorEdge))
            {
                bool same = glm::length(m.position(m.edgePoint(mirrorEdge, 0)) - mirrorStart) < 1e-6 * (1.0 + glm::length(mirrorStart));
                lo.fraction = same ? fraction : 1.0 - fraction;
                geo::insertEdgeLoop(m, mirrorEdge, lo);
            }
            m.compact();
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            return nullptr;
        },
    });

    registerTopologyCommands();
}

scene::NodeId retopoDrawReference(const scene::Graph& g, scene::NodeId retopo)
{
    Node* n = g.node(retopo);
    if (!n || !n->type().isA("mesh"))
        return scene::kNoNode;
    auto src = g.source({retopo, MeshNode::kRetopoDrawReference, -1});
    Node* ref = src ? g.node(src->node) : nullptr;
    return ref && ref->type().isA("mesh") ? ref->id() : scene::kNoNode;
}

std::vector<glm::dvec3> retopoDrawDots(const scene::Node& retopo)
{
    std::vector<glm::dvec3> out;
    const std::string& text = retopo.as<std::string>(MeshNode::kRetopoDrawDots);
    if (text.empty())
        return out;
    json list = json::parse(text, nullptr, false);
    if (!list.is_array())
        return out;
    glm::dmat4 world = worldOf(retopo);
    for (const json& v : list)
        if (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number())
            out.emplace_back(world * glm::dvec4(v[0].get<double>(), v[1].get<double>(), v[2].get<double>(), 1.0));
    return out;
}

std::optional<RetopoDrawTargets> resolveRetopoDraw(const scene::Graph& g, const std::vector<scene::NodeId>& selection)
{
    std::vector<Node*> meshes;
    for (scene::NodeId id : selection)
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            meshes.push_back(n);
    if (meshes.empty())
        return std::nullopt;
    for (Node* n : meshes)
        if (scene::NodeId ref = retopoDrawReference(g, n->id()); ref != scene::kNoNode)
            return RetopoDrawTargets{ref, n->path()};
    Node* ref = meshes[0];
    if (meshes.size() > 1)
        return RetopoDrawTargets{ref->id(), meshes[1]->path()};
    for (scene::NodeId id : g.nodes())
        if (retopoDrawReference(g, id) == ref->id())
            return RetopoDrawTargets{ref->id(), g.node(id)->path()};
    // A lone "<name>_retopo" whose source mesh exists was drawn before the pair was connected: resume on it.
    const std::string suffix = "_retopo";
    const std::string& name = ref->name();
    if (name.size() > suffix.size() && name.ends_with(suffix))
    {
        std::vector<scene::NodeId> found = g.findAll(name.substr(0, name.size() - suffix.size()));
        if (found.size() == 1 && g.node(found[0])->type().isA("mesh"))
            return RetopoDrawTargets{found[0], ref->path()};
    }
    return RetopoDrawTargets{ref->id(), name + suffix};
}
} // namespace lila::retopo
