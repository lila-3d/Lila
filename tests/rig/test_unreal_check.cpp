#include "TestUtil.h"

#include "fbx/Fbx.h"
#include "gltf/Gltf.h"
#include "scene/Anim.h"
#include "scene/Diagnostics.h"
#include "scene/Rig.h"
#include "scene/UnrealCheck.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <tuple>

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

std::vector<const diagnostics::Issue*> roundTripIssues()
{
    std::vector<const diagnostics::Issue*> out;
    for (const diagnostics::Issue& i : diagnostics::issues())
        if (i.source == uecheck::kValidator)
            out.push_back(&i);
    return out;
}

std::string exportPath(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "lila_rig_tests";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}

void needExporters()
{
    if (!ops::find("exportFbx"))
        fbx::registerCommands();
    if (!ops::find("exportGltf"))
        gltf::registerCommands();
}

/** Keys bone_a's scale y from 1 to peak and back over [0, last] frames, and bone_b's rotate z, so the shear peaks mid-range. */
void animateShear(double last, double peak)
{
    for (auto [frame, sy, rz] : {std::tuple{0.0, 1.0, 0.0}, std::tuple{last / 2, peak, 45.0}, std::tuple{last, 1.0, 0.0}})
    {
        const double t = anim::secondsOf(frame);
        anim::setKey(graph(), {node("bone_a").id(), Transform::kScale, 1}, t, sy);
        anim::setKey(graph(), {node("bone_b").id(), Transform::kRotate, 2}, t, rz);
    }
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

TEST_CASE("unreal check: non-uniform scale under a rotated child is flagged on that joint")
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

    std::vector<const diagnostics::Issue*> found = roundTripIssues();
    REQUIRE_FALSE(found.empty());
    bool onB = false;
    for (const diagnostics::Issue* i : found)
    {
        CHECK(i->severity == diagnostics::Severity::Error);
        // Exporters run the check themselves, so the issue never holds the command gate.
        CHECK(i->blocks.empty());
        onB = onB || graph().node(i->node)->name() == "bone_b";
    }
    CHECK(onB);

    // The validator finds the same, and fixing the rig clears it.
    diagnostics::clear();
    diagnostics::validate({uecheck::kValidator});
    CHECK_FALSE(roundTripIssues().empty());
    run("setAttr", {{"args", {"bone_a.scale", v3(1, 1, 1)}}});
    diagnostics::validate({uecheck::kValidator});
    CHECK(roundTripIssues().empty());
}

TEST_CASE("unreal check: exportFbx and exportGltf refuse a skeleton Unreal can't match, unless unrealCheck is off")
{
    needExporters();
    checkedCylinder();
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", anim::secondsOf(20.0)}});
    animateShear(20.0, 1.6);
    const std::string fbxFile = exportPath("ue_check.fbx"), gltfFile = exportPath("ue_check.glb");
    std::filesystem::remove(fbxFile);

    std::string why;
    try
    {
        run("exportFbx", {{"args", {fbxFile, "Tube"}}, {"kind", "skeletal"}});
    }
    catch (const CommandError& e)
    {
        why = e.what();
    }
    CHECK(why.find("bone_b") != std::string::npos);
    CHECK(why.find("unrealCheck=False") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fbxFile));
    CHECK_FALSE(roundTripIssues().empty());
    CHECK_THROWS_AS(run("exportGltf", {{"args", {gltfFile, "Tube"}}}), CommandError);
    // A static export writes no skeleton, so it has nothing to check.
    CHECK_NOTHROW(run("exportFbx", {{"args", {exportPath("ue_check_static.fbx"), "Tube"}}, {"kind", "static"}}));

    CHECK(run("exportFbx", {{"args", {fbxFile, "Tube"}}, {"kind", "skeletal"}, {"unrealCheck", false}}) == fbxFile);
    CHECK(std::filesystem::exists(fbxFile));
    CHECK_NOTHROW(run("exportGltf", {{"args", {gltfFile, "Tube"}}, {"unrealCheck", false}}));
    // Without animation only the pose at frame 0 goes out, which Unreal matches; the joints' issues clear.
    run("currentTime", {{"args", {0.0}}});
    CHECK_NOTHROW(run("exportFbx", {{"args", {fbxFile, "Tube"}}, {"kind", "skeletal"}, {"animation", false}}));
    CHECK(roundTripIssues().empty());
}

