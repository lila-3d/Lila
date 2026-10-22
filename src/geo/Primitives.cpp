#include "geo/Primitives.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <functional>
#include <utility>

namespace lila::geo
{
namespace
{
struct GridFace
{
    glm::ivec3 origin;
    glm::ivec3 u;
    glm::ivec3 v;
    int nu;
    int nv;
    glm::vec2 uvMin;
    glm::vec2 uvMax;
};

/** Six welded grids on the surface of an sx*sy*sz lattice; pos maps lattice coords to space. */
Mesh buildBox(int sx, int sy, int sz, const std::function<glm::dvec3(const glm::ivec3&)>& pos, bool hardSides)
{
    LILA_ZONE();

    Mesh m;
    const glm::ivec3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
    // Maya's cross layout: a column of front, top, back, bottom with right and left beside the front.
    const GridFace faces[6] = {
        {{0, 0, sz}, X, Y, sx, sy, {0.375f, 0.0f}, {0.625f, 0.25f}},     // +Z
        {{0, sy, sz}, X, -Z, sx, sz, {0.375f, 0.25f}, {0.625f, 0.5f}},   // +Y
        {{0, sy, 0}, X, -Y, sx, sy, {0.375f, 0.5f}, {0.625f, 0.75f}},  // -Z
        {{0, 0, 0}, X, Z, sx, sz, {0.375f, 0.75f}, {0.625f, 1.0f}},      // -Y
        {{sx, 0, sz}, -Z, Y, sz, sy, {0.625f, 0.0f}, {0.875f, 0.25f}},   // +X
        {{0, 0, 0}, Z, Y, sz, sy, {0.125f, 0.0f}, {0.375f, 0.25f}},      // -X
    };

    const size_t lattice = size_t(sx + 1) * (sy + 1) * (sz + 1);
    std::vector<Index> ids(lattice, kInvalid);
    auto point = [&](const glm::ivec3& c) {
        Index& id = ids[(size_t(c.z) * (sy + 1) + c.y) * (sx + 1) + c.x];
        if (id == kInvalid)
            id = m.addPoint(pos(c));
        return id;
    };

    const size_t faceCount = 2 * (size_t(sx) * sy + size_t(sy) * sz + size_t(sx) * sz);
    m.reserve(lattice, faceCount * 2, faceCount, faceCount * 4);
    std::vector<int> side;
    side.reserve(faceCount);
    std::vector<glm::vec2> cornerUv;
    cornerUv.reserve(faceCount * 4);
    for (int s = 0; s < 6; ++s)
    {
        const GridFace& g = faces[s];
        for (int b = 0; b < g.nv; ++b)
            for (int a = 0; a < g.nu; ++a)
            {
                const Index q[4] = {point(g.origin + g.u * a + g.v * b), point(g.origin + g.u * (a + 1) + g.v * b),
                                    point(g.origin + g.u * (a + 1) + g.v * (b + 1)),
                                    point(g.origin + g.u * a + g.v * (b + 1))};
                if (m.addFace(q) == kInvalid)
                    continue;
                side.push_back(s);
                const glm::vec2 d = g.uvMax - g.uvMin;
                const float u0 = float(a) / g.nu, u1 = float(a + 1) / g.nu;
                const float v0 = float(b) / g.nv, v1 = float(b + 1) / g.nv;
                cornerUv.push_back(g.uvMin + d * glm::vec2(u0, v0));
                cornerUv.push_back(g.uvMin + d * glm::vec2(u1, v0));
                cornerUv.push_back(g.uvMin + d * glm::vec2(u1, v1));
                cornerUv.push_back(g.uvMin + d * glm::vec2(u0, v1));
            }
    }
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
    std::copy(cornerUv.begin(), cornerUv.end(), uv.begin());

    if (hardSides)
    {
        std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(attr::kHard);
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            const Index c = m.edgeCorner(e);
            const Index r = m.cornerRadial(c);
            hard[e] = side[m.cornerFace(c)] != side[m.cornerFace(r)];
        }
    }
    markUvSeams(m);
    return m;
}

struct ProfilePoint
{
    double r;
    double y;
    bool hard = false;
};

struct Band
{
    enum Mode
    {
        Cylindrical,
        Planar
    };
    int from;
    int to;
    Mode mode;
    glm::vec2 v{0.0f, 1.0f};     // cylindrical v range
    glm::vec2 center{0.5f, 0.5f}; // planar
    float scale = 0.0f;           // planar uv per cm
    bool ngon = false;            // a single cap polygon instead of the fan
};

/**
 * Revolves a profile in (r, y) around +Y. Profile points with r == 0 become a single pole point.
 * Walking the profile with the outside on the right gives outward faces. A closed profile wraps its
 * last band back to point 0.
 */
Mesh lathe(const std::vector<ProfilePoint>& profile, const std::vector<Band>& bands, int axis, bool closed)
{
    LILA_ZONE();

    Mesh m;
    const int np = int(profile.size());
    const double twoPi = glm::two_pi<double>();
    std::vector<Index> ringStart(np);
    std::vector<bool> pole(np);
    for (int k = 0; k < np; ++k)
    {
        pole[k] = std::abs(profile[k].r) < 1e-12;
        ringStart[k] = m.pointCapacity();
        if (pole[k])
        {
            m.addPoint({0.0, profile[k].y, 0.0});
            continue;
        }
        for (int i = 0; i < axis; ++i)
        {
            const double t = twoPi * i / axis;
            m.addPoint({profile[k].r * std::cos(t), profile[k].y, -profile[k].r * std::sin(t)});
        }
    }
    auto pid = [&](int k, int i) { return pole[k] ? ringStart[k] : ringStart[k] + Index(i % axis); };

    std::vector<glm::vec2> cornerUv;
    auto planarUv = [&](const Band& b, Index p, bool facingUp) {
        const glm::dvec3& x = m.position(p);
        return b.center + glm::vec2(float(x.x), float(facingUp ? -x.z : x.z)) * b.scale;
    };

    std::vector<Index> pts;
    std::vector<glm::vec2> uvs;
    auto emit = [&] {
        if (m.addFace(pts) != kInvalid)
            cornerUv.insert(cornerUv.end(), uvs.begin(), uvs.end());
        pts.clear();
        uvs.clear();
    };

    for (const Band& b : bands)
    {
        std::vector<double> len{0.0};
        for (int k = b.from; k < b.to; ++k)
        {
            const ProfilePoint& p0 = profile[k % np];
            const ProfilePoint& p1 = profile[(k + 1) % np];
            len.push_back(len.back() + std::hypot(p1.r - p0.r, p1.y - p0.y));
        }
        auto vAt = [&](int k) { return glm::mix(b.v.x, b.v.y, float(len[k - b.from] / std::max(len.back(), 1e-12))); };

        for (int k = b.from; k < b.to; ++k)
        {
            const int k0 = k % np, k1 = (k + 1) % np;
            if (pole[k0] && pole[k1])
                continue;
            const bool up = profile[k1].r < profile[k0].r;
            if (b.ngon && (pole[k0] || pole[k1]))
            {
                const int rim = pole[k0] ? k1 : k0;
                for (int i = 0; i < axis; ++i)
                {
                    const int ii = pole[k0] ? axis - 1 - i : i;
                    pts.push_back(pid(rim, ii));
                    uvs.push_back(planarUv(b, pts.back(), up));
                }
                emit();
                continue;
            }
            for (int i = 0; i < axis; ++i)
            {
                const float u0 = float(i) / axis, u1 = float(i + 1) / axis, um = (u0 + u1) * 0.5f;
                const float v0 = vAt(k), v1 = vAt(k + 1);
                auto add = [&](int kk, int ii, float u, float v) {
                    pts.push_back(pid(kk % np, ii));
                    if (b.mode == Band::Cylindrical)
                        uvs.push_back({u, v});
                    else
                        uvs.push_back(planarUv(b, pts.back(), up));
                };
                if (pole[k0])
                {
                    add(k, i, um, v0);
                    add(k + 1, i + 1, u1, v1);
                    add(k + 1, i, u0, v1);
                }
                else if (pole[k1])
                {
                    add(k, i, u0, v0);
                    add(k, i + 1, u1, v0);
                    add(k + 1, i, um, v1);
                }
                else
                {
                    add(k, i, u0, v0);
                    add(k, i + 1, u1, v0);
                    add(k + 1, i + 1, u1, v1);
                    add(k + 1, i, u0, v1);
                }
                emit();
            }
        }
    }

    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
    std::copy(cornerUv.begin(), cornerUv.end(), uv.begin());

    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(attr::kHard);
    for (int k = 0; k < np; ++k)
        if (profile[k].hard && !pole[k])
            for (int i = 0; i < axis; ++i)
            {
                const Index e = m.findEdge(pid(k, i), pid(k, i + 1));
                if (e != kInvalid)
                    hard[e] = true;
            }
    (void)closed;
    // N-gon caps leave their centre point unused.
    m.removeAllLoose();
    if (m.hasDeleted())
        m.compact();
    markUvSeams(m);
    return m;
}

void shiftY(Mesh& m, double dy)
{
    for (glm::dvec3& p : m.positions())
        p.y += dy;
}

// Planar cap islands in the lower half of uv space, radius just under a quarter.
constexpr float kCapScale = 0.24f;
const glm::vec2 kCapLeft{0.25f, 0.25f};
const glm::vec2 kCapRight{0.75f, 0.25f};
} // namespace

