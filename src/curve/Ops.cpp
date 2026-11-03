#include "curve/Ops.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Fit.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::curve
{
namespace
{
double lengthBetween(const Piece& p, double t0, double t1)
{
    double total = 0.0;
    constexpr int kParts = 4;
    double h = (t1 - t0) / kParts;
    for (int k = 0; k < kParts; ++k)
    {
        double a = t0 + h * k, b = a + h;
        for (int g = 0; g < 8; ++g)
            total += kGw[g] * (b - a) * 0.5 * glm::length(evaluate(p, (a + b) * 0.5 + (b - a) * 0.5 * kGx[g]).d1);
    }
    return total;
}

/** Parameter at arc length len from the piece's start (fromEnd: from its end). */
double paramAtDistance(const Piece& p, double len, bool fromEnd)
{
    double total = lengthBetween(p, 0.0, 1.0);
    if (total < kEps)
        return fromEnd ? 1.0 : 0.0;
    double target = std::clamp(fromEnd ? total - len : len, 0.0, total);
    double t = target / total;
    for (int it = 0; it < 40; ++it)
    {
        double f = lengthBetween(p, 0.0, t) - target;
        double d = glm::length(evaluate(p, t).d1);
        if (std::abs(f) < 1e-10 || d < kEps)
            break;
        t = std::clamp(t - f / d, 0.0, 1.0);
    }
    return t;
}

D4 h(const D3& p, double w = 1.0) { return D4(p * w, w); }

/** Closest points of two lines; their midpoint, or nullopt when parallel. */
std::optional<D3> lineMeet(const D3& a, const D3& da, const D3& b, const D3& db)
{
    D3 r = a - b;
    double aa = glm::dot(da, da), bb = glm::dot(db, db), ab = glm::dot(da, db);
    double den = aa * bb - ab * ab;
    if (std::abs(den) < 1e-14)
        return std::nullopt;
    double s = (ab * glm::dot(db, r) - bb * glm::dot(da, r)) / den;
    double t = (aa * glm::dot(db, r) - ab * glm::dot(da, r)) / den;
    return ((a + da * s) + (b + db * t)) * 0.5;
}

/** Curvature vector (toward the centre, length = curvature) at the piece's start or end. */
D3 curvatureVector(const Piece& p, bool end)
{
    Sample s = evaluate(p, end ? 1.0 : 0.0);
    double l = glm::length(s.d1);
    if (l < kEps)
        return D3(0.0);
    D3 t = s.d1 / l;
    D3 n = s.d2 - t * glm::dot(s.d2, t);
    return n / (l * l);
}

/** Quintic G2 blend from A (tangent tA, curvature vector kA) to B (arriving tangent tB, curvature kB), sized by d. */
Piece g2Blend(const D3& A, const D3& tA, const D3& kA, const D3& B, const D3& tB, const D3& kB, double d)
{
    double a1 = 0.4 * d, a2 = 0.8 * d;
    // Curvature at a quintic's end is 4/5 |P1-P0 x P2-P1| / |P1-P0|^3; matching it fixes P2's offset off the tangent.
    D3 P2off = kA * (1.25 * a1 * a1);
    D3 P3off = kB * (1.25 * a1 * a1);
    Piece q;
    q.cv = {h(A), h(A + tA * a1), h(A + tA * a2 + P2off), h(B - tB * a2 + P3off), h(B - tB * a1), h(B)};
    return q;
}

double minRadius(const Piece& p)
{
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i <= 64; ++i)
    {
        double k = curvature(evaluate(p, double(i) / 64));
        if (k > kEps)
            best = std::min(best, 1.0 / k);
    }
    return best;
}

/** Point index a junction (span boundary) belongs to, for per-point radii and filtering. */
std::optional<size_t> junctionPoint(const Spline& s, size_t j)
{
    if (s.kind == Kind::Poly || s.kind == Kind::Bezier || (s.kind == Kind::Nurbs && s.degree == 1 && s.knots.empty()))
        return j < s.size() ? std::optional<size_t>(j) : std::nullopt;
    return std::nullopt;
}

std::vector<Piece> rotatePieces(std::vector<Piece> ps, size_t start)
{
    std::rotate(ps.begin(), ps.begin() + ptrdiff_t(start % std::max<size_t>(1, ps.size())), ps.end());
    return ps;
}

Kind sharedKind(const Spline& a, const Spline& b) { return a.kind == b.kind ? a.kind : Kind::Nurbs; }

/** Cubic from a to b leaving along ta and arriving along tb, handles a third of the gap. */
Piece blendCubic(const D3& a, const D3& ta, const D3& b, const D3& tb)
{
    double d = glm::distance(a, b) / 3.0;
    Piece p;
    p.cv = {h(a), h(a + ta * d), h(b - tb * d), h(b)};
    return p;
}
} // namespace

