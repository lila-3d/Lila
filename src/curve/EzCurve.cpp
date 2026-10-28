#include "curve/CurveNodes.h"
#include "curve/EnvironmentNodes.h"
#include "curve/Fit.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
struct EzSurface
{
    MeshPtr mesh;
    std::shared_ptr<const geo::MeshBvh> bvh;
    glm::dmat4 world{1.0}, inverse{1.0};
};

struct EzSurfacePoint
{
    D3 point{0.0}, normal{0.0, 1.0, 0.0};
    double distance = 1e300;
};

/** Object-space BVH per mesh snapshot, so dragging an adjustment re-runs without rebuilding it. */
std::shared_ptr<const geo::MeshBvh> ezBvhFor(const MeshPtr& mesh)
{
    struct Entry
    {
        std::weak_ptr<const geo::Mesh> source;
        std::shared_ptr<const geo::MeshBvh> bvh;
    };
    static std::unordered_map<const geo::Mesh*, Entry> cache;
    if (auto it = cache.find(mesh.get()); it != cache.end() && it->second.source.lock() == mesh)
        return it->second.bvh;
    std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });
    auto bvh = std::make_shared<geo::MeshBvh>(*mesh);
    cache[mesh.get()] = {mesh, bvh};
    return bvh;
}

std::vector<EzSurface> ezSurfaces(const Args& a)
{
    std::vector<EzSurface> out;
    if (!a.has("mesh"))
        return out;
    Graph& g = Graph::current();
    g.evaluate();
    json names = a.flags["mesh"].is_array() ? a.flags["mesh"] : json::array({a.flags["mesh"]});
    for (const json& name : names)
    {
        Node* n = g.find(name.get<std::string>());
        if (!n || !n->type().isA("mesh"))
            throw CommandError("ezCurve: " + name.get<std::string>() + " is not a mesh");
        MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m || m->numFaces() == 0)
            continue;
        glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        out.push_back({m, ezBvhFor(m), w, glm::inverse(w)});
    }
    return out;
}

std::optional<EzSurfacePoint> ezClosest(const std::vector<EzSurface>& surfaces, const D3& world)
{
    std::optional<EzSurfacePoint> best;
    for (const EzSurface& s : surfaces)
    {
        geo::BvhClosest c;
        if (!s.bvh->closestPoint(D3(s.inverse * glm::dvec4(world, 1.0)), c) || c.face == geo::kInvalid || !s.mesh->faceValid(c.face))
            continue;
        const D3 p(s.world * glm::dvec4(c.point, 1.0));
        const double d = glm::length(p - world);
        if (best && d >= best->distance)
            continue;
        D3 n = glm::transpose(glm::inverse(glm::dmat3(s.world))) * s.mesh->faceNormal(c.face);
        const double len = glm::length(n);
        best = EzSurfacePoint{p, len > 1e-12 ? n / len : D3(0.0, 1.0, 0.0), d};
    }
    return best;
}