Index markUvSeams(Mesh& mesh, std::string_view uvSet)
{
    LILA_ZONE();

    std::span<const glm::vec2> uv = std::as_const(mesh).cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty())
        return 0;
    std::vector<uint8_t> seam(mesh.edgeCapacity(), 0);
    Index count = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e) || mesh.edgeCorner(e) == kInvalid)
            continue;
        const Index c = mesh.edgeCorner(e);
        const Index p = mesh.cornerPoint(c);
        bool differs = false;
        mesh.forEachEdgeCorner(e, [&](Index r) {
            if (r == c)
                return;
            // Corner r runs along e either way; find its uvs at c's two points.
            const bool same = mesh.cornerPoint(r) == p;
            const glm::vec2 a = same ? uv[r] : uv[mesh.cornerNext(r)];
            const glm::vec2 b = same ? uv[mesh.cornerNext(r)] : uv[r];
            if (glm::any(glm::greaterThan(glm::abs(a - uv[c]), glm::vec2(1e-6f)))
                || glm::any(glm::greaterThan(glm::abs(b - uv[mesh.cornerNext(c)]), glm::vec2(1e-6f))))
                differs = true;
        });
        if (differs)
        {
            seam[e] = 1;
            ++count;
        }
    }
    std::span<bool> out = mesh.edgeAttrs().getOrAdd<bool>(attr::kSeam);
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        out[e] = seam[e] != 0;
    return count;
}

