#include "groom/Shell.h"

#include "clay/Clay.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "groom/Attrs.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::groom
{
namespace
{
constexpr const char* kShellColors = "groomShell";

/** Matrix taking a capsule along +Y onto the segment a to b. */
glm::dmat4 segmentMatrix(const D3& a, const D3& b)
{
    const D3 y = glm::normalize(b - a);
    const D3 x = glm::normalize(glm::cross(y, std::abs(y.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0)));
    const D3 z = glm::cross(x, y);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x, 0.0);
    m[1] = glm::dvec4(y, 0.0);
    m[2] = glm::dvec4(z, 0.0);
    m[3] = glm::dvec4((a + b) * 0.5, 1.0);
    return m;
}

float lockHash(double id)
{
    uint32_t h = uint32_t(int64_t(id)) * 2654435761u;
    h ^= h >> 15;
    return float(h % 1000u) / 999.0f;
}
} // namespace

clay::Field shellField(const Splines& resolved, const ShellOptions& o)
{
    LILA_ZONE();

    clay::Item root;
    for (size_t li = 0; li < resolved.size(); ++li)
    {
        const Spline& full = resolved[li];
        if (full.size() < 2 || (o.skipStrays && attr::get(full, attr::kStray, 0.0) > 0.5))
            continue;
        const Spline s = o.segments > 0 && int(full.size()) > o.segments + 1 ? attr::resampled(full, o.segments + 1) : full;
        clay::Item lock;
        lock.op = clay::Op::Add;
        lock.blend = std::max(0.0, o.blend);
        const float hash = lockHash(attr::get(s, attr::kId, double(li + 1)));
        // Nothing thinner than the lattice can resolve, or surface nets leaves sheets joined at an edge.
        const double least = std::max(0.02, o.voxelSize) * 1.5;
        // A flat lock is a row of round cones across its width, thickness their radius. Unscaled, so every
        // distance is exact and Clay can cut each brick's program to what reaches it.
        for (size_t i = 0; i + 1 < s.size(); ++i)
        {
            const D3 a = s.points[i], b = s.points[i + 1];
            if (glm::distance(a, b) < 1e-6)
                continue;
            const double hw0 = std::max(least, attr::point(s, attr::kHalfWidth, i, 0.5) + o.inflate);
            const double hw1 = std::max(least, attr::point(s, attr::kHalfWidth, i + 1, 0.5) + o.inflate);
            const double ht0 = std::clamp(attr::point(s, attr::kHalfThick, i, 0.2) + o.inflate, least, hw0);
            const double ht1 = std::clamp(attr::point(s, attr::kHalfThick, i + 1, 0.2) + o.inflate, least, hw1);
            const D3 side0(attr::point(s, attr::kSideX, i, 1.0), attr::point(s, attr::kSideY, i, 0.0), attr::point(s, attr::kSideZ, i, 0.0));
            const D3 side1(attr::point(s, attr::kSideX, i + 1, 1.0), attr::point(s, attr::kSideY, i + 1, 0.0), attr::point(s, attr::kSideZ, i + 1, 0.0));
            const int across = std::clamp(int(std::ceil(std::max(hw0 / ht0, hw1 / ht1) * 0.6)), 1, 7);
            const double v = 0.5 * (attr::point(s, attr::kAlong, i, 0.0) + attr::point(s, attr::kAlong, i + 1, 0.0));
            for (int k = 0; k < across; ++k)
            {
                const double f = across > 1 ? -1.0 + 2.0 * k / (across - 1) : 0.0;
                const D3 pa = a + side0 * (f * (hw0 - ht0)), pb = b + side1 * (f * (hw1 - ht1));
                const double len = glm::distance(pa, pb);
                if (len < 1e-6)
                    continue;
                clay::Item seg;
                seg.op = clay::Op::Add;
                seg.blend = 0.5 * std::min(ht0, ht1);
                seg.matrix = segmentMatrix(pa, pb);
                clay::Primitive p;
                p.shape = clay::Shape::Capsule;
                p.length = len;
                p.radius = ht0;
                p.endRadius = ht1;
                p.color = glm::vec3(float(v), hash, 0.0f);
                seg.shape = p;
                lock.children.push_back(std::move(seg));
            }
        }
        if (!lock.children.empty())
            root.children.push_back(std::move(lock));
    }
    return clay::Field(root);
}

