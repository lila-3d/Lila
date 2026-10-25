#include "curve/Sweep.h"

#include "core/Profile.h"
#include "geo/Attribute.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;
using geo::Index;

constexpr double kEps = 1e-12;

/** Hard and seam edges by their end points, applied once every face exists (edge ids settle only then). */
struct EdgeMarks
{
    std::vector<std::pair<Index, Index>> hard, seam;

    void apply(geo::Mesh& m) const
    {
        auto set = [&](std::string_view name, const std::vector<std::pair<Index, Index>>& pairs) {
            if (pairs.empty())
                return;
            auto v = m.edgeAttrs().getOrAdd<bool>(name);
            for (auto [a, b] : pairs)
                if (Index e = m.findEdge(a, b); e != geo::kInvalid && e < v.size())
                    v[e] = true;
        };
        set(geo::attr::kHard, hard);
        set(geo::attr::kSeam, seam);
    }
};

/** Adds a face with per-corner uvs (in the face's point order); flipped faces keep their uvs on their points. */
Index addFaceUv(geo::Mesh& m, std::span<const Index> pts, std::span<const glm::vec2> uvs, bool flip)
{
    std::vector<Index> p(pts.begin(), pts.end());
    std::vector<glm::vec2> u(uvs.begin(), uvs.end());
    if (flip)
    {
        std::reverse(p.begin(), p.end());
        std::reverse(u.begin(), u.end());
    }
    Index f = m.addFace(p);
    if (f == geo::kInvalid || u.empty())
        return f;
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    size_t i = 0;
    m.forEachFaceCorner(f, [&](Index c) {
        if (i < u.size())
            uv[c] = u[i];
        ++i;
    });
    return f;
}

void setFaceInt(geo::Mesh& m, std::string_view name, Index f, int value)
{
    if (f == geo::kInvalid)
        return;
    auto v = m.faceAttrs().getOrAdd<int32_t>(name);
    if (f < v.size())
        v[f] = value;
}

D3 perpUnit(const D3& v, const D3& t)
{
    D3 r = v - t * glm::dot(v, t);
    double l = glm::length(r);
    return l > 1e-12 ? r / l : D3(0.0);
}

/** Planar uvs for a loop of points in the frame plane, packed into the unit box at u offset. */
std::vector<glm::vec2> planarUvs(const geo::Mesh& m, std::span<const Index> pts, const D3& origin, const D3& ax, const D3& ay, double extent,
                                 double uOffset)
{
    std::vector<glm::vec2> out;
    for (Index p : pts)
    {
        D3 d = m.position(p) - origin;
        out.push_back(glm::vec2(float(uOffset + 0.5 + glm::dot(d, ax) / (2.0 * extent)), float(0.5 + glm::dot(d, ay) / (2.0 * extent))));
    }
    return out;
}
} // namespace