const char* cornerModeName(CornerMode m)
{
    switch (m)
    {
    case CornerMode::Arc: return "arc";
    case CornerMode::Chamfer: return "chamfer";
    case CornerMode::G2: return "g2";
    }
    return "arc";
}

bool parseCornerMode(std::string_view name, CornerMode* out)
{
    for (CornerMode m : {CornerMode::Arc, CornerMode::Chamfer, CornerMode::G2})
        if (name == cornerModeName(m))
        {
            *out = m;
            return true;
        }
    return false;
}

const char* joinTypeName(JoinType j)
{
    switch (j)
    {
    case JoinType::Round: return "round";
    case JoinType::Miter: return "miter";
    case JoinType::Square: return "square";
    }
    return "round";
}

bool parseJoinType(std::string_view name, JoinType* out)
{
    for (JoinType j : {JoinType::Round, JoinType::Miter, JoinType::Square})
        if (name == joinTypeName(j))
        {
            *out = j;
            return true;
        }
    return false;
}

namespace
{
/**
 * Gives a spline rebuilt by fromPieces the tilt and radius of the one it came from. Output piece i runs from
 * spans[i].first to spans[i].second on the input, in its pieces' index + t parameter, which runs past the end across a
 * closed input's wrap. A Bezier or poly result has one point per piece; a NURBS one has `degree` controls per piece.
 */
void carryPointValues(const Spline& from, const std::vector<std::pair<double, double>>& spans, Spline& to)
{
    const size_t D = to.kind == Kind::Nurbs ? size_t(std::max(1, to.degree)) : 1;
    if (spans.empty() || to.size() != spans.size() * D + (from.closed ? 0 : 1))
        return;
    auto valueAt = [&](const std::vector<double>& v, double def, double u) {
        const double n = double(from.size());
        if (from.closed && u >= n)
            u -= n;
        return pointValueAt(from, v, def, u);
    };
    auto carry = [&](const std::vector<double>& src, double def, std::vector<double>& dst) {
        if (src.size() != from.size())
            return;
        dst.clear();
        for (size_t c = 0; c < to.size(); ++c)
        {
            const size_t i = std::min(c / D, spans.size() - 1);
            const double f = c / D < spans.size() ? double(c % D) / double(D) : 1.0;
            dst.push_back(valueAt(src, def, spans[i].first + (spans[i].second - spans[i].first) * f));
        }
    };
    carry(from.tilt, 0.0, to.tilt);
    carry(from.radius, 1.0, to.radius);
}
} // namespace

