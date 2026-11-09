#include <doctest/doctest.h>

#include "deform/BendBones.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

using namespace lila::deform;

namespace
{
/** A tube along X, rings every 2 cm from 0 to 60, 12 points around at radius 5. */
std::vector<glm::dvec3> tube(int rings = 31, double radius = 5.0)
{
    std::vector<glm::dvec3> out;
    for (int r = 0; r < rings; ++r)
        for (int k = 0; k < 12; ++k)
        {
            const double a = 2.0 * glm::pi<double>() * k / 12.0;
            out.push_back({2.0 * r, radius * std::cos(a), radius * std::sin(a)});
        }
    return out;
}

glm::dmat3 aboutZ(double degrees) { return glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(degrees), glm::dvec3(0, 0, 1))); }
glm::dmat3 aboutX(double degrees) { return glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(degrees), glm::dvec3(1, 0, 0))); }

bendbones::Chain straight()
{
    return {.position = {{0, 0, 0}, {30, 0, 0}, {60, 0, 0}}, .rotation = {glm::dmat3(1.0), glm::dmat3(1.0), glm::dmat3(1.0)}};
}

/** The elbow bent 90 degrees about Z: the second bone points up Y. */
bendbones::Chain bent(double degrees = 90.0)
{
    bendbones::Chain c = straight();
    const glm::dmat3 r = aboutZ(degrees);
    c.rotation[1] = c.rotation[2] = r;
    c.position[2] = c.position[1] + r * glm::dvec3(30, 0, 0);
    return c;
}

struct Rig
{
    bendbones::Curve rest;
    std::vector<glm::dvec3> normals;
    bendbones::Binding binding;
};

Rig bindTube(const std::vector<glm::dvec3>& points)
{
    Rig r;
    const bendbones::Chain chain = straight();
    r.rest = bendbones::build(chain);
    r.normals = bendbones::restNormals(chain, r.rest);
    r.rest = bendbones::build(chain, r.normals);
    r.binding = bendbones::bindPoints(std::span(&r.rest, 1), points);
    return r;
}

std::vector<glm::dvec3> pose(const Rig& r, const bendbones::Chain& c, std::vector<glm::dvec3> points, bendbones::Options o = {})
{
    const bendbones::Curve posed = bendbones::build(c, r.normals);
    bendbones::deformPoints(std::span(&r.rest, 1), std::span(&posed, 1), r.binding, o, points);
    return points;
}

/** Spacing of consecutive rings along one strand (point k of each ring): their spread relative to their mean. */
double unevenness(const std::vector<glm::dvec3>& p, int k, int from, int to)
{
    std::vector<double> d;
    for (int r = from; r < to; ++r)
        d.push_back(glm::length(p[size_t((r + 1) * 12 + k)] - p[size_t(r * 12 + k)]));
    double mean = 0.0;
    for (double x : d)
        mean += x;
    mean /= double(d.size());
    double var = 0.0;
    for (double x : d)
        var += (x - mean) * (x - mean);
    return std::sqrt(var / double(d.size())) / mean;
}
} // namespace

TEST_CASE("bend bones: at rest the mesh doesn't move")
{
    const std::vector<glm::dvec3> points = tube();
    const Rig r = bindTube(points);
    const std::vector<glm::dvec3> out = pose(r, straight(), points);
    for (size_t i = 0; i < points.size(); ++i)
        CHECK(glm::length(out[i] - points[i]) < 1e-6);
}

TEST_CASE("bend bones: a bent chain carries the far end round on a smooth curve, keeping the radius")
{
    const std::vector<glm::dvec3> points = tube();
    const Rig r = bindTube(points);
    const std::vector<glm::dvec3> out = pose(r, bent(), points);
    // The far ring sits around the far joint, across the tube's axis there.
    glm::dvec3 centre(0.0);
    for (int k = 0; k < 12; ++k)
        centre += out[size_t(30 * 12 + k)];
    centre /= 12.0;
    CHECK(glm::length(centre - glm::dvec3(30, 30, 0)) < 0.5);
    for (int k = 0; k < 12; ++k)
        CHECK(glm::length(out[size_t(30 * 12 + k)] - centre) == doctest::Approx(5.0).epsilon(0.02));
}

TEST_CASE("bend bones: even bend spreads the inside of a bend evenly where riding the curve bunches it")
{
    const std::vector<glm::dvec3> points = tube();
    const Rig r = bindTube(points);
    // Point 0 of each ring is at +Y: the inside of a bend toward +Y.
    const std::vector<glm::dvec3> riding = pose(r, bent(110.0), points, {.volume = 1.0, .evenBend = 0.0});
    const std::vector<glm::dvec3> even = pose(r, bent(110.0), points, {.volume = 1.0, .evenBend = 1.0});
    // Over the first bone, from where it runs straight into the bend: the inside compresses only where it curves.
    const double before = unevenness(riding, 0, 1, 14), after = unevenness(even, 0, 1, 14);
    MESSAGE("inside spacing spread: riding " << before << ", even " << after);
    CHECK(after < before * 0.5);
    // Nothing on the inside crosses: consecutive inner points still advance.
    for (int ring = 0; ring < 30; ++ring)
        CHECK(glm::length(even[size_t((ring + 1) * 12)] - even[size_t(ring * 12)]) > 0.05);
}