double Profile::signedArea() const
{
    double a = 0.0;
    size_t n = points.size();
    for (size_t i = 0; i < n; ++i)
    {
        const D3& p = points[i];
        const D3& q = points[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5;
}

Profile circleProfile(int sides, double radius)
{
    Profile p;
    sides = std::max(3, sides);
    for (int i = 0; i < sides; ++i)
    {
        double a = glm::two_pi<double>() * i / sides;
        p.points.push_back(D3(std::cos(a) * radius, std::sin(a) * radius, 0.0));
    }
    return p;
}

Profile rectProfile(double width, double height)
{
    Profile p;
    double w = width * 0.5, h = height * 0.5;
    p.points = {D3(-w, -h, 0), D3(w, -h, 0), D3(w, h, 0), D3(-w, h, 0)};
    p.hard.assign(4, 1);
    return p;
}

Profile profileFromCurve(const Polyline& pl, const glm::dvec3& pathStart, const Frame& f, bool alignToPath)
{
    Profile out;
    out.closed = pl.closed;
    size_t n = pl.size();
    if (n == 0)
        return out;
    out.hard.assign(pl.kink.begin(), pl.kink.end());
    if (!alignToPath)
    {
        for (const D3& p : pl.points)
        {
            D3 d = p - pathStart;
            out.points.push_back(D3(glm::dot(d, f.n), glm::dot(d, f.b), glm::dot(d, f.t)));
        }
        return out;
    }
    // The profile's own plane: its normal turns onto the path tangent, its centre onto the path start.
    D3 c(0.0);
    for (const D3& p : pl.points)
        c += p;
    c /= double(n);
    D3 pn = planeNormal(pl.points);
    if (glm::length(pn) < 0.5)
    {
        // A straight profile: lay it across the path in the normal direction.
        D3 dir = glm::length(pl.points.back() - pl.points.front()) > kEps ? glm::normalize(pl.points.back() - pl.points.front()) : D3(1, 0, 0);
        for (const D3& p : pl.points)
            out.points.push_back(D3(glm::dot(p - c, dir), 0.0, 0.0));
        return out;
    }
    if (glm::dot(pn, f.t) < 0.0)
        pn = -pn;
    // The shortest turn from the profile's plane onto the path's, so a profile drawn across the path start keeps its look.
    D3 axis = glm::cross(pn, f.t);
    double sn = glm::length(axis), cs = glm::dot(pn, f.t);
    for (const D3& p : pl.points)
    {
        D3 d = p - c;
        if (sn > 1e-12)
            d = glm::rotate(d, std::atan2(sn, cs), axis / sn);
        out.points.push_back(D3(glm::dot(d, f.n), glm::dot(d, f.b), 0.0));
    }
    return out;
}

Path makePath(const Polyline& pl, const std::vector<Frame>& fr, std::span<const double> scale)
{
    Path p;
    p.points = pl.points;
    p.frames = fr;
    p.s = pl.arcLength;
    p.kink = pl.kink;
    p.tangentIn = pl.tangentIn;
    p.tangentOut = pl.tangentOut;
    p.closed = pl.closed;
    p.length = pl.length;
    p.scale.assign(pl.size(), 1.0);
    for (size_t i = 0; i < std::min(scale.size(), p.scale.size()); ++i)
        p.scale[i] = scale[i];
    return p;
}

Path pathFrom(const Spline& s, const TessOptions& tess, const FrameOptions& fo)
{
    Polyline pl = tessellate(s, tess);
    std::vector<double> tilt = tiltAlong(s, pl);
    std::vector<Frame> fr = frames(pl, fo, tilt);
    std::vector<double> scale;
    if (!s.radius.empty())
        for (double u : pl.params)
            scale.push_back(pointValueAt(s, s.radius, 1.0, u));
    return makePath(pl, fr, scale);
}

const char* capModeName(CapMode m)
{
    switch (m)
    {
    case CapMode::None: return "none";
    case CapMode::Ngon: return "ngon";
    case CapMode::Grid: return "grid";
    case CapMode::Round: return "round";
    }
    return "none";
}

bool parseCapMode(std::string_view name, CapMode* out)
{
    for (CapMode m : {CapMode::None, CapMode::Ngon, CapMode::Grid, CapMode::Round})
        if (name == capModeName(m))
        {
            *out = m;
            return true;
        }
    return false;
}

std::vector<geo::Index> gridFill(geo::Mesh& m, std::span<const geo::Index> loop, bool flip,
                                 const std::function<glm::dvec3(const glm::dvec3& p, double u, double v)>& lift)
{
    std::vector<Index> faces;
    size_t n = loop.size();
    if (n < 3)
        return faces;
    if (n % 2 == 1 || n == 4)
    {
        std::vector<Index> pts(loop.begin(), loop.end());
        if (flip)
            std::reverse(pts.begin(), pts.end());
        if (Index f = m.addFace(pts); f != geo::kInvalid)
            faces.push_back(f);
        return faces;
    }
    int a = int(n / 4), b = int(n / 2) - a;
    std::vector<std::vector<Index>> G(size_t(a) + 1, std::vector<Index>(size_t(b) + 1, geo::kInvalid));
    for (int i = 0; i <= a; ++i)
        G[size_t(i)][0] = loop[size_t(i)];
    for (int j = 0; j <= b; ++j)
        G[size_t(a)][size_t(j)] = loop[size_t(a + j)];
    for (int i = 0; i <= a; ++i)
        G[size_t(i)][size_t(b)] = loop[size_t(a + b + (a - i))];
    for (int j = 0; j <= b; ++j)
        G[0][size_t(j)] = loop[size_t((2 * a + b + (b - j)) % int(n))];
    auto P = [&](int i, int j) { return m.position(G[size_t(i)][size_t(j)]); };
    for (int i = 1; i < a; ++i)
        for (int j = 1; j < b; ++j)
        {
            double u = double(i) / a, v = double(j) / b;
            D3 p = P(i, 0) * (1 - v) + P(i, b) * v + P(0, j) * (1 - u) + P(a, j) * u -
                   (P(0, 0) * (1 - u) * (1 - v) + P(a, 0) * u * (1 - v) + P(0, b) * (1 - u) * v + P(a, b) * u * v);
            if (lift)
                p = lift(p, u, v);
            G[size_t(i)][size_t(j)] = m.addPoint(p);
        }
    for (int i = 0; i < a; ++i)
        for (int j = 0; j < b; ++j)
        {
            std::vector<Index> q = {G[size_t(i)][size_t(j)], G[size_t(i) + 1][size_t(j)], G[size_t(i) + 1][size_t(j) + 1], G[size_t(i)][size_t(j) + 1]};
            if (flip)
                std::reverse(q.begin(), q.end());
            if (Index f = m.addFace(q); f != geo::kInvalid)
                faces.push_back(f);
        }
    return faces;
}

SweepResult sweep(const Profile& input, const Path& path, const SweepOptions& o)
{
    LILA_ZONE();

    SweepResult r;
    if (input.size() < 2 || path.size() < 2)
        return r;
    Profile prof = input;
    if (prof.hard.size() != prof.size())
        prof.hard.assign(prof.size(), 0);
    if (prof.closed && prof.signedArea() < 0.0)
    {
        std::reverse(prof.points.begin() + 1, prof.points.end());
        std::reverse(prof.hard.begin() + 1, prof.hard.end());
    }
    const size_t np = prof.size();
    const bool pc = prof.closed && np >= 3;
    const size_t around = pc ? np : np - 1;

    // U across the profile.
    std::vector<double> pu(np + 1, 0.0);
    for (size_t j = 1; j <= np; ++j)
        pu[j] = pu[j - 1] + (j < np || pc ? glm::distance(prof.points[j - 1], prof.points[j % np]) : 0.0);
    double plen = pc ? pu[np] : pu[np - 1];
    for (double& x : pu)
        x = plen > kEps ? x / plen : 0.0;
    auto U = [&](size_t j) { return float(o.uRange.x + (o.uRange.y - o.uRange.x) * pu[j]); };

    struct Station
    {
        double s = 0.0;
        D3 origin{0.0};
        Frame f;
        double scale = 1.0;
        bool kink = false;
        D3 tin{0.0}, tout{0.0};
        bool split = false;
    };
    std::vector<Station> st;
    for (size_t i = 0; i < path.size(); ++i)
        st.push_back({path.s[i], path.points[i], path.frames[i], path.scale[i], path.kink[i] != 0, path.tangentIn[i], path.tangentOut[i], false});
    double L = path.closed ? path.length : path.s.back();
    // Splits: an existing station when one is close, else an interpolated one.
    std::vector<double> splits = o.splits;
    std::sort(splits.begin(), splits.end());
    for (double sp : splits)
    {
        if (!(sp > 1e-6) || sp >= L - 1e-6)
            continue;
        auto it = std::lower_bound(st.begin(), st.end(), sp, [](const Station& a, double v) { return a.s < v; });
        if (it != st.end() && std::abs(it->s - sp) < 1e-6)
        {
            it->split = true;
            continue;
        }
        if (it != st.begin() && std::abs(std::prev(it)->s - sp) < 1e-6)
        {
            std::prev(it)->split = true;
            continue;
        }
        size_t hi = size_t(it - st.begin());
        const Station& b = hi < st.size() ? st[hi] : st.front();
        const Station& a = st[hi - 1];
        double sb = hi < st.size() ? b.s : L;
        double f = sb - a.s > kEps ? (sp - a.s) / (sb - a.s) : 0.0;
        Station mid;
        mid.s = sp;
        mid.origin = glm::mix(a.origin, b.origin, f);
        mid.f.t = glm::normalize(glm::mix(a.f.t, b.f.t, f));
        mid.f.n = perpUnit(glm::mix(a.f.n, b.f.n, f), mid.f.t);
        if (glm::length(mid.f.n) < 0.5)
            mid.f.n = a.f.n;
        mid.f.b = glm::cross(mid.f.t, mid.f.n);
        mid.scale = glm::mix(a.scale, b.scale, f);
        mid.tin = mid.tout = mid.f.t;
        mid.split = true;
        st.insert(it, mid);
    }
    const size_t ns = st.size();
    const size_t segs = path.closed ? ns : ns - 1;

    // Parts: a split station ends one part and starts the next.
    std::vector<int> partAfter(ns, 0);
    std::vector<double> partStart{0.0};
    for (size_t i = 0; i < ns; ++i)
    {
        if (i > 0)
            partAfter[i] = partAfter[i - 1];
        if (st[i].split && i > 0)
        {
            ++partAfter[i];
            partStart.push_back(st[i].s);
        }
    }
    int parts = partAfter.back() + 1;
    std::vector<double> partEnd(size_t(parts), L);
    for (int k = 0; k + 1 < parts; ++k)
        partEnd[size_t(k)] = partStart[size_t(k) + 1];
    auto V = [&](double s, int part) {
        double s0 = partStart[size_t(part)], s1 = partEnd[size_t(part)];
        if (o.vPerCm > 0.0)
            return float((s - s0) * o.vPerCm);
        return float(s1 - s0 > kEps ? (s - s0) / (s1 - s0) : 0.0);
    };

    geo::Mesh& m = r.mesh;
    EdgeMarks marks;
    auto ringPoint = [&](const Station& sta, const D3& q, Frame* used, double* scaleOut) {
        Frame f = sta.f;
        if (o.twist)
            if (double tw = o.twist(sta.s); tw != 0.0)
            {
                f.n = glm::rotate(f.n, tw, f.t);
                f.b = glm::cross(f.t, f.n);
            }
        double sc = sta.scale * (o.scale ? o.scale(sta.s) : 1.0);
        D3 off = (f.n * q.x + f.b * q.y + f.t * q.z) * sc;
        if (o.miter && sta.kink)
        {
            double theta = std::atan2(glm::length(glm::cross(sta.tin, sta.tout)), glm::dot(sta.tin, sta.tout));
            D3 k = perpUnit(sta.tout - sta.tin, f.t);
            double c = std::cos(theta * 0.5);
            if (glm::length(k) > 0.5 && c > 0.1)
                off += k * glm::dot(off, k) * (1.0 / c - 1.0);
        }
        if (used)
            *used = f;
        if (scaleOut)
            *scaleOut = sc;
        return sta.origin + off;
    };
    // Each station's ring; a detached split station has a second ring that starts the next part.
    std::vector<Index> ringBefore(ns), ringAfter(ns);
    for (size_t i = 0; i < ns; ++i)
    {
        SweepRing ring;
        ring.s = st[i].s;
        ring.origin = st[i].origin;
        ring.part = partAfter[i];
        ring.firstPoint = m.pointCapacity();
        for (const D3& q : prof.points)
            m.addPoint(ringPoint(st[i], q, &ring.frame, &ring.scale));
        ringBefore[i] = ringAfter[i] = ring.firstPoint;
        r.rings.push_back(ring);
        if (o.detachParts && st[i].split && i > 0)
        {
            ringAfter[i] = m.pointCapacity();
            for (size_t j = 0; j < np; ++j)
                m.addPoint(m.position(ringBefore[i] + Index(j)));
        }
    }

    double hardCos = std::cos(o.hardKinkAngle);
    for (size_t i = 0; i < segs; ++i)
    {
        size_t i1 = (i + 1) % ns;
        int part = partAfter[i];
        Index A = ringAfter[i], B = ringBefore[i1];
        double s0 = st[i].s, s1 = i1 == 0 ? L : st[i1].s;
        float v0 = V(s0, part), v1 = V(s1, part);
        for (size_t j = 0; j < around; ++j)
        {
            size_t j1 = (j + 1) % np;
            Index q[4] = {A + Index(j), A + Index(j1), B + Index(j1), B + Index(j)};
            glm::vec2 uv[4] = {{U(j), v0}, {U(j + 1), v0}, {U(j + 1), v1}, {U(j), v1}};
            Index f = addFaceUv(m, q, uv, o.flip);
            setFaceInt(m, "part", f, part);
            if (size_t(part) < o.partMaterials.size())
                setFaceInt(m, geo::attr::kMaterial, f, o.partMaterials[size_t(part)]);
            if (prof.hard[j])
                marks.hard.push_back({A + Index(j), B + Index(j)});
        }
        if (pc)
            marks.seam.push_back({A, B});
    }
    for (size_t i = 0; i < ns; ++i)
    {
        bool ringHard = st[i].kink && glm::dot(st[i].tin, st[i].tout) < hardCos;
        bool ringSeam = (st[i].split && i > 0 && !o.detachParts) || (path.closed && i == 0);
        if (!ringHard && !ringSeam)
            continue;
        for (size_t j = 0; j < around; ++j)
        {
            std::pair<Index, Index> e{ringBefore[i] + Index(j), ringBefore[i] + Index((j + 1) % np)};
            if (ringHard)
                marks.hard.push_back(e);
            if (ringSeam)
                marks.seam.push_back(e);
        }
    }

    // Caps.
    if (pc && !path.closed)
        for (int end = 0; end < 2; ++end)
        {
            CapMode mode = end ? o.endCap : o.startCap;
            if (mode == CapMode::None)
                continue;
            size_t si = end ? ns - 1 : 0;
            Index first = end ? ringBefore[si] : ringAfter[si];
            const SweepRing& ring = r.rings[si];
            D3 dir = end ? ring.frame.t : -ring.frame.t;
            std::vector<Index> loop(np);
            for (size_t j = 0; j < np; ++j)
                loop[j] = first + Index(j);
            // The end loop runs counter-clockwise about +t, so the start cap is the reverse.
            bool flip = (end == 0) != o.flip;
            D3 centre(0.0);
            for (Index p : loop)
                centre += m.position(p);
            centre /= double(np);
            double rad = 0.0;
            for (Index p : loop)
                rad = std::max(rad, glm::distance(m.position(p), centre));
            int part = partAfter[si];
            double extent = std::max(rad, 1e-9);
            double uOff = 1.1 + end * 1.1;
            if (mode == CapMode::Round && o.roundSegments > 1)
            {
                int R = o.roundSegments;
                double meanR = 0.0;
                for (Index p : loop)
                    meanR += glm::distance(m.position(p), centre);
                meanR /= double(np);
                std::vector<Index> prev = loop;
                std::vector<D3> base;
                for (Index p : loop)
                    base.push_back(m.position(p));
                float vBase = V(end ? L : 0.0, part);
                double vStep = (o.vPerCm > 0.0 ? o.vPerCm : 1.0 / std::max(partEnd[size_t(part)] - partStart[size_t(part)], kEps)) * meanR *
                               glm::half_pi<double>() / R;
                for (int k = 1; k < R; ++k)
                {
                    double a = glm::half_pi<double>() * k / R;
                    std::vector<Index> ringPts;
                    for (const D3& b : base)
                        ringPts.push_back(m.addPoint(centre + (b - centre) * std::cos(a) + dir * meanR * std::sin(a)));
                    float va = vBase + float((end ? 1.0 : -1.0) * vStep * (k - 1)), vb = vBase + float((end ? 1.0 : -1.0) * vStep * k);
                    for (size_t j = 0; j < np; ++j)
                    {
                        size_t j1 = (j + 1) % np;
                        Index q[4] = {prev[j], prev[j1], ringPts[j1], ringPts[j]};
                        glm::vec2 uv[4] = {{U(j), va}, {U(j + 1), va}, {U(j + 1), vb}, {U(j), vb}};
                        Index f = addFaceUv(m, q, uv, flip);
                        setFaceInt(m, "part", f, part);
                        if (size_t(part) < o.partMaterials.size())
                            setFaceInt(m, geo::attr::kMaterial, f, o.partMaterials[size_t(part)]);
                    }
                    prev = ringPts;
                }
                double aLast = glm::half_pi<double>() * (R - 1) / R;
                D3 cLast = centre + dir * meanR * std::sin(aLast);
                double rLast = meanR * std::cos(aLast);
                auto lift = [&](const D3& p, double, double) {
                    double d = glm::length(p - cLast - dir * glm::dot(p - cLast, dir));
                    double h = std::sqrt(std::max(0.0, meanR * meanR - d * d)) - std::sqrt(std::max(0.0, meanR * meanR - rLast * rLast));
                    return p + dir * h;
                };
                std::vector<Index> fs = gridFill(m, prev, flip, lift);
                for (Index f : fs)
                {
                    std::vector<Index> pts = m.facePoints(f);
                    auto uvs = planarUvs(m, pts, cLast, ring.frame.n, ring.frame.b, extent, uOff);
                    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
                    size_t c = 0;
                    m.forEachFaceCorner(f, [&](Index cc) { uv[cc] = uvs[c++]; });
                    setFaceInt(m, "part", f, part);
                }
                for (size_t j = 0; j < np; ++j)
                    marks.seam.push_back({prev[j], prev[(j + 1) % np]});
                continue;
            }
            std::vector<Index> fs = mode == CapMode::Ngon ? std::vector<Index>{} : gridFill(m, loop, flip);
            if (mode == CapMode::Ngon || fs.empty())
            {
                std::vector<Index> pts = loop;
                if (flip)
                    std::reverse(pts.begin(), pts.end());
                if (Index f = m.addFace(pts); f != geo::kInvalid)
                    fs.push_back(f);
            }
            for (Index f : fs)
            {
                std::vector<Index> pts = m.facePoints(f);
                auto uvs = planarUvs(m, pts, centre, ring.frame.n, ring.frame.b, extent, uOff);
                auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
                size_t c = 0;
                m.forEachFaceCorner(f, [&](Index cc) { uv[cc] = uvs[c++]; });
                setFaceInt(m, "part", f, part);
                if (size_t(part) < o.partMaterials.size())
                    setFaceInt(m, geo::attr::kMaterial, f, o.partMaterials[size_t(part)]);
            }
            for (size_t j = 0; j < np; ++j)
            {
                marks.hard.push_back({loop[j], loop[(j + 1) % np]});
                marks.seam.push_back({loop[j], loop[(j + 1) % np]});
            }
        }
    marks.apply(m);
    r.parts = parts;
    return r;
}

geo::Mesh revolve(const Polyline& prof, const RevolveOptions& o)
{
    LILA_ZONE();

    geo::Mesh m;
    size_t np = prof.size();
    if (np < 2)
        return m;
    D3 ax = glm::length(o.axis) > kEps ? glm::normalize(o.axis) : D3(0, 1, 0);
    double sweepAngle = o.endAngle - o.startAngle;
    bool full = std::abs(sweepAngle) >= glm::two_pi<double>() - 1e-9;
    int segs = std::max(full ? 3 : 1, o.segments);
    size_t rings = full ? size_t(segs) : size_t(segs) + 1;
    bool pc = prof.closed && np >= 3;
    size_t around = pc ? np : np - 1;

    std::vector<uint8_t> pole(np, 0);
    std::vector<Index> poleIdx(np, geo::kInvalid);
    for (size_t j = 0; j < np; ++j)
    {
        D3 d = prof.points[j] - o.origin;
        D3 radial = d - ax * glm::dot(d, ax);
        if (glm::length(radial) < o.poleTolerance)
        {
            pole[j] = 1;
            poleIdx[j] = m.addPoint(o.origin + ax * glm::dot(d, ax));
        }
    }
    std::vector<std::vector<Index>> idx(rings, std::vector<Index>(np));
    for (size_t r = 0; r < rings; ++r)
    {
        double a = o.startAngle + sweepAngle * double(r) / segs;
        for (size_t j = 0; j < np; ++j)
            idx[r][j] = pole[j] ? poleIdx[j] : m.addPoint(o.origin + glm::rotate(prof.points[j] - o.origin, a, ax));
    }
    std::vector<double> pv(np + 1, 0.0);
    for (size_t j = 1; j <= np; ++j)
        pv[j] = pv[j - 1] + (j < np || pc ? glm::distance(prof.points[j - 1], prof.points[j % np]) : 0.0);
    double plen = pc ? pv[np] : pv[np - 1];
    for (double& x : pv)
        x = plen > kEps ? x / plen : 0.0;

    EdgeMarks marks;
    std::vector<Index> faces;
    for (size_t r = 0; r < size_t(segs); ++r)
    {
        size_t r1 = (r + 1) % rings;
        float u0 = float(double(r) / segs), u1 = float(double(r + 1) / segs);
        for (size_t j = 0; j < around; ++j)
        {
            size_t j1 = (j + 1) % np;
            Index q[4] = {idx[r][j], idx[r1][j], idx[r1][j1], idx[r][j1]};
            glm::vec2 uv[4] = {{u0, float(pv[j])}, {u1, float(pv[j])}, {u1, float(pv[j + 1])}, {u0, float(pv[j + 1])}};
            std::vector<Index> pts;
            std::vector<glm::vec2> uvs;
            for (int k = 0; k < 4; ++k)
                if (pts.empty() || pts.back() != q[k])
                {
                    pts.push_back(q[k]);
                    uvs.push_back(uv[k]);
                }
            if (pts.size() > 1 && pts.back() == pts.front())
            {
                pts.pop_back();
                uvs.pop_back();
            }
            if (pts.size() < 3)
                continue;
            if (Index f = addFaceUv(m, pts, uvs, false); f != geo::kInvalid)
                faces.push_back(f);
        }
        for (size_t j = 0; j < np; ++j)
            if (prof.kink[j] && !pole[j])
                marks.hard.push_back({idx[r][j], idx[r1][j]});
        if (pc && !pole[0])
            marks.seam.push_back({idx[r][0], idx[r1][0]});
    }
    if (full)
        for (size_t j = 0; j < around; ++j)
            marks.seam.push_back({idx[0][j], idx[0][(j + 1) % np]});
    if (!full && pc && o.caps)
        for (int end = 0; end < 2; ++end)
        {
            size_t r = end ? rings - 1 : 0;
            std::vector<Index> loop = idx[r];
            std::vector<Index> fs = gridFill(m, loop, end == 0);
            for (Index f : fs)
            {
                std::vector<Index> pts = m.facePoints(f);
                D3 c(0.0);
                for (Index p : loop)
                    c += m.position(p);
                c /= double(loop.size());
                double ext = 1e-9;
                for (Index p : loop)
                    ext = std::max(ext, glm::distance(m.position(p), c));
                D3 x = glm::normalize(m.position(loop[0]) - c);
                D3 y = glm::cross(ax, x);
                auto uvs = planarUvs(m, pts, c, x, y, ext, 1.1 + end * 1.1);
                auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
                size_t k = 0;
                m.forEachFaceCorner(f, [&](Index cc) { uv[cc] = uvs[k++]; });
                faces.push_back(f);
            }
            for (size_t j = 0; j < np; ++j)
            {
                marks.hard.push_back({loop[j], loop[(j + 1) % np]});
                marks.seam.push_back({loop[j], loop[(j + 1) % np]});
            }
        }
    // Face normals away from the axis on the whole.
    double outward = 0.0;
    for (Index f : faces)
    {
        D3 c = m.faceCentroid(f) - o.origin;
        D3 radial = c - ax * glm::dot(c, ax);
        outward += glm::dot(m.faceAreaVector(f), radial);
    }
    if ((outward < 0.0) != o.flip)
        for (Index f : faces)
            m.flipFace(f);
    marks.apply(m);
    return m;
}

namespace
{
/** count + 1 points (count on closed curves, no repeat) evenly spaced along the polyline between arc lengths a and b. */
std::vector<D3> sampleRun(const Polyline& pl, double a, double b, int count, bool dropLast)
{
    std::vector<D3> out;
    for (int i = 0; i <= count; ++i)
    {
        if (dropLast && i == count)
            break;
        out.push_back(pl.pointAtLength(a + (b - a) * double(i) / count));
    }
    return out;
}

struct Section
{
    std::vector<D3> pts;
    std::vector<uint8_t> hard;
};

/** Similarity taking a to A and b to B (rotation by the shortest arc, uniform scale). */
D3 similarity(const D3& p, const D3& a, const D3& b, const D3& A, const D3& B)
{
    D3 u = b - a, v = B - A;
    double lu = glm::length(u), lv = glm::length(v);
    if (lu < kEps || lv < kEps)
        return p + (A - a);
    D3 du = u / lu, dv = v / lv;
    D3 axis = glm::cross(du, dv);
    double s = glm::length(axis), c = glm::dot(du, dv);
    D3 q = p - a;
    if (s > 1e-12)
        q = glm::rotate(q, std::atan2(s, c), axis / s);
    else if (c < 0.0)
        q = -q;
    return A + q * (lv / lu);
}
} // namespace

geo::Mesh loft(const std::vector<Spline>& sections, const LoftOptions& o, const std::vector<Spline>& rails)
{
    LILA_ZONE();

    geo::Mesh m;
    size_t K = sections.size();
    if (K < 2)
        return m;
    std::vector<Polyline> pls;
    bool closed = true;
    for (const Spline& s : sections)
    {
        pls.push_back(tessellate(s, o.tess));
        if (pls.back().size() < 2)
            return m;
        closed = closed && s.closed;
    }
    // Kinks (run boundaries) per section.
    std::vector<std::vector<double>> cuts(K);
    for (size_t k = 0; k < K; ++k)
        for (size_t i = 0; i < pls[k].size(); ++i)
            if (pls[k].kink[i] && (closed || (i > 0 && i + 1 < pls[k].size())))
                cuts[k].push_back(pls[k].arcLength[i]);
    bool matchKinks = o.samples <= 0 && !cuts[0].empty() && std::ranges::all_of(cuts, [&](const auto& c) { return c.size() == cuts[0].size(); });

    std::vector<Section> secs(K);
    if (matchKinks)
    {
        size_t runs = closed ? cuts[0].size() : cuts[0].size() + 1;
        auto runRange = [&](size_t k, size_t r) -> std::pair<double, double> {
            const auto& c = cuts[k];
            if (closed)
                return {c[r], r + 1 < c.size() ? c[r + 1] : c[0] + pls[k].length};
            return {r == 0 ? 0.0 : c[r - 1], r < c.size() ? c[r] : pls[k].length};
        };
        for (size_t r = 0; r < runs; ++r)
        {
            int count = 1;
            for (size_t k = 0; k < K; ++k)
            {
                auto [a, b] = runRange(k, r);
                int pts = 0;
                for (size_t i = 0; i < pls[k].size(); ++i)
                {
                    double s = pls[k].arcLength[i];
                    if (s > a + 1e-9 && s < b - 1e-9)
                        ++pts;
                    else if (closed && b > pls[k].length && s + pls[k].length > a + 1e-9 && s + pls[k].length < b - 1e-9)
                        ++pts;
                }
                count = std::max(count, pts + 1);
            }
            for (size_t k = 0; k < K; ++k)
            {
                auto [a, b] = runRange(k, r);
                bool last = r + 1 == runs;
                std::vector<D3> p = sampleRun(pls[k], a, b, count, closed || !last);
                for (size_t i = 0; i < p.size(); ++i)
                {
                    secs[k].pts.push_back(p[i]);
                    secs[k].hard.push_back(i == 0 && (closed || r > 0) ? 1 : 0);
                }
            }
        }
    }
    else
    {
        int count = o.samples;
        if (count <= 0)
            for (const Polyline& pl : pls)
                count = std::max(count, int(pl.size()) - (closed ? 0 : 1));
        count = std::max(count, closed ? 3 : 1);
        for (size_t k = 0; k < K; ++k)
        {
            secs[k].pts = sampleRun(pls[k], 0.0, pls[k].length, count, closed);
            secs[k].hard.assign(secs[k].pts.size(), 0);
        }
    }
    size_t N = secs[0].pts.size();
    for (const Section& s : secs)
        if (s.pts.size() != N)
            return m;

    // Line the sections up with each other.
    if (o.autoAlign)
        for (size_t k = 1; k < K; ++k)
        {
            const std::vector<D3>& prev = secs[k - 1].pts;
            Section& cur = secs[k];
            auto cost = [&](size_t shift, bool rev) {
                double c = 0.0;
                for (size_t j = 0; j < N; ++j)
                {
                    size_t src = rev ? (shift + N - j) % N : (shift + j) % N;
                    c += glm::dot(cur.pts[src] - prev[j], cur.pts[src] - prev[j]);
                }
                return c;
            };
            size_t bestShift = 0;
            bool bestRev = false;
            double best = std::numeric_limits<double>::max();
            if (closed)
            {
                for (size_t shift = 0; shift < N; ++shift)
                {
                    // With kinks lined up, only shifts that land kink on kink.
                    if (matchKinks && !cur.hard[shift])
                        continue;
                    for (bool rev : {false, true})
                        if (double c = cost(shift, rev); c < best)
                        {
                            best = c;
                            bestShift = shift;
                            bestRev = rev;
                        }
                }
            }
            else
            {
                double fwd = 0.0, back = 0.0;
                for (size_t j = 0; j < N; ++j)
                {
                    fwd += glm::distance(cur.pts[j], prev[j]);
                    back += glm::distance(cur.pts[N - 1 - j], prev[j]);
                }
                bestRev = back < fwd;
            }
            Section next = cur;
            for (size_t j = 0; j < N; ++j)
            {
                size_t src = closed ? (bestRev ? (bestShift + N - j) % N : (bestShift + j) % N) : (bestRev ? N - 1 - j : j);
                next.pts[j] = cur.pts[src];
                next.hard[j] = cur.hard[src];
            }
            cur = std::move(next);
        }

    // Closed sections run counter-clockwise seen along the loft, so faces point out.
    if (closed && N >= 3)
    {
        D3 c0(0.0), c1(0.0);
        for (const D3& p : secs[0].pts)
            c0 += p;
        for (const D3& p : secs[1].pts)
            c1 += p;
        c0 /= double(N);
        c1 /= double(N);
        D3 along = c1 - c0;
        D3 area(0.0);
        for (size_t j = 0; j < N; ++j)
            area += glm::cross(secs[0].pts[j] - c0, secs[0].pts[(j + 1) % N] - c0);
        if (glm::dot(area, along) < 0.0)
            for (Section& s : secs)
            {
                std::reverse(s.pts.begin() + 1, s.pts.end());
                std::vector<uint8_t> h = s.hard;
                for (size_t j = 0; j < N; ++j)
                    s.hard[j] = h[(N - j) % N];
            }
    }
    std::vector<uint8_t> colHard(N, 0);
    for (const Section& s : secs)
        for (size_t j = 0; j < N; ++j)
            colHard[j] = colHard[j] || s.hard[j];

    // Rings along.
    int between = std::max(1, o.spansBetween);
    bool loop = o.closeLoft && K >= 3;
    size_t gaps = loop ? K : K - 1;
    auto secAt = [&](long i) -> const std::vector<D3>& { return secs[size_t(((i % long(K)) + long(K)) % long(K))].pts; };
    auto colPoint = [&](size_t gap, double f, size_t j) {
        long i = long(gap);
        D3 p1 = secAt(i)[j], p2 = secAt(i + 1)[j];
        if (!o.smooth || K < 3)
            return glm::mix(p1, p2, f);
        D3 p0 = (loop || i > 0) ? secAt(i - 1)[j] : p1 * 2.0 - p2;
        D3 p3 = (loop || i + 2 < long(K)) ? secAt(i + 2)[j] : p2 * 2.0 - p1;
        auto dt = [](const D3& a, const D3& b) { return std::max(std::sqrt(glm::distance(a, b)), 1e-9); };
        double d0 = dt(p0, p1), d1 = dt(p1, p2), d2 = dt(p2, p3);
        D3 m1 = ((p1 - p0) / d0 - (p2 - p0) / (d0 + d1) + (p2 - p1) / d1) * d1;
        D3 m2 = ((p2 - p1) / d1 - (p3 - p1) / (d1 + d2) + (p3 - p2) / d2) * d1;
        double t = f, t2 = t * t, t3 = t2 * t;
        return p1 * (2 * t3 - 3 * t2 + 1) + m1 * (t3 - 2 * t2 + t) + p2 * (-2 * t3 + 3 * t2) + m2 * (t3 - t2);
    };
    struct RingPos
    {
        size_t gap;
        double f;
    };
    std::vector<RingPos> ringPos;
    for (size_t g = 0; g < gaps; ++g)
        for (int k = 0; k < between; ++k)
            ringPos.push_back({g, double(k) / between});
    if (!loop)
        ringPos.push_back({K - 2, 1.0});
    std::vector<std::vector<D3>> rings;
    for (const RingPos& rp : ringPos)
    {
        std::vector<D3> ring(N);
        for (size_t j = 0; j < N; ++j)
            ring[j] = colPoint(rp.gap, rp.f, j);
        rings.push_back(std::move(ring));
    }
    // Rails steer the rings: each rail's attachment follows it between the sections' own attachment parameters.
    if (!rails.empty())
    {
        size_t cols[2] = {0, 0};
        std::vector<std::vector<double>> railU(std::min<size_t>(rails.size(), 2), std::vector<double>(K, 0.0));
        for (size_t r = 0; r < railU.size(); ++r)
        {
            if (r == 1)
            {
                double best = std::numeric_limits<double>::max();
                for (size_t j = 0; j < N; ++j)
                {
                    D3 q;
                    closestParam(rails[1], secs[0].pts[j], &q);
                    if (double d = glm::distance(q, secs[0].pts[j]); d < best)
                    {
                        best = d;
                        cols[1] = j;
                    }
                }
            }
            for (size_t k = 0; k < K; ++k)
                railU[r][k] = closestParam(rails[r], secs[k].pts[cols[r]]);
        }
        for (size_t ri = 0; ri < rings.size(); ++ri)
        {
            const RingPos& rp = ringPos[ri];
            size_t k0 = rp.gap % K, k1 = (rp.gap + 1) % K;
            D3 target[2];
            for (size_t r = 0; r < railU.size(); ++r)
                target[r] = pointAt(rails[r], glm::mix(railU[r][k0], railU[r][k1], rp.f));
            std::vector<D3>& ring = rings[ri];
            if (railU.size() == 1)
            {
                D3 shift = target[0] - ring[cols[0]];
                for (D3& p : ring)
                    p += shift;
            }
            else if (cols[1] != cols[0])
            {
                D3 a = ring[cols[0]], b = ring[cols[1]];
                for (D3& p : ring)
                    p = similarity(p, a, b, target[0], target[1]);
            }
        }
    }

    size_t R = rings.size();
    std::vector<std::vector<Index>> idx(R, std::vector<Index>(N));
    for (size_t r = 0; r < R; ++r)
        for (size_t j = 0; j < N; ++j)
            idx[r][j] = m.addPoint(rings[r][j]);
    // U by arc length across each ring, V by the mean distance between rings.
    std::vector<std::vector<double>> u(R, std::vector<double>(N + 1, 0.0));
    for (size_t r = 0; r < R; ++r)
    {
        for (size_t j = 1; j <= N; ++j)
            u[r][j] = u[r][j - 1] + (j < N || closed ? glm::distance(rings[r][j - 1], rings[r][j % N]) : 0.0);
        double total = closed ? u[r][N] : u[r][N - 1];
        for (double& x : u[r])
            x = total > kEps ? x / total : 0.0;
    }
    size_t ringSegs = loop ? R : R - 1;
    std::vector<double> v(R + 1, 0.0);
    for (size_t r = 1; r <= R; ++r)
    {
        if (r == R && !loop)
            break;
        double d = 0.0;
        for (size_t j = 0; j < N; ++j)
            d += glm::distance(rings[r - 1][j], rings[r % R][j]);
        v[r] = v[r - 1] + d / double(N);
    }
    double vTotal = v[ringSegs];
    for (double& x : v)
        x = vTotal > kEps ? x / vTotal : 0.0;
    EdgeMarks marks;
    size_t around = closed ? N : N - 1;
    for (size_t r = 0; r < ringSegs; ++r)
    {
        size_t r1 = (r + 1) % R;
        for (size_t j = 0; j < around; ++j)
        {
            size_t j1 = (j + 1) % N;
            Index q[4] = {idx[r][j], idx[r][j1], idx[r1][j1], idx[r1][j]};
            glm::vec2 uv[4] = {{float(u[r][j]), float(v[r])}, {float(u[r][j + 1]), float(v[r])}, {float(u[r1][j + 1]), float(v[r + 1])},
                               {float(u[r1][j]), float(v[r + 1])}};
            addFaceUv(m, q, uv, o.flip);
        }
        for (size_t j = 0; j < N; ++j)
            if (colHard[j])
                marks.hard.push_back({idx[r][j], idx[r1][j]});
        if (closed)
            marks.seam.push_back({idx[r][0], idx[r1][0]});
    }
    if (loop)
        for (size_t j = 0; j < around; ++j)
            marks.seam.push_back({idx[0][j], idx[0][(j + 1) % N]});
    if (closed && !loop && o.caps)
        for (int end = 0; end < 2; ++end)
        {
            const std::vector<Index>& ringIdx = idx[end ? R - 1 : 0];
            std::vector<Index> fs = gridFill(m, ringIdx, (end == 0) != o.flip);
            D3 c(0.0);
            for (Index p : ringIdx)
                c += m.position(p);
            c /= double(N);
            double ext = 1e-9;
            for (Index p : ringIdx)
                ext = std::max(ext, glm::distance(m.position(p), c));
            D3 x = glm::normalize(m.position(ringIdx[0]) - c);
            D3 nrm = planeNormal(rings[end ? R - 1 : 0]);
            if (glm::length(nrm) < 0.5)
                nrm = glm::normalize(glm::cross(x, m.position(ringIdx[N / 4]) - c));
            D3 y = glm::cross(nrm, x);
            for (Index f : fs)
            {
                std::vector<Index> pts = m.facePoints(f);
                auto uvs = planarUvs(m, pts, c, x, y, ext, 1.1 + end * 1.1);
                auto uvl = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
                size_t k = 0;
                m.forEachFaceCorner(f, [&](Index cc) { uvl[cc] = uvs[k++]; });
            }
            for (size_t j = 0; j < N; ++j)
            {
                marks.hard.push_back({ringIdx[j], ringIdx[(j + 1) % N]});
                marks.seam.push_back({ringIdx[j], ringIdx[(j + 1) % N]});
            }
        }
    marks.apply(m);
    return m;
}

geo::Mesh coonsPatch(const std::array<Spline, 4>& sides, int nu, int nv)
{
    LILA_ZONE();

    geo::Mesh m;
    nu = std::max(1, nu);
    nv = std::max(1, nv);
    TessOptions to;
    to.chordError = 1e-3;
    std::array<Polyline, 4> pl;
    for (int k = 0; k < 4; ++k)
    {
        pl[size_t(k)] = tessellate(sides[size_t(k)], to);
        if (pl[size_t(k)].size() < 2)
            return m;
    }
    auto along = [&](int k, double f) { return pl[size_t(k)].pointAtLength(pl[size_t(k)].length * f); };
    // Sides run around: bottom (u), right (v), top (backwards in u), left (backwards in v).
    auto bottom = [&](double uu) { return along(0, uu); };
    auto right = [&](double vv) { return along(1, vv); };
    auto top = [&](double uu) { return along(2, 1.0 - uu); };
    auto left = [&](double vv) { return along(3, 1.0 - vv); };
    D3 p00 = bottom(0), p10 = bottom(1), p01 = top(0), p11 = top(1);
    std::vector<Index> idx(size_t(nu + 1) * size_t(nv + 1));
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i)
        {
            double uu = double(i) / nu, vv = double(j) / nv;
            D3 p = bottom(uu) * (1 - vv) + top(uu) * vv + left(vv) * (1 - uu) + right(vv) * uu -
                   (p00 * (1 - uu) * (1 - vv) + p10 * uu * (1 - vv) + p01 * (1 - uu) * vv + p11 * uu * vv);
            idx[size_t(j) * size_t(nu + 1) + size_t(i)] = m.addPoint(p);
        }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
        {
            auto at = [&](int a, int b) { return idx[size_t(b) * size_t(nu + 1) + size_t(a)]; };
            Index q[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
            glm::vec2 uv[4] = {{float(i) / nu, float(j) / nv}, {float(i + 1) / nu, float(j) / nv}, {float(i + 1) / nu, float(j + 1) / nv},
                               {float(i) / nu, float(j + 1) / nv}};
            addFaceUv(m, q, uv, false);
        }
    return m;
}
} // namespace lila::curve