Mesh makeCube(const CubeOptions& o)
{
    LILA_ZONE();

    const int sx = std::max(1, o.subdivX), sy = std::max(1, o.subdivY), sz = std::max(1, o.subdivZ);
    const glm::dvec3 size(o.width, o.height, o.depth);
    const glm::dvec3 offset(-0.5 * o.width, o.baseAtOrigin ? 0.0 : -0.5 * o.height, -0.5 * o.depth);
    return buildBox(
        sx, sy, sz,
        [&](const glm::ivec3& c) { return offset + size * glm::dvec3(c) / glm::dvec3(sx, sy, sz); }, true);
}

Mesh makeQuadSphere(const QuadSphereOptions& o)
{
    LILA_ZONE();

    const int n = std::max(1, o.subdiv);
    return buildBox(
        n, n, n,
        [&](const glm::ivec3& c) {
            // Tangent warp spreads the grid evenly over the sphere.
            glm::dvec3 a = glm::dvec3(c) / double(n) * 2.0 - 1.0;
            a = glm::tan(a * glm::quarter_pi<double>());
            return glm::normalize(a) * o.radius;
        },
        false);
}

Mesh makeIcoSphere(const IcoSphereOptions& o)
{
    LILA_ZONE();

    const double t = (1.0 + std::sqrt(5.0)) * 0.5;
    const glm::dvec3 corners[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                    {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                              {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                              {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    // Each level splits every triangle in four, as in Blender.
    const int n = 1 << std::clamp(o.subdivisions - 1, 0, 7);

    Mesh m;
    // Lattice points on shared icosahedron edges coincide exactly, so welding by rounded position is safe.
    std::map<std::array<int64_t, 3>, Index> welded;
    auto point = [&](const glm::dvec3& p) {
        glm::dvec3 s = glm::normalize(p);
        std::array<int64_t, 3> key = {std::llround(s.x * 1e9), std::llround(s.y * 1e9), std::llround(s.z * 1e9)};
        auto [it, added] = welded.try_emplace(key, 0);
        if (added)
            it->second = m.addPoint(s * o.radius);
        return std::pair(it->second, s);
    };
    auto uvOf = [](const glm::dvec3& s) {
        return glm::vec2(float(0.5 + std::atan2(s.x, s.z) / glm::two_pi<double>()), float(0.5 + std::asin(std::clamp(s.y, -1.0, 1.0)) / glm::pi<double>()));
    };
    std::vector<glm::vec2> uvs;
    auto addTri = [&](const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) {
        auto [ia, sa] = point(a);
        auto [ib, sb] = point(b);
        auto [ic, sc] = point(c);
        m.addFace({ia, ib, ic});
        glm::vec2 tri[3] = {uvOf(sa), uvOf(sb), uvOf(sc)};
        // Unwrap triangles straddling the u seam, and give pole corners the u of their face.
        float maxU = std::max({tri[0].x, tri[1].x, tri[2].x});
        for (glm::vec2& uv : tri)
            if (maxU - uv.x > 0.5f)
                uv.x += 1.0f;
        glm::dvec3 ss[3] = {sa, sb, sc};
        for (int k = 0; k < 3; ++k)
            if (std::abs(ss[k].y) > 1.0 - 1e-9)
                tri[k].x = (tri[(k + 1) % 3].x + tri[(k + 2) % 3].x) * 0.5f;
        uvs.insert(uvs.end(), tri, tri + 3);
    };
    for (const auto& f : faces)
    {
        const glm::dvec3 a = corners[f[0]], b = corners[f[1]], c = corners[f[2]];
        auto at = [&](int i, int j) { return a + (b - a) * (double(i) / n) + (c - a) * (double(j) / n); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i + j < n; ++i)
            {
                addTri(at(i, j), at(i + 1, j), at(i, j + 1));
                if (i + j + 1 < n)
                    addTri(at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
            }
    }
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
    std::copy(uvs.begin(), uvs.end(), uv.begin());
    m.edgeAttrs().add(attr::kHard, AttrType::Bool);
    m.edgeAttrs().add(attr::kSeam, AttrType::Bool);
    markUvSeams(m);
    if (o.baseAtOrigin)
        shiftY(m, o.radius);
    return m;
}

Mesh makePlane(const PlaneOptions& o)
{
    LILA_ZONE();

    const int sx = std::max(1, o.subdivX), sz = std::max(1, o.subdivZ);
    Mesh m;
    m.reserve(size_t(sx + 1) * (sz + 1), size_t(sx) * sz * 2 + sx + sz, size_t(sx) * sz, size_t(sx) * sz * 4);
    for (int j = 0; j <= sz; ++j)
        for (int i = 0; i <= sx; ++i)
            m.addPoint({(double(i) / sx - 0.5) * o.width, 0.0, (0.5 - double(j) / sz) * o.depth});
    std::vector<glm::vec2> uvs;
    uvs.reserve(size_t(sx) * sz * 4);
    auto id = [&](int i, int j) { return Index(j * (sx + 1) + i); };
    for (int j = 0; j < sz; ++j)
        for (int i = 0; i < sx; ++i)
        {
            // Rows advance toward -Z, so this order faces +Y.
            m.addFace({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
            uvs.push_back({float(i) / sx, float(j) / sz});
            uvs.push_back({float(i + 1) / sx, float(j) / sz});
            uvs.push_back({float(i + 1) / sx, float(j + 1) / sz});
            uvs.push_back({float(i) / sx, float(j + 1) / sz});
        }
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
    std::copy(uvs.begin(), uvs.end(), uv.begin());
    m.edgeAttrs().add(attr::kHard, AttrType::Bool);
    m.edgeAttrs().add(attr::kSeam, AttrType::Bool);
    return m;
}

Mesh makeSphere(const SphereOptions& o)
{
    LILA_ZONE();

    const int h = std::max(2, o.subdivHeight);
    std::vector<ProfilePoint> profile;
    for (int j = 0; j <= h; ++j)
    {
        const double phi = glm::pi<double>() * j / h;
        profile.push_back({j == 0 || j == h ? 0.0 : o.radius * std::sin(phi), -o.radius * std::cos(phi)});
    }
    Mesh m = lathe(profile, {{0, h, Band::Cylindrical}}, std::max(3, o.subdivAxis), false);
    if (o.baseAtOrigin)
        shiftY(m, o.radius);
    return m;
}

Mesh makeCylinder(const CylinderOptions& o)
{
    LILA_ZONE();

    const int hs = std::max(1, o.subdivHeight);
    const int cs = std::max(0, o.subdivCaps);
    const int rings = std::max(1, cs);
    const double y0 = o.baseAtOrigin ? 0.0 : -0.5 * o.height;
    std::vector<ProfilePoint> profile;
    std::vector<Band> bands;
    const float scale = kCapScale / float(o.radius);
    if (o.caps)
    {
        for (int r = 0; r < rings; ++r)
            profile.push_back({o.radius * r / rings, y0});
        bands.push_back({0, rings, Band::Planar, {}, kCapLeft, scale, cs == 0});
    }
    const int sideStart = int(profile.size());
    for (int j = 0; j <= hs; ++j)
        profile.push_back({o.radius, y0 + o.height * j / hs, o.caps && (j == 0 || j == hs)});
    bands.push_back({sideStart, sideStart + hs, Band::Cylindrical, {0.5f, 1.0f}});
    if (o.caps)
    {
        const int top = int(profile.size()) - 1;
        for (int r = rings - 1; r >= 0; --r)
            profile.push_back({o.radius * r / rings, y0 + o.height});
        bands.push_back({top, top + rings, Band::Planar, {}, kCapRight, scale, cs == 0});
    }
    return lathe(profile, bands, std::max(3, o.subdivAxis), false);
}

Mesh makeCone(const ConeOptions& o)
{
    LILA_ZONE();

    const int hs = std::max(1, o.subdivHeight);
    const int cs = std::max(0, o.subdivCap);
    const int rings = std::max(1, cs);
    const double y0 = o.baseAtOrigin ? 0.0 : -0.5 * o.height;
    std::vector<ProfilePoint> profile;
    for (int r = 0; r < rings; ++r)
        profile.push_back({o.radius * r / rings, y0});
    for (int j = 0; j <= hs; ++j)
        profile.push_back({o.radius * (1.0 - double(j) / hs), y0 + o.height * j / hs, j == 0});
    std::vector<Band> bands = {{0, rings, Band::Planar, {}, kCapLeft, kCapScale / float(o.radius), cs == 0},
                               {rings, rings + hs, Band::Cylindrical, {0.5f, 1.0f}}};
    return lathe(profile, bands, std::max(3, o.subdivAxis), false);
}

Mesh makeTorus(const TorusOptions& o)
{
    LILA_ZONE();

    const int h = std::max(3, o.subdivHeight);
    std::vector<ProfilePoint> profile;
    for (int j = 0; j < h; ++j)
    {
        // Starting on the inside keeps the uv seam out of sight.
        const double t = glm::two_pi<double>() * j / h + glm::pi<double>();
        profile.push_back({o.radius + o.sectionRadius * std::cos(t), o.sectionRadius * std::sin(t)});
    }
    return lathe(profile, {{0, h, Band::Cylindrical}}, std::max(3, o.subdivAxis), true);
}

Mesh makeDisc(const DiscOptions& o)
{
    LILA_ZONE();

    const int rs = std::max(0, o.subdivRings);
    const int rings = std::max(1, rs);
    std::vector<ProfilePoint> profile;
    for (int r = rings; r >= 0; --r)
        profile.push_back({o.radius * r / rings, 0.0});
    return lathe(profile, {{0, rings, Band::Planar, {}, {0.5f, 0.5f}, 0.5f / float(o.radius), rs == 0}},
                 std::max(3, o.subdivAxis), false);
}

Mesh makePipe(const PipeOptions& o)
{
    LILA_ZONE();

    const int hs = std::max(1, o.subdivHeight);
    const double y0 = o.baseAtOrigin ? 0.0 : -0.5 * o.height;
    const double ri = std::max(0.0, o.radius - o.thickness);
    std::vector<ProfilePoint> profile;
    for (int j = 0; j <= hs; ++j)
        profile.push_back({o.radius, y0 + o.height * j / hs, j == 0 || j == hs});
    for (int j = hs; j >= 0; --j)
        profile.push_back({ri, y0 + o.height * j / hs, j == 0 || j == hs});
    const int innerTop = hs + 1;
    const int innerBottom = 2 * hs + 1;
    const float scale = kCapScale / float(o.radius);
    std::vector<Band> bands = {
        {0, hs, Band::Cylindrical, {0.5f, 0.75f}},
        {hs, innerTop, Band::Planar, {}, kCapRight, scale},
        {innerTop, innerBottom, Band::Cylindrical, {0.75f, 1.0f}},
        {innerBottom, innerBottom + 1, Band::Planar, {}, kCapLeft, scale},
    };
    return lathe(profile, bands, std::max(3, o.subdivAxis), true);
}
} // namespace lila::geo
