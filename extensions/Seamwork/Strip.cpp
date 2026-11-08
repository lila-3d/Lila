#include "Strip.h"

#include <lila/Mesh.h>

#include <algorithm>
#include <cmath>

namespace seamwork
{
using lila::geo::Index;

namespace
{
glm::dvec3 unitOr(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

struct Row
{
    glm::dvec3 p, t, na, nb;
    double along = 0.0;
};

/** Rows along a curve every spacing cm, its ends included (a closed curve wraps instead). */
std::vector<Row> resample(const Curve& c, double spacing)
{
    std::vector<Row> rows;
    const size_t n = c.points.size();
    if (n < 2)
        return rows;
    const size_t segs = c.closed ? n : n - 1;
    std::vector<double> at(segs + 1, 0.0);
    for (size_t s = 0; s < segs; ++s)
        at[s + 1] = at[s] + glm::length(c.points[(s + 1) % n] - c.points[s]);
    const double total = at.back();
    if (total < 1e-9)
        return rows;
    const int count = std::max(1, int(std::ceil(total / std::max(0.1, spacing))));
    const int last = c.closed ? count - 1 : count;
    size_t s = 0;
    for (int k = 0; k <= last; ++k)
    {
        const double d = total * double(k) / double(count);
        while (s + 1 < segs && at[s + 1] < d)
            ++s;
        const size_t i0 = s, i1 = (s + 1) % n;
        const double len = at[s + 1] - at[s];
        const double t = len > 1e-12 ? std::clamp((d - at[s]) / len, 0.0, 1.0) : 0.0;
        Row r;
        r.p = c.points[i0] + (c.points[i1] - c.points[i0]) * t;
        r.t = unitOr(c.points[i1] - c.points[i0], {1, 0, 0});
        r.na = unitOr(c.normalA[i0] * (1.0 - t) + c.normalA[i1] * t, c.normalA[i0]);
        r.nb = unitOr(c.normalB[i0] * (1.0 - t) + c.normalB[i1] * t, c.normalB[i0]);
        r.along = d;
        rows.push_back(r);
    }
    return rows;
}

/** Direction from the join across a side: up the wall (side 0) or out over the ground (side 1). */
glm::dvec3 across(const Row& r, int side)
{
    const glm::dvec3 surface = side == 0 ? r.na : r.nb, other = side == 0 ? r.nb : r.na;
    glm::dvec3 d = other - surface * glm::dot(other, surface);
    if (glm::length(d) < 1e-9)
        d = glm::cross(surface, r.t);
    return glm::normalize(d);
}
} // namespace

Mesh buildStrips(const Result& r, const StripOptions& o, const ConformFn& conform, size_t* strips)
{
    std::vector<glm::dvec3> pts;
    std::vector<glm::vec2> uvs;
    std::vector<float> alpha, side;
    std::vector<Index> sizes, idx;
    size_t made = 0;
    const int div = std::max(1, o.divisions);
    Band band = o.band;
    band.width = o.width;
    for (const Curve& c : r.curves)
    {
        if (!(o.kinds & (1 << int(c.kind))))
            continue;
        const std::vector<Row> rows = resample(c, o.spacing);
        if (rows.size() < 2)
            continue;
        // Columns across: wall edge to join to ground edge, as the side asks.
        std::vector<std::pair<int, double>> cols; // (side, distance from the join)
        if (o.side == Side::Wall || o.side == Side::Both)
            for (int k = div; k >= (o.side == Side::Both ? 1 : 0); --k)
                cols.push_back({0, o.width * k / div});
        if (o.side == Side::Ground || o.side == Side::Both)
            for (int k = 0; k <= div; ++k)
                cols.push_back({1, o.width * k / div});
        const Index base = Index(pts.size());
        const size_t nc = cols.size();
        for (const Row& row : rows)
            for (size_t ci = 0; ci < nc; ++ci)
            {
                const auto [s, d] = cols[ci];
                const glm::dvec3 n = s == 0 ? row.na : row.nb;
                glm::dvec3 q = row.p + across(row, s) * d;
                glm::dvec3 hit, hn;
                if (conform && d > 0.0 && conform(q + n * 10.0, -n, hit, hn))
                    q = hit + hn * o.offset;
                else
                    q += n * o.offset;
                pts.push_back(q);
                double v;
                if (o.side == Side::Both)
                    v = s == 0 ? 0.5 - 0.5 * d / o.width : 0.5 + 0.5 * d / o.width;
                else
                    v = d / std::max(1e-9, o.width);
                uvs.push_back(glm::vec2(float(row.along / std::max(1e-3, o.tile)), float(v)));
                alpha.push_back(float(band.weight(d, q)));
                side.push_back(float(s));
            }
        const size_t nr = rows.size();
        // Curves run either way round, so wind each strip to face out of the surface it lies on.
        const Row& r0 = rows.front();
        const bool flip = o.side == Side::Wall ? glm::dot(glm::cross(r0.t, -across(r0, 0)), r0.na) < 0.0
                                               : glm::dot(glm::cross(r0.t, across(r0, 1)), r0.nb) < 0.0;
        const size_t spans = c.closed && nr > 2 ? nr : nr - 1;
        for (size_t k = 0; k < spans; ++k)
        {
            const size_t k1 = (k + 1) % nr;
            for (size_t ci = 0; ci + 1 < nc; ++ci)
            {
                const Index a = base + Index(k * nc + ci), b = base + Index(k * nc + ci + 1);
                const Index c1 = base + Index(k1 * nc + ci + 1), d1 = base + Index(k1 * nc + ci);
                sizes.push_back(4);
                if (flip)
                    idx.insert(idx.end(), {a, b, c1, d1});
                else
                    idx.insert(idx.end(), {a, d1, c1, b});
            }
        }
        ++made;
    }
    Mesh m = Mesh::fromPolygons(pts, sizes, idx);
    if (strips)
        *strips = made;
    if (m.numFaces() == 0)
        return m;
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    auto col = lila::geo::colors::add(m, "Col", glm::vec4(1.0f));
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
        {
            const Index p = m.cornerPoint(c);
            uv[c] = uvs[p];
            col[c] = glm::vec4(1.0f, 1.0f, 1.0f, alpha[p]);
        }
    if (!o.material.empty())
    {
        m.materialNames() = {o.material};
        auto mat = m.faceAttrs().getOrAdd<int>("material");
        std::fill(mat.begin(), mat.end(), 0);
    }
    auto sd = m.pointAttrs().getOrAdd<float>("side");
    for (Index p = 0; p < m.pointCapacity(); ++p)
        sd[p] = side[p];
    return m;
}

std::vector<DecalBox> decalBoxes(const Result& r, const StripOptions& o, double segment)
{
    std::vector<DecalBox> out;
    for (size_t ci = 0; ci < r.curves.size(); ++ci)
    {
        const Curve& c = r.curves[ci];
        if (!(o.kinds & (1 << int(c.kind))))
            continue;
        // Boxes never straddle a corner of the curve: runs between corners are cut on their own.
        const size_t np = c.points.size();
        const size_t segs = c.closed && np > 2 ? np : np - 1;
        std::vector<size_t> corners{0};
        for (size_t s = 1; s < segs; ++s)
        {
            const glm::dvec3 d0 = c.points[s] - c.points[s - 1], d1 = c.points[(s + 1) % np] - c.points[s];
            if (glm::length(d0) > 1e-9 && glm::length(d1) > 1e-9 && glm::dot(glm::normalize(d0), glm::normalize(d1)) < std::cos(glm::radians(15.0)))
                corners.push_back(s);
        }
        corners.push_back(segs);
        std::vector<std::pair<Row, Row>> spans;
        for (size_t ri = 0; ri + 1 < corners.size(); ++ri)
        {
            const glm::dvec3 p0 = c.points[corners[ri]], p1 = c.points[corners[ri + 1] % np];
            const double len = glm::length(p1 - p0);
            const int pieces = std::max(1, int(std::ceil(len / std::max(1.0, segment))));
            const size_t i0 = corners[ri], i1 = corners[ri + 1] % np;
            for (int k = 0; k < pieces; ++k)
            {
                auto at = [&](double t) {
                    return Row{p0 + (p1 - p0) * t, unitOr(p1 - p0, {1, 0, 0}), unitOr(c.normalA[i0] * (1.0 - t) + c.normalA[i1] * t, c.normalA[i0]),
                               unitOr(c.normalB[i0] * (1.0 - t) + c.normalB[i1] * t, c.normalB[i0]), 0.0};
                };
                spans.emplace_back(at(double(k) / pieces), at(double(k + 1) / pieces));
            }
        }
        for (size_t k = 0; k < spans.size(); ++k)
        {
            const Row& a = spans[k].first;
            const Row& b = spans[k].second;
            const glm::dvec3 mid = (a.p + b.p) * 0.5;
            const double len = glm::length(b.p - a.p);
            if (len < 1e-6)
                continue;
            const Row m{mid, unitOr(b.p - a.p, a.t), unitOr(a.na + b.na, a.na), unitOr(a.nb + b.nb, a.nb), 0.0};
            for (int s : {0, 1})
            {
                if ((s == 0 && o.side == Side::Ground) || (s == 1 && o.side == Side::Wall))
                    continue;
                const glm::dvec3 n = s == 0 ? m.na : m.nb;
                DecalBox d;
                d.key = std::to_string(ci) + ":" + std::to_string(k) + ":" + (s == 0 ? "w" : "g");
                d.center = mid + across(m, s) * (o.width * 0.5);
                d.project = -n;
                d.along = m.t;
                d.size = glm::dvec3(std::max(10.0, o.width * 0.5), o.width * 0.5, len * 0.5 + 1.0);
                out.push_back(d);
            }
        }
    }
    return out;
}
} // namespace seamwork