namespace
{
uint64_t edgeKey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

/**
 * Surface nets joins two sheets that pass within a voxel of each other along an edge with four faces. Gives one
 * sheet its own copy of an end point of such an edge, so every edge keeps two faces.
 */
geo::Mesh resolvePinches(const geo::Mesh& m)
{
    std::vector<D3> pts(m.pointCapacity());
    std::vector<std::vector<uint32_t>> faces;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            pts[p] = m.position(p);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            std::vector<uint32_t> ring;
            m.forEachFaceCorner(f, [&](geo::Index c) { ring.push_back(m.cornerPoint(c)); });
            faces.push_back(std::move(ring));
        }
    std::unordered_map<uint64_t, std::vector<uint32_t>> edgeFaces;
    for (uint32_t f = 0; f < faces.size(); ++f)
        for (size_t k = 0; k < faces[f].size(); ++k)
            edgeFaces[edgeKey(faces[f][k], faces[f][(k + 1) % faces[f].size()])].push_back(f);
    std::vector<uint64_t> pinched;
    for (const auto& [key, fs] : edgeFaces)
        if (fs.size() == 4)
            pinched.push_back(key);
    if (pinched.empty())
        return m;
    std::sort(pinched.begin(), pinched.end());
    const std::vector<glm::vec4> colors = geo::colors::has(m, kShellColors) ? geo::colors::pointColors(m, kShellColors) : std::vector<glm::vec4>{};
    std::vector<glm::vec4> outColors = colors;
    // The faces round point v reached from face start across edges with two faces, stopping at the pinched edge.
    auto fan = [&](uint32_t v, uint32_t start, uint64_t stop) {
        std::vector<uint32_t> out{start};
        std::vector<uint32_t> frontier{start};
        while (!frontier.empty())
        {
            const uint32_t f = frontier.back();
            frontier.pop_back();
            const auto& ring = faces[f];
            for (size_t k = 0; k < ring.size(); ++k)
            {
                if (ring[k] != v)
                    continue;
                for (uint32_t other : {ring[(k + 1) % ring.size()], ring[(k + ring.size() - 1) % ring.size()]})
                {
                    const uint64_t e = edgeKey(v, other);
                    if (e == stop)
                        continue;
                    auto it = edgeFaces.find(e);
                    if (it == edgeFaces.end() || it->second.size() != 2)
                        continue;
                    for (uint32_t g : it->second)
                        if (std::ranges::find(out, g) == out.end())
                        {
                            out.push_back(g);
                            frontier.push_back(g);
                        }
                }
            }
        }
        return out;
    };
    auto replace = [&](uint32_t f, uint32_t from, uint32_t to) {
        for (uint32_t& p : faces[f])
            if (p == from)
                p = to;
    };
    for (uint64_t key : pinched)
    {
        const std::vector<uint32_t> four = edgeFaces[key];
        if (four.size() != 4)
            continue;
        for (uint32_t v : {uint32_t(key >> 32), uint32_t(key & 0xffffffffu)})
        {
            const std::vector<uint32_t> first = fan(v, four[0], key);
            const size_t sheet = size_t(std::ranges::count_if(four, [&](uint32_t f) { return std::ranges::find(first, f) != first.end(); }));
            // Only when the fan of one sheet holds exactly two of the four faces: the sheets part at v.
            if (sheet != 2)
                continue;
            std::vector<uint32_t> second;
            for (uint32_t f : four)
                if (std::ranges::find(first, f) == first.end())
                {
                    second = fan(v, f, key);
                    break;
                }
            const uint32_t copy = uint32_t(pts.size());
            pts.push_back(pts[v]);
            if (!colors.empty())
                outColors.push_back(outColors[v]);
            for (uint32_t f : second)
                replace(f, v, copy);
            // One end is enough: both sheets keep the other end, so every edge still has two faces.
            break;
        }
        // The edges changed: rebuild the lookup for the pinches still to come.
        edgeFaces.clear();
        for (uint32_t f = 0; f < faces.size(); ++f)
            for (size_t k = 0; k < faces[f].size(); ++k)
                edgeFaces[edgeKey(faces[f][k], faces[f][(k + 1) % faces[f].size()])].push_back(f);
    }
    geo::Mesh out;
    std::vector<geo::Index> ids(pts.size(), geo::kInvalid);
    for (const auto& ring : faces)
    {
        std::vector<geo::Index> r;
        for (uint32_t p : ring)
        {
            if (ids[p] == geo::kInvalid)
                ids[p] = out.addPoint(pts[p]);
            r.push_back(ids[p]);
        }
        out.addFace(r);
    }
    if (!outColors.empty())
    {
        std::vector<glm::vec4> perPoint(out.pointCapacity(), glm::vec4(1.0f));
        for (size_t p = 0; p < pts.size(); ++p)
            if (ids[p] != geo::kInvalid)
                perPoint[ids[p]] = outColors[p];
        geo::colors::add(out, kShellColors);
        geo::colors::setFromPoints(out, kShellColors, perPoint);
    }
    return out;
}

clay::MeshOptions mesherOptions(const ShellOptions& o)
{
    clay::MeshOptions mo;
    mo.voxelSize = std::max(0.02, o.voxelSize);
    mo.projectSteps = std::clamp(o.projectSteps, 0, 4);
    mo.gpu = o.gpu;
    mo.colors = true;
    return mo;
}

void renameColors(geo::Mesh& m)
{
    if (geo::colors::has(m, "clay"))
        geo::colors::rename(m, "clay", kShellColors);
}

/** The mesher's output as the shell: colours renamed, pinched edges parted. */
geo::Mesh finish(geo::Mesh m)
{
    renameColors(m);
    return resolvePinches(m);
}
} // namespace

geo::Mesh shellMesh(const Splines& resolved, const ShellOptions& o)
{
    const clay::Field f = shellField(resolved, o);
    if (f.empty())
        return {};
    return finish(clay::mesh(f, mesherOptions(o)));
}

ShellBuilder::ShellBuilder() : mMesher(std::make_unique<clay::Mesher>()) {}
ShellBuilder::~ShellBuilder() = default;

std::shared_ptr<const geo::Mesh> ShellBuilder::update(const Splines& resolved, const ShellOptions& o)
{
    const clay::Field f = shellField(resolved, o);
    if (f.empty())
        return std::make_shared<geo::Mesh>();
    std::shared_ptr<const geo::Mesh> m = mMesher->update(f, mesherOptions(o));
    if (!m)
        return m;
    if (m.get() != mLastIn.get())
    {
        mLastIn = m;
        mLastOut = std::make_shared<geo::Mesh>(finish(*m));
    }
    return mLastOut;
}
} // namespace lila::groom
