// Hand-written stages in fixtures/: composition, xformOp orders, instancing, curves, materials and the prims Lila
// reports instead of importing.

#include "UsdTestUtil.h"

#include "curve/CurveNodes.h"
#include "curve/Spline.h"
#include "geo/ColorSets.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/MeshCache.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;

namespace
{
json importFixture(const char* name, json flags = json::object())
{
    fresh();
    flags["args"] = {fixture(name).generic_string()};
    flags["report"] = true;
    return run("importUsd", flags);
}

bool listed(const json& list, const std::string& part)
{
    for (const json& s : list)
        if (s.get<std::string>().find(part) != std::string::npos)
            return true;
    return false;
}

glm::dmat4 trs(const glm::dvec3& t, const glm::dmat4& r, const glm::dvec3& s = glm::dvec3(1.0))
{
    return glm::translate(glm::dmat4(1.0), t) * r * glm::scale(glm::dmat4(1.0), s);
}

MeshPtr storedMesh(const std::string& name) { return std::get<MeshPtr>(graph().storedValue({node(name).id(), MeshNode::kInMesh, -1})); }
} // namespace

TEST_CASE("usd fixture: every xformOp kind and order")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("xformops.usda");
    INFO(r.dump());
    CHECK(near(world("Trs"), trs({10, 20, 30}, eulerToMatrix({10, 20, 30}, RotateOrder::ZYX), {1, 2, 3}), 1e-4));
    CHECK(node("Trs").as<int64_t>(Transform::kRotateOrder) == int64_t(RotateOrder::ZYX));
    CHECK(near(node("Trs").as<glm::dvec3>(Transform::kRotate), {10, 20, 30}, 1e-4));

    const glm::dmat4 pivot = glm::translate(glm::dmat4(1.0), glm::dvec3(0, 10, 0));
    CHECK(near(world("Pivoted"), glm::translate(glm::dmat4(1.0), glm::dvec3(5, 0, 0)) * pivot * eulerToMatrix({0, 0, 90}, RotateOrder::XYZ) *
                                     glm::inverse(pivot),
               1e-4));
    CHECK(near(node("Pivoted").as<glm::dvec3>(Transform::kRotatePivot), {0, 10, 0}, 1e-6));

    glm::dmat4 m(glm::dvec4(0, 1, 0, 0), glm::dvec4(-1, 0, 0, 0), glm::dvec4(0, 0, 1, 0), glm::dvec4(7, 8, 9, 1));
    CHECK(near(world("Matrix"), m, 1e-5));
    CHECK(near(world("Orient"), trs({1, 2, 3}, glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0, 1, 0))), 1e-4));
    CHECK(near(glm::dvec3(world("RotateThenMove")[3]), {0, 0, -10}, 1e-4));

    graph().setTime(0.5);
    CHECK(near(glm::dvec3(world("Animated")[3]), {24, 0, 0}, 1e-4));
    graph().setTime(1.0);
    CHECK(near(glm::dvec3(world("Animated")[3]), {0, 48, 0}, 1e-4));
    graph().setTime(0.0);
}

TEST_CASE("usd fixture: references, payloads, sublayers and variants")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("composition.usda");
    INFO(r.dump());
    CHECK(graph().find("Referenced|Box"));
    CHECK(near(glm::dvec3(world("Referenced")[3]), {100, 0, 0}, 1e-6));
    CHECK(graph().find("Payloaded|Box"));
    CHECK(graph().find("FromSublayer"));
    CHECK(graph().find("Chair|Seat"));
    CHECK(!graph().find("Frame"));
    CHECK(outMesh("Seat")->numFaces() == 1);

    r = importFixture("composition.usda", {{"loadPayloads", false}, {"variants", {{"style", "metal"}}}});
    INFO(r.dump());
    CHECK(!graph().find("Payloaded|Box"));
    CHECK(graph().find("Payloaded"));
    CHECK(listed(r["skipped"], "payload not loaded"));
    CHECK(graph().find("Chair|Frame"));
    CHECK(!graph().find("Seat"));

    r = importFixture("composition.usda", {{"variants", {{"/Root/Chair", {{"style", "metal"}}}}}, {"prim", "/Root/Chair"}});
    CHECK(graph().find("Chair|Frame"));
    CHECK(!graph().find("Referenced"));
}