Spline fillet(const Spline& s, const FilletOptions& o)
{
    LILA_ZONE();

    std::vector<Piece> ps = pieces(s);
    size_t K = ps.size();
    if (K < 2 && !(s.closed && K >= 1))
        return s;
    struct Corner
    {
        bool on = false;
        double d = 0.0; // setback along each neighbour
        double theta = 0.0;
        double radius = 0.0;
    };
    std::vector<Corner> corners(K);
    for (size_t j = 0; j < K; ++j)
    {
        if (!s.closed && j == 0)
            continue;
        const Piece& prev = ps[(j + K - 1) % K];
        const Piece& next = ps[j];
        D3 tin = endTangent(prev, true), tout = endTangent(next, false);
        double theta = angleBetween(tin, tout);
        if (theta < o.minAngle || theta > glm::pi<double>() - 1e-3)
            continue;
        std::optional<size_t> pt = junctionPoint(s, j);
        if (!o.only.empty() && (!pt || std::ranges::find(o.only, *pt) == o.only.end()))
            continue;
        double r = o.radius;
        if (o.perPoint && pt && *pt < s.cornerRadius.size() && s.cornerRadius[*pt] > 0.0)
            r = s.cornerRadius[*pt];
        if (r <= 0.0)
            continue;
        corners[j] = {true, r * std::tan(theta * 0.5), theta, r};
        if (o.mode == CornerMode::G2)
        {
            // The blend's shape scales with its setback: size a unit one, then scale to reach the radius.
            D3 A = -tin, B = tout;
            Piece unitBlend = g2Blend(A, tin, D3(0.0), B, tout, D3(0.0), 1.0);
            double rho = minRadius(unitBlend);
            if (rho > kEps && rho < 1e12)
                corners[j].d = r / rho;
        }
    }
    // Clamp setbacks to what each piece can give: all of it, or half when both its ends are cut.
    std::vector<double> lens(K);
    for (size_t k = 0; k < K; ++k)
        lens[k] = lengthBetween(ps[k], 0.0, 1.0);
    for (size_t j = 0; j < K; ++j)
    {
        if (!corners[j].on)
            continue;
        size_t pi = (j + K - 1) % K, ni = j;
        bool prevBoth = corners[pi].on;
        bool nextBoth = corners[(ni + 1) % K].on && (s.closed || ni + 1 < K);
        double maxPrev = lens[pi] * (prevBoth ? 0.5 : 1.0) * 0.999;
        double maxNext = lens[ni] * (nextBoth ? 0.5 : 1.0) * 0.999;
        corners[j].d = std::min({corners[j].d, maxPrev, maxNext});
    }
    // Trim range per piece.
    std::vector<double> t0(K, 0.0), t1(K, 1.0);
    for (size_t j = 0; j < K; ++j)
        if (corners[j].on)
        {
            size_t pi = (j + K - 1) % K;
            t1[pi] = paramAtDistance(ps[pi], corners[j].d, true);
            t0[j] = paramAtDistance(ps[j], corners[j].d, false);
        }
    auto cornerPiece = [&](size_t j) -> std::vector<Piece> {
        size_t pi = (j + K - 1) % K;
        Sample ea = evaluate(ps[pi], t1[pi]), eb = evaluate(ps[j], t0[j]);
        D3 A = ea.p, B = eb.p;
        D3 tA = unit(ea.d1), tB = unit(eb.d1);
        if (glm::length(tA) < 0.5)
            tA = endTangent(ps[pi], true);
        if (glm::length(tB) < 0.5)
            tB = endTangent(ps[j], false);
        switch (o.mode)
        {
        case CornerMode::Chamfer: return {linePiece(A, B)};
        case CornerMode::G2:
        {
            Piece pa = subPiece(ps[pi], t0[pi], t1[pi]), pb = subPiece(ps[j], t0[j], t1[j]);
            double d = corners[j].d;
            return {g2Blend(A, tA, curvatureVector(pa, true), B, tB, curvatureVector(pb, false), d)};
        }
        case CornerMode::Arc:
        default:
        {
            std::optional<D3> V = lineMeet(A, tA, B, tB);
            if (!V)
                return {linePiece(A, B)};
            double phi = angleBetween(tA, tB);
            Piece q;
            double w = std::cos(phi * 0.5);
            q.cv = {h(A), h(*V, w), h(B)};
            if (o.exactArcs)
                return {q};
            return approximateCubic(q, 1e-4 * std::max(1.0, corners[j].radius));
        }
        }
    };
    std::vector<Piece> out;
    // Where each output piece starts and ends on the input, so per-point values follow the trimmed path.
    std::vector<std::pair<double, double>> spans;
    for (size_t k = 0; k < K; ++k)
    {
        if (corners[k].on && (s.closed || k > 0))
        {
            std::vector<Piece> blend = cornerPiece(k);
            const double a = double((k + K - 1) % K) + t1[(k + K - 1) % K];
            const double b = double(k) + t0[k] + (k == 0 ? double(K) : 0.0);
            for (size_t i = 0; i < blend.size(); ++i)
            {
                out.push_back(std::move(blend[i]));
                spans.push_back({a + (b - a) * double(i) / double(blend.size()), a + (b - a) * double(i + 1) / double(blend.size())});
            }
        }
        if (t1[k] - t0[k] > 1e-9)
        {
            out.push_back(subPiece(ps[k], t0[k], t1[k]));
            spans.push_back({double(k) + t0[k], double(k) + t1[k]});
        }
    }
    Kind want = s.kind;
    Spline result = fromPieces(out, s.closed, want);
    if (want == Kind::Poly && result.kind != Kind::Poly)
        result = fromPieces(out, s.closed, Kind::Nurbs);
    carryPointValues(s, spans, result);
    return result;
}

namespace
{
/** Basis of the offset plane: origin, in-plane axes, normal. */
struct Plane
{
    D3 o{0.0}, x{1, 0, 0}, y{0, 0, -1}, n{0, 1, 0};

    glm::dvec2 to2(const D3& p) const { return {glm::dot(p - o, x), glm::dot(p - o, y)}; }
    D3 to3(const glm::dvec2& q) const { return o + x * q.x + y * q.y; }
};

Plane planeFor(const std::vector<D3>& pts, const D3& want)
{
    Plane pl;
    D3 n = glm::length(want) > kEps ? glm::normalize(want) : planeNormal(pts, 1e-3);
    if (glm::length(n) < 0.5)
        n = D3(0, 1, 0);
    pl.n = n;
    D3 ref = std::abs(n.x) < 0.9 ? D3(1, 0, 0) : D3(0, 0, 1);
    pl.x = glm::normalize(ref - n * glm::dot(ref, n));
    pl.y = glm::cross(n, pl.x);
    D3 c(0.0);
    for (const D3& p : pts)
        c += p;
    pl.o = pts.empty() ? c : c / double(pts.size());
    return pl;
}

using V2 = glm::dvec2;

double distToPolyline(const V2& p, const std::vector<V2>& pl, bool closed)
{
    double best = std::numeric_limits<double>::max();
    size_t n = pl.size();
    size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        V2 a = pl[i], b = pl[(i + 1) % n];
        V2 d = b - a;
        double l2 = glm::dot(d, d);
        double t = l2 > kEps ? std::clamp(glm::dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(p - (a + d * t)));
    }
    return best;
}

