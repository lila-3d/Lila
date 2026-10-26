#include "BladeExtrude.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace lila;
using namespace lila::geo;

namespace blades
{
namespace
{
const char* const kFieldNames[kFieldCount] = {"thickness", "offset", "localTranslateZ"};
const char* const kFieldLabels[kFieldCount] = {"Thickness", "Offset", "Local Translate Z"};
const glm::dvec2 kFieldRanges[kFieldCount] = {{0.5, 1.5}, {0.0, 0.4}, {0.0, 1.0}};

/** Adds an int layer whose missing entries read -1, existing points included. */
void ensureIdLayer(AttributeSet& set, const char* name)
{
    if (set.find(name))
        return;
    Attribute& a = set.add(name, AttrType::Int);
    a.setDefault(int32_t(-1));
    for (int32_t& v : a.values<int32_t>())
        v = -1;
}

void ensureLayers(Mesh& m)
{
    ensureIdLayer(m.pointAttrs(), kBladeLayer);
    for (const char* name : {kRootLayer, kNormalLayer, kInsetLayer, kValuesLayer, kBaseLayer})
        m.pointAttrs().add(name, AttrType::Float3);
    m.pointAttrs().add(kLevelLayer, AttrType::Float);
    ensureIdLayer(m.faceAttrs(), kCapLayer);
    ensureIdLayer(m.faceAttrs(), kWallLayer);
}

glm::vec3 toVec(const BladeValues& v) { return glm::vec3(float(v[0]), float(v[1]), float(v[2])); }
BladeValues fromVec(const glm::vec3& v) { return BladeValues{{double(v.x), double(v.y), double(v.z)}}; }

glm::dvec3 place(const glm::dvec3& root, const glm::dvec3& normal, const glm::dvec3& inset, double level, const BladeValues& v)
{
    return root + normal * ((v[kThickness] + v[kTranslateZ]) * level) + inset * (v[kOffset] * level);
}

int nextBladeId(const Mesh& m)
{
    int next = 0;
    for (int32_t b : m.pointAttrs().get<int32_t>(kBladeLayer))
        next = std::max(next, b + 1);
    return next;
}

/** Per boundary point, the in-plane direction that moves the border edges beside it inward by one unit. */
glm::dvec3 mitre(const std::vector<glm::dvec3>& inward, const glm::dvec3& towardCentre)
{
    if (inward.size() == 2)
    {
        glm::dvec3 m = inward[0] + inward[1];
        const double len = glm::length(m);
        if (len > 1e-9)
        {
            m /= len;
            const double c = glm::dot(m, inward[0]);
            return m / std::max(c, 0.25);
        }
    }
    if (inward.size() == 1)
        return inward[0];
    const double len = glm::length(towardCentre);
    return len > 1e-9 ? towardCentre / len : glm::dvec3(0.0);
}

/** One blade: the faces repointed onto new or moved points, walls round its border. */
void extrudeOne(Mesh& m, const std::vector<Index>& faces, int id, const ExtrudeOptions& o)
{
    const std::set<Index> region(faces.begin(), faces.end());
    glm::dvec3 normal(0.0);
    for (Index f : faces)
        normal += m.faceAreaVector(f);
    if (glm::length(normal) < 1e-12)
        normal = glm::dvec3(0.0, 1.0, 0.0);
    normal = glm::normalize(normal);

    struct Edge
    {
        Index face, cornerA, a, b;
    };
    std::vector<Edge> border;
    std::set<Index> regionPoints;
    for (Index f : faces)
        m.forEachFaceCorner(f, [&](Index c) {
            regionPoints.insert(m.cornerPoint(c));
            int count = 0;
            m.forEachEdgeCorner(m.cornerEdge(c), [&](Index other) { count += region.count(m.cornerFace(other)) ? 1 : 0; });
            if (count == 1)
                border.push_back({f, c, m.cornerPoint(c), m.cornerNextPoint(c)});
        });

    glm::dvec3 centre(0.0);
    for (Index p : regionPoints)
        centre += m.position(p);
    centre /= double(std::max<size_t>(regionPoints.size(), 1));

    std::map<Index, std::vector<glm::dvec3>> inward;
    for (const Edge& e : border)
    {
        glm::dvec3 t = m.position(e.b) - m.position(e.a);
        glm::dvec3 in = glm::cross(normal, t);
        const double len = glm::length(in);
        if (len > 1e-12)
        {
            inward[e.a].push_back(in / len);
            inward[e.b].push_back(in / len);
        }
        else
        {
            inward[e.a];
            inward[e.b];
        }
    }

    const int divisions = std::max(1, o.divisions);
    struct Made
    {
        Index point;
        glm::dvec3 root, inset;
        double level;
    };
    std::vector<Made> made;
    std::map<Index, std::vector<Index>> layers; // border point -> its copies at levels 0..divisions
    std::map<Index, Index> top;
    for (Index p : regionPoints)
    {
        const glm::dvec3 root = m.position(p);
        auto it = inward.find(p);
        if (it == inward.end())
        {
            made.push_back({p, root, glm::dvec3(0.0), 1.0});
            top[p] = p;
            continue;
        }
        glm::dvec3 towardCentre = centre - root;
        towardCentre -= normal * glm::dot(towardCentre, normal);
        const glm::dvec3 inset = mitre(it->second, towardCentre);
        std::vector<Index>& chain = layers[p];
        chain.push_back(p);
        for (int k = 1; k <= divisions; ++k)
        {
            const double level = double(k) / double(divisions);
            const Index q = m.addPoint(place(root, normal, inset, level, o.values));
            chain.push_back(q);
            made.push_back({q, root, inset, level});
        }
        top[p] = chain.back();
    }

    for (Index f : faces)
    {
        std::vector<Index> pts;
        m.forEachFaceCorner(f, [&](Index c) { pts.push_back(top[m.cornerPoint(c)]); });
        m.setFacePoints(f, pts);
    }

    std::vector<Index> walls;
    for (const Edge& e : border)
    {
        const std::vector<Index>& a = layers[e.a];
        const std::vector<Index>& b = layers[e.b];
        const Index cb = m.cornerNext(e.cornerA);
        const Index src[4] = {e.cornerA, cb, cb, e.cornerA};
        for (int k = 0; k < divisions; ++k)
        {
            const Index quad[4] = {a[k], b[k], b[k + 1], a[k + 1]};
            const Index w = m.addFace(std::span<const Index>(quad, 4), e.face, std::span<const Index>(src, 4));
            if (w != kInvalid)
                walls.push_back(w);
        }
    }

    auto blade = m.pointAttrs().get<int32_t>(kBladeLayer);
    auto level = m.pointAttrs().get<float>(kLevelLayer);
    auto roots = m.pointAttrs().get<glm::vec3>(kRootLayer);
    auto normals = m.pointAttrs().get<glm::vec3>(kNormalLayer);
    auto insets = m.pointAttrs().get<glm::vec3>(kInsetLayer);
    auto values = m.pointAttrs().get<glm::vec3>(kValuesLayer);
    auto base = m.pointAttrs().get<glm::vec3>(kBaseLayer);
    for (const Made& d : made)
    {
        blade[d.point] = id;
        level[d.point] = float(d.level);
        roots[d.point] = glm::vec3(d.root);
        normals[d.point] = glm::vec3(normal);
        insets[d.point] = glm::vec3(d.inset);
        values[d.point] = toVec(o.values);
        base[d.point] = toVec(o.values);
        m.setPosition(d.point, place(d.root, normal, d.inset, d.level, o.values));
    }
    auto caps = m.faceAttrs().get<int32_t>(kCapLayer);
    auto wallIds = m.faceAttrs().get<int32_t>(kWallLayer);
    for (Index f : faces)
        caps[f] = id;
    for (Index w : walls)
        wallIds[w] = id;
}
} // namespace

const char* fieldName(int field) { return kFieldNames[field]; }
const char* fieldLabel(int field) { return kFieldLabels[field]; }
glm::dvec2 fieldDefaultRange(int field) { return kFieldRanges[field]; }

int fieldIndex(const std::string& name)
{
    for (int i = 0; i < kFieldCount; ++i)
        if (name == kFieldNames[i])
            return i;
    return -1;
}

std::vector<int> extrude(Mesh& mesh, std::span<const Index> faces, const ExtrudeOptions& options)
{
    std::vector<Index> valid;
    for (Index f : faces)
        if (mesh.faceValid(f))
            valid.push_back(f);
    std::sort(valid.begin(), valid.end());
    valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
    if (valid.empty())
        return {};

    ensureLayers(mesh);
    std::vector<std::vector<Index>> groups;
    if (options.keepFacesTogether)
        groups = faceRegions(mesh, valid);
    else
        for (Index f : valid)
            groups.push_back({f});

    std::vector<int> ids;
    int id = nextBladeId(mesh);
    for (const std::vector<Index>& g : groups)
    {
        extrudeOne(mesh, g, id, options);
        ids.push_back(id++);
    }
    return ids;
}

bool hasBlades(const Mesh& mesh)
{
    for (int32_t b : mesh.pointAttrs().get<int32_t>(kBladeLayer))
        if (b >= 0)
            return true;
    return false;
}

std::vector<Blade> blades(const Mesh& mesh)
{
    std::map<int, Blade> byId;
    auto ids = mesh.pointAttrs().get<int32_t>(kBladeLayer);
    auto values = mesh.pointAttrs().get<glm::vec3>(kValuesLayer);
    auto base = mesh.pointAttrs().get<glm::vec3>(kBaseLayer);
    if (ids.empty() || values.empty() || base.empty())
        return {};
    for (Index p = 0; p < mesh.pointCapacity() && p < ids.size(); ++p)
        if (mesh.pointValid(p) && ids[p] >= 0)
        {
            Blade& b = byId[ids[p]];
            if (b.points.empty())
            {
                b.id = ids[p];
                b.values = fromVec(values[p]);
                b.base = fromVec(base[p]);
            }
            b.points.push_back(p);
        }
    auto caps = mesh.faceAttrs().get<int32_t>(kCapLayer);
    auto walls = mesh.faceAttrs().get<int32_t>(kWallLayer);
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        if (f < caps.size() && caps[f] >= 0 && byId.count(caps[f]))
            byId[caps[f]].caps.push_back(f);
        if (f < walls.size() && walls[f] >= 0 && byId.count(walls[f]))
            byId[walls[f]].walls.push_back(f);
    }
    std::vector<Blade> out;
    for (auto& [id, b] : byId)
    {
        b.root = groupRoot(mesh, b.points);
        out.push_back(std::move(b));
    }
    return out;
}

