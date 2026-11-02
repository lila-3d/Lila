#include "scene/ComponentPick.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Triangulate.h"
#include "scene/Node.h"

#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace lila::scene::componentPick
{
std::optional<glm::dvec2> Screen::project(const glm::dvec3& world) const
{
    glm::dvec4 clip = viewProj * glm::dvec4(world, 1.0);
    if (clip.w <= 1e-9)
        return std::nullopt;
    return glm::dvec2((clip.x / clip.w * 0.5 + 0.5) * width, (clip.y / clip.w * 0.5 + 0.5) * height);
}

namespace
{
/** Signed distance to the near plane in clip space, positive in front: reversed-Z puts the near plane at z = w. */
double inFront(const glm::dvec4& clip)
{
    return clip.w - clip.z;
}

glm::dvec4 nearCrossing(const glm::dvec4& a, const glm::dvec4& b)
{
    const double da = inFront(a), db = inFront(b);
    return glm::mix(a, b, da / (da - db));
}

glm::dvec2 toPixels(const glm::dvec4& clip, double width, double height)
{
    return glm::dvec2((clip.x / clip.w * 0.5 + 0.5) * width, (clip.y / clip.w * 0.5 + 0.5) * height);
}
} // namespace

std::optional<std::pair<glm::dvec2, glm::dvec2>> Screen::projectSegment(const glm::dvec3& a, const glm::dvec3& b) const
{
    glm::dvec4 ca = viewProj * glm::dvec4(a, 1.0), cb = viewProj * glm::dvec4(b, 1.0);
    const bool aIn = inFront(ca) >= 0.0, bIn = inFront(cb) >= 0.0;
    if (!aIn && !bIn)
        return std::nullopt;
    if (!aIn)
        ca = nearCrossing(ca, cb);
    else if (!bIn)
        cb = nearCrossing(cb, ca);
    if (ca.w <= 1e-12 || cb.w <= 1e-12)
        return std::nullopt;
    return std::pair{toPixels(ca, width, height), toPixels(cb, width, height)};
}

std::vector<glm::dvec2> Screen::projectPolygon(const std::vector<glm::dvec3>& corners) const
{
    std::vector<glm::dvec4> clip;
    clip.reserve(corners.size());
    for (const glm::dvec3& c : corners)
        clip.push_back(viewProj * glm::dvec4(c, 1.0));
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i < clip.size(); ++i)
    {
        const glm::dvec4& a = clip[i];
        const glm::dvec4& b = clip[(i + 1) % clip.size()];
        const bool aIn = inFront(a) >= 0.0, bIn = inFront(b) >= 0.0;
        if (aIn && a.w > 1e-12)
            out.push_back(toPixels(a, width, height));
        if (aIn != bIn)
            if (const glm::dvec4 x = nearCrossing(a, b); x.w > 1e-12)
                out.push_back(toPixels(x, width, height));
    }
    return out;
}

double radius(ComponentMode mode)
{
    return mode == ComponentMode::Vertex ? 12.0 : 8.0;
}

namespace
{
double segDistance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    glm::dvec2 d = b - a;
    double len2 = glm::dot(d, d);
    double t = len2 > 0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(a + d * t - p);
}
} // namespace

std::vector<Candidate> nearby(const geo::Mesh& m, const glm::dmat4& world, ComponentMode mode, const Screen& screen,
                              glm::dvec2 cursor, double maxPx)
{
    LILA_ZONE();

    if (mode != ComponentMode::Vertex && mode != ComponentMode::Edge)
        return {};
    auto at = [&](geo::Index p) { return screen.project(glm::dvec3(world * glm::dvec4(m.position(p), 1.0))); };
    tbb::combinable<std::vector<Candidate>> found;
    size_t count = mode == ComponentMode::Vertex ? m.pointCapacity() : m.edgeCapacity();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count, 8192), [&](const tbb::blocked_range<size_t>& r) {
        std::vector<Candidate>& out = found.local();
        for (size_t i = r.begin(); i != r.end(); ++i)
        {
            geo::Index id = geo::Index(i);
            double d;
            if (mode == ComponentMode::Vertex)
            {
                if (!m.pointValid(id))
                    continue;
                auto s = at(id);
                if (!s)
                    continue;
                d = glm::length(*s - cursor);
            }
            else
            {
                if (!m.edgeValid(id))
                    continue;
                auto worldOf = [&](geo::Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
                auto seg = screen.projectSegment(worldOf(m.edgePoint(id, 0)), worldOf(m.edgePoint(id, 1)));
                if (!seg)
                    continue;
                d = segDistance(cursor, seg->first, seg->second);
            }
            if (d < maxPx)
                out.push_back({d, id});
        }
    });
    std::vector<Candidate> all;
    found.combine_each([&](const std::vector<Candidate>& v) { all.insert(all.end(), v.begin(), v.end()); });
    std::sort(all.begin(), all.end());
    return all;
}