D3 ezVec3(const Args& a, const char* flag)
{
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("ezCurve: ") + flag + " expects [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

D3 ezUp() { return math::upAxis() == math::UpAxis::Z ? D3(0, 0, 1) : D3(0, 1, 0); }

/** Unit vector perpendicular to axis, as close to want as possible. */
D3 ezPerpendicular(const D3& axis, D3 want)
{
    want -= axis * glm::dot(want, axis);
    if (glm::length(want) < 1e-6)
        want = glm::cross(axis, std::abs(axis.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0));
    return glm::normalize(want);
}

/** Respaces a polyline to count points evenly along its length, ends kept. */
std::vector<D3> ezResample(const std::vector<D3>& pts, size_t count)
{
    std::vector<double> at(pts.size(), 0.0);
    for (size_t i = 1; i < pts.size(); ++i)
        at[i] = at[i - 1] + glm::length(pts[i] - pts[i - 1]);
    std::vector<D3> out;
    size_t seg = 1;
    for (size_t k = 0; k < count; ++k)
    {
        const double want = at.back() * double(k) / double(count - 1);
        while (seg + 1 < pts.size() && at[seg] < want)
            ++seg;
        const double span = at[seg] - at[seg - 1];
        const double f = span > 1e-12 ? std::clamp((want - at[seg - 1]) / span, 0.0, 1.0) : 0.0;
        out.push_back(glm::mix(pts[seg - 1], pts[seg], f));
    }
    return out;
}

/**
 * The EZ curve's points: the chord from start to end, laid onto the surfaces when following them, then bowed out by bend
 * (in the plane turned by twist about the chord) and down by sag, both as fractions of the chord's length.
 */
std::vector<D3> ezPoints(const Args& a, const std::vector<EzSurface>& surfaces)
{
    const D3 start = ezVec3(a, "start"), end = ezVec3(a, "end");
    const D3 chord = end - start;
    const double length = glm::length(chord);
    if (length < 1e-6)
        throw CommandError("ezCurve: start and end are the same point");
    const D3 axis = chord / length;
    const size_t count = size_t(std::clamp(a.get<int>("points", 12), 3, 128));
    const double lift = a.get<double>("lift", 0.0);
    const bool follow = !surfaces.empty() && a.get<bool>("follow", true);
    const bool clearance = !surfaces.empty() && a.get<bool>("clearance", true);

    D3 up = ezUp();
    if (a.has("startNormal") || a.has("endNormal"))
        up = (a.has("startNormal") ? ezVec3(a, "startNormal") : D3(0.0)) + (a.has("endNormal") ? ezVec3(a, "endNormal") : D3(0.0));
    const D3 bendDir = glm::rotate(ezPerpendicular(axis, up), glm::radians(a.get<double>("twist", 0.0)), axis);

    std::vector<D3> pts(count);
    for (size_t i = 0; i < count; ++i)
        pts[i] = start + chord * (double(i) / double(count - 1));

    std::vector<D3> normals(count, bendDir);
    if (follow)
        // Projecting bunches points where the surface curves away from the chord; respacing between passes evens them out.
        for (int pass = 0; pass < 4; ++pass)
        {
            for (size_t i = 0; i < count; ++i)
                if (auto c = ezClosest(surfaces, pts[i]))
                {
                    pts[i] = c->point;
                    normals[i] = c->normal;
                }
            if (pass < 3)
                pts = ezResample(pts, count);
        }

    const double bend = a.get<double>("bend", 0.0) * length, sag = a.get<double>("sag", 0.0) * length;
    const D3 down = -ezUp();
    for (size_t i = 0; i < count; ++i)
    {
        const double arch = std::sin(glm::pi<double>() * double(i) / double(count - 1));
        // Following a surface, the bow leaves it along its normal there, turned by twist about the chord.
        const D3 out = follow ? glm::rotate(normals[i], glm::radians(a.get<double>("twist", 0.0)), axis) : bendDir;
        pts[i] += (follow ? normals[i] * lift : D3(0.0)) + out * (bend * arch) + down * (sag * arch);
    }

    if (clearance)
        for (D3& p : pts)
            if (auto c = ezClosest(surfaces, p); c && glm::dot(p - c->point, c->normal) < lift)
                p = c->point + c->normal * lift;
    return pts;
}
} // namespace

void registerEzCurveCommand()
{
    ops::registerCommand({
        .name = "ezCurve",
        .doc = "Create a curve between two points that adapts to the meshes it runs over: it follows their surface (follow) and "
               "is kept out of them (clearance), lifted off by lift. bend bows it out and sag droops it, as fractions of the "
               "distance between the points; twist turns the bow about that line. Returns the curve.",
        .flags = {{"start", "s", T::Vec3, nullptr, "[x, y, z] start, world space, cm."},
                  {"end", "e", T::Vec3, nullptr, "[x, y, z] end, world space, cm."},
                  {"startNormal", "sn", T::Vec3, nullptr, "Surface normal at the start, which way the bow faces."},
                  {"endNormal", "en", T::Vec3, nullptr, "Surface normal at the end."},
                  {"mesh", "m", T::List, nullptr, "Meshes to adapt to (names)."},
                  {"bend", "b", T::Float, 0.0, "Bow out from the straight line, fraction of its length."},
                  {"twist", "tw", T::Float, 0.0, "Turns the bow about the line, degrees."},
                  {"sag", "sg", T::Float, 0.0, "Droop down under gravity, fraction of the length."},
                  {"lift", "l", T::Float, 0.0, "Height off the surface, cm."},
                  {"follow", "f", T::Bool, true, "Follow the surface between the points."},
                  {"clearance", "c", T::Bool, true, "Push the curve out of the meshes wherever it dips into them."},
                  {"points", "p", T::Int, 12, "Points the curve passes through.", {}, {}, 3.0, 128.0},
                  {"symmetry", "sym", T::String, "off", "Mirror the curve live across the world plane facing this axis.", {"off", "x", "y", "z"}},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            if (!a.has("start") || !a.has("end"))
                throw CommandError("ezCurve: give start= and end=");
            const std::vector<EzSurface> surfaces = ezSurfaces(a);
            const std::vector<D3> pts = ezPoints(a, surfaces);
            Graph& g = Graph::current();
            Node* n = g.createNode(*types::find("curve"), a.get<std::string>("name", "EzCurve"), kNoNode, true);
            g.setValue({n->id(), CurveShape::kCreate, -1}, makeCurveData({interpolate(pts, 3, false)}));
            if (const std::string axis = a.get<std::string>("symmetry", "off"); axis != "off")
                g.setValue({n->id(), n->type().attrIndex("symmetry"), -1}, int64_t(axis == "x" ? 1 : axis == "y" ? 2 : 3));
            g.select({n->id()});
            return n->path();
        },
        .adjustable = true,
        .examples = {"lila.ops.ezCurve(start=[0, 0, 0], end=[100, 0, 0], bend=0.25, twist=30)"},
    });
}
} // namespace lila::curve