/** An offset path: its points and whether it closes. */
struct OffsetPath
{
    std::vector<V2> pts;
    bool closed = false;
};

/**
 * Offsets a 2D polyline d to the left of its direction of travel, with joins on the outside of bends. The raw offset is
 * split where it crosses itself, pieces closer than |d| to the source (loops and cusps) are dropped, and the rest is
 * chained back together end to end.
 */
std::vector<OffsetPath> offsetPoly(const std::vector<V2>& src, bool closed, double d, JoinType join, double miterLimit, double tol)
{
    std::vector<OffsetPath> result;
    size_t n = src.size();
    if (n < 2 || (closed && n < 3))
        return result;
    auto segNormal = [&](size_t i) {
        V2 t = src[(i + 1) % n] - src[i];
        double l = glm::length(t);
        return l > kEps ? V2(-t.y, t.x) / l : V2(0.0);
    };
    std::vector<V2> raw;
    auto joinAt = [&](size_t i, size_t prevSeg, size_t nextSeg) {
        V2 pn = segNormal(prevSeg), nn = segNormal(nextSeg);
        V2 prevEnd = src[i] + pn * d, a = src[i] + nn * d;
        double turn = cross2(pn, nn) * (d > 0 ? 1.0 : -1.0);
        if (turn > 1e-12 || glm::dot(pn, nn) > 1.0 - 1e-12)
        {
            // Inside of the bend: the two offset segments cross; the trim below sorts it out.
            raw.push_back(prevEnd);
            raw.push_back(a);
        }
        else if (join == JoinType::Round)
        {
            V2 sp = pn * (d > 0 ? 1.0 : -1.0), sn = nn * (d > 0 ? 1.0 : -1.0);
            double a0 = std::atan2(sp.y, sp.x), a1 = std::atan2(sn.y, sn.x);
            double sweepA = a1 - a0;
            while (sweepA > glm::pi<double>())
                sweepA -= glm::two_pi<double>();
            while (sweepA < -glm::pi<double>())
                sweepA += glm::two_pi<double>();
            double ad = std::abs(d);
            int steps = std::max(1, int(std::ceil(std::abs(sweepA) / std::max(2.0 * std::acos(std::clamp(1.0 - tol / ad, -1.0, 1.0)), 0.05))));
            for (int k = 0; k <= steps; ++k)
            {
                double ang = a0 + sweepA * k / steps;
                raw.push_back(src[i] + V2(std::cos(ang), std::sin(ang)) * ad);
            }
        }
        else
        {
            V2 bis = pn + nn;
            double bl = glm::length(bis);
            double cosHalf = bl * 0.5;
            if (join == JoinType::Miter && bl > kEps && 1.0 / cosHalf <= miterLimit)
                raw.push_back(src[i] + bis / bl * (d / cosHalf));
            else
            {
                // Square (or a miter past its limit): cut the corner |d| out along both runs.
                V2 dirPrev = glm::normalize(src[i] - src[(i + n - 1) % n]), dirNext = glm::normalize(src[(i + 1) % n] - src[i]);
                double ext = std::abs(d);
                raw.push_back(prevEnd + dirPrev * ext);
                raw.push_back(a - dirNext * ext);
            }
        }
    };
    if (closed)
        for (size_t i = 0; i < n; ++i)
            joinAt(i, (i + n - 1) % n, i);
    else
    {
        raw.push_back(src[0] + segNormal(0) * d);
        for (size_t i = 1; i + 1 < n; ++i)
            joinAt(i, i - 1, i);
        raw.push_back(src[n - 1] + segNormal(n - 2) * d);
    }
    size_t m = raw.size();
    size_t rawSegs = closed ? m : m - 1;
    struct Hit
    {
        size_t seg;
        double t;
        V2 p;
    };
    std::vector<Hit> hits;
    for (size_t i = 0; i < rawSegs; ++i)
        for (size_t j = i + 2; j < rawSegs; ++j)
        {
            if (closed && i == 0 && j == rawSegs - 1)
                continue;
            V2 p = raw[i], r = raw[(i + 1) % m] - raw[i], q = raw[j], sv = raw[(j + 1) % m] - raw[j];
            double den = cross2(r, sv);
            if (std::abs(den) < 1e-15)
                continue;
            double t = cross2(q - p, sv) / den, u = cross2(q - p, r) / den;
            if (t > 1e-9 && t < 1 - 1e-9 && u > 1e-9 && u < 1 - 1e-9)
            {
                hits.push_back({i, t, p + r * t});
                hits.push_back({j, u, q + sv * u});
            }
        }
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.seg != y.seg ? x.seg < y.seg : x.t < y.t; });
    std::vector<std::vector<V2>> pieces(1);
    size_t hi = 0;
    for (size_t i = 0; i < rawSegs; ++i)
    {
        pieces.back().push_back(raw[i]);
        while (hi < hits.size() && hits[hi].seg == i)
        {
            pieces.back().push_back(hits[hi].p);
            pieces.push_back({hits[hi].p});
            ++hi;
        }
    }
    pieces.back().push_back(raw[closed ? 0 : m - 1]);
    if (closed && pieces.size() > 1)
    {
        // The loop starts mid piece: the last piece runs on into the first.
        pieces.back().insert(pieces.back().end(), pieces.front().begin() + 1, pieces.front().end());
        pieces.erase(pieces.begin());
    }
    double need = std::abs(d) - std::max(tol, std::abs(d) * 1e-3);
    std::vector<std::vector<V2>> kept;
    for (const auto& pc : pieces)
    {
        if (pc.size() < 2)
            continue;
        // Kept when its middle (by length) stays the full distance from the source.
        double len = 0.0;
        for (size_t i = 1; i < pc.size(); ++i)
            len += glm::length(pc[i] - pc[i - 1]);
        V2 mid = pc[0];
        double acc = 0.0;
        for (size_t i = 1; i < pc.size(); ++i)
        {
            double l = glm::length(pc[i] - pc[i - 1]);
            if (acc + l >= len * 0.5)
            {
                mid = glm::mix(pc[i - 1], pc[i], l > kEps ? (len * 0.5 - acc) / l : 0.0);
                break;
            }
            acc += l;
        }
        if (distToPolyline(mid, src, closed) >= need)
            kept.push_back(pc);
    }
    if (closed && pieces.size() == 1 && kept.size() == 1)
    {
        kept[0].pop_back();
        result.push_back({kept[0], true});
        return result;
    }
    // Chain pieces whose ends meet.
    std::vector<uint8_t> used(kept.size(), 0);
    for (size_t s0 = 0; s0 < kept.size(); ++s0)
    {
        if (used[s0])
            continue;
        used[s0] = 1;
        std::vector<V2> chain = kept[s0];
        bool loop = false;
        for (bool grew = true; grew;)
        {
            grew = false;
            if (chain.size() > 3 && glm::length(chain.back() - chain.front()) < 1e-7)
            {
                loop = true;
                break;
            }
            for (size_t k = 0; k < kept.size(); ++k)
                if (!used[k] && glm::length(kept[k].front() - chain.back()) < 1e-7)
                {
                    chain.insert(chain.end(), kept[k].begin() + 1, kept[k].end());
                    used[k] = 1;
                    grew = true;
                    break;
                }
        }
        if (loop)
            chain.pop_back();
        if (chain.size() >= 2)
            result.push_back({chain, loop});
    }
    return result;
}

