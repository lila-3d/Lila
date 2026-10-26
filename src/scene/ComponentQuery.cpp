#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace lila::scene
{
namespace
{
using components::Ids;
using geo::Index;

Node& meshNamed(const std::string& name, const char* cmd)
{
    Node* n = Graph::current().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** The mesh the command works on: the named one, else the one with selected components, else the selected one. */
Node& targetMesh(const Args& a, const char* cmd)
{
    if (!a.positional.empty())
        return meshNamed(a.positional.back().get<std::string>(), cmd);
    if (components::mode() != ComponentMode::Object)
        for (auto& [node, ids] : components::selected(components::mode()))
            if (Node* n = Graph::current().node(node))
                return *n;
    auto objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": no mesh");
    return meshNamed(objs.back(), cmd);
}

ComponentMode kindArg(const Args& a, const char* cmd)
{
    try
    {
        ComponentMode k = components::modeFromName(a.get<std::string>("kind", "face"));
        if (k == ComponentMode::Object)
            throw std::invalid_argument("kind must be vertex, edge or face");
        return k;
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

glm::dvec3 vec(const json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

json round3(const glm::dvec3& v)
{
    auto r = [](double x) { return std::round(x * 1e4) / 1e4; };
    return json::array({r(v.x), r(v.y), r(v.z)});
}

Ids idsFrom(const json& j)
{
    Ids out;
    for (const json& v : j)
    {
        if (v.is_array() && v.size() == 2)
            for (uint32_t i = v[0].get<uint32_t>(); i <= v[1].get<uint32_t>(); ++i)
                out.push_back(i);
        else
            out.push_back(v.get<uint32_t>());
    }
    return out;
}

/** Everything a predicate looks at for one component, in object space. */
struct Sample
{
    Index id = geo::kInvalid;
    glm::dvec3 pos{0.0};
    glm::dvec3 normal{0.0};
    bool border = false;
    glm::dvec3 direction{0.0}; // edges only: unit vector along the edge
};

glm::dvec3 pointNormal(const geo::Mesh& m, Index p)
{
    glm::dvec3 n(0.0);
    for (Index f : m.pointFaces(p))
        n += m.faceAreaVector(f);
    double len = glm::length(n);
    return len > 1e-12 ? n / len : n;
}

glm::dvec3 edgeNormal(const geo::Mesh& m, Index e)
{
    glm::dvec3 n(0.0);
    m.forEachEdgeCorner(e, [&](Index c) { n += m.faceNormal(m.cornerFace(c)); });
    double len = glm::length(n);
    return len > 1e-12 ? n / len : n;
}

std::vector<Sample> samples(const geo::Mesh& m, ComponentMode kind)
{
    std::vector<Sample> out;
    if (kind == ComponentMode::Face)
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            bool border = false;
            m.forEachFaceCorner(f, [&](Index c) { border |= m.isBoundaryEdge(m.cornerEdge(c)); });
            out.push_back({f, m.faceCentroid(f), m.faceNormal(f), border});
        }
    else if (kind == ComponentMode::Edge)
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            double len = glm::length(b - a);
            out.push_back({e, (a + b) * 0.5, edgeNormal(m, e), m.isBoundaryEdge(e), len > 1e-12 ? (b - a) / len : glm::dvec3(0.0)});
        }
    else
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                out.push_back({p, m.position(p), pointNormal(m, p), m.isBoundaryPoint(p)});
    return out;
}

struct Shell
{
    size_t faces = 0;
    glm::dvec3 lo{1e300}, hi{-1e300}; // object space
};

/** Connected shells, largest first, and each face's shell number in that order (kInvalid for unused slots). */
std::vector<Shell> shellsOf(const geo::Mesh& m, std::vector<Index>& faceShell)
{
    Index count = geo::faceComponents(m, faceShell);
    std::vector<Shell> shells(count);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            Shell& s = shells[faceShell[f]];
            ++s.faces;
            m.forEachFaceCorner(f, [&](Index c) {
                s.lo = glm::min(s.lo, m.position(m.cornerPoint(c)));
                s.hi = glm::max(s.hi, m.position(m.cornerPoint(c)));
            });
        }
    std::vector<Index> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](Index a, Index b) { return shells[a].faces > shells[b].faces; });
    std::vector<Index> rank(count);
    std::vector<Shell> sorted;
    for (Index i = 0; i < count; ++i)
    {
        rank[order[i]] = i;
        sorted.push_back(shells[order[i]]);
    }
    for (Index& s : faceShell)
        if (s != geo::kInvalid)
            s = rank[s];
    return sorted;
}

