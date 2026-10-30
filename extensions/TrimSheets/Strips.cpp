#include "Strips.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace trims
{
namespace
{
glm::dvec3 stripUnit(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

glm::dvec3 stationCentre(const Station& s) { return (s.across.front() + s.across.back()) * 0.5; }

double stationWidth(const Station& s) { return glm::length(s.across.back() - s.across.front()); }

Station stationLerp(const Station& a, const Station& b, double t)
{
    Station out;
    out.across.resize(a.across.size());
    for (size_t c = 0; c < a.across.size(); ++c)
        out.across[c] = a.across[c] + (b.across[c] - a.across[c]) * t;
    out.normal = stripUnit(a.normal + (b.normal - a.normal) * t, a.normal);
    return out;
}

enum class StripPart : uint8_t
{
    Head,
    Body,
    Tail,
};

/** Adds face pts, turned to face `outward`, with a uv per point. */
void stripFace(geo::Mesh& m, std::vector<geo::Index> pts, std::vector<glm::dvec2> uvs, const glm::dvec3& outward, int trim)
{
    glm::dvec3 n{0.0};
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const glm::dvec3 a = m.position(pts[i]), b = m.position(pts[(i + 1) % pts.size()]);
        n += glm::cross(a, b);
    }
    if (glm::dot(n, outward) < 0.0)
    {
        std::reverse(pts.begin(), pts.end());
        std::reverse(uvs.begin(), uvs.end());
    }
    const geo::Index f = m.addFace(std::span<const geo::Index>(pts));
    if (f == geo::kInvalid)
        return;
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    for (size_t i = 0; i < pts.size(); ++i)
        if (const geo::Index c = m.findCorner(f, pts[i]); c != geo::kInvalid)
            uv[c] = glm::vec2(uvs[i]);
    m.faceAttrs().getOrAdd<int32_t>(kFaceTrim)[f] = trim + 1;
}
} // namespace

geo::Mesh stripMesh(const Sheet& sheet, std::span<const Station> input, const StripOptions& o)
{
    geo::Mesh m;
    if (input.size() < 2 || o.trim >= sheet.trims.size())
        return m;
    std::vector<Station> st(input.begin(), input.end());
    const size_t cols = st.front().across.size();
    if (cols < 2 || std::ranges::any_of(st, [&](const Station& s) { return s.across.size() != cols; }))
        return m;

    auto lengths = [&] {
        std::vector<double> s(st.size(), 0.0);
        for (size_t j = 1; j < st.size(); ++j)
            s[j] = s[j - 1] + glm::length(stationCentre(st[j]) - stationCentre(st[j - 1]));
        return s;
    };
    std::vector<double> s = lengths();
    const double open = s.back();
    const double total = o.closed ? open + glm::length(stationCentre(st.front()) - stationCentre(st.back())) : open;
    if (total <= 1e-9)
        return m;

    // Caps take the cap trim's aspect at the strip's width, at each end; the stations split where they meet the body.
    double capLen = 0.0;
    const bool caps = o.cap >= 0 && size_t(o.cap) < sheet.trims.size() && !o.closed;
    if (caps)
    {
        const glm::ivec2 px = sheet.trims[size_t(o.cap)].sizePx();
        capLen = stationWidth(st.front()) * double(px.x) / std::max(1, px.y);
        if (capLen * 2.0 >= open)
            capLen = 0.0;
    }
    if (capLen > 0.0)
    {
        for (double at : {capLen, open - capLen})
        {
            for (size_t j = 1; j < st.size(); ++j)
                if (s[j] > at + 1e-9)
                {
                    if (at - s[j - 1] > 1e-9)
                        st.insert(st.begin() + std::ptrdiff_t(j), stationLerp(st[j - 1], st[j], (at - s[j - 1]) / (s[j] - s[j - 1])));
                    break;
                }
            s = lengths();
        }
    }

    const Trim& body = sheet.trims[o.trim];
    const double inset = body.insetPx / sheet.height;
    const glm::dvec2 lo = sheet.uvMin(body), hi = sheet.uvMax(body);
    std::vector<double> vAt = o.v;
    if (vAt.size() != cols)
    {
        vAt.resize(cols);
        for (size_t c = 0; c < cols; ++c)
            vAt[c] = double(c) / double(cols - 1);
    }
    const double v0 = lo.y + inset, v1 = hi.y - inset;
    // A loop repeats a tiling trim a whole number of times so its ends meet.
    const double sheetCm = sheet.worldExtent().x;
    const double uScale = o.closed ? std::max(1.0, std::round(total / sheetCm)) / total : 1.0 / sheetCm;
    const double bodyStart = capLen, bodyEnd = open - capLen;
    auto uvAt = [&](StripPart part, double along, size_t c) -> glm::dvec2 {
        if (part != StripPart::Body)
        {
            const Trim& cap = sheet.trims[size_t(o.cap)];
            const glm::dvec2 clo = sheet.uvMin(cap), chi = sheet.uvMax(cap);
            const double t = part == StripPart::Head ? along / capLen : 1.0 - (along - bodyEnd) / capLen;
            return {clo.x + (chi.x - clo.x) * std::clamp(t, 0.0, 1.0), clo.y + (chi.y - clo.y) * vAt[c]};
        }
        const double v = v0 + (v1 - v0) * vAt[c];
        if (body.kind == uv::TrimKind::StripU)
            return {o.uOffset + (along - bodyStart) * uScale, v};
        const double t = (along - bodyStart) / std::max(1e-9, (o.closed ? total : bodyEnd) - bodyStart);
        return {lo.x + inset + (hi.x - lo.x - 2.0 * inset) * t, v};
    };
    auto partOf = [&](double mid) {
        if (capLen <= 0.0)
            return StripPart::Body;
        return mid < bodyStart ? StripPart::Head : (mid > bodyEnd ? StripPart::Tail : StripPart::Body);
    };

    const double depth = o.cutDepth;
    const double lift = depth > 0.0 ? 0.05 * depth + 0.01 : 0.0;
    std::vector<geo::Index> top(st.size() * cols), floor;
    for (size_t j = 0; j < st.size(); ++j)
        for (size_t c = 0; c < cols; ++c)
            top[j * cols + c] = m.addPoint(st[j].across[c] + st[j].normal * lift);
    if (depth > 0.0)
    {
        floor.resize(top.size());
        for (size_t j = 0; j < st.size(); ++j)
            for (size_t c = 0; c < cols; ++c)
                floor[j * cols + c] = m.addPoint(st[j].across[c] - st[j].normal * depth);
    }

    const size_t segments = o.closed ? st.size() : st.size() - 1;
    for (size_t j = 0; j < segments; ++j)
    {
        const size_t k = (j + 1) % st.size();
        const double sa = s[j], sb = k == 0 ? total : s[k];
        const StripPart part = partOf((sa + sb) * 0.5);
        const int trim = part == StripPart::Body ? int(o.trim) : o.cap;
        const glm::dvec3 n = stripUnit(st[j].normal + st[k].normal, st[j].normal);
        for (size_t c = 0; c + 1 < cols; ++c)
        {
            const std::vector<glm::dvec2> uvs = {uvAt(part, sa, c), uvAt(part, sb, c), uvAt(part, sb, c + 1), uvAt(part, sa, c + 1)};
            const auto at = [&](const std::vector<geo::Index>& layer) {
                return std::vector<geo::Index>{layer[j * cols + c], layer[k * cols + c], layer[k * cols + c + 1], layer[j * cols + c + 1]};
            };
            stripFace(m, at(top), uvs, n, trim);
            if (depth > 0.0)
                stripFace(m, at(floor), uvs, -n, trim);
        }
        if (depth > 0.0)
            // The slab's sides, mapped to the trim's edge bands: walls of the groove once it is cut.
            for (const size_t c : {size_t(0), cols - 1})
            {
                const size_t inner = c == 0 ? 1 : cols - 2;
                const glm::dvec3 out = stripUnit(st[j].across[c] - st[j].across[inner], n);
                const glm::dvec2 ua = uvAt(part, sa, c), ub = uvAt(part, sb, c);
                const double band = (v1 - v0) * std::min(0.25, depth / std::max(1e-9, stationWidth(st[j])));
                const double dv = c == 0 ? band : -band;
                stripFace(m, {top[j * cols + c], top[k * cols + c], floor[k * cols + c], floor[j * cols + c]},
                        {ua, ub, ub + glm::dvec2(0.0, dv), ua + glm::dvec2(0.0, dv)}, out, trim);
            }
    }
    if (depth > 0.0 && !o.closed)
        for (const size_t j : {size_t(0), st.size() - 1})
        {
            const glm::dvec3 out = stripUnit(j == 0 ? stationCentre(st[0]) - stationCentre(st[1]) : stationCentre(st[j]) - stationCentre(st[j - 1]), st[j].normal);
            std::vector<geo::Index> ring;
            std::vector<glm::dvec2> uvs;
            const StripPart part = j == 0 ? partOf(0.0) : partOf(open);
            for (size_t c = 0; c < cols; ++c)
            {
                ring.push_back(top[j * cols + c]);
                uvs.push_back(uvAt(part, s[j], c));
            }
            for (size_t c = cols; c-- > 0;)
            {
                ring.push_back(floor[j * cols + c]);
                uvs.push_back(uvAt(part, s[j], c));
            }
            stripFace(m, ring, uvs, out, part == StripPart::Body ? int(o.trim) : o.cap);
        }
    return m;
}

std::vector<Station> stationsAlong(std::span<const glm::dvec3> pts, bool closed, double width, StripOrient orient, double offset, glm::dvec3 up)
{
    std::vector<Station> out;
    const size_t n = pts.size();
    if (n < 2)
        return out;
    up = stripUnit(up, {0.0, 1.0, 0.0});
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 prev = i > 0 ? pts[i - 1] : (closed ? pts[n - 1] : pts[i]);
        const glm::dvec3 next = i + 1 < n ? pts[i + 1] : (closed ? pts[0] : pts[i]);
        const glm::dvec3 t = stripUnit(stripUnit(next - pts[i], {}) + stripUnit(pts[i] - prev, {}), {1.0, 0.0, 0.0});
        glm::dvec3 across, normal;
        if (orient == StripOrient::Flat)
        {
            across = stripUnit(glm::cross(t, up), stripUnit(glm::cross(t, {1.0, 0.0, 0.0}), {0.0, 0.0, 1.0}));
            normal = stripUnit(glm::cross(across, t), up);
        }
        else
        {
            across = stripUnit(up - t * glm::dot(up, t), {0.0, 1.0, 0.0});
            normal = stripUnit(glm::cross(t, across), {0.0, 0.0, 1.0});
        }
        Station s;
        s.normal = normal;
        const glm::dvec3 base = pts[i] + normal * offset;
        if (orient == StripOrient::Flat)
            s.across = {base - across * (width * 0.5), base + across * (width * 0.5)};
        else
            s.across = {base, base + across * width};
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<EdgeChain> edgeChains(const geo::Mesh& m, std::span<const geo::Index> edges)
{
    std::map<geo::Index, std::vector<geo::Index>> at;
    std::vector<geo::Index> valid;
    for (geo::Index e : edges)
        if (m.edgeValid(e))
        {
            valid.push_back(e);
            at[m.edgePoint(e, 0)].push_back(e);
            at[m.edgePoint(e, 1)].push_back(e);
        }
    std::map<geo::Index, bool> used;
    std::vector<EdgeChain> out;
    auto walk = [&](geo::Index start, geo::Index first) {
        EdgeChain chain;
        chain.points.push_back(start);
        geo::Index p = start, e = first;
        while (e != geo::kInvalid && !used[e])
        {
            used[e] = true;
            p = m.edgeOtherPoint(e, p);
            if (p == start)
            {
                chain.closed = true;
                break;
            }
            chain.points.push_back(p);
            e = geo::kInvalid;
            if (at[p].size() == 2)
                for (geo::Index next : at[p])
                    if (!used[next])
                        e = next;
        }
        out.push_back(std::move(chain));
    };
    // Open chains start at their ends (or branch points), then whatever is left are loops.
    for (const auto& [p, list] : at)
        if (list.size() != 2)
            for (geo::Index e : list)
                if (!used[e])
                    walk(p, e);
    for (geo::Index e : valid)
        if (!used[e])
            walk(m.edgePoint(e, 0), e);
    return out;
}

std::vector<Station> stationsOverEdges(const geo::Mesh& m, const EdgeChain& chain, double width, double offset)
{
    const size_t n = chain.points.size();
    std::vector<Station> out;
    if (n < 2)
        return out;
    struct Sides
    {
        glm::dvec3 dirL{0.0}, dirR{0.0}, nL{0.0}, nR{0.0};
    };
    std::vector<Sides> sides(n);
    const size_t segments = chain.closed ? n : n - 1;
    for (size_t k = 0; k < segments; ++k)
    {
        const geo::Index a = chain.points[k], b = chain.points[(k + 1) % n];
        const geo::Index e = m.findEdge(a, b);
        if (e == geo::kInvalid)
            continue;
        const glm::dvec3 t = stripUnit(m.position(b) - m.position(a), {1.0, 0.0, 0.0});
        Sides seg;
        m.forEachEdgeCorner(e, [&](geo::Index c) {
            const glm::dvec3 fn = m.faceNormal(m.cornerFace(c));
            // A face running a to b has the chain on its right, so it lies to the chain's left.
            if (m.cornerPoint(c) == a)
            {
                seg.nL = fn;
                seg.dirL = stripUnit(glm::cross(fn, t), {});
            }
            else
            {
                seg.nR = fn;
                seg.dirR = stripUnit(glm::cross(t, fn), {});
            }
        });
        for (const size_t i : {k, (k + 1) % n})
        {
            sides[i].dirL += seg.dirL;
            sides[i].dirR += seg.dirR;
            sides[i].nL += seg.nL;
            sides[i].nR += seg.nR;
        }
    }
    for (size_t i = 0; i < n; ++i)
    {
        Sides sd = sides[i];
        if (glm::length(sd.nL) < 1e-9)
        {
            sd.nL = sd.nR;
            sd.dirL = -sd.dirR;
        }
        if (glm::length(sd.nR) < 1e-9)
        {
            sd.nR = sd.nL;
            sd.dirR = -sd.dirL;
        }
        const glm::dvec3 nL = stripUnit(sd.nL, {0.0, 1.0, 0.0}), nR = stripUnit(sd.nR, nL);
        const glm::dvec3 p = m.position(chain.points[i]);
        const glm::dvec3 nMid = stripUnit(nL + nR, nL);
        Station s;
        s.normal = nMid;
        s.across = {p + stripUnit(sd.dirR, {}) * (width * 0.5) + nR * offset, p + nMid * offset, p + stripUnit(sd.dirL, {}) * (width * 0.5) + nL * offset};
        out.push_back(std::move(s));
    }
    return out;
}

int bestStripTrim(const Sheet& sheet, double width, std::string_view tag)
{
    int best = -1;
    double bestErr = 0.0;
    for (size_t i = 0; i < sheet.trims.size(); ++i)
    {
        const Trim& t = sheet.trims[i];
        const bool tagged = !tag.empty() && std::ranges::find(t.tags, tag) != t.tags.end();
        if ((!tag.empty() && !tagged) || (tag.empty() && (t.reserved || t.kind != uv::TrimKind::StripU)))
            continue;
        const double err = std::abs(std::log2(std::max(1e-6, sheet.worldSize(t).y) / std::max(1e-6, width)));
        if (best < 0 || err < bestErr)
        {
            best = int(i);
            bestErr = err;
        }
    }
    return best;
}

int capTrim(const Sheet& sheet)
{
    for (size_t i = 0; i < sheet.trims.size(); ++i)
        if (std::ranges::find(sheet.trims[i].tags, "cap") != sheet.trims[i].tags.end())
            return int(i);
    return -1;
}
} // namespace trims