TEST_CASE("usd fixture: point instancers and instanceable prims are realized sharing their mesh data")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("instancing.usda");
    INFO(r.dump());
    CHECK(graph().find("Scatter|Pebble_0|Pebble"));
    CHECK(graph().find("Scatter|Rock_1|Rock|Body"));
    CHECK(graph().find("Scatter|Pebble_2|Pebble"));
    CHECK(!graph().find("Rock_3"));
    CHECK(!graph().find("Library"));
    CHECK(node("Rock_1").as<glm::dvec3>(Transform::kScale).x == doctest::Approx(2.0));
    CHECK(near(glm::dvec3(world("Pebble_2")[3]), {20, 0, 0}, 1e-5));
    // Orientations are half precision.
    CHECK(near(node("Pebble_2").as<glm::dvec3>(Transform::kRotate), {0, 0, 90}, 0.05));
    CHECK(storedMesh("Pebble_0|Pebble") == storedMesh("Pebble_2|Pebble"));
    CHECK(storedMesh("InstA|Tri") == storedMesh("InstB|Tri"));
    CHECK(near(glm::dvec3(world("InstB|Tri")[3]), {0, 100, 0}, 1e-6));
    CHECK(r["instances"].get<int>() >= 5);
}

TEST_CASE("usd fixture: basis and NURBS curves")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("curves.usda");
    INFO(r.dump());
    CHECK(r["curves"].size() == 5);
    CHECK(listed(r["skipped"], "widths"));
    const curve::Splines& lines = curve::shapeSplines(graph(), node("Lines").id());
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].kind == curve::Kind::Poly);
    CHECK(lines[1].points.size() == 3);

    const curve::Splines& bez = curve::shapeSplines(graph(), node("Bezier").id());
    REQUIRE(bez.size() == 1);
    CHECK(bez[0].kind == curve::Kind::Bezier);
    CHECK(bez[0].points.size() == 3);
    CHECK(near(curve::pointAt(bez[0], 0.5), {2.0, 1.5, 0.0}, 1e-6));

    const curve::Splines& spline = curve::shapeSplines(graph(), node("Spline").id());
    REQUIRE(spline.size() == 1);
    CHECK(spline[0].kind == curve::Kind::Nurbs);
    CHECK(near(curve::pointAt(spline[0], 0.0), {0, 0, 0}, 1e-6));
    CHECK(near(curve::pointAt(spline[0], double(curve::spanCount(spline[0]))), {8, 0, 0}, 1e-6));

    const curve::Splines& loop = curve::shapeSplines(graph(), node("Loop").id());
    REQUIRE(loop.size() == 1);
    CHECK(loop[0].closed);
    const glm::dvec3 corners[4] = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    for (int k = 0; k < 4; ++k)
        CHECK(near(curve::pointAt(loop[0], double(k)), corners[k], 1e-6));

    const curve::Splines& arc = curve::shapeSplines(graph(), node("Arc").id());
    REQUIRE(arc.size() == 1);
    for (double u : {0.25, 0.5, 0.75})
        CHECK(glm::length(curve::pointAt(arc[0], u)) == doctest::Approx(10.0).epsilon(1e-5));
}