TEST_CASE("bend bones: stretching thins the tube to keep its volume, and twist ramps along the bone")
{
    const std::vector<glm::dvec3> points = tube();
    const Rig r = bindTube(points);
    bendbones::Chain longer = straight();
    longer.position[2] = {90, 0, 0};
    const std::vector<glm::dvec3> thin = pose(r, longer, points);
    const glm::dvec3 ring(thin[size_t(25 * 12)].x, 0, 0);
    CHECK(glm::length(thin[size_t(25 * 12)] - ring) == doctest::Approx(5.0 / std::sqrt(2.0)).epsilon(0.02));
    const std::vector<glm::dvec3> kept = pose(r, longer, points, {.volume = 0.0});
    CHECK(glm::length(kept[size_t(25 * 12)] - glm::dvec3(kept[size_t(25 * 12)].x, 0, 0)) == doctest::Approx(5.0).epsilon(0.02));

    // Twisting the end joint 90 degrees: the end ring turns 90, the middle of the second bone 45, the first bone not at all.
    bendbones::Chain twisted = straight();
    twisted.rotation[2] = aboutX(90.0);
    const std::vector<glm::dvec3> out = pose(r, twisted, points);
    auto angleOf = [&](int ringIndex) {
        const glm::dvec3 p = out[size_t(ringIndex * 12)];
        return glm::degrees(std::atan2(p.z, p.y));
    };
    CHECK(angleOf(30) == doctest::Approx(90.0).epsilon(0.02));
    CHECK(angleOf(23) == doctest::Approx(45.0).epsilon(0.08));
    CHECK(std::abs(angleOf(10)) < 1.0);
}

TEST_CASE("bend bones: bone CVs reshape the bone without rebinding, and two chains blend only at their join")
{
    const std::vector<glm::dvec3> points = tube();
    const Rig r = bindTube(points);
    // A CV on the first bone, lifted: the bone bows up through it; the rest pose binding still applies.
    bendbones::Chain bowed = straight();
    bowed.cvs = {{{15, 6, 0}}, {}};
    bendbones::Chain restWithCv = straight();
    restWithCv.cvs = {{{15, 0, 0}}, {}};
    Rig withCv = r;
    withCv.rest = bendbones::build(restWithCv, r.normals);
    std::vector<glm::dvec3> out = points;
    const bendbones::Curve posed = bendbones::build(bowed, r.normals);
    bendbones::deformPoints(std::span(&withCv.rest, 1), std::span(&posed, 1), r.binding, {}, out);
    CHECK(out[size_t(7 * 12)].y - points[size_t(7 * 12)].y > 4.0);
    glm::dvec3 end(0.0);
    for (int k = 0; k < 12; ++k)
        end += out[size_t(30 * 12 + k)] / 12.0;
    CHECK(glm::length(end - glm::dvec3(60, 0, 0)) < 1e-3);

    // Two chains side by side: points near one bind to it alone.
    bendbones::Chain other = straight();
    for (glm::dvec3& p : other.position)
        p.z += 40.0;
    const bendbones::Curve curves[] = {bendbones::build(straight()), bendbones::build(other)};
    const bendbones::Binding b = bendbones::bindPoints(curves, points);
    for (size_t i = 0; i < points.size(); ++i)
        CHECK(b.offsets[i + 1] - b.offsets[i] == 1);
}

#include "TestUtil.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

TEST_CASE("bendBones deformer: a cylinder bound to a leg bends with the knee and a bone CV bows the thigh")
{
    using namespace deformtest;
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 45, 0)}, {"name", "hip"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "knee"}});
    run("joint", {{"position", v3(0, -45, 0)}, {"name", "ankle"}});
    const std::string mesh = run("polyCylinder", {{"name", "Leg"}, {"radius", 6}, {"height", 90}, {"subdivisionsHeight", 30}})[0];
    const std::string d = run("bendBones", {{"args", {mesh}}, {"chains", {{"hip", "knee", "ankle"}}}}).get<std::string>();
    const auto rest = outMesh(mesh);
    REQUIRE(rest);
    const std::vector<glm::dvec3> before(rest->positions().begin(), rest->positions().end());
    // Bound in place: nothing moves.
    for (size_t i = 0; i < before.size(); ++i)
        CHECK(glm::length(outMesh(mesh)->positions()[i] - before[i]) < 1e-6);

    run("rotate", {{"args", {"knee"}}, {"by", v3(0, 0, 80)}});
    const auto bentMesh = outMesh(mesh);
    double lowest = 1e9, farthest = 0.0;
    for (const glm::dvec3& p : bentMesh->positions())
    {
        lowest = std::min(lowest, p.y);
        farthest = std::max(farthest, std::abs(p.x));
    }
    // The shin swung out sideways, so the mesh's bottom came up and it reaches out.
    CHECK(lowest > -25.0);
    CHECK(farthest > 30.0);

    run("rotate", {{"args", {"knee"}}, {"by", v3(0, 0, -80)}});
    const std::string cv = run("bendBoneCV", {{"args", {d}}, {"joint", "hip"}, {"at", 0.5}}).get<std::string>();
    for (size_t i = 0; i < before.size(); ++i)
        CHECK(glm::length(outMesh(mesh)->positions()[i] - before[i]) < 1e-3);
    run("move", {{"args", {cv}}, {"by", v3(8, 0, 0)}});
    double mid = 0.0;
    int count = 0;
    for (const glm::dvec3& p : outMesh(mesh)->positions())
        if (std::abs(p.y - 22.5) < 3.0)
        {
            mid += p.x;
            ++count;
        }
    REQUIRE(count > 0);
    CHECK(mid / count > 5.0);
    run("undo");
    run("undo");
    CHECK(run("bendBoneCV", {{"args", {d}}, {"joint", "knee"}}).is_string());
    CHECK_THROWS(run("bendBoneCV", {{"args", {d}}, {"joint", "ankle"}}));
}