void setValues(Mesh& mesh, int blade, const BladeValues& v)
{
    auto ids = mesh.pointAttrs().get<int32_t>(kBladeLayer);
    auto level = mesh.pointAttrs().get<float>(kLevelLayer);
    auto roots = mesh.pointAttrs().get<glm::vec3>(kRootLayer);
    auto normals = mesh.pointAttrs().get<glm::vec3>(kNormalLayer);
    auto insets = mesh.pointAttrs().get<glm::vec3>(kInsetLayer);
    auto values = mesh.pointAttrs().get<glm::vec3>(kValuesLayer);
    if (ids.empty() || level.empty() || roots.empty() || normals.empty() || insets.empty() || values.empty())
        return;
    for (Index p = 0; p < mesh.pointCapacity() && p < ids.size(); ++p)
        if (mesh.pointValid(p) && ids[p] == blade)
        {
            values[p] = toVec(v);
            mesh.setPosition(p, place(glm::dvec3(roots[p]), glm::dvec3(normals[p]), glm::dvec3(insets[p]), level[p], v));
        }
}

std::vector<Index> capFaces(const Mesh& mesh)
{
    std::vector<Index> out;
    auto caps = mesh.faceAttrs().get<int32_t>(kCapLayer);
    for (Index f = 0; f < mesh.faceCapacity() && f < caps.size(); ++f)
        if (mesh.faceValid(f) && caps[f] >= 0)
            out.push_back(f);
    return out;
}