/** Points whose faces do not form one fan: two parts touching at a single vertex (a bowtie). */
size_t nonManifoldPoints(const geo::Mesh& m)
{
    size_t bad = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        std::vector<Index> faces = m.pointFaces(p);
        if (faces.size() < 2)
            continue;
        std::vector<size_t> parent(faces.size());
        std::iota(parent.begin(), parent.end(), size_t(0));
        auto root = [&](size_t i) {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        auto slot = [&](Index f) { return size_t(std::find(faces.begin(), faces.end(), f) - faces.begin()); };
        m.forEachPointEdge(p, [&](Index e) {
            size_t first = faces.size();
            m.forEachEdgeCorner(e, [&](Index c) {
                size_t s = slot(m.cornerFace(c));
                if (s == faces.size())
                    return;
                if (first == faces.size())
                    first = s;
                else
                    parent[root(s)] = root(first);
            });
        });
        size_t fans = 0;
        for (size_t i = 0; i < faces.size(); ++i)
            fans += root(i) == i;
        bad += fans > 1;
    }
    return bad;
}

/** "+x", "-y", ... as axis index and sign; "center" as sign 0 on the axis given, x by default. */
std::pair<int, int> sideArg(const std::string& s, const char* cmd)
{
    auto axisOf = [&](char c) {
        if (c < 'x' || c > 'z')
            throw CommandError(std::string(cmd) + ": side must look like '+x', '-z' or 'center'");
        return c - 'x';
    };
    if (s == "center" || s == "centre" || s == "middle")
        return {0, 0};
    if (s.size() == 2 && (s[0] == '+' || s[0] == '-'))
        return {axisOf(s[1]), s[0] == '+' ? 1 : -1};
    if (s.size() == 1)
        return {axisOf(s[0]), 1};
    if (s.size() == 7 && s.starts_with("center"))
        return {axisOf(s[6]), 0};
    throw CommandError(std::string(cmd) + ": side must look like '+x', '-z' or 'center'");
}

std::vector<FlagDef> queryFlags()
{
    using T = ArgType;
    return {
        {"kind", "k", T::String, "face", "vertex, edge or face.", {"vertex", "edge", "face"}},
        {"normal", "n", T::Vec3, nullptr, "Keep components whose normal is within angle of this direction (world space)."},
        {"angle", "", T::Float, 30.0, "Tolerance for normal=, degrees."},
        {"side", "s", T::String, nullptr,
         "'+x', '-x', '+y', ... keeps components whose centre is on that side of the object-space origin plane; 'center' (or "
         "'centerx', 'centery', 'centerz') keeps those on the plane."},
        {"box", "b", T::List, nullptr,
         "[[minX, minY, minZ], [maxX, maxY, maxZ]], world space: keep components whose centre is inside. None leaves that side open, "
         "so [[None, 20, None], [None, None, None]] is everything above y=20."},
        {"direction", "dir", T::Vec3, nullptr,
         "Edges only: keep edges running along this world direction (either way) within angle; (0, 1, 0) finds vertical edges, "
         "whose ring polyInsertLoop cuts with a horizontal loop."},
        {"near", "", T::Vec3, nullptr, "World point; with radius= keeps components within it, else keeps the count nearest."},
        {"radius", "r", T::Float, nullptr, "Distance for near=, cm."},
        {"extreme", "x", T::String, nullptr,
         "'+y', '-z', ...: keep the count components furthest along that world direction (the top face is extreme='+y')."},
        {"count", "c", T::Int, 1, "How many to keep for extreme= or near= without radius."},
        {"border", "", T::Bool, nullptr, "True keeps only components on an open border, False only interior ones."},
        {"shell", "", T::Int, nullptr, "Keep components of one connected shell: 0 is the largest, numbered as meshCheck lists them."},
        {"fromSelection", "fs", T::Bool, false, "Only consider the currently selected components of that kind."},
        {"tolerance", "t", T::Float, nullptr, "Distance counted as on a plane for side=, cm (default scales with the mesh)."},
    };
}

