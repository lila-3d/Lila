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

/** Six welded grids on the surface of an sx*sy*sz lattice (five with openBottom); pos maps lattice coords to space. */
Mesh buildBox(int sx, int sy, int sz, const std::function<glm::dvec3(const glm::ivec3&)>& pos, bool hardSides, bool openBottom = false)
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
        if (openBottom && s == 3)
            continue;
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
    CapFill fill = CapFill::Fan;  // how a band ending in a pole closes
};

/**
 * Closes both ends of a partial lathe with the profile polygon at each, run back along the axis when the profile is open.
 * The caps get their own uv islands in a strip along the bottom, with the swept surface scaled into the space above.
 */
std::vector<Index> closeCuts(Mesh& m, std::vector<glm::vec2>& cornerUv, const std::vector<ProfilePoint>& profile, const std::vector<bool>& pole,
                             const std::function<Index(int, int)>& pid, int axis, double sweep, bool closed)
{
    const int np = int(profile.size());
    Index axisFirst = kInvalid, axisLast = kInvalid;
    auto axisPoint = [&](Index& id, double y) {
        if (id == kInvalid)
            id = m.addPoint({0.0, y, 0.0});
        return id;
    };
    struct Cap
    {
        std::vector<Index> pts;
        std::vector<glm::dvec2> local;
    };
    std::vector<Cap> caps;
    for (int end = 0; end < 2; ++end)
    {
        const glm::dvec3 out = end ? glm::dvec3(-std::sin(sweep), 0.0, -std::cos(sweep)) : glm::dvec3(0.0, 0.0, 1.0);
        Cap cap;
        for (int k = 0; k < np; ++k)
            cap.pts.push_back(pid(k, end ? axis : 0));
        if (!closed)
        {
            if (!pole[np - 1])
                cap.pts.push_back(axisPoint(axisLast, profile[np - 1].y));
            if (!pole[0])
                cap.pts.push_back(axisPoint(axisFirst, profile[0].y));
        }
        glm::dvec3 normal(0.0);
        for (size_t a = 0; a < cap.pts.size(); ++a)
            normal += glm::cross(m.position(cap.pts[a]), m.position(cap.pts[(a + 1) % cap.pts.size()]));
        if (glm::length(normal) < 1e-9)
            continue;
        if (glm::dot(normal, out) < 0.0)
            std::reverse(cap.pts.begin(), cap.pts.end());
        const glm::dvec3 u = glm::cross(glm::dvec3(0.0, 1.0, 0.0), out);
        for (Index p : cap.pts)
            cap.local.push_back({glm::dot(m.position(p), u), m.position(p).y});
        caps.push_back(std::move(cap));
    }
    if (caps.empty())
        return {};

    for (glm::vec2& uv : cornerUv)
        uv = uv * 0.78f + glm::vec2(0.0f, 0.22f);
    std::vector<Index> faces;
    for (size_t k = 0; k < caps.size(); ++k)
    {
        const Index f = m.addFace(caps[k].pts);
        if (f == kInvalid)
            continue;
        faces.push_back(f);
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const glm::dvec2& l : caps[k].local)
        {
            lo = glm::min(lo, l);
            hi = glm::max(hi, l);
        }
        const glm::dvec2 boxMin(0.01 + 0.5 * double(k), 0.01), boxSize(0.48, 0.19);
        const glm::dvec2 extent = glm::max(hi - lo, glm::dvec2(1e-12));
        const double scale = std::min(boxSize.x / extent.x, boxSize.y / extent.y);
        const glm::dvec2 offset = boxMin + (boxSize - extent * scale) * 0.5;
        for (const glm::dvec2& l : caps[k].local)
            cornerUv.push_back(glm::vec2(offset + (l - lo) * scale));
    }
    return faces;
}

/**
 * Revolves a profile in (r, y) around +Y. Profile points with r == 0 become a single pole point.
 * Walking the profile with the outside on the right gives outward faces. A closed profile wraps its
 * last band back to point 0. A fraction below 1 sweeps only that part of the turn, from +X toward -Z;
 * cutCaps then closes each cut end with the profile polygon, run back along the axis when it is open.
 */