std::vector<Index> stemFaces(const Mesh& mesh)
{
    std::vector<Index> out;
    auto ids = mesh.pointAttrs().get<int32_t>(kBladeLayer);
    if (ids.empty())
        return out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        bool moved = false;
        mesh.forEachFaceCorner(f, [&](Index c) { moved = moved || ids[mesh.cornerPoint(c)] >= 0; });
        if (moved)
            out.push_back(f);
    }
    return out;
}

std::vector<std::vector<Index>> faceRegions(const Mesh& mesh, std::span<const Index> faces)
{
    std::set<Index> remaining(faces.begin(), faces.end());
    std::vector<std::vector<Index>> out;
    while (!remaining.empty())
    {
        const Index seed = *remaining.begin();
        remaining.erase(remaining.begin());
        std::vector<Index> group{seed}, queue{seed};
        while (!queue.empty())
        {
            const Index f = queue.back();
            queue.pop_back();
            mesh.forEachFaceCorner(f, [&](Index c) {
                mesh.forEachEdgeCorner(mesh.cornerEdge(c), [&](Index other) {
                    const Index n = mesh.cornerFace(other);
                    if (remaining.erase(n))
                    {
                        group.push_back(n);
                        queue.push_back(n);
                    }
                });
            });
        }
        std::sort(group.begin(), group.end());
        out.push_back(std::move(group));
    }
    return out;
}