/** Ids of one mesh's components passing every predicate given. */
Ids query(Node& n, const Args& a, const char* cmd)
{
    LILA_ZONE();

    ComponentMode kind = kindArg(a, cmd);
    Graph& g = Graph::current();
    g.evaluate();
    MeshPtr mesh = modifiers::cage(g, n.id());
    if (!mesh)
        return {};
    const geo::Mesh& m = *mesh;
    glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
    glm::dmat3 normalWorld = glm::inverseTranspose(glm::dmat3(world));
    auto toWorld = [&](const glm::dvec3& p) { return glm::dvec3(world * glm::dvec4(p, 1.0)); };
    auto dirWorld = [&](const glm::dvec3& d) {
        glm::dvec3 w = normalWorld * d;
        double len = glm::length(w);
        return len > 1e-12 ? w / len : w;
    };

    std::vector<Sample> all = samples(m, kind);
    if (a.get<bool>("fromSelection"))
    {
        const Ids& sel = components::selected(n.id(), kind);
        std::erase_if(all, [&](const Sample& s) { return !std::binary_search(sel.begin(), sel.end(), s.id); });
    }
    if (a.has("normal"))
    {
        glm::dvec3 want = vec(a.flags["normal"]);
        if (glm::length(want) < 1e-12)
            throw CommandError(std::string(cmd) + ": normal= must not be zero");
        want = glm::normalize(want);
        double cosMax = std::cos(glm::radians(std::clamp(a.get<double>("angle", 30.0), 0.0, 180.0)));
        std::erase_if(all, [&](const Sample& s) { return glm::dot(dirWorld(s.normal), want) < cosMax - 1e-9; });
    }
    if (a.has("side"))
    {
        auto [axis, sign] = sideArg(a.get<std::string>("side"), cmd);
        double tol = a.has("tolerance") ? a.get<double>("tolerance") : std::max(1e-3, glm::length(m.bounds().size()) * 1e-4);
        std::erase_if(all, [&](const Sample& s) {
            double v = s.pos[axis];
            return sign == 0 ? std::abs(v) > tol : sign > 0 ? v <= tol : v >= -tol;
        });
    }
    if (a.has("direction"))
    {
        if (kind != ComponentMode::Edge)
            throw CommandError(std::string(cmd) + ": direction= is for kind='edge'");
        glm::dvec3 want = vec(a.flags["direction"]);
        if (glm::length(want) < 1e-12)
            throw CommandError(std::string(cmd) + ": direction= must not be zero");
        want = glm::normalize(want);
        double cosMax = std::cos(glm::radians(std::clamp(a.get<double>("angle", 30.0), 0.0, 90.0)));
        std::erase_if(all, [&](const Sample& s) {
            glm::dvec3 d = glm::dvec3(world * glm::dvec4(s.direction, 0.0));
            double len = glm::length(d);
            return len < 1e-12 || std::abs(glm::dot(d / len, want)) < cosMax - 1e-9;
        });
    }
    if (a.has("box"))
    {
        const json& b = a.flags["box"];
        if (!b.is_array() || b.size() != 2 || !b[0].is_array() || !b[1].is_array() || b[0].size() != 3 || b[1].size() != 3)
            throw CommandError(std::string(cmd) + ": box= is [[minX, minY, minZ], [maxX, maxY, maxZ]]");
        auto bound = [](const json& v, double open) { return v.is_number() ? v.get<double>() : open; };
        glm::dvec3 lo, hi;
        for (int i = 0; i < 3; ++i)
        {
            lo[i] = bound(b[0][i], -1e300);
            hi[i] = bound(b[1][i], 1e300);
        }
        std::erase_if(all, [&](const Sample& s) {
            glm::dvec3 p = toWorld(s.pos);
            return glm::any(glm::lessThan(p, lo)) || glm::any(glm::greaterThan(p, hi));
        });
    }
    if (a.has("border"))
    {
        bool want = a.get<bool>("border");
        std::erase_if(all, [&](const Sample& s) { return s.border != want; });
    }
    if (a.has("shell"))
    {
        std::vector<Index> faceShell;
        shellsOf(m, faceShell);
        Index want = Index(a.get<int>("shell"));
        auto shellOf = [&](Index id) {
            Index f = geo::kInvalid;
            if (kind == ComponentMode::Face)
                f = id;
            else if (kind == ComponentMode::Edge)
                m.forEachEdgeCorner(id, [&](Index c) { f = m.cornerFace(c); });
            else if (auto fs = m.pointFaces(id); !fs.empty())
                f = fs.front();
            return f == geo::kInvalid ? geo::kInvalid : faceShell[f];
        };
        std::erase_if(all, [&](const Sample& s) { return shellOf(s.id) != want; });
    }
    int count = std::max(1, a.get<int>("count", 1));
    if (a.has("near"))
    {
        glm::dvec3 at = vec(a.flags["near"]);
        if (a.has("radius"))
        {
            double r = a.get<double>("radius");
            std::erase_if(all, [&](const Sample& s) { return glm::length(toWorld(s.pos) - at) > r; });
        }
        else
        {
            std::stable_sort(all.begin(), all.end(), [&](const Sample& x, const Sample& y) {
                return glm::length(toWorld(x.pos) - at) < glm::length(toWorld(y.pos) - at);
            });
            if (int(all.size()) > count)
                all.resize(size_t(count));
        }
    }
    if (a.has("extreme"))
    {
        auto [axis, sign] = sideArg(a.get<std::string>("extreme"), cmd);
        if (sign == 0)
            throw CommandError(std::string(cmd) + ": extreme= is a direction like '+y'");
        std::stable_sort(all.begin(), all.end(), [&](const Sample& x, const Sample& y) {
            return toWorld(x.pos)[axis] * sign > toWorld(y.pos)[axis] * sign;
        });
        if (int(all.size()) > count)
            all.resize(size_t(count));
    }
    Ids out;
    for (const Sample& s : all)
        out.push_back(s.id);
    std::sort(out.begin(), out.end());
    return out;
}