TEST_CASE("usd fixture: UsdPreviewSurface materials, packed maps and collection bindings")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("materials.usda");
    INFO(r.dump());
    CHECK(!graph().find("Looks"));
    CHECK(materials::slotName(*outMesh("Ball"), 0) == "Gold");
    CHECK(materials::slotName(*outMesh("Leaf"), 0) == "Foliage");

    Node* gold = materials::find(graph(), "Gold");
    REQUIRE(gold);
    CHECK(near(gold->as<glm::dvec3>(material::kBaseColor), {1, 0.8, 0.2}, 1e-6));
    CHECK(gold->as<double>(material::kMetallic) == doctest::Approx(1.0));
    CHECK(gold->as<double>(material::kRoughness) == doctest::Approx(0.25));
    CHECK(gold->as<double>(material::kSpecular) == doctest::Approx(1.0));
    CHECK(near(gold->as<glm::dvec3>(material::kEmissiveColor), {0.5, 0, 0}, 1e-6));

    Node* leaf = materials::find(graph(), "Foliage");
    REQUIRE(leaf);
    CHECK(leaf->as<std::string>(material::kBaseColorMap).ends_with("tex/leaf_albedo.png"));
    CHECK(leaf->as<std::string>(material::kOrmMap).ends_with("tex/leaf_orm.png"));
    CHECK(leaf->as<int64_t>(material::kPackedLayout) == int64_t(material::PackedLayout::ORM));
    CHECK(leaf->as<std::string>(material::kNormalMap).ends_with("tex/leaf_normal.png"));
    CHECK(leaf->as<int64_t>(material::kNormalFormat) == int64_t(material::NormalFormat::OpenGL));
    CHECK(leaf->as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Masked));
    CHECK(leaf->as<double>(material::kOpacityMaskClip) == doctest::Approx(0.5));
    CHECK(leaf->as<int64_t>(material::kUvSet) == 1);
    CHECK(leaf->as<bool>(material::kTwoSided));
    CHECK(listed(r["warnings"], "UsdTransform2d"));

    // Texture wrap and filter.
    const material::TextureSampling albedo = materials::sampling(*leaf, material::kBaseColorMap);
    CHECK(albedo.wrapU == material::TextureWrap::Clamp);
    CHECK(albedo.wrapV == material::TextureWrap::Mirror);
    CHECK(albedo.filter == material::TextureFilter::Linear);
    const material::TextureSampling normal = materials::sampling(*leaf, material::kNormalMap);
    CHECK(normal.wrapU == material::TextureWrap::Black);
    CHECK(normal.wrapV == material::TextureWrap::Repeat);
    CHECK(normal.filter == material::TextureFilter::Nearest);
    CHECK(materials::sampling(*leaf, material::kOrmMap).isDefault());

    // MaterialX's unlit surface, textured and plain.
    Node* sky = materials::find(graph(), "Sky");
    REQUIRE(sky);
    CHECK(sky->as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Unlit));
    CHECK(sky->as<std::string>(material::kBaseColorMap).ends_with("tex/leaf_albedo.png"));
    const material::TextureSampling skyMap = materials::sampling(*sky, material::kBaseColorMap);
    CHECK(skyMap.wrapU == material::TextureWrap::Clamp);
    CHECK(skyMap.wrapV == material::TextureWrap::Repeat);
    CHECK(skyMap.filter == material::TextureFilter::Nearest);
    CHECK(sky->as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Translucent));
    CHECK(sky->as<double>(material::kOpacity) == doctest::Approx(0.5));
    Node* neon = materials::find(graph(), "Neon");
    REQUIRE(neon);
    CHECK(neon->as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Unlit));
    CHECK(near(neon->as<glm::dvec3>(material::kBaseColor), {0.5, 1.0, 0.0}, 1e-6));
}

TEST_CASE("usd fixture: time-sampled points become a mesh cache; changing topology stays at rest")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("pointcache.usda");
    INFO(r.dump());
    REQUIRE(r["meshCaches"].size() == 1);
    CHECK(listed(r["warnings"], "/World/Growing: the topology changes over time"));
    CHECK(!listed(r["warnings"], "/World/Flag"));
    const NodeId flag = node("Flag").id();
    const NodeId cache = meshCache::find(graph(), flag);
    REQUIRE(cache != kNoNode);
    CHECK(graph().node(cache)->name() == r["meshCaches"][0].get<std::string>());
    CHECK(meshCache::find(graph(), node("Growing").id()) == kNoNode);
    const auto samples = meshCache::samples(graph(), cache);
    REQUIRE(samples);
    CHECK(samples->sampleCount() == 3);
    CHECK(samples->times.back() == doctest::Approx(1.0));

    auto corner = [&](double t) {
        graph().setTime(t);
        MeshPtr m = outMesh("Flag");
        REQUIRE(m);
        glm::dvec3 hi(-1e9);
        for (const glm::dvec3& p : m->positions())
            hi = glm::max(hi, p);
        return hi;
    };
    CHECK(corner(0.0).z == doctest::Approx(0.0));
    CHECK(corner(0.5).z == doctest::Approx(4.0));
    CHECK(corner(0.75).z == doctest::Approx(6.0));
    CHECK(corner(1.0).z == doctest::Approx(8.0));
    // Normals follow the moving points.
    graph().setTime(1.0);
    MeshPtr m = outMesh("Flag");
    auto normals = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(!normals.empty());
    CHECK(normals[0].x == doctest::Approx(-0.625).epsilon(0.01));
    graph().setTime(0.0);
    CHECK(r["end"].get<double>() == doctest::Approx(1.0));

    run("undo");
    CHECK(!graph().find("Flag"));
}

