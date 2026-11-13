#include "uv/Bff.h"
#include "uv/Flow.h"
#include "uv/Layout.h"
#include "uv/Seams.h"
#include "geo/Primitives.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace lila;
using geo::Index;

namespace
{
/** A patch of a unit cylinder about Y, unrolled flat and then turned by `turn` in uv. */
uv::Chart cylinderPatch(double turn, int around = 16, int rows = 10, double span = 1.6)
{
    uv::Chart ch;
    const double c = std::cos(turn), s = std::sin(turn);
    for (int r = 0; r <= rows; ++r)
        for (int k = 0; k <= around; ++k)
        {
            const double a = span * double(k) / around, y = double(r) / rows;
            ch.pos.push_back({std::cos(a), y, std::sin(a)});
            ch.uv.push_back({a * c - y * s, a * s + y * c});
        }
    auto id = [&](int r, int k) { return Index(r * (around + 1) + k); };
    for (int r = 0; r < rows; ++r)
        for (int k = 0; k < around; ++k)
        {
            ch.tris.push_back({id(r, k), id(r, k + 1), id(r + 1, k + 1)});
            ch.tris.push_back({id(r, k), id(r + 1, k + 1), id(r + 1, k)});
        }
    ch.pinned.assign(ch.pos.size(), 0);
    return ch;
}

/** How far the angle is from a multiple of a quarter turn, in degrees. */
double offQuarter(double a)
{
    const double q = glm::half_pi<double>();
    const double r = std::fmod(std::fmod(a, q) + q, q);
    return glm::degrees(std::min(r, q - r));
}
} // namespace

TEST_CASE("an island's flow turns it so the surface's own directions run along U and V")
{
    for (double turn : {0.0, 0.3, -0.7, 1.2})
    {
        CAPTURE(turn);
        const uv::Chart ch = cylinderPatch(turn);
        double agreement = 0.0;
        const double a = uv::flowAngle(ch, &agreement);
        CHECK(agreement > 0.9);
        CHECK(offQuarter(a + turn) < 3.0);
    }
}

TEST_CASE("orienting by flow lays the cylinder's axis straight along U or V")
{
    uv::Chart ch = cylinderPatch(0.5);
    uv::orientChart(ch, uv::OrientMode::Flow);
    // Points round one ring share a height: the ring is a straight row.
    double spreadU = 0.0, spreadV = 0.0;
    for (int k = 1; k <= 16; ++k)
    {
        spreadU = std::max(spreadU, std::abs(ch.uv[k].x - ch.uv[0].x));
        spreadV = std::max(spreadV, std::abs(ch.uv[k].y - ch.uv[0].y));
    }
    CHECK(std::min(spreadU, spreadV) < 0.03);
}

TEST_CASE("upright keeps the flow when it costs little room and the smallest box when it costs a lot")
{
    // Unrolled straight: the flow and the smallest box agree.
    uv::Chart plain = cylinderPatch(0.4);
    CHECK(offQuarter(uv::uprightAngle(plain) + 0.4) < 3.0);
    // The long side ends up along U.
    uv::orientChart(plain, uv::OrientMode::Upright);
    const glm::dvec2 size = plain.uvMax() - plain.uvMin();
    CHECK(size.x >= size.y);
}

