#include "uv/Flow.h"
#include "uv/Layout.h"

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
