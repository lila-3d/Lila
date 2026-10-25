#include "curve/EdgeCurve.h"

#include "core/Profile.h"
#include "curve/Fit.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"

#include <algorithm>

namespace lila::curve
{
using D3 = glm::dvec3;

namespace
{
double fitError(const Spline& s, std::span<const D3> pts)
{
    double worst = 0.0;
    for (const D3& p : pts)
    {
        D3 at;
        closestParam(s, p, &at);
        worst = std::max(worst, glm::distance(at, p));
    }
    return worst;
}

/** The NURBS with the fewest uniform spans that stays within tolerance of every point; interpolation when none does. */
Spline fitNurbsWithin(std::span<const D3> pts, int degree, bool closed, double tolerance)
{
    int n = int(pts.size());
    int lo = 1, hi = closed ? n : n - 1;
    auto fits = [&](int spans, Spline* out) {
        Spline s = fitNurbs(pts, degree, spans, closed);
        if (!isValid(s) || fitError(s, pts) > tolerance)
            return false;
        *out = std::move(s);
        return true;
    };
    Spline best;
    if (hi < 1 || !fits(hi, &best))
        return interpolate(pts, degree, closed);
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        Spline s;
        if (fits(mid, &s))
        {
            hi = mid;
            best = std::move(s);
        }
        else
            lo = mid + 1;
    }
    return best;
}

/** Clamped open NURBS of one degree joined end to end, with a full-multiplicity knot (a corner) at each join. */
Spline joinClamped(const Splines& runs)
{
    int p = runs.front().degree;
    Spline out;
    out.kind = Kind::Nurbs;
    out.degree = p;
    bool rational = std::ranges::any_of(runs, [](const Spline& s) { return s.rational(); });
    double at = 0.0;
    for (size_t k = 0; k < runs.size(); ++k)
    {
        const Spline& s = runs[k];
        std::vector<double> U = knotVector(s);
        double shift = at - U.front();
        if (k == 0)
            out.knots.assign(size_t(p) + 1, at);
        else
            out.knots.insert(out.knots.end(), size_t(p), at);
        for (size_t i = size_t(p) + 1; i + size_t(p) + 1 < U.size(); ++i)
            out.knots.push_back(U[i] + shift);
        at = U.back() + shift;
        for (size_t i = k == 0 ? 0 : 1; i < s.size(); ++i)
        {
            out.points.push_back(s.points[i]);
            if (rational)
                out.weights.push_back(s.weight(i));
        }
    }
    out.knots.insert(out.knots.end(), size_t(p) + 1, at);
    return out;
}

Spline runSpline(std::span<const D3> pts, bool closed, const EdgeCurveOptions& o)
{
    if (o.kind == Kind::Bezier)
        return fitBezier(pts, closed, o.tolerance, 4.0);
    int degree = std::clamp(o.degree, 1, std::max(1, int(pts.size()) - 1));
    return o.smooth ? fitNurbsWithin(pts, degree, closed, std::max(o.tolerance, 1e-6)) : interpolate(pts, degree, closed);
}
} // namespace

std::vector<size_t> turnCorners(std::span<const glm::dvec3> pts, bool closed, double angle)
{
    std::vector<size_t> out;
    size_t n = pts.size();
    if (n < 3)
        return out;
    for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i)
    {
        D3 a = pts[i] - pts[(i + n - 1) % n], b = pts[(i + 1) % n] - pts[i];
        double la = glm::length(a), lb = glm::length(b);
        if (la < 1e-12 || lb < 1e-12)
            continue;
        if (std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0)) > angle)
            out.push_back(i);
    }
    return out;
}

Spline splineThrough(std::span<const glm::dvec3> pts, bool closed, const EdgeCurveOptions& o)
{
    LILA_ZONE();

    closed = closed && pts.size() >= 3;
    Spline s;
    s.kind = o.kind;
    s.closed = closed;
    if (o.kind == Kind::Poly)
    {
        if (o.smooth)
            for (size_t i : simplifyIndices(pts, std::max(o.tolerance, 1e-6), closed))
                s.points.push_back(pts[i]);
        else
            s.points.assign(pts.begin(), pts.end());
        return s;
    }
    std::vector<size_t> corners = turnCorners(pts, closed, o.cornerAngle);
    if (o.kind == Kind::Bezier && !o.smooth)
    {
        s.points.assign(pts.begin(), pts.end());
        s.handleL.assign(s.size(), D3(0.0));
        s.handleR.assign(s.size(), D3(0.0));
        s.typeL.assign(s.size(), HandleType::Auto);
        for (size_t c : corners)
            s.typeL[c] = HandleType::Vector;
        s.typeR = s.typeL;
        updateHandles(s);
        s.normalize();
        return s;
    }
    if (corners.empty())
        return runSpline(pts, closed, o);

    // Runs from corner to corner; on an open chain the ends bound the first and last.
    std::vector<size_t> cuts = corners;
    size_t n = pts.size();
    if (!closed)
    {
        cuts.insert(cuts.begin(), 0);
        cuts.push_back(n - 1);
    }
    else
        cuts.push_back(corners.front() + n);
    Splines runs;
    for (size_t k = 0; k + 1 < cuts.size(); ++k)
    {
        std::vector<D3> run;
        for (size_t i = cuts[k]; i <= cuts[k + 1]; ++i)
            run.push_back(pts[i % n]);
        runs.push_back(runSpline(run, false, o));
    }
    bool sameDegree = std::ranges::all_of(runs, [&](const Spline& r) { return r.kind == Kind::Nurbs && !r.closed && r.degree == runs.front().degree; });
    if (o.kind == Kind::Nurbs && !closed && sameDegree)
        return joinClamped(runs);
    std::vector<Piece> all;
    for (const Spline& r : runs)
        for (Piece& p : pieces(r))
            all.push_back(std::move(p));
    return fromPieces(all, closed, o.kind);
}

Splines edgeCurves(const geo::Mesh& mesh, const glm::dmat4& toWorld, std::span<const geo::Index> edges, const EdgeCurveOptions& o,
                   const glm::dvec3& origin)
{
    LILA_ZONE();

    std::vector<geo::Index> valid;
    for (geo::Index e : edges)
        if (e < mesh.edgeCapacity() && mesh.edgeValid(e))
            valid.push_back(e);
    Splines out;
    for (const geo::EdgePath& path : geo::chainEdges(mesh, valid))
    {
        std::vector<D3> pts;
        for (geo::Index p : path.points)
            pts.push_back(D3(toWorld * glm::dvec4(mesh.position(p), 1.0)) - origin);
        if (pts.size() < 2)
            continue;
        Spline s = splineThrough(pts, path.closed, o);
        if (isValid(s))
            out.push_back(std::move(s));
    }
    return out;
}
} // namespace lila::curve
