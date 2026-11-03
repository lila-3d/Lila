#include "curve/CurveBoolean.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Fit.h"

#ifdef _MSC_VER
// The optimizer reports Clipper2's unreachable code after any warning scope around the include ends.
#pragma warning(disable : 4702)
#endif
#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::curve
{
namespace
{
namespace c2 = Clipper2Lib;

/** Where a clip vertex lies on the source curves: curve index and span parameter. */
struct SourceRef
{
    int curve = -1;
    double u = 0.0;
};

/** The source curves of a clip and, per clip vertex id (the clipper z), where that vertex lies on them. */
struct Sources
{
    std::vector<const Spline*> curves;
    std::vector<int> spans;
    std::vector<std::vector<SourceRef>> refs; // by z - 1
    PlaneFrame plane;

    int64_t add(std::vector<SourceRef> r)
    {
        refs.push_back(std::move(r));
        return int64_t(refs.size());
    }
    const std::vector<SourceRef>* at(int64_t z) const { return z >= 1 && size_t(z) <= refs.size() ? &refs[size_t(z) - 1] : nullptr; }
    double period(int c) const { return double(spans[size_t(c)]); }
    D2 point(int c, double u) const { return plane.to2(pointAt(*curves[size_t(c)], wrap(c, u))); }
    double wrap(int c, double u) const
    {
        double P = period(c);
        if (P <= 0.0)
            return u;
        u = std::fmod(u, P);
        return u < 0.0 ? u + P : u;
    }
    /** Shortest signed parameter step from a to b on a closed curve. */
    double step(int c, double a, double b) const
    {
        double P = period(c), d = b - a;
        if (P <= 0.0)
            return d;
        d = std::fmod(d, P);
        if (d > P * 0.5)
            d -= P;
        else if (d < -P * 0.5)
            d += P;
        return d;
    }
};

int precisionFor(double extent)
{
    int p = int(std::floor(12.0 - std::log10(std::max(extent, 1e-6))));
    return std::clamp(p, 2, 8);
}

/** Refs for a point part way along a clip edge: the curves both ends lie on, the parameter interpolated. */
std::vector<SourceRef> refsOnEdge(const Sources& src, const c2::PointD& a, const c2::PointD& b, const c2::PointD& pt)
{
    std::vector<SourceRef> out;
    const auto* ra = src.at(a.z);
    const auto* rb = src.at(b.z);
    if (!ra || !rb)
        return out;
    D2 pa(a.x, a.y), pb(b.x, b.y), pp(pt.x, pt.y);
    D2 d = pb - pa;
    double l2 = glm::dot(d, d);
    double t = l2 > 0.0 ? std::clamp(glm::dot(pp - pa, d) / l2, 0.0, 1.0) : 0.0;
    for (const SourceRef& x : *ra)
        for (const SourceRef& y : *rb)
            if (x.curve == y.curve)
                out.push_back({x.curve, src.wrap(x.curve, x.u + src.step(x.curve, x.u, y.u) * t)});
    return out;
}

c2::PathsD toPaths(const Loops2& loops, Sources& src, int curveBase)
{
    c2::PathsD out;
    for (const Loop2& l : loops)
    {
        c2::PathD p;
        for (size_t i = 0; i < l.points.size(); ++i)
            p.push_back(c2::PointD(l.points[i].x, l.points[i].y, src.add({{curveBase + l.source, l.params[i]}})));
        out.push_back(std::move(p));
    }
    return out;
}

/** Runs a clip with the z callback keeping every new vertex traced back to the source curves. */
c2::PathsD clip(Sources& src, c2::ClipType type, c2::FillRule fill, const c2::PathsD& subject, const c2::PathsD& clipPaths, int precision)
{
    c2::ClipperD c(precision);
    c.SetZCallback([&src](const c2::PointD& e1b, const c2::PointD& e1t, const c2::PointD& e2b, const c2::PointD& e2t, c2::PointD& pt) {
        std::vector<SourceRef> r = refsOnEdge(src, e1b, e1t, pt);
        for (const SourceRef& x : refsOnEdge(src, e2b, e2t, pt))
            r.push_back(x);
        pt.z = r.empty() ? 0 : src.add(std::move(r));
    });
    c.AddSubject(subject);
    if (!clipPaths.empty())
        c.AddClip(clipPaths);
    c2::PathsD out;
    c.Execute(type, fill, out);
    return out;
}

void reversePiece(Piece& p) { std::reverse(p.cv.begin(), p.cv.end()); }

/** Refines where curves a and b cross near (ua, ub) by Newton steps in the plane. False leaves them. */
bool refineCrossing(const Sources& src, int a, double& ua, int b, double& ub)
{
    double xa = ua, xb = ub;
    double scale = 1.0;
    for (int it = 0; it < 12; ++it)
    {
        Sample sa = evaluate(*src.curves[size_t(a)], src.wrap(a, xa));
        Sample sb = evaluate(*src.curves[size_t(b)], src.wrap(b, xb));
        D2 f = src.plane.to2(sa.p) - src.plane.to2(sb.p);
        D2 da(glm::dot(sa.d1, src.plane.x), glm::dot(sa.d1, src.plane.y));
        D2 db(glm::dot(sb.d1, src.plane.x), glm::dot(sb.d1, src.plane.y));
        if (it == 0)
            scale = std::max({glm::length(da), glm::length(db), 1e-9});
        if (glm::length(f) < 1e-11 * scale)
        {
            if (std::abs(src.step(a, ua, xa)) > 0.5 || std::abs(src.step(b, ub, xb)) > 0.5)
                return false;
            ua = src.wrap(a, xa);
            ub = src.wrap(b, xb);
            return true;
        }
        // Solve da * s - db * t = -f.
        double det = da.x * -db.y - (-db.x) * da.y;
        if (std::abs(det) < 1e-14 * scale * scale)
            return false;
        double s = (-f.x * -db.y - (-db.x) * -f.y) / det;
        double t = (da.x * -f.y - -f.x * da.y) / det;
        xa += std::clamp(s, -0.25, 0.25);
        xb += std::clamp(t, -0.25, 0.25);
    }
    return false;
}

/** A run of a result loop lying along one source curve. */
struct Run
{
    int curve = -1; // -1: a straight segment
    double u0 = 0.0, delta = 0.0;
    D2 a{0.0}, b{0.0}; // straight runs
};

/** Builds the exact curve for a result loop from the clip vertices' source refs. */
Spline rebuildLoop(const Sources& src, const c2::PathD& path, Kind preferred, double tol)
{
    size_t m = path.size();
    std::vector<D2> v(m);
    for (size_t i = 0; i < m; ++i)
        v[i] = D2(path[i].x, path[i].y);

    // Per edge: which curve it runs along, from where, and how far.
    std::vector<Run> edges(m);
    for (size_t i = 0; i < m; ++i)
    {
        const auto* ra = src.at(path[i].z);
        const auto* rb = src.at(path[(i + 1) % m].z);
        D2 mid = (v[i] + v[(i + 1) % m]) * 0.5;
        double edgeLen = glm::length(v[(i + 1) % m] - v[i]);
        double best = std::max(tol * 4.0, edgeLen * 0.05) + 1e-9;
        Run e;
        e.a = v[i];
        e.b = v[(i + 1) % m];
        if (ra && rb)
            for (const SourceRef& x : *ra)
                for (const SourceRef& y : *rb)
                {
                    if (x.curve != y.curve)
                        continue;
                    double P = src.period(x.curve);
                    double fwd = std::fmod(y.u - x.u, P);
                    if (fwd < 0.0)
                        fwd += P;
                    for (double d : {fwd, fwd - P})
                    {
                        double dist = glm::length(src.point(x.curve, x.u + d * 0.5) - mid);
                        if (dist < best)
                        {
                            best = dist;
                            e.curve = x.curve;
                            e.u0 = x.u;
                            e.delta = d;
                        }
                    }
                }
        edges[i] = e;
    }

    auto sameRun = [](const Run& x, const Run& y) {
        return x.curve >= 0 && x.curve == y.curve && (x.delta == 0.0 || y.delta == 0.0 || (x.delta > 0.0) == (y.delta > 0.0));
    };
    size_t start = 0;
    while (start < m && sameRun(edges[(start + m - 1) % m], edges[start]))
        ++start;
    if (start == m)
    {
        // The whole loop is one source curve.
        double total = 0.0;
        for (const Run& e : edges)
            total += e.delta;
        Spline s = *src.curves[size_t(edges[0].curve)];
        if (total < 0.0)
            reverse(s);
        return s;
    }
    std::vector<Run> runs;
    for (size_t k = 0; k < m; ++k)
    {
        const Run& e = edges[(start + k) % m];
        if (!runs.empty() && sameRun(runs.back(), e))
        {
            runs.back().delta += e.delta;
            runs.back().b = e.b;
        }
        else
            runs.push_back(e);
    }
    // Where two curves meet, find the exact crossing so the pieces join without a step.
    for (size_t k = 0; k < runs.size(); ++k)
    {
        Run& r = runs[k];
        Run& n = runs[(k + 1) % runs.size()];
        if (r.curve < 0 || n.curve < 0 || r.curve == n.curve)
            continue;
        double ua = r.u0 + r.delta, ub = n.u0;
        if (refineCrossing(src, r.curve, ua, n.curve, ub))
        {
            r.delta += src.step(r.curve, r.u0 + r.delta, ua);
            double moved = src.step(n.curve, n.u0, ub);
            n.u0 = ub;
            n.delta -= moved;
        }
    }
    // Ends within clip precision of an anchor are that anchor, so corners on another curve's edge stay exact.
    auto snap = [](double u) { return std::abs(u - std::round(u)) < 1e-7 ? std::round(u) : u; };
    for (Run& r : runs)
        if (r.curve >= 0)
        {
            double end = snap(r.u0 + r.delta);
            r.u0 = snap(r.u0);
            r.delta = end - r.u0;
        }
    std::vector<Piece> ps;
    for (const Run& r : runs)
    {
        if (r.curve < 0)
        {
            ps.push_back(linePiece(src.plane.to3(r.a), src.plane.to3(r.b)));
            continue;
        }
        for (Piece& p : subPieces(*src.curves[size_t(r.curve)], r.u0, r.delta))
            ps.push_back(std::move(p));
    }
    // Drop slivers and make every piece start exactly where the last ended.
    std::vector<Piece> clean;
    double tiny = std::max(tol * 1e-3, 1e-9);
    for (Piece& p : ps)
    {
        double hull = 0.0;
        for (size_t i = 1; i < p.cv.size(); ++i)
            hull += glm::length(p.point(i) - p.point(i - 1));
        if (hull > tiny)
            clean.push_back(std::move(p));
    }
    for (size_t k = 0; k < clean.size(); ++k)
    {
        D3 end = clean[(k + clean.size() - 1) % clean.size()].end();
        D4& c0 = clean[k].cv.front();
        c0 = D4(end * c0.w, c0.w);
    }
    if (clean.size() < 2)
        return {};
    Spline s = fromPieces(clean, true, preferred);
    vectorHandlesOnLines(s);
    return s;
}

Kind preferredKind(const std::vector<Splines>& operands)
{
    bool allPoly = true, anyNurbs = false;
    for (const Splines& op : operands)
        for (const Spline& s : op)
        {
            allPoly = allPoly && s.kind == Kind::Poly;
            anyNurbs = anyNurbs || s.kind == Kind::Nurbs;
        }
    if (allPoly)
        return Kind::Poly;
    if (!operands.empty() && !operands[0].empty())
        return operands[0][0].kind == Kind::Poly ? Kind::Bezier : operands[0][0].kind;
    return anyNurbs ? Kind::Nurbs : Kind::Bezier;
}
} // namespace

const char* booleanOpName(BooleanOp op)
{
    switch (op)
    {
    case BooleanOp::Union: return "union";
    case BooleanOp::Subtract: return "subtract";
    case BooleanOp::Intersect: return "intersect";
    case BooleanOp::Xor: return "xor";
    }
    return "union";
}

bool parseBooleanOp(std::string_view name, BooleanOp* out)
{
    for (BooleanOp op : {BooleanOp::Union, BooleanOp::Subtract, BooleanOp::Intersect, BooleanOp::Xor})
        if (name == booleanOpName(op))
        {
            *out = op;
            return true;
        }
    if (name == "difference")
    {
        *out = BooleanOp::Subtract;
        return true;
    }
    return false;
}

std::vector<Piece> subPieces(const Spline& s, double u0, double delta)
{
    std::vector<Piece> out;
    int P = spanCount(s);
    if (P == 0 || std::abs(delta) < 1e-12)
        return out;
    std::vector<Piece> all = pieces(s);
    if (delta < 0.0)
    {
        out = subPieces(s, u0 + delta, -delta);
        std::reverse(out.begin(), out.end());
        for (Piece& p : out)
            reversePiece(p);
        return out;
    }
    auto wrapU = [&](double u) {
        if (!s.closed)
            return std::clamp(u, 0.0, double(P));
        u = std::fmod(u, double(P));
        return u < 0.0 ? u + P : u;
    };
    double u = wrapU(u0), left = delta;
    for (int guard = 0; left > 1e-12 && guard < 4 * P + 8; ++guard)
    {
        int span = std::min(int(std::floor(u)), P - 1);
        double t0 = u - span;
        if (t0 >= 1.0 - 1e-12)
        {
            u = wrapU(span + 1.0);
            if (!s.closed && span + 1 >= P)
                break;
            continue;
        }
        double t1 = std::min(1.0, t0 + left);
        if (t1 - t0 > 1e-12)
            out.push_back(subPiece(all[size_t(span)], t0, t1));
        left -= t1 - t0;
        if (!s.closed && span + 1 >= P && t1 >= 1.0)
            break;
        u = wrapU(span + t1);
    }
    return out;
}

CurveBooleanResult curveBoolean(const std::vector<Splines>& operands, const CurveBooleanOptions& o)
{
    LILA_ZONE();

    CurveBooleanResult res;
    Splines all;
    for (const Splines& op : operands)
        all.insert(all.end(), op.begin(), op.end());
    if (operands.empty() || all.empty())
    {
        res.warnings.push_back("no curves to combine");
        return res;
    }
    double deviation = 0.0;
    res.plane = fitPlane(all, o.normal, &deviation);
    if (deviation > o.planarTolerance)
        res.warnings.push_back("the curves are not planar (up to " + std::to_string(deviation) + " cm off their plane); the result follows their plane");

    TessOptions tess;
    tess.chordError = std::max(o.tolerance, 1e-5);
    tess.maxAngle = 0.05;
    Sources src;
    src.plane = res.plane;
    double extent = 0.0;
    std::vector<c2::PathsD> regions;
    for (const Splines& op : operands)
    {
        int base = int(src.curves.size());
        for (const Spline& s : op)
        {
            src.curves.push_back(&s);
            src.spans.push_back(spanCount(s));
        }
        Loops2 loops = loopsOf(op, res.plane, tess, &res.warnings);
        for (const Loop2& l : loops)
            for (const D2& p : l.points)
                extent = std::max({extent, std::abs(p.x), std::abs(p.y)});
        regions.push_back(toPaths(loops, src, base));
    }
    int precision = precisionFor(extent);
    // Each operand fills on its own (even-odd: a letter's counter is a hole whatever its direction).
    for (c2::PathsD& r : regions)
        r = clip(src, c2::ClipType::Union, o.nonZero ? c2::FillRule::NonZero : c2::FillRule::EvenOdd, r, {}, precision);

    c2::PathsD result = regions[0];
    switch (o.op)
    {
    case BooleanOp::Union:
    {
        c2::PathsD rest;
        for (size_t k = 1; k < regions.size(); ++k)
            rest.insert(rest.end(), regions[k].begin(), regions[k].end());
        result = clip(src, c2::ClipType::Union, c2::FillRule::NonZero, result, rest, precision);
        break;
    }
    case BooleanOp::Subtract:
    {
        c2::PathsD tools;
        for (size_t k = 1; k < regions.size(); ++k)
            tools.insert(tools.end(), regions[k].begin(), regions[k].end());
        if (!tools.empty())
            result = clip(src, c2::ClipType::Difference, c2::FillRule::NonZero, result, tools, precision);
        break;
    }
    case BooleanOp::Intersect:
        for (size_t k = 1; k < regions.size(); ++k)
            result = clip(src, c2::ClipType::Intersection, c2::FillRule::NonZero, result, regions[k], precision);
        break;
    case BooleanOp::Xor:
        for (size_t k = 1; k < regions.size(); ++k)
            result = clip(src, c2::ClipType::Xor, c2::FillRule::NonZero, result, regions[k], precision);
        break;
    }

    Kind kind = preferredKind(operands);
    for (const c2::PathD& p : result)
    {
        if (p.size() < 3)
            continue;
        Spline s = rebuildLoop(src, p, kind, tess.chordError);
        if (s.size() >= 2 && isValid(s))
            res.curves.push_back(std::move(s));
    }
    if (res.curves.empty())
        res.warnings.push_back("the result is empty");
    return res;
}

namespace
{
/** A closed outline as Beziers: where it turns sharply it keeps a corner, a run between two corners is a line, the rest is fit. */
Spline fitOutline(const std::vector<D3>& pts, double tolerance)
{
    const size_t n = pts.size();
    std::vector<size_t> corners;
    for (size_t i = 0; i < n; ++i)
    {
        D3 u = pts[i] - pts[(i + n - 1) % n], v = pts[(i + 1) % n] - pts[i];
        double lu = glm::length(u), lv = glm::length(v);
        if (lu > 0.0 && lv > 0.0 && std::acos(std::clamp(glm::dot(u, v) / (lu * lv), -1.0, 1.0)) > 0.35)
            corners.push_back(i);
    }
    if (corners.empty())
        return fitBezier(pts, true, tolerance, 0.3);
    std::vector<Piece> ps;
    for (size_t k = 0; k < corners.size(); ++k)
    {
        size_t a = corners[k], b = corners[(k + 1) % corners.size()];
        std::vector<D3> run;
        for (size_t i = a;; i = (i + 1) % n)
        {
            run.push_back(pts[i]);
            if (i == b && run.size() > 1)
                break;
        }
        if (run.size() == 2)
        {
            ps.push_back(linePiece(run[0], run[1]));
            continue;
        }
        for (Piece& p : pieces(fitBezier(run, false, tolerance, 0.3)))
            ps.push_back(std::move(p));
    }
    return fromPieces(ps, true, Kind::Bezier);
}
} // namespace

CurveBooleanResult regionOffset(const Splines& curves, const RegionOffsetOptions& o)
{
    LILA_ZONE();

    CurveBooleanResult res;
    double deviation = 0.0;
    res.plane = fitPlane(curves, o.normal, &deviation);
    if (deviation > 0.01)
        res.warnings.push_back("the curves are not planar (up to " + std::to_string(deviation) + " cm off their plane)");
    TessOptions tess;
    tess.chordError = std::max(o.tolerance * 0.25, 1e-5);
    tess.maxAngle = 0.05;
    Loops2 loops = loopsOf(curves, res.plane, tess, &res.warnings);
    if (loops.empty())
        return res;
    double extent = std::abs(o.distance);
    for (const Loop2& l : loops)
        for (const D2& p : l.points)
            extent = std::max({extent, std::abs(p.x), std::abs(p.y)});
    int precision = precisionFor(extent);
    Sources src;
    src.plane = res.plane;
    for (const Spline& s : curves)
    {
        src.curves.push_back(&s);
        src.spans.push_back(spanCount(s));
    }
    c2::PathsD region = clip(src, c2::ClipType::Union, c2::FillRule::EvenOdd, toPaths(loops, src, 0), {}, precision);
    c2::JoinType jt = o.join == JoinType::Round ? c2::JoinType::Round : o.join == JoinType::Miter ? c2::JoinType::Miter : c2::JoinType::Square;
    double arcTol = std::max(o.tolerance * 0.25, 1e-5);
    c2::PathsD grown = c2::InflatePaths(region, o.distance, jt, c2::EndType::Polygon, std::max(1.0, o.miterLimit), precision, arcTol);
    grown = c2::SimplifyPaths(grown, std::max(o.tolerance * 0.05, 1e-7));

    bool allPoly = std::ranges::all_of(curves, [](const Spline& s) { return s.kind == Kind::Poly; });
    bool polyOut = allPoly && o.join != JoinType::Round;
    for (const c2::PathD& p : grown)
    {
        if (p.size() < 3)
            continue;
        std::vector<D3> pts;
        for (const c2::PointD& q : p)
            pts.push_back(res.plane.to3(D2(q.x, q.y)));
        Spline s;
        if (polyOut)
        {
            s.kind = Kind::Poly;
            s.closed = true;
            for (size_t i : simplifyIndices(pts, o.tolerance * 0.1, true))
                s.points.push_back(pts[i]);
            s.normalize();
        }
        else
        {
            s = fitOutline(pts, o.tolerance);
            vectorHandlesOnLines(s);
        }
        if (s.size() >= 2 && isValid(s))
            res.curves.push_back(std::move(s));
    }
    if (res.curves.empty())
        res.warnings.push_back("nothing is left after the offset");
    return res;
}
} // namespace lila::curve