Spline refit(const std::vector<D3>& pts, bool closed, Kind kind, double tol)
{
    if (kind == Kind::Poly)
    {
        Spline s;
        s.kind = Kind::Poly;
        s.closed = closed;
        for (size_t i : simplifyIndices(pts, tol * 0.1, closed))
            s.points.push_back(pts[i]);
        return s;
    }
    Spline b = fitBezier(pts, closed, tol, 0.6);
    return kind == Kind::Nurbs ? toNurbs(b) : b;
}
} // namespace

Splines offset(const Spline& s, const OffsetOptions& o)
{
    LILA_ZONE();

    Splines out;
    TessOptions to;
    to.chordError = std::max(o.tolerance * 0.25, 1e-4);
    to.maxAngle = 0.05;
    Polyline pl = tessellate(s, to);
    if (pl.size() < 2)
        return out;
    Plane plane = planeFor(pl.points, o.normal);
    std::vector<V2> pts;
    for (const D3& p : pl.points)
        pts.push_back(plane.to2(p));
    double height = 0.0;
    for (const D3& p : pl.points)
        height += glm::dot(p - plane.o, plane.n);
    height /= double(pl.size());
    auto back = [&](const std::vector<V2>& path) {
        std::vector<D3> r;
        for (const V2& q : path)
            r.push_back(plane.to3(q) + plane.n * height);
        return r;
    };
    if (s.closed)
    {
        // Counter-clockwise about the plane normal grows with a positive distance.
        double area = 0.0;
        for (size_t i = 0; i < pts.size(); ++i)
            area += cross2(pts[i], pts[(i + 1) % pts.size()]);
        // Left of travel is inside a counter-clockwise loop.
        double left = area >= 0.0 ? -o.distance : o.distance;
        for (const OffsetPath& r : offsetPoly(pts, true, left, o.join, o.miterLimit, o.tolerance))
            if (r.pts.size() >= 3)
                out.push_back(refit(back(r.pts), r.closed, s.kind, o.tolerance));
        return out;
    }
    for (const OffsetPath& r : offsetPoly(pts, false, o.distance, o.join, o.miterLimit, o.tolerance))
        out.push_back(refit(back(r.pts), r.closed, s.kind, o.tolerance));
    return out;
}

