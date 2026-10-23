#include <doctest/doctest.h>

#include "rig/RigMath.h"

#include <tuple>

using namespace puppeteer::math;
using Approx = doctest::Approx;

namespace
{
bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) { return glm::length(a - b) <= eps; }
bool near(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-9)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps)
                return false;
    return true;
}
glm::dvec3 apply(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

FootPivots leftFoot()
{
    // A left foot (outer edge at +x): heel behind the ankle, ball and toe ahead.
    FootPivots p;
    p.heel = {0, 0, -4};
    p.ball = {0, 0, 12};
    p.toe = {0, 0, 20};
    p.inner = {-4, 0, 12};
    p.outer = {4, 0, 12};
    return p;
}
} // namespace

TEST_CASE("reverse foot: the roll splits between heel, ball and toe and always adds up")
{
    const FootLimits limits{30.0, 60.0};
    for (double roll : {-40.0, -5.0, 0.0, 10.0, 30.0, 45.0, 60.0, 90.0})
    {
        const RollSplit s = splitRoll(roll, limits);
        CHECK(s.heel + s.ball + s.toe == Approx(roll));
    }
    CHECK(splitRoll(-20.0, limits).heel == Approx(-20.0));
    CHECK(splitRoll(20.0, limits).ball == Approx(20.0));
    CHECK(splitRoll(20.0, limits).toe == Approx(0.0));
    // Past the ball limit the ball hands its share over to the toe, all of it at the toe limit.
    CHECK(splitRoll(45.0, limits).ball == Approx(15.0));
    CHECK(splitRoll(60.0, limits).ball == Approx(0.0));
    CHECK(splitRoll(80.0, limits).toe == Approx(80.0));
}

TEST_CASE("reverse foot: each channel pivots where it should")
{
    const FootPivots p = leftFoot();
    const FootLimits limits;
    const glm::dvec3 ankle(0, 8, 0);

    // Ball roll lifts the heel and the ankle, keeping the ball (and the toes beyond it, through the ball frame) planted.
    FootSolve s = solveFoot(p, {.roll = 20.0}, limits);
    CHECK(near(apply(s.ankle, p.ball), p.ball));
    CHECK(apply(s.ankle, p.heel).y > 0.0);
    CHECK(apply(s.ankle, ankle).y > ankle.y);
    CHECK(near(apply(s.ball, p.toe), p.toe));

    // Heel roll pivots on the heel and lifts the toes.
    s = solveFoot(p, {.roll = -20.0}, limits);
    CHECK(near(apply(s.ankle, p.heel), p.heel));
    CHECK(apply(s.ankle, p.toe).y > 0.0);

    // Toe roll pivots on the toe tip.
    s = solveFoot(p, {.roll = 70.0}, limits);
    CHECK(near(apply(s.ankle, p.toe), p.toe));
    CHECK(apply(s.ankle, p.ball).y > 0.0);

    // Bank tips over one edge and lifts the other, for either sign.
    s = solveFoot(p, {.bank = 15.0}, limits);
    CHECK(near(apply(s.ankle, p.outer), p.outer));
    CHECK(apply(s.ankle, p.inner).y > 0.0);
    s = solveFoot(p, {.bank = -15.0}, limits);
    CHECK(near(apply(s.ankle, p.inner), p.inner));
    CHECK(apply(s.ankle, p.outer).y > 0.0);

    // A right foot (edges mirrored) still lifts the far edge.
    FootPivots r = p;
    std::swap(r.inner, r.outer);
    s = solveFoot(r, {.bank = 15.0}, limits);
    CHECK(apply(s.ankle, r.inner).y > 0.0);

    // Twist swivels on the ball, around the sole's up axis.
    s = solveFoot(p, {.twist = 30.0}, limits);
    CHECK(near(apply(s.ankle, p.ball), p.ball));
    CHECK(apply(s.ankle, p.heel).y == Approx(0.0));
    CHECK(apply(s.ankle, p.heel).x != Approx(0.0));
}

TEST_CASE("reverse foot: snap and align keep the ankle where it is")
{
    const FootPivots p = leftFoot();
    const FootLimits limits;
    glm::dmat4 control = glm::rotate(glm::dmat4(1.0), radians(25.0), glm::dvec3(0, 1, 0));
    control[3] = glm::dvec4(10, 2, -3, 1);
    glm::dmat4 restAnkle = glm::rotate(glm::dmat4(1.0), radians(10.0), glm::dvec3(1, 0, 0));
    restAnkle[3] = glm::dvec4(0, 8, 0, 1);
    const FootChannels posed{.roll = 25.0, .twist = -12.0, .bank = 8.0};
    const glm::dmat4 ankle = control * solveFoot(p, posed, limits).ankle * restAnkle;

    // Snap: the control on the sole, channels zeroed, same ankle.
    const glm::dmat4 snapped = snapControl(ankle, restAnkle);
    CHECK(near(snapped * restAnkle, ankle, 1e-9));

    // Align: level control, the pose moved into the channels, same ankle.
    const FootAlign a = alignControl(ankle, restAnkle, p, limits);
    CHECK(near(glm::dvec3(a.control[1]), glm::dvec3(0, 1, 0), 1e-9));
    CHECK(near(a.control * solveFoot(p, a.channels, limits).ankle * restAnkle, ankle, 1e-6));
}

