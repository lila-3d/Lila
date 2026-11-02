#include "TestUtil.h"

#include "scene/Diagnostics.h"
#include "scene/Rig.h"
#include "scene/UnrealCheck.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** A 100 cm cylinder along Y with bone_a, bone_b, bone_c up its middle, skinned. */
void checkedCylinder(bool compensate = false)
{
    run("newScene");
    diagnostics::clear();
    const std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}, {"segmentScaleCompensate", compensate}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
}

json ueCheck(const std::string& root)
{
    return run("unrealCheck", {{"args", {root}}, {"start", 0.0}, {"end", 0.0}, {"fps", 30.0}});
}

bool mismatched(const json& r, const std::string& joint)
{
    for (const json& m : r["mismatches"])
        if (m["joint"].get<std::string>().ends_with(joint))
            return true;
    return false;
}
} // namespace

TEST_CASE("unreal check: FTransform composition hands scale down per axis without shear")
{
    uecheck::UeTransform parent{.translation = {1, 2, 3}, .rotation = glm::angleAxis(0.5, glm::dvec3(0, 0, 1)), .scale = {2, 1, 1}};
    uecheck::UeTransform child{.translation = {4, 0, 0}, .rotation = glm::angleAxis(0.7, glm::dvec3(0, 0, 1)), .scale = {1, 3, 1}};
    const uecheck::UeTransform w = uecheck::compose(child, parent);
    CHECK(w.scale.x == doctest::Approx(2.0));
    CHECK(w.scale.y == doctest::Approx(3.0));
    checkNear(w.translation, glm::dvec3(parent.rotation * glm::dvec3(8, 0, 0)) + parent.translation);
    // Unrotated children match plain matrix composition exactly; a rotated child under non-uniform scale does not.
    child.rotation = glm::dquat(1, 0, 0, 0);
    const glm::dmat4 m = uecheck::toMatrix(parent) * uecheck::toMatrix(child);
    const glm::dmat4 u = uecheck::toMatrix(uecheck::compose(child, parent));
    for (int c = 0; c < 4; ++c)
        checkNear(glm::dvec3(m[c]), glm::dvec3(u[c]));
    // fromLocal is the inverse of toMatrix for a clean TRS.
    const uecheck::UeTransform back = uecheck::fromLocal(uecheck::toMatrix(parent), RotateOrder::XYZ);
    checkNear(back.translation, parent.translation);
    checkNear(back.scale, parent.scale);
    CHECK(std::abs(glm::dot(back.rotation, parent.rotation)) == doctest::Approx(1.0));
}

TEST_CASE("unreal check: a clean hierarchy passes, with uniform scale on a parent and non-uniform scale on a leaf")
{
    checkedCylinder();
    run("setAttr", {{"args", {"bone_a.scale", v3(1.2, 1.2, 1.2)}}});
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 45)}}});
    run("setAttr", {{"args", {"bone_c.scale", v3(1, 2, 0.5)}}});
    const json r = ueCheck("bone_a");
    CHECK(r["skins"] == 1);
    CHECK(r["joints"] == 3);
    CHECK(r["worst"].get<double>() < 1e-4);
    CHECK(r["mismatches"].empty());
    CHECK(diagnostics::blockers("exportFbx").empty());
}

TEST_CASE("unreal check: non-uniform scale under a rotated child is flagged on that joint and blocks export")
{
    checkedCylinder();
    run("setAttr", {{"args", {"bone_a.scale", v3(1, 1.6, 1)}}});
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 45)}}});
    const json r = ueCheck("bone_a");
    CHECK(r["worst"].get<double>() > 1.0);
    CHECK(mismatched(r, "bone_b"));
    CHECK_FALSE(mismatched(r, "bone_a"));
    bool namesParent = false;
    for (const json& m : r["mismatches"])
        namesParent = namesParent || m["scaledAncestor"].get<std::string>().ends_with("bone_a");
    CHECK(namesParent);

    std::vector<const diagnostics::Issue*> stop = diagnostics::blockers("exportFbx");
    REQUIRE_FALSE(stop.empty());
    bool onB = false;
    for (const diagnostics::Issue* i : stop)
    {
        CHECK(i->source == uecheck::kValidator);
        CHECK(i->severity == diagnostics::Severity::Error);
        onB = onB || graph().node(i->node)->name() == "bone_b";
    }
    CHECK(onB);

    // The validator finds the same, and fixing the rig clears it.
    diagnostics::clear();
    diagnostics::validate({uecheck::kValidator});
    CHECK_FALSE(diagnostics::blockers("exportFbx").empty());
    run("setAttr", {{"args", {"bone_a.scale", v3(1, 1, 1)}}});
    diagnostics::validate({uecheck::kValidator});
    CHECK(diagnostics::blockers("exportFbx").empty());
}

TEST_CASE("unreal check: the scale-safe bake of a segment-scale-compensated rig passes where the plain bake does not")
{
    // Lila's mesh has no shear here: bone_b compensates bone_a's scale. Exported as-is, Unreal would shear it back in.
    checkedCylinder(true);
    run("setAttr", {{"args", {"bone_a.scale", v3(1, 1.6, 1)}}});
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 45)}}});
    CHECK(mismatched(ueCheck("bone_a"), "bone_b"));

    const json plain = run("bakeToDeformSkeleton", {{"args", {"bone_a"}}, {"start", 0.0}, {"end", 0.0}, {"fps", 30.0}, {"suffix", "_plain"},
                                                    {"scaleSafe", false}});
    REQUIRE(plain.size() == 1);
    const json plainCheck = run("unrealCheck", {{"args", {plain[0]}}, {"start", 0.0}, {"end", 0.0}, {"suffix", "_plain"}});
    CHECK(plainCheck["skins"] == 1);
    CHECK_FALSE(plainCheck["mismatches"].empty());

    const json safe = run("bakeToDeformSkeleton", {{"args", {"bone_a"}}, {"start", 0.0}, {"end", 0.0}, {"fps", 30.0}});
    REQUIRE(safe.size() == 1);
    CHECK(safe[0] == "bone_a_drv_bake");
    // The scale sits on a leaf; the children follow the unscaled driver.
    REQUIRE(graph().find("bone_a_bake"));
    CHECK(graph().parent(node("bone_a_bake").id()) == node("bone_a_drv_bake").id());
    CHECK(graph().parent(node("bone_b_bake").id()) == node("bone_a_drv_bake").id());
    CHECK(graph().children(node("bone_a_bake").id()).empty());
    checkNear(node("bone_a_drv_bake").as<glm::dvec3>(Transform::kScale), glm::dvec3(1.0));
    // The bake reproduces the rig in Lila and passes in Unreal.
    for (const char* n : {"bone_a", "bone_b", "bone_c"})
    {
        const glm::dmat4 a = world(n), b = world(std::string(n) + "_bake");
        for (int c = 0; c < 4; ++c)
            checkNear(glm::dvec3(a[c]), glm::dvec3(b[c]), 1e-6);
    }
    const json safeCheck = run("unrealCheck", {{"args", {safe[0]}}, {"start", 0.0}, {"end", 0.0}});
    CHECK(safeCheck["skins"] == 1);
    CHECK(safeCheck["mismatches"].empty());
    CHECK(safeCheck["worst"].get<double>() < 1e-4);
}