Splines project(const Spline& s, const geo::Mesh& mesh, const geo::MeshBvh& bvh, const ProjectOptions& o)
{
    LILA_ZONE();

    Splines out;
    TessOptions to;
    to.chordError = std::max(o.tolerance * 0.25, 1e-4);
    to.maxAngle = 0.05;
    Polyline pl = tessellate(s, to);
    if (pl.size() < 2 || bvh.empty())
        return out;
    // Dense enough to follow the surface between the curve's own points.
    std::vector<D3> samples;
    geo::Bounds b = bvh.bounds();
    double size = glm::length(b.size());
    double step = std::max(size / 400.0, o.tolerance * 4.0);
    size_t segs = pl.closed ? pl.size() : pl.size() - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        D3 a = pl.points[i], c = pl.points[(i + 1) % pl.size()];
        int n = std::max(1, int(std::ceil(glm::distance(a, c) / step)));
        for (int k = 0; k < n; ++k)
            samples.push_back(glm::mix(a, c, double(k) / n));
    }
    if (!pl.closed)
        samples.push_back(pl.points.back());
    D3 dir = glm::length(o.direction) > kEps ? glm::normalize(o.direction) : D3(0.0);
    std::vector<std::optional<D3>> hit(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const D3& p = samples[i];
        geo::Index face = geo::kInvalid;
        D3 at;
        if (glm::length(dir) > 0.5)
        {
            geo::BvhHit hh;
            if (bvh.raycast(p - dir * (size * 4.0 + glm::length(p - b.center())), dir, hh))
            {
                at = hh.point;
                face = hh.face;
            }
        }
        else
        {
            geo::BvhClosest c;
            if (bvh.closestPoint(p, c))
            {
                at = c.point;
                face = c.face;
            }
        }
        if (face == geo::kInvalid)
            continue;
        if (o.offset != 0.0 && mesh.faceValid(face))
            at += mesh.faceNormal(face) * o.offset;
        hit[i] = at;
    }
    std::vector<std::vector<D3>> runs(1);
    for (size_t i = 0; i < hit.size(); ++i)
    {
        if (hit[i])
            runs.back().push_back(*hit[i]);
        else if (!runs.back().empty())
            runs.emplace_back();
    }
    bool whole = runs.size() == 1 && runs[0].size() == samples.size();
    if (pl.closed && !whole && runs.size() > 1 && hit.front() && hit.back())
    {
        // The break falls inside a loop: the last run carries on into the first.
        runs.front().insert(runs.front().begin(), runs.back().begin(), runs.back().end());
        runs.pop_back();
    }
    for (const auto& r : runs)
        if (r.size() >= 2)
            out.push_back(refit(r, pl.closed && whole, s.kind, o.tolerance));
    return out;
}