TEST_CASE("inherit rotate: the child keeps its world rotation under a turned parent")
{
    const glm::dmat3 parentBefore(glm::rotate(glm::dmat4(1.0), radians(20.0), glm::normalize(glm::dvec3(1, 2, 3))));
    const glm::dmat3 orient(glm::rotate(glm::dmat4(1.0), radians(-35.0), glm::dvec3(0, 0, 1)));
    const glm::dmat3 childR(glm::rotate(glm::dmat4(1.0), radians(50.0), glm::dvec3(0, 1, 0)));
    const glm::dmat3 childWorld = parentBefore * orient * childR;

    const glm::dmat3 turn(glm::rotate(glm::dmat4(1.0), radians(70.0), glm::dvec3(1, 0, 0)));
    const glm::dmat3 parentAfter = turn * parentBefore;
    const glm::dmat3 r = keepWorldRotation(parentAfter, orient, childWorld);
    const glm::dmat3 after = parentAfter * orient * r;
    for (int c = 0; c < 3; ++c)
        CHECK(near(after[c], childWorld[c], 1e-12));
    // Inherit on would have left R alone and turned the child with the parent.
    CHECK_FALSE(near((parentAfter * orient * childR)[0], childWorld[0], 1e-6));
}

TEST_CASE("toon scale: children keep their world size")
{
    const glm::dvec3 ratio(2.0, 0.5, 1.0);
    // Aligned: exact.
    CHECK(near(compensatedScale(glm::dvec3(1.0), ratio, glm::dmat3(1.0)), glm::dvec3(0.5, 2.0, 1.0), 1e-12));
    // Child turned 90 degrees about Z: its X lies along the parent's Y.
    const glm::dmat3 turned(glm::rotate(glm::dmat4(1.0), radians(90.0), glm::dvec3(0, 0, 1)));
    CHECK(near(compensatedScale(glm::dvec3(1.0), ratio, turned), glm::dvec3(2.0, 0.5, 1.0), 1e-9));
}

TEST_CASE("breakdown: interpolates toward the previous or next key")
{
    CHECK(breakdown(4.0, 0.0, 10.0, 0.0) == Approx(4.0));
    CHECK(breakdown(4.0, 0.0, 10.0, 0.5) == Approx(7.0));
    CHECK(breakdown(4.0, 0.0, 10.0, 1.0) == Approx(10.0));
    CHECK(breakdown(4.0, 0.0, 10.0, -0.5) == Approx(2.0));
    CHECK(breakdown(4.0, 0.0, 10.0, -1.0) == Approx(0.0));
    // Past full it overshoots along the same line.
    CHECK(breakdown(4.0, 0.0, 10.0, 1.5) == Approx(13.0));
    CHECK(breakdownWeight(15.0, 20.0, 100.0) == Approx(0.0));
    CHECK(breakdownWeight(60.0, 20.0, 100.0) == Approx(0.5));
    CHECK(breakdownWeight(-100.0, 20.0, 100.0) == Approx(-1.0));
    CHECK(breakdownWeight(180.0, 20.0, 100.0) == Approx(2.0));

    // Rotations slerp the short way.
    const glm::dquat cur = glm::angleAxis(radians(10.0), glm::dvec3(0, 0, 1));
    const glm::dquat next = glm::angleAxis(radians(90.0), glm::dvec3(0, 0, 1));
    const glm::dquat half = breakdown(cur, cur, next, 0.5);
    CHECK(glm::degrees(glm::angle(half)) == Approx(50.0));
    // The same rotation written with the opposite sign still takes the short way.
    const glm::dquat flipped = breakdown(cur, cur, -next, 0.5);
    CHECK(std::abs(glm::dot(flipped, half)) == Approx(1.0));
    CHECK(glm::degrees(glm::angle(breakdown(cur, cur, next, 1.5))) == Approx(130.0));

    CHECK(quantize(7.4, 5.0) == Approx(5.0));
    CHECK(quantize(7.6, 5.0) == Approx(10.0));
    CHECK(quantize(7.6, 0.0) == Approx(7.6));
}

TEST_CASE("reverse foot: limits clamp the channels and snap near-zero values")
{
    FootLimits l;
    l.heelMax = 30.0;
    l.toeMax = 80.0;
    l.bankMax = 20.0;
    l.twistMax = 45.0;
    l.zeroSnap = 1.0;
    const FootChannels c = limitChannels({.roll = -50.0, .twist = 100.0, .bank = 0.5}, l);
    CHECK(c.roll == Approx(-30.0));
    CHECK(c.twist == Approx(45.0));
    CHECK(c.bank == Approx(0.0));
    CHECK(limitChannels({.roll = 200.0}, l).roll == Approx(80.0));
    // The solve sees the limited channels.
    const FootPivots p = leftFoot();
    CHECK(near(apply(solveFoot(p, {.roll = -50.0}, l).ankle, p.toe), apply(solveFoot(p, {.roll = -30.0}, l).ankle, p.toe), 1e-12));
}