json faceInfo(const geo::Mesh& m, Index f, const glm::dmat4& w, const glm::dmat3& nw)
{
    json pts = json::array(), edges = json::array(), nbrs = json::array();
    m.forEachFaceCorner(f, [&](Index c) {
        pts.push_back(m.cornerPoint(c));
        edges.push_back(m.cornerEdge(c));
        Index o = m.oppositeFace(m.cornerEdge(c), f);
        if (o != geo::kInvalid)
            nbrs.push_back(o);
    });
    glm::dvec3 n = nw * m.faceNormal(f);
    return {{"id", f},
            {"center", round3(glm::dvec3(w * glm::dvec4(m.faceCentroid(f), 1.0)))},
            {"normal", round3(glm::length(n) > 1e-12 ? glm::normalize(n) : n)},
            {"area", std::round(m.faceArea(f) * 1e3) / 1e3},
            {"points", pts},
            {"edges", edges},
            {"neighbors", nbrs}};
}

json edgeInfo(const geo::Mesh& m, Index e, const glm::dmat4& w)
{
    json faces = json::array();
    m.forEachEdgeCorner(e, [&](Index c) { faces.push_back(m.cornerFace(c)); });
    glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
    return {{"id", e},
            {"points", {m.edgePoint(e, 0), m.edgePoint(e, 1)}},
            {"center", round3(glm::dvec3(w * glm::dvec4((a + b) * 0.5, 1.0)))},
            {"length", std::round(m.edgeLength(e) * 1e3) / 1e3},
            {"faces", faces},
            {"border", m.isBoundaryEdge(e)}};
}

json pointInfo(const geo::Mesh& m, Index p, const glm::dmat4& w, const glm::dmat3& nw)
{
    json edges = json::array(), nbrs = json::array(), faces = json::array();
    m.forEachPointEdge(p, [&](Index e) {
        edges.push_back(e);
        nbrs.push_back(m.edgeOtherPoint(e, p));
    });
    for (Index f : m.pointFaces(p))
        faces.push_back(f);
    glm::dvec3 n = nw * pointNormal(m, p);
    return {{"id", p},
            {"position", round3(glm::dvec3(w * glm::dvec4(m.position(p), 1.0)))},
            {"normal", round3(glm::length(n) > 1e-12 ? glm::normalize(n) : n)},
            {"edges", edges},
            {"neighbors", nbrs},
            {"faces", faces}};
}
} // namespace