namespace
{
/** A cap of an ellipsoid (round across, `height` tall) down to `polar` radians from its top, rings round a top point. */
uv::Chart sphereCap(double polar, double height = 1.0, int rings = 12, int around = 32)
{
    uv::Chart ch;
    ch.pos.push_back({0.0, height, 0.0});
    for (int r = 1; r <= rings; ++r)
        for (int k = 0; k < around; ++k)
        {
            const double t = polar * r / rings, p = glm::two_pi<double>() * k / around;
            ch.pos.push_back({std::sin(t) * std::cos(p), height * std::cos(t), std::sin(t) * std::sin(p)});
        }
    auto id = [&](int r, int k) { return Index(1 + (r - 1) * around + (k % around)); };
    for (int k = 0; k < around; ++k)
        ch.tris.push_back({0, id(1, k + 1), id(1, k)});
    for (int r = 1; r < rings; ++r)
        for (int k = 0; k < around; ++k)
        {
            ch.tris.push_back({id(r, k), id(r, k + 1), id(r + 1, k + 1)});
            ch.tris.push_back({id(r, k), id(r + 1, k + 1), id(r + 1, k)});
        }
    ch.uv.assign(ch.pos.size(), glm::dvec2(0.0));
    ch.pinned.assign(ch.pos.size(), 0);
    // Each triangle its own face, as a chart built from a triangle mesh has them.
    for (size_t t = 0; t < ch.tris.size(); ++t)
    {
        ch.faces.push_back(Index(t));
        ch.faceStart.push_back(Index(ch.corners.size()));
        for (Index v : ch.tris[t])
        {
            ch.cornerVertex.push_back(v);
            ch.corners.push_back(Index(ch.corners.size()));
        }
        ch.triFace.push_back(Index(t));
    }
    ch.faceStart.push_back(Index(ch.corners.size()));
    for (size_t v = 0; v < ch.pos.size(); ++v)
        ch.vertexPoint.push_back(Index(v));
    return ch;
}

/** Spread of log(uv area / 3D area) over the triangles: 0 when every triangle keeps its area. */
double areaSpread(const uv::Chart& ch)
{
    std::vector<double> l;
    double mean = 0.0;
    for (const auto& t : ch.tris)
    {
        const double a3 = glm::length(glm::cross(ch.pos[t[1]] - ch.pos[t[0]], ch.pos[t[2]] - ch.pos[t[0]]));
        const glm::dvec2 e1 = ch.uv[t[1]] - ch.uv[t[0]], e2 = ch.uv[t[2]] - ch.uv[t[0]];
        const double a2 = std::abs(e1.x * e2.y - e1.y * e2.x);
        if (a3 > 0.0 && a2 > 0.0)
        {
            l.push_back(std::log(a2 / a3));
            mean += l.back();
        }
    }
    mean /= double(std::max<size_t>(1, l.size()));
    double var = 0.0;
    for (double x : l)
        var += (x - mean) * (x - mean);
    return std::sqrt(var / double(std::max<size_t>(1, l.size())));
}
} // namespace

TEST_CASE("a cap's flow turns at its top, and a cone there flattens it with less area stretch")
{
    const uv::Chart cap = sphereCap(1.4, 2.5);
    const std::vector<uv::FlowCone> cones = uv::flowCones(cap);
    REQUIRE(!cones.empty());
    CHECK(cones.front().index > 0);
    CHECK(glm::length(cap.pos[cones.front().vertex] - glm::dvec3(0.0, 2.5, 0.0)) < 0.4);

    uv::Chart plain = cap, coned = cap;
    REQUIRE(uv::bff(plain).ok);
    uv::BffOptions o;
    o.cones = uv::flowConeVertices(cap, 1);
    REQUIRE(o.cones.size() == 1);
    const uv::BffResult r = uv::bff(coned, o);
    REQUIRE(r.ok);
    CHECK(r.cones.size() == 1);
    uv::Chart greedy = cap;
    uv::BffOptions og;
    og.autoCones = 1;
    const uv::BffResult rg = uv::bff(greedy, og);
    MESSAGE("area spread without a cone " << areaSpread(plain) << ", with the flow's cone " << areaSpread(coned) << ", the stretch cone " << areaSpread(greedy));
    CHECK(areaSpread(coned) < 0.8 * areaSpread(plain));
    CHECK(areaSpread(coned) <= 1.05 * areaSpread(greedy));
}

TEST_CASE("auto seams dart a pointed cap rather than splitting it")
{
    uv::AutoSeamOptions plainO;
    plainO.darts = false;
    plainO.visibility = false;
    plainO.merge = false;
    uv::AutoSeamOptions dartO = plainO;
    dartO.darts = true;
    {
        const uv::Chart cap = sphereCap(1.3, 1.8, 16, 48);
        std::vector<Index> sizes(cap.tris.size(), 3), corners;
        for (const auto& t : cap.tris)
            corners.insert(corners.end(), t.begin(), t.end());
        geo::Mesh a = geo::Mesh::fromPolygons(cap.pos, sizes, corners), b = a;
        const uv::AutoSeamResult plain = uv::autoSeams(a, {}, plainO);
        const uv::AutoSeamResult darted = uv::autoSeams(b, {}, dartO);
        MESSAGE("split " << plain.charts.size() << " islands, " << plain.seamEdges << " seam edges; darts "
                       << darted.charts.size() << " islands, " << darted.seamEdges << " seam edges");
        CHECK(plain.charts.size() == 2);
        CHECK(darted.charts.size() == 1);
        CHECK(darted.seamEdges < plain.seamEdges);
    }
}