TEST_CASE("reverse foot: roll and bank together pivot on the sole's corner")
{
    const FootPivots p = leftFoot();
    const FootLimits limits;
    // Heel roll plus a bank onto the outer edge rocks on the heel's outer corner.
    const glm::dvec3 heelOuter(p.outer.x, 0.0, p.heel.z);
    FootSolve s = solveFoot(p, {.roll = -20.0, .bank = 12.0}, limits);
    CHECK(near(apply(s.ankle, heelOuter), heelOuter, 1e-9));
    // Toe roll plus a bank onto the inner edge rocks on the toe's inner corner.
    const glm::dvec3 toeInner(p.inner.x, 0.0, p.toe.z);
    s = solveFoot(p, {.roll = 75.0, .bank = -12.0}, limits);
    CHECK(near(apply(s.ankle, toeInner), toeInner, 1e-9));
    // Nothing goes below the ground.
    for (const glm::dvec3& q : {p.heel, p.toe, p.inner, p.outer, p.ball})
        CHECK(apply(s.ankle, q).y >= -1e-9);
}

TEST_CASE("reverse foot: pivots come from the skinned sole")
{
    // A box-ish foot: sole at y = 2, from z = -5 (heel) to 20 (toe), 8 wide; its top at y = 9.
    std::vector<glm::dvec3> pts;
    for (double z : {-5.0, 0.0, 6.0, 12.0, 20.0})
        for (double x : {-4.0, 4.0})
        {
            pts.push_back({x, 2.0, z});
            pts.push_back({x, 9.0, z});
        }
    double ground = 0.0;
    const auto f = pivotsFromSole(pts, 0.5, 12.0, 1.0, &ground);
    REQUIRE(f);
    CHECK(ground == Approx(2.0));
    CHECK(near(f->heel, glm::dvec3(0.0, 2.0, -5.0)));
    CHECK(near(f->toe, glm::dvec3(0.0, 2.0, 20.0)));
    CHECK(near(f->ball, glm::dvec3(0.5, 2.0, 12.0)));
    CHECK(near(f->inner, glm::dvec3(4.0, 2.0, 12.0)));
    CHECK(near(f->outer, glm::dvec3(-4.0, 2.0, 12.0)));
    CHECK_FALSE(pivotsFromSole({{0, 0, 0}}, 0.0, 0.0, 1.0));
}

TEST_CASE("spinner maths: toon scale, trackball and signed angles")
{
    const glm::dvec3 t = toonScale(0, 1.37, 1.0 / 3.0);
    CHECK(t.x == Approx(1.37));
    CHECK(t.y == Approx(std::pow(1.37, -1.0 / 3.0)));
    CHECK(t.y == Approx(0.90).epsilon(0.01));
    CHECK(toonScale(1, 2.0, 0.0) == glm::dvec3(1.0, 2.0, 1.0));

    // View looking down -Z: dragging right turns about +Y, dragging down about +X.
    auto [ax, angle] = trackball({40.0, 0.0}, glm::dmat3(1.0), 0.5);
    CHECK(near(ax, glm::dvec3(0, 1, 0)));
    CHECK(angle == Approx(20.0));
    std::tie(ax, angle) = trackball({0.0, 10.0}, glm::dmat3(1.0), 0.5);
    CHECK(near(ax, glm::dvec3(1, 0, 0)));
    CHECK(trackball({0.0, 0.0}, glm::dmat3(1.0), 0.5).second == 0.0);

    CHECK(signedAngle({1, 0, 0}, {0, 1, 0}, {0, 0, 1}) == Approx(90.0));
    CHECK(signedAngle({1, 0, 0}, {0, -1, 5}, {0, 0, 1}) == Approx(-90.0));
}

TEST_CASE("sticky aim frame: the primary axis follows the aim, the secondary keeps or leans")
{
    const glm::dvec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    const glm::dvec3 aim = glm::normalize(glm::dvec3(1, 1, 0));
    const glm::dmat3 w = stickyAimFrame(x, y, aim, z, nullptr, false);
    CHECK(near(w * x, aim));
    CHECK(std::abs(glm::determinant(w) - 1.0) < 1e-12);
    // Keeping the previous secondary (here +Z, already across the aim).
    const glm::dmat3 k = stickyAimFrame(x, z, aim, z, nullptr, false);
    CHECK(near(k * z, z));
    // Sticky 2nd leans into the given plane direction; keep direction flips it toward the previous one.
    const glm::dvec3 plane(0, 0, -1);
    CHECK(near(stickyAimFrame(x, z, aim, z, &plane, false) * z, glm::dvec3(0, 0, -1)));
    CHECK(near(stickyAimFrame(x, z, aim, z, &plane, true) * z, glm::dvec3(0, 0, 1)));
}