Mesh lathe(const std::vector<ProfilePoint>& profile, const std::vector<Band>& bands, int axis, bool closed, double fraction = 1.0,
           bool cutCaps = false)
{
    LILA_ZONE();

    Mesh m;
    const int np = int(profile.size());
    const bool partial = fraction < 1.0 - 1e-9;
    const double sweep = glm::two_pi<double>() * std::clamp(fraction, 0.01, 1.0);
    const int ring = partial ? axis + 1 : axis;
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
        for (int i = 0; i < ring; ++i)
        {
            const double t = sweep * i / axis;
            m.addPoint({profile[k].r * std::cos(t), profile[k].y, -profile[k].r * std::sin(t)});
        }
    }
    auto pid = [&](int k, int i) { return pole[k] ? ringStart[k] : ringStart[k] + Index(i % ring); };

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
    // Cap faces are built wound for a top cap (rim i, rim i + 1, inside) and reversed for a bottom one.
    auto emitCap = [&](const Band& b, std::vector<Index> face, bool bottom, bool up) {
        if (bottom)
            std::reverse(face.begin(), face.end());
        for (Index p : face)
        {
            pts.push_back(p);
            uvs.push_back(planarUv(b, p, up));
        }
        emit();
    };
    /**
     * Fills a whole circle of rim points with a Coons-patch grid of a x c quads (2(a + c) border points), joined to the rim
     * by a ring of quads. An odd rim has one point more than the grid's border, so its ring closes with a triangle.
     */
    auto quadCap = [&](const Band& b, int rim, bool bottom, bool up) {
        const int border = axis - axis % 2;
        const int a = border / 4, c = border / 2 - a;
        const double y = profile[rim].y;
        // Sized so the ring is about as wide as the grid's cells.
        const double rho = profile[rim].r / (1.0 + 1.3 / std::max(a, c));
        auto onCircle = [&](int j) {
            // Each border point faces its rim point; with an odd rim the last border gap spans the triangle's two segments.
            const double t = glm::two_pi<double>() * (j == border ? axis : j) / axis;
            return glm::dvec3(rho * std::cos(t), y, -rho * std::sin(t));
        };
        const std::array<int, 5> corner = {0, a, a + c, 2 * a + c, border};
        std::vector<glm::dvec3> loop(border);
        for (int side = 0; side < 4; ++side)
        {
            const int len = corner[side + 1] - corner[side];
            for (int k = 0; k < len; ++k)
            {
                const int j = corner[side] + k;
                // Straight sides rounded toward the circle keep the grid's corner quads square.
                loop[j] = glm::mix(glm::mix(onCircle(corner[side]), onCircle(corner[side + 1]), double(k) / len), onCircle(j), 0.4);
            }
        }
        auto borderIndex = [&](int u, int v) {
            if (v == 0)
                return u;
            if (u == a)
                return a + v;
            if (v == c)
                return a + c + (a - u);
            return (2 * a + c + (c - v)) % border;
        };
        std::vector<Index> inner(border);
        for (int j = 0; j < border; ++j)
            inner[j] = m.addPoint(loop[j]);
        std::vector<Index> grid((a + 1) * (c + 1));
        auto at = [&](int u, int v) { return loop[borderIndex(u, v)]; };
        for (int v = 0; v <= c; ++v)
            for (int u = 0; u <= a; ++u)
            {
                if (u == 0 || v == 0 || u == a || v == c)
                {
                    grid[v * (a + 1) + u] = inner[borderIndex(u, v)];
                    continue;
                }
                const double s = double(u) / a, t = double(v) / c;
                const glm::dvec3 p = (1 - s) * at(0, v) + s * at(a, v) + (1 - t) * at(u, 0) + t * at(u, c) -
                                     ((1 - s) * (1 - t) * at(0, 0) + s * (1 - t) * at(a, 0) + (1 - s) * t * at(0, c) + s * t * at(a, c));
                grid[v * (a + 1) + u] = m.addPoint(p);
            }
        for (int v = 0; v < c; ++v)
            for (int u = 0; u < a; ++u)
                emitCap(b, {grid[v * (a + 1) + u], grid[v * (a + 1) + u + 1], grid[(v + 1) * (a + 1) + u + 1], grid[(v + 1) * (a + 1) + u]}, bottom, up);
        for (int j = 0; j < border; ++j)
            emitCap(b, {pid(rim, j), pid(rim, j + 1), inner[(j + 1) % border], inner[j]}, bottom, up);
        if (border < axis)
            emitCap(b, {pid(rim, axis - 1), pid(rim, 0), inner[0]}, bottom, up);
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
            if (b.fill == CapFill::Quads && (pole[k0] || pole[k1]) && (partial || axis >= 4))
            {
                const int rim = pole[k0] ? k1 : k0;
                const Index centre = pid(pole[k0] ? k0 : k1, 0);
                if (!partial)
                    quadCap(b, rim, pole[k0], up);
                // A wedge pairs up its fan: each quad spans two segments, an odd one out stays a triangle.
                else
                    for (int i = 0; i < axis; i += 2)
                        emitCap(b, i + 1 < axis ? std::vector<Index>{pid(rim, i), pid(rim, i + 1), pid(rim, i + 2), centre}
                                                : std::vector<Index>{pid(rim, i), pid(rim, i + 1), centre},
                                pole[k0], up);
                continue;
            }
            if (b.fill != CapFill::Fan && (pole[k0] || pole[k1]))
            {
                const int rim = pole[k0] ? k1 : k0;
                // A partial sweep's cap is a wedge, so it keeps its centre point.
                if (partial && pole[k0])
                    pts.push_back(pid(k0, 0));
                for (int i = 0; i < ring; ++i)
                    pts.push_back(pid(rim, pole[k0] ? ring - 1 - i : i));
                if (partial && pole[k1])
                    pts.push_back(pid(k1, 0));
                for (Index p : pts)
                    uvs.push_back(planarUv(b, p, up));
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

    std::vector<Index> capFaces;
    if (partial && cutCaps)
        capFaces = closeCuts(m, cornerUv, profile, pole, [&](int k, int i) { return pid(k, i); }, axis, sweep, closed);

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
    for (Index f : capFaces)
        m.forEachFaceCorner(f, [&](Index c) { hard[m.cornerEdge(c)] = true; });
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
    const double q = glm::quarter_pi<double>();
    if (o.fraction >= 1.0 - 1e-6)
        return buildBox(
            n, n, n,
            [&](const glm::ivec3& c) {
                // Tangent warp spreads the grid evenly over the sphere.
                glm::dvec3 a = glm::dvec3(c) / double(n) * 2.0 - 1.0;
                a = glm::tan(a * q);
                return glm::normalize(a) * o.radius;
            },
            false);

    // The top face keeps its grid; each side column runs down its meridian from the top face to the cut circle.
    const double s = 1.0 - 2.0 * std::clamp(o.fraction, 0.01, 1.0);
    const double rho = std::sqrt(1.0 - s * s);
    // Shallow domes shrink the top face so it stays well inside the cut.
    const double g = std::min(1.0, std::tan(std::acos(s) * std::atan(std::sqrt(2.0)) / glm::half_pi<double>()) / std::sqrt(2.0));
    auto cutRow = [&](double h) { return std::atan(s * h / rho) / q; };
    const int m = std::max(1, int(std::lround(0.5 * n * (1.0 - cutRow(g)))));
    return buildBox(
        n, m, n,
        [&](const glm::ivec3& c) {
            const glm::dvec2 h = g * glm::tan((glm::dvec2(c.x, c.z) / double(n) * 2.0 - 1.0) * q);
            if (c.y == 0)
            {
                const glm::dvec2 d = glm::normalize(h) * rho;
                return glm::dvec3(d.x, s, d.y) * o.radius;
            }
            const double ay = c.y == m ? 1.0 : glm::mix(cutRow(glm::length(h)), 1.0, double(c.y) / m);
            return glm::normalize(glm::dvec3(h.x, std::tan(ay * q), h.y)) * o.radius;
        },
        false, true);
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
    const bool cut = o.fraction < 1.0 - 1e-6;
    const double yc = 1.0 - 2.0 * std::clamp(o.fraction, 0.01, 1.0);
    const double rho = std::sqrt(1.0 - yc * yc);
    auto onCut = [&](const glm::dvec3& s) {
        const glm::dvec2 d = glm::normalize(glm::dvec2(s.x, s.z)) * rho;
        return glm::dvec3(d.x, yc, d.y);
    };
    // Points this close to the cut move onto it, so clipping leaves no slivers; never so far that a shallow cap flattens.
    const double snap = std::min(0.3 * 1.05 / n, 0.25 * (1.0 - yc));

    Mesh m;
    // Lattice points on shared icosahedron edges coincide exactly, so welding by rounded position is safe.
    std::map<std::array<int64_t, 3>, Index> welded;
    auto point = [&](const glm::dvec3& s) {
        std::array<int64_t, 3> key = {std::llround(s.x * 1e9), std::llround(s.y * 1e9), std::llround(s.z * 1e9)};
        auto [it, added] = welded.try_emplace(key, 0);
        if (added)
            it->second = m.addPoint(s * o.radius);
        return it->second;
    };
    auto uvOf = [](const glm::dvec3& s) {
        return glm::vec2(float(0.5 + std::atan2(s.x, s.z) / glm::two_pi<double>()), float(0.5 + std::asin(std::clamp(s.y, -1.0, 1.0)) / glm::pi<double>()));
    };
    std::vector<glm::vec2> uvs;
    std::vector<Index> ids;
    std::vector<glm::vec2> polyUv;
    auto addTri = [&](const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) {
        glm::dvec3 ss[3] = {glm::normalize(a), glm::normalize(b), glm::normalize(c)};
        if (cut)
            for (glm::dvec3& p : ss)
                if (std::abs(p.y - yc) < snap && glm::length(glm::dvec2(p.x, p.z)) > 1e-6)
                    p = onCut(p);
        glm::vec2 tri[3] = {uvOf(ss[0]), uvOf(ss[1]), uvOf(ss[2])};
        // Unwrap triangles straddling the u seam, and give pole corners the u of their face.
        float maxU = std::max({tri[0].x, tri[1].x, tri[2].x});
        for (glm::vec2& uv : tri)
            if (maxU - uv.x > 0.5f)
                uv.x += 1.0f;
        for (int k = 0; k < 3; ++k)
            if (std::abs(ss[k].y) > 1.0 - 1e-9)
                tri[k].x = (tri[(k + 1) % 3].x + tri[(k + 2) % 3].x) * 0.5f;

        ids.clear();
        polyUv.clear();
        auto keep = [&](const glm::dvec3& p, const glm::vec2& uv) {
            const Index id = point(p);
            if (!ids.empty() && (ids.back() == id || ids.front() == id))
                return;
            ids.push_back(id);
            polyUv.push_back(uv);
        };
        bool above = !cut;
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3& p0 = ss[k];
            const glm::dvec3& p1 = ss[(k + 1) % 3];
            const bool in0 = !cut || p0.y >= yc - 1e-12, in1 = !cut || p1.y >= yc - 1e-12;
            if (in0)
            {
                keep(p0, tri[k]);
                above = above || p0.y > yc + 1e-12;
            }
            if (in0 != in1)
            {
                const double t = (p0.y - yc) / (p0.y - p1.y);
                keep(onCut(glm::mix(p0, p1, t)), glm::mix(tri[k], tri[(k + 1) % 3], float(t)));
            }
        }
        // A triangle touching the cut only along its edge, or lying in it, leaves nothing.
        if (ids.size() < 3 || !above || m.addFace(ids) == kInvalid)
            return;
        uvs.insert(uvs.end(), polyUv.begin(), polyUv.end());
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
    m.removeAllLoose();
    if (m.hasDeleted())
        m.compact();
    markUvSeams(m);
    if (o.baseAtOrigin)
        shiftY(m, -(cut ? yc : -1.0) * o.radius);
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

    const double pi = glm::pi<double>();
    // The cut sits on a ring: the rings kept are spread evenly from it to the top pole, about as many as that arc had.
    const double cut = o.fraction >= 1.0 - 1e-9 ? 0.0 : std::acos(2.0 * std::clamp(o.fraction, 0.01, 1.0) - 1.0);
    const int h = std::max(cut > 0.0 ? 1 : 2, int(std::lround(std::max(2, o.subdivHeight) * (pi - cut) / pi)));
    std::vector<ProfilePoint> profile;
    for (int j = 0; j <= h; ++j)
    {
        const double phi = cut + (pi - cut) * j / h;
        profile.push_back({j == h || (j == 0 && cut == 0.0) ? 0.0 : o.radius * std::sin(phi), -o.radius * std::cos(phi)});
    }
    Mesh m = lathe(profile, {{0, h, Band::Cylindrical}}, std::max(3, o.subdivAxis), false);
    if (o.baseAtOrigin)
        shiftY(m, o.radius * std::cos(cut));
    return m;
}

Mesh makeCylinder(const CylinderOptions& o)
{
    LILA_ZONE();

    const int hs = std::max(1, o.subdivHeight);
    const int rings = std::max(1, o.subdivCaps);
    const double y0 = o.baseAtOrigin ? 0.0 : -0.5 * o.height;
    std::vector<ProfilePoint> profile;
    std::vector<Band> bands;
    const float scale = kCapScale / float(o.radius);
    const bool caps = o.caps && o.capFill != CapFill::None;
    if (caps)
    {
        for (int r = 0; r < rings; ++r)
            profile.push_back({o.radius * r / rings, y0});
        bands.push_back({0, rings, Band::Planar, {}, kCapLeft, scale, o.capFill});
    }
    const int sideStart = int(profile.size());
    for (int j = 0; j <= hs; ++j)
        profile.push_back({o.radius, y0 + o.height * j / hs, caps && (j == 0 || j == hs)});
    bands.push_back({sideStart, sideStart + hs, Band::Cylindrical, {0.5f, 1.0f}});
    if (caps)
    {
        const int top = int(profile.size()) - 1;
        for (int r = rings - 1; r >= 0; --r)
            profile.push_back({o.radius * r / rings, y0 + o.height});
        bands.push_back({top, top + rings, Band::Planar, {}, kCapRight, scale, o.capFill});
    }
    return lathe(profile, bands, std::max(3, o.subdivAxis), false, o.fraction, caps && o.cutCaps);
}

Mesh makeCapsule(const CapsuleOptions& o)
{
    LILA_ZONE();

    const int caps = std::max(1, o.subdivCaps);
    const int hs = std::max(1, o.subdivHeight);
    const double half = std::max(0.0, 0.5 * o.height - o.radius);
    std::vector<ProfilePoint> profile;
    for (int j = 0; j <= caps; ++j)
    {
        const double phi = 0.5 * glm::pi<double>() * j / caps;
        profile.push_back({j == 0 ? 0.0 : o.radius * std::sin(phi), -half - o.radius * std::cos(phi)});
    }
    for (int j = 1; j <= hs && half > 0.0; ++j)
        profile.push_back({o.radius, -half + 2.0 * half * j / hs});
    for (int j = 1; j <= caps; ++j)
    {
        const double phi = 0.5 * glm::pi<double>() * j / caps;
        profile.push_back({j == caps ? 0.0 : o.radius * std::cos(phi), half + o.radius * std::sin(phi)});
    }
    Mesh m = lathe(profile, {{0, int(profile.size()) - 1, Band::Cylindrical}}, std::max(3, o.subdivAxis), false);
    if (o.baseAtOrigin)
        shiftY(m, half + o.radius);
    return m;
}

Mesh makeCone(const ConeOptions& o)
{
    LILA_ZONE();

    const int hs = std::max(1, o.subdivHeight);
    const bool cap = o.capFill != CapFill::None;
    const int rings = cap ? std::max(1, o.subdivCap) : 0;
    const double y0 = o.baseAtOrigin ? 0.0 : -0.5 * o.height;
    std::vector<ProfilePoint> profile;
    for (int r = 0; r < rings; ++r)
        profile.push_back({o.radius * r / rings, y0});
    for (int j = 0; j <= hs; ++j)
        profile.push_back({o.radius * (1.0 - double(j) / hs), y0 + o.height * j / hs, cap && j == 0});
    std::vector<Band> bands = {{rings, rings + hs, Band::Cylindrical, {0.5f, 1.0f}}};
    if (cap)
        bands.insert(bands.begin(), Band{0, rings, Band::Planar, {}, kCapLeft, kCapScale / float(o.radius), o.capFill});
    return lathe(profile, bands, std::max(3, o.subdivAxis), false, o.fraction, cap && o.cutCaps);
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
    return lathe(profile, {{0, h, Band::Cylindrical}}, std::max(3, o.subdivAxis), true, o.fraction, o.cutCaps);
}

Mesh makeDisc(const DiscOptions& o)
{
    LILA_ZONE();

    const int rs = std::max(0, o.subdivRings);
    const int rings = std::max(1, rs);
    std::vector<ProfilePoint> profile;
    for (int r = rings; r >= 0; --r)
        profile.push_back({o.radius * r / rings, 0.0});
    return lathe(profile, {{0, rings, Band::Planar, {}, {0.5f, 0.5f}, 0.5f / float(o.radius), rs == 0 ? CapFill::Ngon : CapFill::Fan}},
                 std::max(3, o.subdivAxis), false, o.fraction);
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
    return lathe(profile, bands, std::max(3, o.subdivAxis), true, o.fraction, o.cutCaps);
}
} // namespace lila::geo