TEST_CASE("unreal check: an export of a rig that can't shear evaluates nothing; a scaling one samples the range")
{
    checkedCylinder();
    // Rotation and translation keys (stretch) never shear.
    for (auto [frame, rz, ty] : {std::tuple{0.0, 0.0, 50.0}, std::tuple{100.0, 60.0, 80.0}})
    {
        anim::setKey(graph(), {node("bone_b").id(), Transform::kRotate, 2}, anim::secondsOf(frame), rz);
        anim::setKey(graph(), {node("bone_c").id(), Transform::kTranslate, 1}, anim::secondsOf(frame), ty);
    }
    const std::vector<NodeId> mesh{node("Tube").id()};
    CHECK_FALSE(uecheck::mayShear(graph(), {node("bone_a").id()}));
    uecheck::ExportCheck c = uecheck::checkExport(graph(), mesh, true, 0.0, anim::secondsOf(100.0), 30.0, "test");
    CHECK_FALSE(c.evaluated);
    CHECK(c.error.empty());

    // Non-uniform scale on a leaf can't shear; keyed on a joint with joints below, it can.
    run("setAttr", {{"args", {"bone_c.scale", v3(1, 2, 1)}}});
    CHECK_FALSE(uecheck::mayShear(graph(), {node("bone_a").id()}));
    animateShear(100.0, 1.6);
    CHECK(uecheck::mayShear(graph(), {node("bone_a").id()}));
    c = uecheck::checkExport(graph(), mesh, true, 0.0, anim::secondsOf(100.0), 30.0, "test");
    CHECK(c.evaluated);
    CHECK(c.report.frames == uecheck::kExportFrames);
    CHECK(c.error.find("bone_b") != std::string::npos);

    uecheck::SkipScope skip;
    CHECK(uecheck::checkExport(graph(), mesh, true, 0.0, anim::secondsOf(100.0), 30.0, "test").error.empty());
}

TEST_CASE("unreal check: export timing on a dense skinned limb" * doctest::skip(!std::getenv("LILA_TIMING")))
{
    run("newScene");
    diagnostics::clear();
    const std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 200},
                                                  {"subdivisionsAxis", 100}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_0"}});
    for (int k = 1; k <= 20; ++k)
        run("joint", {{"position", v3(0, -50 + 5.0 * k, 0)}, {"name", "bone_" + std::to_string(k)}});
    run("skinCluster", {{"args", {"bone_0", mesh}}});
    for (int k = 1; k <= 20; ++k)
        for (auto [frame, rz] : {std::pair{0.0, 0.0}, std::pair{300.0, 10.0}})
            anim::setKey(graph(), {node("bone_" + std::to_string(k)).id(), Transform::kRotate, 2}, anim::secondsOf(frame), rz);
    const std::vector<NodeId> ids{node("Tube").id()};
    const double end = anim::secondsOf(300.0);
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };

    auto t = Clock::now();
    uecheck::ExportCheck c = uecheck::checkExport(graph(), ids, true, 0.0, end, 30.0, "t");
    MESSAGE("no scale, export check: evaluated=" << c.evaluated << " " << ms(t) << " ms");
    t = Clock::now();
    uecheck::check(graph(), {.start = 0.0, .end = end, .fps = 30.0});
    MESSAGE("no scale, unrealCheck every frame: " << ms(t) << " ms");

    for (auto [frame, s] : {std::pair{0.0, 1.0}, std::pair{150.0, 1.3}, std::pair{300.0, 1.0}})
        anim::setKey(graph(), {node("bone_5").id(), Transform::kScale, 0}, anim::secondsOf(frame), s);
    t = Clock::now();
    c = uecheck::checkExport(graph(), ids, true, 0.0, end, 30.0, "t");
    MESSAGE("keyed scale, export check: frames=" << c.report.frames << " " << ms(t) << " ms, mismatches=" << c.report.mismatches.size());
    t = Clock::now();
    uecheck::check(graph(), {.start = 0.0, .end = end, .fps = 30.0});
    MESSAGE("keyed scale, unrealCheck every frame: " << ms(t) << " ms");
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