/** Counts and health of a mesh for meshInfo: topology, open borders, non-manifold edges, shells, stack and symmetry. */
json meshStats(Node& n)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    g.evaluate();
    MeshPtr m = n.as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        return {{"points", 0}, {"edges", 0}, {"faces", 0}, {"triangles", 0}};
    size_t tris = 0, triFaces = 0, quads = 0, ngons = 0;
    for (Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
        {
            Index k = m->faceSize(f);
            tris += k - 2;
            (k == 3 ? triFaces : k == 4 ? quads : ngons)++;
        }
    size_t border = 0, nonManifold = 0;
    for (Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e))
        {
            Index c = m->edgeFaceCount(e);
            border += c == 1;
            nonManifold += c > 2;
        }
    std::vector<Index> comp;
    Index shells = geo::faceComponents(*m, comp);
    geo::Bounds b = m->bounds();
    glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
    glm::dvec3 lo(1e300), hi(-1e300);
    for (int i = 0; i < 8; ++i)
    {
        glm::dvec3 c = glm::dvec3(w * glm::dvec4(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z, 1.0));
        lo = glm::min(lo, c);
        hi = glm::max(hi, c);
    }
    json mods = json::array();
    for (NodeId id : modifiers::stack(g, n.id()))
        if (Node* mod = g.node(id))
            mods.push_back(mod->name() + " (" + mod->type().name + ")");
    static const char* axes[] = {"off", "x", "y", "z", "radial"};
    int64_t sym = std::clamp<int64_t>(n.as<int64_t>(MeshNode::kSymmetry), 0, kSymmetryRadial);
    json out = {{"points", m->numPoints()},
                {"edges", m->numEdges()},
                {"faces", m->numFaces()},
                {"triangles", tris},
                {"triFaces", triFaces},
                {"quads", quads},
                {"ngons", ngons},
                {"borderEdges", border},
                {"boundaryLoops", geo::boundaryLoops(*m).size()},
                {"nonManifoldEdges", nonManifold},
                {"shells", shells},
                {"closed", border == 0 && nonManifold == 0},
                {"boundsMin", {b.min.x, b.min.y, b.min.z}},
                {"boundsMax", {b.max.x, b.max.y, b.max.z}},
                {"worldBoundsMin", round3(lo)},
                {"worldBoundsMax", round3(hi)},
                {"symmetry", axes[sym]},
                {"smoothPreview", n.as<int64_t>(MeshNode::kSmoothPreview)},
                {"modifiers", mods},
                {"materialSlots", m->materialNames()}};
    if (MeshPtr cage = modifiers::cage(g, n.id()); cage && cage != m)
        out["cage"] = {{"points", cage->numPoints()}, {"edges", cage->numEdges()}, {"faces", cage->numFaces()}};
    return out;
}

namespace
{
json worldBox(const glm::dmat4& w, const glm::dvec3& lo, const glm::dvec3& hi)
{
    glm::dvec3 a(1e300), b(-1e300);
    for (int i = 0; i < 8; ++i)
    {
        glm::dvec3 c = glm::dvec3(w * glm::dvec4(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z, 1.0));
        a = glm::min(a, c);
        b = glm::max(b, c);
    }
    auto r = [](const glm::dvec3& v) { return json::array({std::round(v.x * 100) / 100, std::round(v.y * 100) / 100, std::round(v.z * 100) / 100}); };
    return json::array({r(a), r(b)});
}

/** Pass/fail validation of a finished model, small enough to print after every block of edits. */
json meshCheck(Node& n, const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    g.evaluate();
    MeshPtr mesh = n.as<MeshPtr>(MeshNode::kOutMesh);
    if (!mesh)
        throw CommandError("meshCheck: " + n.path() + " has no mesh");
    const geo::Mesh& m = *mesh;
    glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);

