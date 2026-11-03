#include "curve/HairCards.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Tessellate.h"
#include "geo/Attribute.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace lila::curve
{
namespace
{
/** The curve as points along it: the tessellation, then resampled to n evenly by length. */
std::vector<D3> along(const Spline& s, int n)
{
    if (!isValid(s))
        return {};
    return groom::resample(tessellate(s).points, n);
}

/** Card sample count for a curve: segments + 1, or its own points when segments is 0. */
int samplesFor(const Spline& s, const CardOptions& o) { return std::max(2, o.segments > 0 ? o.segments + 1 : int(s.size())); }

/** Root mean square distance of the members' samples from the centreline's, i.e. how wide the clump is. */
double spread(const std::vector<std::vector<D3>>& members, const std::vector<D3>& line)
{
    double sum = 0.0;
    size_t n = 0;
    for (const auto& m : members)
        for (size_t i = 0; i < m.size() && i < line.size(); ++i)
        {
            sum += glm::dot(m[i] - line[i], m[i] - line[i]);
            ++n;
        }
    return n ? std::sqrt(sum / double(n)) : 0.0;
}

/** Clump index of each root: k-means seeded by farthest point sampling, so the result is the same every time. */
std::vector<int> clumpRoots(const std::vector<D3>& roots, int k)
{
    const size_t n = roots.size();
    std::vector<int> label(n, 0);
    if (n == 0 || k <= 1)
        return label;
    k = std::min<int>(k, int(n));
    std::vector<D3> centres = {roots[0]};
    std::vector<double> nearest(n, 1e300);
    while (int(centres.size()) < k)
    {
        size_t far = 0;
        for (size_t i = 0; i < n; ++i)
        {
            nearest[i] = std::min(nearest[i], glm::distance(roots[i], centres.back()));
            if (nearest[i] > nearest[far])
                far = i;
        }
        centres.push_back(roots[far]);
    }
    for (int it = 0; it < 12; ++it)
    {
        for (size_t i = 0; i < n; ++i)
        {
            double best = 1e300;
            for (int c = 0; c < k; ++c)
                if (double d = glm::distance(roots[i], centres[size_t(c)]); d < best)
                {
                    best = d;
                    label[i] = c;
                }
        }
        std::vector<D3> sum(size_t(k), D3(0.0));
        std::vector<int> count(size_t(k), 0);
        for (size_t i = 0; i < n; ++i)
        {
            sum[size_t(label[i])] += roots[i];
            ++count[size_t(label[i])];
        }
        for (int c = 0; c < k; ++c)
            if (count[size_t(c)])
                centres[size_t(c)] = sum[size_t(c)] / double(count[size_t(c)]);
    }
    return label;
}

/** A unit vector square to t, as close to hint as it can be. */
D3 perpendicular(const D3& t, const D3& hint)
{
    for (const D3& h : {hint, D3(0, 1, 0), D3(1, 0, 0), D3(0, 0, 1)})
    {
        const D3 p = h - t * glm::dot(h, t);
        if (glm::length(p) > 0.05)
            return glm::normalize(p);
    }
    return D3(1, 0, 0);
}

struct Card
{
    std::vector<D3> line;
    double width = 1.0;
};

void addCard(geo::Mesh& m, const Card& card, const groom::Surface* scalp, const CardOptions& o, int cell, bool back)
{
    const std::vector<D3>& P = card.line;
    const size_t n = P.size();
    const int W = std::max(1, o.widthSegments);
    std::vector<D3> T(n), F(n), S(n);
    for (size_t i = 0; i < n; ++i)
    {
        const D3 d = P[std::min(i + 1, n - 1)] - P[i == 0 ? 0 : i - 1];
        T[i] = glm::length(d) > 1e-12 ? glm::normalize(d) : (i ? T[i - 1] : D3(0, 1, 0));
    }
    for (size_t i = 0; i < n; ++i)
    {
        // Carried along from the last sample where the wanted facing runs along the curve.
        const D3 carried = i ? glm::rotation(T[i - 1], T[i]) * F[i - 1] : perpendicular(T[0], o.up);
        D3 want = carried;
        if (o.facing == CardFacing::UpVector)
            want = o.up;
        else if (o.facing == CardFacing::SurfaceNormal)
        {
            want = o.up;
            if (scalp)
                if (auto h = scalp->closest(P[i]))
                    want = h->normal;
        }
        D3 f = want - T[i] * glm::dot(want, T[i]);
        F[i] = glm::length(f) > 0.05 ? glm::normalize(f) : perpendicular(T[i], carried);
    }
    const double cols = std::max(1, o.columns), rows = std::max(1, o.rows);
    const double du = 1.0 / cols, dv = 1.0 / rows;
    const double u0 = (cell % int(cols)) * du + du * o.padding, v0 = (cell / int(cols)) * dv + dv * o.padding;
    const double uw = du * (1.0 - 2.0 * o.padding), vh = dv * (1.0 - 2.0 * o.padding);
    std::vector<geo::Index> ids(n * size_t(W + 1));
    std::vector<glm::vec2> uv(ids.size());
    for (size_t i = 0; i < n; ++i)
    {
        const double s = n > 1 ? double(i) / double(n - 1) : 0.0;
        const glm::dquat turn = glm::angleAxis(o.tilt + o.twist * s, T[i]);
        const D3 side = turn * glm::normalize(glm::cross(T[i], F[i]));
        const D3 face = turn * F[i];
        // A taper all the way to zero would leave degenerate quads at the tip.
        const double w = card.width * std::max(0.01, o.taper ? o.taper(s) : 1.0);
        for (int j = 0; j <= W; ++j)
        {
            const double a = double(j) / W;
            const size_t k = i * size_t(W + 1) + size_t(j);
            ids[k] = m.addPoint(P[i] + side * ((a - 0.5) * w) + face * o.offset);
            const double u = o.flipU ? 1.0 - a : a;
            const double v = o.flipV ? s : 1.0 - s;
            uv[k] = glm::vec2(float(u0 + u * uw), float(v0 + v * vh));
        }
    }
    for (size_t i = 0; i + 1 < n; ++i)
        for (int j = 0; j < W; ++j)
        {
            const size_t a = i * size_t(W + 1) + size_t(j), b = a + 1, c = a + size_t(W + 1) + 1, d = a + size_t(W + 1);
            // Counter-clockwise seen from the facing side: side, then along the curve.
            std::array<size_t, 4> q = back ? std::array<size_t, 4>{a, d, c, b} : std::array<size_t, 4>{a, b, c, d};
            const geo::Index pts[4] = {ids[q[0]], ids[q[1]], ids[q[2]], ids[q[3]]};
            const geo::Index f = m.addFace(std::span<const geo::Index>(pts, 4));
            if (f == geo::kInvalid)
                continue;
            auto uvs = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            size_t k = 0;
            m.forEachFaceCorner(f, [&](geo::Index cn) { uvs[cn] = uv[q[k++]]; });
        }
}
} // namespace

CardResult hairCards(const Splines& curves, const Splines& guides, const groom::Surface* scalp, const CardOptions& o)
{
    LILA_ZONE();

    CardResult out;
    std::vector<Card> cards;
    int skipped = 0;
    std::vector<std::vector<D3>> hair;
    std::vector<D3> roots;
    for (const Spline& s : curves)
    {
        std::vector<D3> a = along(s, samplesFor(s, o));
        if (a.size() < 2 || glm::distance(a.front(), a.back()) + glm::distance(a.front(), a[a.size() / 2]) < 1e-6)
        {
            ++skipped;
            continue;
        }
        hair.push_back(std::move(a));
        roots.push_back(hair.back().front());
    }
    CardSource source = o.source;
    if (source == CardSource::Guides && guides.empty())
    {
        out.warnings.push_back("no guide curves: one card per curve");
        source = CardSource::Curves;
    }
    auto resampled = [](const std::vector<D3>& line, size_t n) { return line.size() == n ? line : groom::resample(line, int(n)); };
    switch (source)
    {
    case CardSource::Curves:
        for (auto& h : hair)
            cards.push_back({h, o.width});
        break;
    case CardSource::Clumps:
    {
        const std::vector<int> label = clumpRoots(roots, std::max(1, o.clumps));
        const int k = label.empty() ? 0 : *std::max_element(label.begin(), label.end()) + 1;
        for (int c = 0; c < k; ++c)
        {
            std::vector<std::vector<D3>> members;
            size_t n = 0;
            for (size_t i = 0; i < hair.size(); ++i)
                if (label[i] == c)
                {
                    members.push_back(hair[i]);
                    n = std::max(n, hair[i].size());
                }
            if (members.empty())
                continue;
            for (auto& mbr : members)
                mbr = resampled(mbr, n);
            std::vector<D3> line(n, D3(0.0));
            for (const auto& mbr : members)
                for (size_t i = 0; i < n; ++i)
                    line[i] += mbr[i] / double(members.size());
            const double w = o.autoWidth ? std::max(2.0 * spread(members, line) * o.widthScale, o.width * 0.1) : o.width;
            cards.push_back({line, w});
        }
        break;
    }
    case CardSource::Guides:
    {
        std::vector<std::vector<D3>> lines;
        for (const Spline& g : guides)
        {
            std::vector<D3> a = along(g, samplesFor(g, o));
            if (a.size() < 2 || glm::distance(a.front(), a.back()) < 1e-6)
            {
                ++skipped;
                continue;
            }
            lines.push_back(std::move(a));
        }
        std::vector<std::vector<std::vector<D3>>> members(lines.size());
        for (size_t i = 0; i < hair.size(); ++i)
        {
            size_t best = 0;
            double bd = 1e300;
            for (size_t g = 0; g < lines.size(); ++g)
                if (double d = glm::distance(hair[i].front(), lines[g].front()); d < bd)
                {
                    bd = d;
                    best = g;
                }
            if (!lines.empty())
                members[best].push_back(resampled(hair[i], lines[best].size()));
        }
        for (size_t g = 0; g < lines.size(); ++g)
        {
            const double w = o.autoWidth && !members[g].empty() ? std::max(2.0 * spread(members[g], lines[g]) * o.widthScale, o.width * 0.1) : o.width;
            cards.push_back({lines[g], w});
        }
        break;
    }
    }
    if (skipped)
        out.warnings.push_back(std::to_string(skipped) + " zero-length curves skipped");
    const int cells = std::max(1, std::max(1, o.columns) * std::max(1, o.rows));
    const int pickFrom = o.variations > 0 ? std::min(o.variations, cells) : cells;
    std::mt19937 rng(o.seed);
    std::uniform_int_distribution<int> pick(0, pickFrom - 1);
    for (const Card& c : cards)
    {
        const int cell = pick(rng);
        addCard(out.mesh, c, scalp, o, cell, false);
        if (o.doubleSided)
            addCard(out.mesh, c, scalp, o, cell, true);
        out.cells.push_back(cell);
        ++out.cards;
    }
    return out;
}
} // namespace lila::curve