std::vector<Index> facePoints(const Mesh& mesh, std::span<const Index> faces)
{
    std::set<Index> pts;
    for (Index f : faces)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](Index c) { pts.insert(mesh.cornerPoint(c)); });
    return {pts.begin(), pts.end()};
}

std::vector<std::vector<Index>> pointGroups(const Mesh& mesh, std::span<const Index> points)
{
    std::set<Index> remaining(points.begin(), points.end());
    std::vector<std::vector<Index>> out;
    while (!remaining.empty())
    {
        const Index seed = *remaining.begin();
        remaining.erase(remaining.begin());
        std::vector<Index> group{seed}, queue{seed};
        while (!queue.empty())
        {
            const Index p = queue.back();
            queue.pop_back();
            mesh.forEachPointEdge(p, [&](Index e) {
                const Index n = mesh.edgeOtherPoint(e, p);
                if (remaining.erase(n))
                {
                    group.push_back(n);
                    queue.push_back(n);
                }
            });
        }
        std::sort(group.begin(), group.end());
        out.push_back(std::move(group));
    }
    return out;
}

glm::dvec3 groupRoot(const Mesh& mesh, std::span<const Index> points)
{
    const std::set<Index> inside(points.begin(), points.end());
    std::set<Index> ring;
    for (Index p : points)
        mesh.forEachPointEdge(p, [&](Index e) {
            const Index n = mesh.edgeOtherPoint(e, p);
            if (!inside.count(n))
                ring.insert(n);
        });
    const std::set<Index>& used = ring.empty() ? inside : ring;
    glm::dvec3 sum(0.0);
    for (Index p : used)
        sum += mesh.position(p);
    return used.empty() ? sum : sum / double(used.size());
}

Rng::Rng(uint64_t seed) : mEngine(seed ? seed : std::random_device{}()) {}

double Rng::uniform() { return double(mEngine() >> 11) * (1.0 / 9007199254740992.0); }

double biased(Rng& rng, double low, double high, double fraction, double bias)
{
    double weight = rng.uniform();
    if (bias != 0.0)
    {
        const double target = bias > 0.0 ? fraction : 1.0 - fraction;
        weight = weight * (1.0 - std::abs(bias)) + target * std::abs(bias);
    }
    return low + (high - low) * weight;
}

std::vector<double> fractions(std::span<const double> values)
{
    if (values.empty())
        return {};
    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    const double span = *hi - *lo;
    std::vector<double> out;
    for (double v : values)
        out.push_back(span < 1e-9 ? 0.5 : (v - *lo) / span);
    return out;
}
} // namespace blades