    size_t tris = 0, triFaces = 0, quads = 0, ngons = 0, degenerate = 0;
    double scale = glm::length(m.bounds().size());
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            Index k = m.faceSize(f);
            tris += k - 2;
            (k == 3 ? triFaces : k == 4 ? quads : ngons)++;
            degenerate += m.faceArea(f) < 1e-8 * scale * scale;
        }
    size_t border = 0, nonManifold = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            Index c = m.edgeFaceCount(e);
            border += c == 1;
            nonManifold += c > 2;
        }
    size_t bowties = nonManifoldPoints(m);
    std::vector<Index> faceShell;
    std::vector<Shell> shells = shellsOf(m, faceShell);
    json shellList = json::array();
    for (size_t i = 0; i < shells.size() && i < 12; ++i)
        shellList.push_back({{"faces", shells[i].faces}, {"box", worldBox(w, shells[i].lo, shells[i].hi)}});

    int expect = a.get<int>("expectShells", 1);
    json issues = json::array(), notes = json::array();
    if (expect > 0 && int(shells.size()) != expect)
        issues.push_back(std::format(
            "{} shells, expected {}: parts float separately instead of growing out of one body. Rebuild them by extruding faces of the "
            "body, or join each to the body: polyCombine, delete the facing faces of both parts, select the two open border loops "
            "(selectComponentsWhere(kind='edge', border=True)) and polyBridge. findComponents(shell=i) selects one.",
            shells.size(), expect));
    if (border)
        issues.push_back(std::format("{} open border edges in {} holes: select them (kind='edge', border=True) and polyFillHole, or "
                                     "polyBridge two loops.",
                                     border, geo::boundaryLoops(m).size()));
    if (nonManifold)
        issues.push_back(std::format("{} non-manifold edges (3+ faces on one edge), usually an extrude into existing faces or merged "
                                     "overlapping parts: undo it, or delete the extra faces.",
                                     nonManifold));
    if (bowties)
        issues.push_back(std::format("{} non-manifold vertices (parts touching at one point): polyMerge / polyBridge them properly.", bowties));
    if (degenerate)
        issues.push_back(std::format("{} zero-area faces: polyMerge(distance=0.01) on the mesh.", degenerate));
    if (a.has("maxTriangles") && int64_t(tris) > a.get<int64_t>("maxTriangles"))
        issues.push_back(std::format("{} triangles, over the budget of {}.", tris, a.get<int64_t>("maxTriangles")));
    if (ngons)
        notes.push_back(std::format("{} ngons (fine flat-shaded; triangulate or split them before subdividing).", ngons));

    json out = {{"mesh", n.path()},
                {"ok", issues.empty()},
                {"faces", m.numFaces()},
                {"triangles", tris},
                {"quads", quads},
                {"tris", triFaces},
                {"ngons", ngons},
                {"shells", shells.size()},
                {"shellList", shellList},
                {"borderEdges", border},
                {"nonManifoldEdges", nonManifold},
                {"nonManifoldVerts", bowties},
                {"box", worldBox(w, m.bounds().min, m.bounds().max)}};
    if (n.as<int64_t>(MeshNode::kSymmetry) > 0)
    {
        try
        {
            json s = ops::execute("symmetryCheck", {{"args", {n.path()}}})[n.path()];
            out["symmetry"] = {{"axis", s["axis"]}, {"maxAsymmetry", s["maxAsymmetry"]}, {"unmatched", s["unmatched"]}};
            if (s["unmatched"].get<int64_t>() > 0 || s["maxAsymmetry"].get<double>() > 1e-3 * std::max(1.0, scale))
                notes.push_back("halves differ: symmetrize(mesh) if that is not intended.");
        }
        catch (const std::exception&)
        {
        }
    }
    out["issues"] = issues;
    if (!notes.empty())
        out["notes"] = notes;
    return out;
}
} // namespace

void registerComponentQueryCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "meshCheck",
        .doc = "Validate a model in one compact report: ok (no issues), shells with their world boxes (largest first), open border "
               "edges, non-manifold edges and vertices, zero-area faces, face/triangle counts, symmetry drift. issues says what is "
               "wrong and how to fix it. Changes nothing.",
        .positionalDoc = "[mesh]",
        .flags = {{"expectShells", "", T::Int, 1, "Connected parts the model should have (0 skips the check)."},
                  {"maxTriangles", "", T::Int, nullptr, "Triangle budget; more is an issue."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return meshCheck(targetMesh(a, "meshCheck"), a); },
    });

    ops::registerCommand({
        .name = "findComponents",
        .doc = "Ids of a mesh's vertices, edges or faces matching every predicate given (normal direction, side of the symmetry "
               "plane, box, distance, extreme along an axis, open border). Ids index the editable cage, like selectComponents. "
               "Changes nothing; see selectComponentsWhere to select them.",
        .positionalDoc = "[mesh]",
        .flags = queryFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json { return query(targetMesh(a, "findComponents"), a, "findComponents"); },
    });

    std::vector<FlagDef> selFlags = queryFlags();
    selFlags.push_back({"add", "", T::Bool, false, "Add to the selection of that kind instead of replacing it."});
    selFlags.push_back({"deselect", "", T::Bool, false, "Remove the matches from the selection."});
    ops::registerCommand({
        .name = "selectComponentsWhere",
        .doc = "Select a mesh's components by predicates (the flags of findComponents) and switch to that component mode. "
               "With symmetry on, edits still mirror; select one side. Returns the selected ids.",
        .positionalDoc = "[mesh]",
        .flags = selFlags,
        .run = [](const Args& a) -> json {
            Node& n = targetMesh(a, "selectComponentsWhere");
            ComponentMode kind = kindArg(a, "selectComponentsWhere");
            Ids ids = query(n, a, "selectComponentsWhere");
            components::Op op = a.get<bool>("add") ? components::Op::Add : a.get<bool>("deselect") ? components::Op::Remove : components::Op::Replace;
            Graph& g = Graph::current();
            // Picking components hilites their mesh, as clicking it would.
            if (!g.isSelected(n.id()))
                ops::execute("select", {{"args", {n.path()}}});
            components::select(n.id(), kind, ids, op);
            components::setMode(kind);
            return components::selected(n.id(), kind);
        },
    });

    ops::registerCommand({
        .name = "componentInfo",
        .doc = "Positions, normals and adjacency of a mesh's components in bulk. Faces: center, normal, area, points, edges, "
               "neighbors (faces across each edge). Edges: points, center, length, faces, border. Vertices: position, normal, "
               "edges, neighbors (points), faces. World space unless space='object'. ids= picks some (default: the selected ones of "
               "that kind, or all).",
        .positionalDoc = "[mesh]",
        .flags = {{"kind", "k", T::String, "face", "vertex, edge or face.", {"vertex", "edge", "face"}},
                  {"ids", "", T::List, nullptr, "Component ids; [first, last] pairs are ranges."},
                  {"space", "", T::String, "world", "world or object.", {"world", "object"}},
                  {"fields", "", T::List, nullptr, "Keys to keep per component, e.g. ['id', 'center', 'normal'] (default all)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = targetMesh(a, "componentInfo");
            ComponentMode kind = kindArg(a, "componentInfo");
            Graph& g = Graph::current();
            g.evaluate();
            MeshPtr mesh = modifiers::cage(g, n.id());
            json out = json::array();
            if (!mesh)
                return out;
            const geo::Mesh& m = *mesh;
            glm::dmat4 w = a.get<std::string>("space", "world") == "object" ? glm::dmat4(1.0) : n.as<glm::dmat4>(Transform::kWorldMatrix);
            glm::dmat3 nw = glm::inverseTranspose(glm::dmat3(w));
            Ids ids;
            if (a.has("ids"))
                ids = idsFrom(a.flags["ids"]);
            else if (!components::selected(n.id(), kind).empty())
                ids = components::selected(n.id(), kind);
            else
            {
                Index cap = kind == ComponentMode::Face ? m.faceCapacity() : kind == ComponentMode::Edge ? m.edgeCapacity() : m.pointCapacity();
                ids.resize(cap);
                std::iota(ids.begin(), ids.end(), 0u);
            }
            for (Index id : ids)
            {
                if (kind == ComponentMode::Face && m.faceValid(id))
                    out.push_back(faceInfo(m, id, w, nw));
                else if (kind == ComponentMode::Edge && m.edgeValid(id))
                    out.push_back(edgeInfo(m, id, w));
                else if (kind == ComponentMode::Vertex && m.pointValid(id))
                    out.push_back(pointInfo(m, id, w, nw));
            }
            if (a.has("fields"))
            {
                auto keep = a.flags["fields"].get<std::vector<std::string>>();
                for (json& c : out)
                {
                    json trimmed = json::object();
                    for (const std::string& k : keep)
                        if (c.contains(k))
                            trimmed[k] = c[k];
                    c = std::move(trimmed);
                }
            }
            return out;
        },
    });
}
} // namespace lila::scene
