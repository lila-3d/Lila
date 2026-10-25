#include <doctest/doctest.h>

#include "curve/BevelPlus.h"
#include "curve/CurveBoolean.h"
#include "curve/Roof.h"
#include "curve/Skeleton.h"
#include "curve/Text.h"
#include "geo/Mesh.h"

#include <cmath>

using namespace lila;
using D2 = glm::dvec2;
using D3 = glm::dvec3;

namespace
{
curve::Spline poly(std::vector<D3> pts, bool closed = true)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.closed = closed;
    s.points = std::move(pts);
    s.normalize();
    return s;
}

bool finite(const geo::Mesh& m)
{
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && !(std::isfinite(m.position(p).x) && std::isfinite(m.position(p).y) && std::isfinite(m.position(p).z)))
            return false;
    return true;
}

bool finite(const curve::Splines& s)
{
    for (const curve::Spline& sp : s)
        for (const D3& p : sp.points)
            if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)))
                return false;
    return true;
}
} // namespace

TEST_CASE("planar tools warn on degenerate input instead of failing")
{
    curve::Spline open = poly({D3(0), D3(10, 0, 0), D3(10, 10, 0)}, false);
    curve::Spline flat = poly({D3(0), D3(5, 0, 0), D3(10, 0, 0)});
    curve::Spline repeated = poly({D3(0), D3(0), D3(10, 0, 0), D3(10, 0, 0), D3(10, 10, 0), D3(0, 10, 0)});
    curve::Spline single = poly({D3(1, 2, 3), D3(1, 2, 3), D3(1, 2, 3)});
    // A figure eight crosses itself.
    curve::Spline eight = poly({D3(0), D3(10, 10, 0), D3(10, 0, 0), D3(0, 10, 0)});

    curve::CurveBooleanResult b = curve::curveBoolean({{open}, {flat}}, {});
    CHECK(b.curves.empty());
    CHECK(!b.warnings.empty());
    b = curve::curveBoolean({{repeated}, {eight}}, {});
    CHECK(!b.curves.empty());
    CHECK(finite(b.curves));
    b = curve::curveBoolean({}, {});
    CHECK(!b.warnings.empty());

    curve::BevelPlusOptions o;
    for (const curve::Spline& s : {open, flat, single})
    {
        curve::BevelPlusResult r = curve::bevelPlus({s}, o);
        CHECK(r.mesh.numFaces() == 0);
        CHECK(!r.warnings.empty());
    }
    curve::BevelPlusResult r = curve::bevelPlus({repeated}, o);
    CHECK(r.warnings.empty());
    CHECK(r.mesh.numFaces() > 0);
    CHECK(finite(r.mesh));
    r = curve::bevelPlus({eight}, o);
    CHECK(!r.warnings.empty());
    CHECK(r.mesh.numFaces() > 0);
    CHECK(finite(r.mesh));
    // Huge and zero bevels.
    o.front.width = o.back.width = 1000.0;
    r = curve::bevelPlus({repeated}, o);
    CHECK(r.mesh.numFaces() > 0);
    CHECK(finite(r.mesh));
    o.front.width = o.back.width = 0.0;
    o.extrudeDepth = 0.0;
    r = curve::bevelPlus({repeated}, o);
    CHECK(r.mesh.numFaces() == 2);
    CHECK(finite(r.mesh));

    curve::RoofResult roof = curve::roof({open}, {});
    CHECK(roof.mesh.numFaces() == 0);
    CHECK(!roof.warnings.empty());
    curve::RoofOptions ro;
    ro.overhang = 50.0;
    roof = curve::roof({poly({D3(0), D3(10, 0, 0), D3(10, 0, -10), D3(5, 0, -10), D3(5, 0, -1), D3(4, 0, -1), D3(4, 0, -10), D3(0, 0, -10)})}, ro);
    CHECK(!roof.warnings.empty());
    CHECK(finite(roof.mesh));

    CHECK(curve::insetCurves({flat}, 1.0, 1, D3(0), 0.01).curves.empty());
    CHECK(!curve::insetCurves({repeated}, -1.0, 1, D3(0), 0.01).warnings.empty());

    curve::TextOptions t;
    t.text = "";
    CHECK(curve::textCurves(t).curves.empty());
    t.text = "\n\t \xff\xfe";
    curve::TextResult tr = curve::textCurves(t);
    CHECK(finite(tr.curves));

    // Skeletons of loops too short to be polygons, and cleaning that leaves nothing.
    CHECK(!curve::straightSkeleton({{D2(0), D2(1, 0)}}).ok());
    CHECK(curve::cleanLoop({D2(0), D2(0), D2(1e-12, 0)}, 1e-9).empty());
}