const char* selectByCenterKey(ComponentMode mode)
{
    return mode == ComponentMode::Face ? kFacesByCenter : kEdgesByCenter;
}

bool selectByCenter(ComponentMode mode)
{
    return (mode == ComponentMode::Edge || mode == ComponentMode::Face) && PrefStore::get().value<bool>("general", selectByCenterKey(mode), false);
}

double centerRadius()
{
    return 12.0;
}

std::vector<Candidate> nearbyCenters(const geo::Mesh& m, ComponentMode mode, const std::function<glm::dvec3(uint32_t)>& centerOf, const Screen& screen,
                                     glm::dvec2 cursor, double maxPx)
{
    LILA_ZONE();

    if (mode != ComponentMode::Edge && mode != ComponentMode::Face)
        return {};
    const geo::Index count = mode == ComponentMode::Edge ? m.edgeCapacity() : m.faceCapacity();
    std::vector<Candidate> all;
    for (geo::Index i = 0; i < count; ++i)
    {
        if (mode == ComponentMode::Edge ? !m.edgeValid(i) : !m.faceValid(i))
            continue;
        if (std::optional<glm::dvec2> s = screen.project(centerOf(i)))
            if (const double d = glm::length(*s - cursor); d < maxPx)
                all.push_back({d, i});
    }
    std::sort(all.begin(), all.end());
    return all;
}

glm::dvec3 anchor(const geo::Mesh& m, const glm::dmat4& world, ComponentMode mode, uint32_t id)
{
    auto worldOf = [&](geo::Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
    if (mode == ComponentMode::Edge)
        return (worldOf(m.edgePoint(id, 0)) + worldOf(m.edgePoint(id, 1))) * 0.5;
    return worldOf(id);
}
} // namespace lila::scene::componentPick

namespace lila::scene::componentPick
{
std::vector<uint32_t> fillTriangles(const geo::Mesh& m, uint32_t f)
{
    std::vector<geo::Index> corners;
    geo::triangulateFace(m, f, corners);
    std::vector<uint32_t> out;
    out.reserve(corners.size());
    for (geo::Index c : corners)
        out.push_back(m.cornerPoint(c));
    return out;
}

Topology faceTopology(const geo::Mesh& m, uint32_t f)
{
    const geo::Index n = m.faceSize(f);
    return n == 3 ? Topology::Triangle : n >= 5 ? Topology::Ngon : Topology::Plain;
}

Topology pointTopology(const geo::Mesh& m, uint32_t p)
{
    int edges = 0;
    m.forEachPointEdge(p, [&](geo::Index) { ++edges; });
    return edges >= 5 ? Topology::Pole : Topology::Plain;
}

namespace
{
const char* topologyName(Topology t)
{
    return t == Topology::Triangle ? "triangles" : t == Topology::Ngon ? "ngons" : t == Topology::Pole ? "poles" : "plain";
}
} // namespace

std::string topologyEnabledKey(Topology t)
{
    return std::string("/viewport/topology/") + topologyName(t);
}

std::string topologyColorKey(Topology t)
{
    return topologyEnabledKey(t) + "Color";
}

const char* topologyDefaultColor(Topology t)
{
    return t == Topology::Ngon ? "#e03c3c" : "#f0922b";
}
} // namespace lila::scene::componentPick

namespace lila::scene::selectionMask
{
const char* name(Kind k)
{
    switch (k)
    {
    case Kind::Curve: return "curve";
    case Kind::Mesh: return "mesh";
    case Kind::Joint: return "joint";
    case Kind::Locator: return "locator";
    case Kind::Other: return "other";
    case Kind::Light: return "light";
    }
    return "other";
}

std::string prefKey(Kind k)
{
    return std::string("/select/mask/") + name(k);
}

bool enabled(Kind k)
{
    return PrefStore::get().value<bool>("general", prefKey(k), true);
}

void setEnabled(Kind k, bool on)
{
    PrefStore::get().set("general", prefKey(k), on);
}

std::string showKey(Kind k)
{
    return std::string("/viewport/show/") + name(k);
}

bool shown(Kind k)
{
    return PrefStore::get().value<bool>("general", showKey(k), true);
}

void setShown(Kind k, bool on)
{
    PrefStore::get().set("general", showKey(k), on);
}

bool shown(const Node& node)
{
    return shown(kindOf(node));
}

Kind kindOf(const Node& node)
{
    const NodeType& t = node.type();
    // Cameras and lights derive from locator but are not what the locator mask means.
    return t.isA("mesh") ? Kind::Mesh : t.isA("curve") ? Kind::Curve : t.isA("joint") ? Kind::Joint : t.isA("light") ? Kind::Light : t.isA("camera") ? Kind::Other
         : t.isA("locator") ? Kind::Locator : Kind::Other;
}

bool pickable(const Node& node)
{
    return enabled(kindOf(node)) && shown(kindOf(node));
}
} // namespace lila::scene::selectionMask