Spline attach(const Spline& a0, const Spline& b0, bool blend, AttachEnds ends, double tolerance)
{
    Spline a = a0, b = b0;
    if (ends == AttachEnds::Nearest && !a.closed && !b.closed && a.size() && b.size())
    {
        std::vector<Piece> pa = pieces(a), pb = pieces(b);
        if (!pa.empty() && !pb.empty())
        {
            D3 as = pa.front().start(), ae = pa.back().end(), bs = pb.front().start(), be = pb.back().end();
            double d[4] = {glm::distance(ae, bs), glm::distance(ae, be), glm::distance(as, bs), glm::distance(as, be)};
            int best = int(std::min_element(d, d + 4) - d);
            if (best == 1)
                reverse(b);
            else if (best == 2)
                reverse(a);
            else if (best == 3)
            {
                reverse(a);
                reverse(b);
            }
        }
    }
    std::vector<Piece> pa = pieces(a), pb = pieces(b);
    if (pa.empty())
        return b;
    if (pb.empty())
        return a;
    Kind kind = sharedKind(a, b);
    // Poly and Bezier keep their points' values, joined in order.
    D3 e = pa.back().end(), st = pb.front().start();
    std::vector<Piece> all = pa;
    bool merge = glm::distance(e, st) <= tolerance;
    if (merge)
    {
        D3 mid = (e + st) * 0.5;
        Piece& last = all.back();
        last.cv.back() = h(mid) * last.cv.back().w;
        pb.front().cv.front() = h(mid) * pb.front().cv.front().w;
    }
    else if (blend)
        all.push_back(blendCubic(e, endTangent(pa.back(), true), st, endTangent(pb.front(), false)));
    else
        all.push_back(linePiece(e, st));
    all.insert(all.end(), pb.begin(), pb.end());
    if (kind == Kind::Poly && blend && !merge)
        kind = Kind::Bezier;
    Spline out = fromPieces(all, false, kind);
    if (kind == Kind::Bezier && out.kind == Kind::Bezier)
    {
        // Carry the anchors' own handle types across where the anchors survive unchanged.
        for (size_t i = 0; i < a.size() && i < out.size() && a.kind == Kind::Bezier; ++i)
        {
            out.typeL[i] = a.typeL[i];
            out.typeR[i] = a.typeR[i];
        }
    }
    auto concat = [&](const std::vector<double>& va, const std::vector<double>& vb, double def, std::vector<double>& dst) {
        if ((va.empty() && vb.empty()) || out.kind == Kind::Nurbs)
            return;
        std::vector<double> x = va.empty() ? std::vector<double>(a.size(), def) : va;
        std::vector<double> y = vb.empty() ? std::vector<double>(b.size(), def) : vb;
        if (merge && !y.empty())
            y.erase(y.begin());
        x.insert(x.end(), y.begin(), y.end());
        if (x.size() == out.size())
            dst = x;
    };
    if (a.kind == b.kind && a.kind != Kind::Nurbs)
    {
        concat(a.tilt, b.tilt, 0.0, out.tilt);
        concat(a.radius, b.radius, 1.0, out.radius);
        concat(a.cornerRadius, b.cornerRadius, 0.0, out.cornerRadius);
    }
    return out;
}

Splines detach(const Spline& s, double u)
{
    Splines out;
    std::vector<Piece> ps = pieces(s);
    size_t K = ps.size();
    if (K == 0)
        return {s};
    u = std::clamp(u, 0.0, double(K));
    size_t k = std::min(size_t(std::floor(u)), K - 1);
    double t = u - double(k);
    if (t >= 1.0 - 1e-9 && k + 1 < K)
    {
        ++k;
        t = 0.0;
    }
    Piece left, right;
    std::vector<Piece> a, b;
    if (t > 1e-9 && t < 1.0 - 1e-9)
        split(ps[k], t, left, right);
    for (size_t i = 0; i < k; ++i)
        a.push_back(ps[i]);
    bool cut = t > 1e-9 && t < 1.0 - 1e-9;
    if (cut)
        a.push_back(left);
    else if (t >= 1.0 - 1e-9)
        a.push_back(ps[k]);
    if (cut)
        b.push_back(right);
    else if (t <= 1e-9)
        b.push_back(ps[k]);
    for (size_t i = k + 1; i < K; ++i)
        b.push_back(ps[i]);
    if (s.closed)
    {
        std::vector<Piece> loop = b;
        loop.insert(loop.end(), a.begin(), a.end());
        out.push_back(fromPieces(loop, false, s.kind));
        return out;
    }
    if (!a.empty())
        out.push_back(fromPieces(a, false, s.kind));
    if (!b.empty())
        out.push_back(fromPieces(b, false, s.kind));
    return out;
}

Spline closeCurve(const Spline& s, bool blend)
{
    if (s.closed || s.size() < 2)
        return s;
    Spline out = s;
    std::vector<Piece> ps = pieces(s);
    if (ps.empty())
        return s;
    D3 a = ps.front().start(), b = ps.back().end();
    bool meet = glm::distance(a, b) < 1e-6 * std::max(1.0, glm::length(a));
    if (s.kind == Kind::Poly || s.kind == Kind::Bezier)
    {
        if (meet && out.size() > 2)
        {
            if (out.kind == Kind::Bezier)
                out.handleL[0] = out.handleL.back();
            out.erasePoint(out.size() - 1);
        }
        out.closed = true;
        if (out.kind == Kind::Bezier && !meet)
        {
            if (blend)
            {
                out.typeR.back() = out.typeR.back() == HandleType::Vector ? HandleType::Auto : out.typeR.back();
                out.typeL.front() = out.typeL.front() == HandleType::Vector ? HandleType::Auto : out.typeL.front();
                // Aligned end handles keep the curve's direction through the new span.
                D3 tb = endTangent(ps.back(), true), ta = endTangent(ps.front(), false);
                double d = glm::distance(a, b) / 3.0;
                out.handleR.back() = b + tb * d;
                out.handleL.front() = a - ta * d;
                out.typeR.back() = HandleType::Aligned;
                out.typeL.front() = HandleType::Aligned;
            }
            else
            {
                out.typeR.back() = HandleType::Vector;
                out.typeL.front() = HandleType::Vector;
            }
            updateHandles(out);
        }
        out.knots.clear();
        return out;
    }
    if (meet)
    {
        D3 mid = (a + b) * 0.5;
        ps.front().cv.front() = h(mid) * ps.front().cv.front().w;
        ps.back().cv.back() = h(mid) * ps.back().cv.back().w;
    }
    else
        ps.push_back(blend ? blendCubic(b, endTangent(ps.back(), true), a, endTangent(ps.front(), false)) : linePiece(b, a));
    return fromPieces(ps, true, Kind::Nurbs);
}