TEST_CASE("usd fixture: cameras, hidden and guide prims, holes, creases, primvars, and what is reported")
{
    UpAxisScope axis(math::UpAxis::Z);
    json r = importFixture("misc.usda");
    INFO(r.dump());
    CHECK(listed(r["lights"], "Key"));
    CHECK(listed(r["skipped"], "point cloud '/World/Dust'"));
    CHECK(graph().find("Key")->type().isA("light"));
    CHECK(listed(r["skipped"], "purpose guide"));
    CHECK(listed(r["skipped"], "model kind on 1 prim"));
    CHECK(listed(r["skipped"], "/World.userProperties:weight"));
    CHECK(!graph().find("Guides"));
    CHECK(!node("Hidden").as<bool>(Transform::kVisibility));

    MeshPtr crate = outMesh("Crate");
    REQUIRE(crate);
    CHECK(crate->numFaces() == 6);
    CHECK(crate->bounds().size().x == doctest::Approx(20.0));

    Node& shot = node("Shot");
    CHECK(shot.type().isA("camera"));
    CHECK(shot.as<double>(Camera::kHorizontalFilmAperture) == doctest::Approx(36.0 / 25.4));
    CHECK(shot.as<double>(Camera::kNearClipPlane) == doctest::Approx(1.0));
    CHECK(shot.as<double>(Camera::kFarClipPlane) == doctest::Approx(5000.0));
    CHECK(near(-glm::dvec3(world("Shot")[2]), {0, 1, 0}, 1e-5));
    graph().setTime(2.0);
    graph().evaluate();
    CHECK(std::get<double>(graph().value({shot.id(), Camera::kFocalLength, -1})) == doctest::Approx(85.0));
    graph().setTime(0.0);
    CHECK(anim::playback().fps == doctest::Approx(24.0));
    CHECK(anim::playback().maxTime == doctest::Approx(2.0));

    MeshPtr holey = outMesh("Holey");
    REQUIRE(holey);
    CHECK(holey->numFaces() == 2);
    CHECK(listed(r["warnings"], "hole face"));
    for (geo::Index f = 0; f < holey->faceCapacity(); ++f)
        if (holey->faceValid(f))
            CHECK(holey->faceNormal(f).z < 0.0);
    auto crease = holey->edgeAttrs().get<float>(geo::attr::kCrease);
    REQUIRE(!crease.empty());
    CHECK(crease[holey->findEdge(0, 1)] == doctest::Approx(2.5f));
    CHECK(crease[holey->findEdge(1, 2)] == doctest::Approx(2.5f));
    CHECK(node("Holey").as<int64_t>(MeshNode::kSmoothPreview) == 2);
    CHECK(holey->pointAttrs().get<float>("heat").size() == holey->pointCapacity());
    CHECK(holey->pointAttrs().get<float>("heat")[7] == doctest::Approx(7.0f));
    CHECK(geo::colors::has(*holey, "displayColor"));

    r = importFixture("misc.usda", {{"purposes", "all"}});
    CHECK(graph().find("Guides|GuideChild"));
}

TEST_CASE("usd fixture: a Y-up stage in metres lands upright in centimetres")
{
    UpAxisScope axis(math::UpAxis::Z);
    importFixture("yup_meters.usda");
    CHECK(near(glm::dvec3(world("Up")[3]), {0, 0, 200}, 1e-6));
    MeshPtr tri = outMesh("Tri");
    REQUIRE(tri);
    CHECK(glm::length(tri->position(1) - tri->position(0)) == doctest::Approx(100.0));
    CHECK(tri->position(2).z == doctest::Approx(100.0));
}
