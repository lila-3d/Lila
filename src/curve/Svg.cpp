#include "curve/Svg.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Fit.h"
#include "curve/Planar.h"

#ifdef _MSC_VER
// The optimizer reports nanosvg's unreachable code after the header's warning scope ends.
#pragma warning(disable : 4702)
#pragma warning(push, 0)
#pragma warning(disable : 4996)
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <sstream>

namespace lila::curve
{
namespace
{
/** SVG units (y down) to the plane. */
D3 toPlane(double x, double y, const SvgImportOptions& o, const D2& origin)
{
    double u = (x - origin.x) * o.scale, v = -(y - origin.y) * o.scale;
    switch (o.plane)
    {
    case SvgPlane::XY: return {u, v, 0.0};
    case SvgPlane::XZ: return {u, 0.0, -v};
    case SvgPlane::YZ: return {0.0, v, -u};
    }
    return {u, v, 0.0};
}

D2 fromPlane(const D3& p, SvgPlane plane)
{
    switch (plane)
    {
    case SvgPlane::XY: return {p.x, p.y};
    case SvgPlane::XZ: return {p.x, -p.z};
    case SvgPlane::YZ: return {-p.z, p.y};
    }
    return {p.x, p.y};
}

std::string shortNumber(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", std::abs(v) < 1e-9 ? 0.0 : v);
    return buf;
}
} // namespace

const char* svgPlaneName(SvgPlane p)
{
    switch (p)
    {
    case SvgPlane::XY: return "xy";
    case SvgPlane::XZ: return "xz";
    case SvgPlane::YZ: return "yz";
    }
    return "xy";
}

bool parseSvgPlane(std::string_view name, SvgPlane* out)
{
    for (SvgPlane p : {SvgPlane::XY, SvgPlane::XZ, SvgPlane::YZ})
        if (name == svgPlaneName(p))
        {
            *out = p;
            return true;
        }
    return false;
}

SvgDocument readSvg(std::string text, const SvgImportOptions& o)
{
    LILA_ZONE();

    SvgDocument doc;
    if (text.find("<svg") == std::string::npos)
    {
        doc.warnings.push_back("not an SVG document");
        return doc;
    }
    for (const char* unsupported : {"<text", "<use", "<image"})
        if (text.find(unsupported) != std::string::npos)
            doc.warnings.push_back(std::string("skipped ") + (unsupported + 1) + " elements (only shapes and paths come in)");
    NSVGimage* img = nsvgParse(text.data(), "px", 96.0f);
    if (!img)
    {
        doc.warnings.push_back("could not parse the SVG");
        return doc;
    }
    D2 lo(1e300), hi(-1e300);
    for (NSVGshape* sh = img->shapes; sh; sh = sh->next)
        for (NSVGpath* p = sh->paths; p; p = p->next)
            for (int k = 0; k < p->npts; ++k)
            {
                lo = glm::min(lo, D2(p->pts[2 * k], p->pts[2 * k + 1]));
                hi = glm::max(hi, D2(p->pts[2 * k], p->pts[2 * k + 1]));
            }
    D2 origin = o.center && lo.x <= hi.x ? (lo + hi) * 0.5 : D2(0.0);
    double eps = std::max(1e-9, glm::length(hi - lo) * 1e-9);
    int unnamed = 0;
    for (NSVGshape* sh = img->shapes; sh; sh = sh->next)
    {
        SvgShape shape;
        shape.id = sh->id[0] ? std::string(sh->id) : "path" + std::to_string(++unnamed);
        shape.evenOdd = sh->fillRule == NSVG_FILLRULE_EVENODD;
        for (NSVGpath* p = sh->paths; p; p = p->next)
        {
            int segs = (p->npts - 1) / 3;
            if (segs < 1)
                continue;
            auto pt = [&](int k) { return D2(p->pts[2 * k], p->pts[2 * k + 1]); };
            bool closed = p->closed || glm::length(pt(0) - pt(segs * 3)) <= eps;
            std::vector<Piece> ps;
            for (int s = 0; s < segs; ++s)
            {
                D2 a = pt(3 * s), b = pt(3 * s + 3);
                D2 c1 = pt(3 * s + 1), c2 = pt(3 * s + 2);
                if (glm::length(b - a) <= eps && glm::length(c1 - a) <= eps && glm::length(c2 - a) <= eps)
                    continue;
                ps.push_back({{D4(toPlane(a.x, a.y, o, origin), 1.0), D4(toPlane(c1.x, c1.y, o, origin), 1.0), D4(toPlane(c2.x, c2.y, o, origin), 1.0),
                               D4(toPlane(b.x, b.y, o, origin), 1.0)}});
            }
            if (ps.empty())
                continue;
            if (closed && glm::length(ps.back().end() - ps.front().start()) > eps * o.scale)
                ps.push_back(linePiece(ps.back().end(), ps.front().start()));
            if (closed && ps.size() < 2)
                continue;
            Spline s = fromPieces(ps, closed, Kind::Bezier);
            vectorHandlesOnLines(s);
            if (isValid(s))
                shape.curves.push_back(std::move(s));
        }
        if (!shape.curves.empty())
            doc.shapes.push_back(std::move(shape));
    }
    nsvgDelete(img);
    if (doc.shapes.empty())
        doc.warnings.push_back("the SVG has no paths or shapes");
    return doc;
}

std::string writeSvg(const std::vector<Splines>& curves, const SvgExportOptions& o)
{
    LILA_ZONE();

    double scale = std::max(o.scale, 1e-9);
    std::vector<std::string> paths;
    D2 lo(1e300), hi(-1e300);
    std::vector<std::vector<std::vector<Piece>>> all;
    for (const Splines& group : curves)
    {
        std::vector<std::vector<Piece>> g;
        for (const Spline& s : group)
        {
            if (spanCount(s) == 0)
                continue;
            Spline b = s.kind == Kind::Bezier ? s : toBezier(s, o.tolerance);
            std::vector<Piece> ps = pieces(b);
            for (const Piece& p : ps)
                for (size_t k = 0; k < p.cv.size(); ++k)
                {
                    D2 q = fromPlane(p.point(k), o.plane) / scale;
                    lo = glm::min(lo, D2(q.x, -q.y));
                    hi = glm::max(hi, D2(q.x, -q.y));
                }
            if (!ps.empty())
                g.push_back(std::move(ps));
            if (b.closed && !g.empty())
                g.back().push_back(Piece{}); // marks the loop closed
        }
        all.push_back(std::move(g));
    }
    if (lo.x > hi.x)
        lo = hi = D2(0.0);
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"" << shortNumber(lo.x) << " " << shortNumber(lo.y) << " " << shortNumber(std::max(hi.x - lo.x, 1e-6)) << " "
        << shortNumber(std::max(hi.y - lo.y, 1e-6)) << "\" width=\"" << shortNumber(std::max(hi.x - lo.x, 1e-6)) << "\" height=\"" << shortNumber(std::max(hi.y - lo.y, 1e-6))
        << "\">\n";
    auto P = [&](const D3& p) {
        D2 q = fromPlane(p, o.plane) / scale;
        return shortNumber(q.x) + " " + shortNumber(-q.y);
    };
    for (const auto& g : all)
    {
        std::string d;
        for (const std::vector<Piece>& loop : g)
        {
            bool closed = !loop.empty() && loop.back().cv.empty();
            size_t n = loop.size() - (closed ? 1 : 0);
            if (n == 0)
                continue;
            d += (d.empty() ? "M" : " M") + P(loop[0].start());
            for (size_t k = 0; k < n; ++k)
            {
                Piece c = loop[k].degree() == 3 ? loop[k] : elevate(loop[k], 3);
                if (isStraight(c))
                    d += " L" + P(c.end());
                else
                    d += " C" + P(c.point(1)) + " " + P(c.point(2)) + " " + P(c.end());
            }
            if (closed)
                d += " Z";
        }
        if (!d.empty())
            out << "  <path fill-rule=\"evenodd\" fill=\"black\" stroke=\"none\" d=\"" << d << "\"/>\n";
    }
    out << "</svg>\n";
    return out.str();
}
} // namespace lila::curve