Spline openCurve(const Spline& s, double u)
{
    if (!s.closed)
        return s;
    if ((s.kind == Kind::Poly || s.kind == Kind::Bezier) && std::abs(u - std::round(u)) < 1e-9)
    {
        Spline out = s;
        size_t n = s.size();
        size_t k = size_t(std::llround(u)) % std::max<size_t>(1, n);
        auto rot = [&](auto& v) {
            if (v.size() == n)
                std::rotate(v.begin(), v.begin() + ptrdiff_t(k), v.end());
        };
        rot(out.points);
        rot(out.handleL);
        rot(out.handleR);
        rot(out.typeL);
        rot(out.typeR);
        rot(out.tilt);
        rot(out.radius);
        rot(out.cornerRadius);
        // The start point repeats at the end, so the closing span stays.
        auto dup = [&](auto& v) {
            if (v.size() == n)
                v.push_back(v.front());
        };
        dup(out.points);
        dup(out.handleL);
        dup(out.handleR);
        dup(out.typeL);
        dup(out.typeR);
        dup(out.tilt);
        dup(out.radius);
        dup(out.cornerRadius);
        out.closed = false;
        return out;
    }
    Splines parts = detach(s, u);
    return parts.empty() ? s : parts.front();
}

size_t insertPointAt(Spline& s, double u)
{
    int spans = spanCount(s);
    if (spans == 0)
        return 0;
    u = std::clamp(u, 0.0, double(spans));
    size_t k = std::min(size_t(std::floor(u)), size_t(spans - 1));
    double t = u - double(k);
    size_t n = s.size();
    switch (s.kind)
    {
    case Kind::Poly:
    {
        D3 p = pointAt(s, u);
        s.insertPoint(k + 1, p);
        return k + 1;
    }
    case Kind::Bezier:
    {
        Piece pc;
        size_t b = (k + 1) % n;
        pc.cv = {h(s.points[k]), h(s.handleR[k]), h(s.handleL[b]), h(s.points[b])};
        Piece l, r;
        split(pc, std::clamp(t, 1e-6, 1.0 - 1e-6), l, r);
        s.insertPoint(k + 1, l.end());
        size_t at = k + 1;
        size_t nb = (at + 1) % s.size();
        s.handleR[k] = l.point(1);
        s.handleL[at] = l.point(2);
        s.handleR[at] = r.point(1);
        s.handleL[nb] = r.point(2);
        s.typeL[at] = s.typeR[at] = HandleType::Aligned;
        if (s.typeR[k] == HandleType::Auto || s.typeR[k] == HandleType::Vector)
            s.typeR[k] = HandleType::Free;
        if (s.typeL[nb] == HandleType::Auto || s.typeL[nb] == HandleType::Vector)
            s.typeL[nb] = HandleType::Free;
        return at;
    }
    case Kind::Nurbs:
    {
        insertKnot(s, u, 1);
        D3 p = pointAt(s, u);
        size_t best = 0;
        double bd = std::numeric_limits<double>::max();
        for (size_t i = 0; i < s.size(); ++i)
            if (double d = glm::distance(s.points[i], p); d < bd)
            {
                bd = d;
                best = i;
            }
        return best;
    }
    }
    return 0;
}

void moveEnds(Spline& s, const std::optional<glm::dvec3>& start, const std::optional<glm::dvec3>& end)
{
    if (s.closed || s.size() < 2)
        return;
    auto move = [&](size_t i, const D3& to) {
        D3 d = to - s.points[i];
        s.points[i] = to;
        if (s.kind == Kind::Bezier)
        {
            s.handleL[i] += d;
            s.handleR[i] += d;
        }
    };
    if (start)
        move(0, *start);
    if (end)
        move(s.size() - 1, *end);
    if (s.kind == Kind::Bezier)
        updateHandles(s);
}
} // namespace lila::curve
