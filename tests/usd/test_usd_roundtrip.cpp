// Lila scenes exported with exportUsd and read back with importUsd: what goes out comes back.

#include "UsdTestUtil.h"

#include "curve/CurveNodes.h"
#include "curve/Spline.h"
#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/Camera.h"
#include "scene/LodGroup.h"
#include "scene/Morphs.h"
#include "scene/Skin.h"
#include "scene/Take.h"

#include <algorithm>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;

namespace
{
/** A pentagonal prism (two n-gons, five quads) with two UV sets, two color sets, two material slots and a crease. */
geo::Mesh decoratedPrism()
{
    std::vector<glm::dvec3> pts;
    for (int ring = 0; ring < 2; ++ring)
        for (int k = 0; k < 5; ++k)
        {
            const double a = glm::radians(72.0 * k);
            pts.push_back({30.0 * std::cos(a), ring * 40.0, 30.0 * std::sin(a)});
        }
    std::vector<geo::Index> sizes{5, 5}, idx{4, 3, 2, 1, 0, 5, 6, 7, 8, 9};
    for (int k = 0; k < 5; ++k)
    {
        const geo::Index a = geo::Index(k), b = geo::Index((k + 1) % 5);
        sizes.push_back(4);
        idx.insert(idx.end(), {a, b, b + 5, a + 5});
    }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    geo::updateCornerNormals(m);
    auto uv0 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    auto uv1 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(1));
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
    {
        const glm::dvec3 p = m.position(m.cornerPoint(c));
        uv0[c] = glm::vec2(float(p.x / 60.0 + 0.5), float(p.y / 40.0));
        uv1[c] = glm::vec2(float(c) * 0.01f, 0.25f);
    }
    auto paint = geo::colors::add(m, "paint");
    auto dirt = geo::colors::add(m, "dirt");
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
    {
        paint[c] = glm::vec4(float(c % 3) * 0.5f, 0.2f, 0.9f, 1.0f);
        dirt[c] = glm::vec4(0.1f, float(c % 4) * 0.25f, 0.0f, 0.5f);
    }
    geo::colors::setCurrent(m, "paint");
    m.materialNames() = {"Brick", "Glass"};
    auto mat = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        mat[f] = f < 2 ? 1 : 0;
    auto crease = m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
    crease[m.findEdge(0, 1)] = 1.5f;
    return m;
}

void buildDecorated()
{
    fresh();
    Graph& g = graph();
    Node* group = g.createNode(*types::find("transform"), "Group", kNoNode, false);
    g.setValue({group->id(), Transform::kTranslate, -1}, glm::dvec3(10, 20, 30));
    g.setValue({group->id(), Transform::kRotate, -1}, glm::dvec3(10, 45, -20));
    g.setValue({group->id(), Transform::kRotateOrder, -1}, int64_t(RotateOrder::ZXY));
    g.setValue({group->id(), Transform::kScale, -1}, glm::dvec3(1, 2, 1));
    Node* house = addMesh("House", decoratedPrism(), group->id());
    g.setValue({house->id(), Transform::kTranslate, -1}, glm::dvec3(0, 5, 0));
    g.createNode(*types::find("transform"), "Empty", group->id(), false);

    const fs::path tex = tempDir() / "src_tex";
    fs::create_directories(tex);
    for (const char* f : {"glass_base.png", "glass_n.png", "glass_mro.png", "glass_e.png"})
        std::ofstream(tex / f) << "png";
    Node* brick = materials::create(g, "Brick");
    g.setValue({brick->id(), material::kBaseColor, -1}, glm::dvec3(0.6, 0.25, 0.1));
    g.setValue({brick->id(), material::kMetallic, -1}, 0.2);
    g.setValue({brick->id(), material::kRoughness, -1}, 0.7);
    g.setValue({brick->id(), material::kSpecular, -1}, 0.3);
    g.setValue({brick->id(), material::kEmissiveColor, -1}, glm::dvec3(0.1, 0.0, 0.0));
    g.setValue({brick->id(), material::kEmissiveIntensity, -1}, 2.0);
    Node* glass = materials::create(g, "Glass");
    auto path = [&](const char* f) { return (tex / f).generic_string(); };
    g.setValue({glass->id(), material::kBaseColorMap, -1}, path("glass_base.png"));
    g.setValue({glass->id(), material::kNormalMap, -1}, path("glass_n.png"));
    g.setValue({glass->id(), material::kOrmMap, -1}, path("glass_mro.png"));
    g.setValue({glass->id(), material::kPackedLayout, -1}, int64_t(material::PackedLayout::MRO));
    g.setValue({glass->id(), material::kEmissiveMap, -1}, path("glass_e.png"));
    g.setValue({glass->id(), material::kEmissiveIntensity, -1}, 3.0);
    g.setValue({glass->id(), material::kBlendMode, -1}, int64_t(material::BlendMode::Translucent));
    g.setValue({glass->id(), material::kOpacity, -1}, 0.3);
    g.setValue({glass->id(), material::kTwoSided, -1}, true);
    g.setValue({glass->id(), material::kUvSet, -1}, int64_t(1));
}

/** Per face, in order: its world points, normals, both UVs, both colors and its slot. */
struct FaceData
{
    std::vector<glm::dvec3> points;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uv0, uv1;
    std::vector<glm::vec4> paint, dirt;
    std::string slot;
};

std::vector<FaceData> faceData(const std::string& name)
{
    MeshPtr m = outMesh(name);
    REQUIRE(m);
    const glm::dmat4 w = world(name);
    const glm::dmat3 nm = glm::inverse(glm::transpose(glm::dmat3(w)));
    std::vector<FaceData> out;
    auto normals = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    auto uv0 = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    auto uv1 = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
    auto paint = geo::colors::values(*m, "paint");
    auto dirt = geo::colors::values(*m, "dirt");
    auto mat = m->faceAttrs().get<int32_t>(geo::attr::kMaterial);
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
    {
        if (!m->faceValid(f))
            continue;
        FaceData d;
        m->forEachFaceCorner(f, [&](geo::Index c) {
            d.points.push_back(glm::dvec3(w * glm::dvec4(m->position(m->cornerPoint(c)), 1.0)));
            if (!normals.empty())
                d.normals.push_back(glm::vec3(glm::normalize(nm * glm::dvec3(normals[c]))));
            if (!uv0.empty())
                d.uv0.push_back(uv0[c]);
            if (!uv1.empty())
                d.uv1.push_back(uv1[c]);
            if (!paint.empty())
                d.paint.push_back(paint[c]);
            if (!dirt.empty())
                d.dirt.push_back(dirt[c]);
        });
        d.slot = materials::slotName(*m, mat.empty() ? 0 : mat[f]);
        out.push_back(std::move(d));
    }
    return out;
}

void compareFaces(const std::vector<FaceData>& a, const std::vector<FaceData>& b)
{
    REQUIRE(a.size() == b.size());
    for (size_t f = 0; f < a.size(); ++f)
    {
        REQUIRE(a[f].points.size() == b[f].points.size());
        REQUIRE(a[f].normals.size() == b[f].normals.size());
        REQUIRE(a[f].uv0.size() == b[f].uv0.size());
        REQUIRE(a[f].uv1.size() == b[f].uv1.size());
        REQUIRE(a[f].paint.size() == b[f].paint.size());
        REQUIRE(a[f].dirt.size() == b[f].dirt.size());
        CHECK(a[f].slot == b[f].slot);
        for (size_t k = 0; k < a[f].points.size(); ++k)
        {
            CHECK(near(a[f].points[k], b[f].points[k], 1e-3));
            CHECK(near(glm::dvec3(a[f].normals[k]), glm::dvec3(b[f].normals[k]), 1e-4));
            CHECK(near(glm::dvec3(a[f].uv0[k], 0.0), glm::dvec3(b[f].uv0[k], 0.0), 1e-6));
            CHECK(near(glm::dvec3(a[f].uv1[k], 0.0), glm::dvec3(b[f].uv1[k], 0.0), 1e-6));
            CHECK(glm::length(a[f].paint[k] - b[f].paint[k]) < 1e-6f);
            CHECK(glm::length(a[f].dirt[k] - b[f].dirt[k]) < 1e-6f);
        }
    }
}

std::map<std::string, json> materialValues(const std::string& name)
{
    std::map<std::string, json> out;
    Node* n = materials::find(graph(), name);
    REQUIRE_MESSAGE(n, name);
    for (size_t a = 0; a < n->type().attrs.size(); ++a)
        out[n->type().attrs[a].name] = scene::toJson(n->value(int(a)));
    return out;
}

void compareMaterial(const std::map<std::string, json>& a, const std::map<std::string, json>& b)
{
    for (const auto& [key, value] : a)
    {
        INFO(key);
        const json& other = b.at(key);
        if (value.is_string() && !value.get<std::string>().empty())
        {
            REQUIRE(other.is_string());
            std::error_code ec;
            CHECK(fs::equivalent(fs::path(value.get<std::string>()), fs::path(other.get<std::string>()), ec));
        }
        else if (value.is_number_float())
            CHECK(other.get<double>() == doctest::Approx(value.get<double>()).epsilon(1e-5));
        else if (value.is_array())
            for (size_t i = 0; i < value.size(); ++i)
                CHECK(other[i].get<double>() == doctest::Approx(value[i].get<double>()).epsilon(1e-5));
        else
            CHECK(value == other);
    }
}
} // namespace

TEST_CASE("usd round trip: hierarchy, transforms, n-gons, normals, UV and color sets, slots, creases and materials")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        UpAxisScope axis(up);
        buildDecorated();
        const auto faces = faceData("House");
        const glm::dmat4 groupWorld = world("Group"), emptyWorld = world("Empty");
        const auto brick = materialValues("Brick"), glass = materialValues("Glass");
        const fs::path file = tempDir() / "decorated.usda";
        run("exportUsd", {{"args", {file.generic_string(), "Group"}}});
        const std::string text = readAll(file);
        CHECK(text.find("upAxis = \"Z\"") != std::string::npos);
        CHECK(text.find("metersPerUnit = 0.01") != std::string::npos);
        CHECK(text.find("UsdPreviewSurface") != std::string::npos);
        CHECK(text.find("familyName = \"materialBind\"") != std::string::npos);
        CHECK(text.find("primvars:st1") != std::string::npos);
        CHECK(text.find("displayColor") != std::string::npos);

        fresh();
        json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
        INFO(r.dump());
        CHECK(r["warnings"].empty());
        REQUIRE(graph().find("Group"));
        CHECK(graph().parent(node("House").id()) == node("Group").id());
        CHECK(graph().parent(node("Empty").id()) == node("Group").id());
        CHECK(near(world("Group"), groupWorld, 1e-4));
        CHECK(near(world("Empty"), emptyWorld, 1e-4));
        CHECK(node("Group").as<int64_t>(Transform::kRotateOrder) == int64_t(RotateOrder::ZXY));
        CHECK(near(node("Group").as<glm::dvec3>(Transform::kRotate), glm::dvec3(10, 45, -20), 1e-4));
        compareFaces(faces, faceData("House"));

        MeshPtr m = outMesh("House");
        CHECK(geo::colors::current(*m) == "paint");
        auto crease = m->edgeAttrs().get<float>(geo::attr::kCrease);
        REQUIRE(!crease.empty());
        CHECK(crease[m->findEdge(0, 1)] == doctest::Approx(1.5f));
        CHECK(std::count_if(crease.begin(), crease.end(), [](float c) { return c > 0.0f; }) == 1);

        compareMaterial(brick, materialValues("Brick"));
        compareMaterial(glass, materialValues("Glass"));
    }
}

TEST_CASE("usd encodings: usda, usdc, usd and usdz")
{
    buildDecorated();
    const auto faces = faceData("House");
    struct Case
    {
        const char* name;
        json flags;
        const char* magic;
    };
    const Case cases[] = {{"enc.usda", json::object(), "#usda"},
                          {"enc.usdc", json::object(), "PXR-USDC"},
                          {"enc.usd", json::object(), "PXR-USDC"},
                          {"enc_text.usd", {{"format", "usda"}}, "#usda"},
                          {"enc.usdz", json::object(), "PK"}};
    for (const Case& c : cases)
    {
        INFO(c.name);
        buildDecorated();
        const fs::path file = tempDir() / c.name;
        json call = c.flags;
        call["args"] = {file.generic_string(), "Group"};
        run("exportUsd", call);
        CHECK(readAll(file).rfind(c.magic, 0) == 0);
        fresh();
        run("importUsd", {{"args", {file.generic_string()}}});
        compareFaces(faces, faceData("House"));
        if (std::string(c.name).ends_with(".usdz"))
        {
            // Textures went into the package and come back out beside it.
            const std::string base = materialValues("Glass").at("baseColorMap").get<std::string>();
            CHECK(base.find("enc_usdz") != std::string::npos);
            CHECK(fs::exists(fs::path(base)));
            CHECK(readAll(file).find("textures/glass_base.png") != std::string::npos);
        }
    }
}

TEST_CASE("usd up axis and units")
{
    UpAxisScope axis(math::UpAxis::Z);
    buildDecorated();
    const auto faces = faceData("House");
    struct Case
    {
        const char* up;
        double mpu;
        const char* expectUp;
        const char* expectUnits;
    };
    for (const Case& c : {Case{"z", 0.01, "upAxis = \"Z\"", "metersPerUnit = 0.01"}, Case{"y", 1.0, "upAxis = \"Y\"", "metersPerUnit = 1"},
                          Case{"y", 0.001, "upAxis = \"Y\"", "metersPerUnit = 0.001"}})
    {
        INFO(c.up << " " << c.mpu);
        buildDecorated();
        const fs::path file = tempDir() / "axes.usda";
        run("exportUsd", {{"args", {file.generic_string(), "Group"}}, {"upAxis", c.up}, {"metersPerUnit", c.mpu}});
        const std::string text = readAll(file);
        CHECK(text.find(c.expectUp) != std::string::npos);
        CHECK(text.find(c.expectUnits) != std::string::npos);
        fresh();
        run("importUsd", {{"args", {file.generic_string()}}});
        compareFaces(faces, faceData("House"));
    }
}

namespace
{
const std::vector<std::string> kJoints = {"bone_a", "bone_b", "bone_c"};

/** The FBX skeletal test's rig: a cylinder on a three-joint chain with jointOrients and a rotate order, animated. */
void buildSkinned()
{
    fresh();
    run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
    run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 12}, {"caps", false}});
    run("setAttr", {{"args", {"Tube.translate", json::array({3, 0, 0})}}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", json::array({3, -50, 0})}, {"name", "bone_a"}, {"orientation", json::array({0, 0, 90})}});
    run("joint", {{"position", json::array({3, 0, 0})}, {"name", "bone_b"}, {"orientation", json::array({10, 0, -20})}});
    run("joint", {{"position", json::array({3, 50, 0})}, {"name", "bone_c"}, {"orientation", json::array({0, 15, 0})}});
    run("setAttr", {{"args", {"bone_b.rotateOrder", 1}}});
    run("skinCluster", {{"args", {"bone_a", "Tube"}}});
    Graph& g = graph();
    auto key = [&](const char* joint, int attr, int axis, double frame, double value) {
        anim::setKey(g, {node(joint).id(), attr, axis}, frame / 24.0, value);
    };
    key("bone_a", Transform::kTranslate, 0, 0, node("bone_a").as<glm::dvec3>(Transform::kTranslate).x);
    key("bone_a", Transform::kTranslate, 0, 24, 40);
    key("bone_b", Transform::kRotate, 2, 0, 0);
    key("bone_b", Transform::kRotate, 2, 10, 70);
    key("bone_b", Transform::kRotate, 2, 24, -35);
    key("bone_c", Transform::kRotate, 1, 0, 0);
    key("bone_c", Transform::kRotate, 1, 24, 120);
    key("bone_c", Transform::kScale, 1, 0, 1);
    key("bone_c", Transform::kScale, 1, 24, 1.5);
}

struct Frame
{
    std::map<std::string, glm::dmat4> joints;
    std::vector<glm::dvec3> points;
};

std::vector<Frame> sample()
{
    Graph& g = graph();
    std::vector<Frame> out;
    for (int f = 0; f <= 24; f += 3)
    {
        g.setTime(f / 24.0);
        g.evaluate();
        Frame fr;
        for (const std::string& j : kJoints)
            fr.joints[j] = node(j).as<glm::dmat4>(Transform::kWorldMatrix);
        MeshPtr m = node("Tube").as<MeshPtr>(MeshNode::kOutMesh);
        REQUIRE(m);
        const glm::dmat4 w = node("Tube").as<glm::dmat4>(Transform::kWorldMatrix);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                fr.points.push_back(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)));
        out.push_back(std::move(fr));
    }
    g.setTime(0.0);
    g.evaluate();
    return out;
}

void compareFrames(const std::vector<Frame>& a, const std::vector<Frame>& b, double eps)
{
    REQUIRE(a.size() == b.size());
    double worstJoint = 0.0, worstPoint = 0.0;
    for (size_t f = 0; f < a.size(); ++f)
    {
        for (const auto& [name, m] : a[f].joints)
            for (int c = 0; c < 4; ++c)
                worstJoint = std::max(worstJoint, glm::length(m[c] - b[f].joints.at(name)[c]));
        REQUIRE(a[f].points.size() == b[f].points.size());
        for (size_t p = 0; p < a[f].points.size(); ++p)
            worstPoint = std::max(worstPoint, glm::length(a[f].points[p] - b[f].points[p]));
    }
    MESSAGE("worst joint matrix column error " << worstJoint << ", worst skinned point error " << worstPoint << " cm");
    CHECK(worstJoint < eps);
    CHECK(worstPoint < eps);
}
} // namespace

TEST_CASE("usd round trip: skeleton, skin weights and joint animation")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        UpAxisScope axis(up);
        buildSkinned();
        const std::vector<Frame> before = sample();
        const fs::path file = tempDir() / "skinned.usda";
        run("exportUsd", {{"args", {file.generic_string()}}, {"selectionOnly", false}});
        const std::string text = readAll(file);
        CHECK(text.find("def SkelRoot") != std::string::npos);
        CHECK(text.find("def Skeleton") != std::string::npos);
        CHECK(text.find("primvars:skel:jointIndices") != std::string::npos);
        CHECK(text.find("def SkelAnimation") != std::string::npos);
        CHECK(text.find("timeCodesPerSecond = 24") != std::string::npos);

        fresh();
        json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
        INFO(r.dump());
        CHECK(r["skinClusters"].size() == 1);
        CHECK(r["joints"].size() == 3);
        CHECK(anim::playback().fps == doctest::Approx(24.0));
        CHECK(anim::playback().maxTime == doctest::Approx(1.0));
        compareFrames(before, sample(), 1e-3);
    }
}

TEST_CASE("usd round trip: blend shapes with in-betweens and animated weights")
{
    fresh();
    Graph& g = graph();
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", 1.0}});
    geo::Mesh plane = geo::makePlane({.width = 100, .depth = 100, .subdivX = 2, .subdivZ = 2});
    Node* mesh = addMesh("Face", std::move(plane));
    const NodeId morph = morphs::create(g, mesh->id(), "Morph");
    auto full = std::make_shared<MorphDeltas>();
    full->indices = {0, 4};
    full->deltas = {{0, 10, 0}, {0, 20, 5}};
    const NodeId smile = morphs::addTarget(g, morph, "Smile", full, 0.0);
    auto half = std::make_shared<MorphDeltas>();
    half->indices = {0, 4};
    half->deltas = {{0, 2, 0}, {3, 4, 0}};
    morphs::setInBetween(g, smile, 0.5, half);
    anim::setKey(g, {smile, morphs::kWeight, -1}, 0.0, 0.0);
    anim::setKey(g, {smile, morphs::kWeight, -1}, 1.0, 1.0);
    auto points = [&](double t) {
        g.setTime(t);
        g.evaluate();
        std::vector<glm::dvec3> out;
        MeshPtr m = node("Face").as<MeshPtr>(MeshNode::kOutMesh);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                out.push_back(m->position(p));
        return out;
    };
    // On frames: the export samples every frame and USD interpolates linearly between them.
    std::vector<std::vector<glm::dvec3>> before;
    for (double t : {0.0, 0.2, 0.5, 0.8, 1.0})
        before.push_back(points(t));

    const fs::path file = tempDir() / "shapes.usda";
    run("exportUsd", {{"args", {file.generic_string(), "Face"}}});
    const std::string text = readAll(file);
    CHECK(text.find("def BlendShape \"Smile\"") != std::string::npos);
    CHECK(text.find("inbetweens:") != std::string::npos);
    CHECK(text.find("blendShapeWeights.timeSamples") != std::string::npos);

    fresh();
    json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    INFO(r.dump());
    const NodeId m2 = morphs::find(graph(), node("Face").id());
    REQUIRE(m2 != kNoNode);
    const auto targets = morphs::targetNodes(graph(), m2);
    REQUIRE(targets.size() == 1);
    CHECK(graph().node(targets[0])->as<std::string>(morphs::kMorphName) == "Smile");
    CHECK(morphs::targets(graph(), m2).size() == 2);
    size_t k = 0;
    for (double t : {0.0, 0.2, 0.5, 0.8, 1.0})
    {
        INFO("t " << t);
        const auto after = points(t);
        REQUIRE(after.size() == before[k].size());
        for (size_t p = 0; p < after.size(); ++p)
            CHECK(near(after[p], before[k][p], 1e-4));
        ++k;
    }
}

TEST_CASE("usd round trip: camera lens, film back, clipping and animation")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        UpAxisScope axis(up);
        fresh();
        Graph& g = graph();
        Node* cam = g.createNode(*types::find("camera"), "ShotCam", kNoNode, false);
        g.setValue({cam->id(), Camera::kFocalLength, -1}, 50.0);
        g.setValue({cam->id(), Camera::kHorizontalFilmAperture, -1}, 1.2);
        g.setValue({cam->id(), Camera::kVerticalFilmAperture, -1}, 0.6);
        g.setValue({cam->id(), Camera::kNearClipPlane, -1}, 2.0);
        g.setValue({cam->id(), Camera::kFarClipPlane, -1}, 20000.0);
        g.setValue({cam->id(), Camera::kFilmFit, -1}, int64_t(2));
        g.setValue({cam->id(), Transform::kTranslate, -1}, glm::dvec3(100, 50, -300));
        g.setValue({cam->id(), Transform::kRotate, -1}, glm::dvec3(-10, 160, 5));
        anim::setKey(g, {cam->id(), Camera::kFocalLength, -1}, 0.0, 50.0);
        anim::setKey(g, {cam->id(), Camera::kFocalLength, -1}, 1.0, 100.0);
        run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", 1.0}});
        const glm::dmat4 w = world("ShotCam");
        const fs::path file = tempDir() / "camera.usda";
        run("exportUsd", {{"args", {file.generic_string(), "ShotCam"}}});
        CHECK(readAll(file).find("def Camera") != std::string::npos);

        fresh();
        run("importUsd", {{"args", {file.generic_string()}}});
        Node& c = node("ShotCam");
        CHECK(c.type().isA("camera"));
        CHECK(near(world("ShotCam"), w, 1e-4));
        CHECK(c.as<double>(Camera::kHorizontalFilmAperture) == doctest::Approx(1.2).epsilon(1e-5));
        CHECK(c.as<double>(Camera::kVerticalFilmAperture) == doctest::Approx(0.6).epsilon(1e-5));
        CHECK(c.as<double>(Camera::kNearClipPlane) == doctest::Approx(2.0).epsilon(1e-5));
        CHECK(c.as<double>(Camera::kFarClipPlane) == doctest::Approx(20000.0).epsilon(1e-5));
        CHECK(c.as<int64_t>(Camera::kFilmFit) == 2);
        graph().setTime(0.5);
        graph().evaluate();
        CHECK(std::get<double>(graph().value({c.id(), Camera::kFocalLength, -1})) == doctest::Approx(75.0).epsilon(1e-3));
        graph().setTime(0.0);
    }
}

TEST_CASE("usd round trip: curves")
{
    fresh();
    Graph& g = graph();
    curve::Spline poly;
    poly.kind = curve::Kind::Poly;
    poly.points = {{0, 0, 0}, {10, 0, 0}, {10, 5, 3}};
    curve::Spline bez;
    bez.kind = curve::Kind::Bezier;
    bez.closed = true;
    bez.points = {{0, 0, 0}, {10, 0, 0}, {5, 8, 0}};
    bez.normalize();
    curve::Spline nurbs;
    nurbs.kind = curve::Kind::Nurbs;
    nurbs.degree = 3;
    nurbs.points = {{0, 0, 0}, {5, 10, 0}, {10, -5, 2}, {15, 0, 0}, {20, 4, 1}};
    nurbs.weights = {1, 2, 0.5, 1, 1};
    const std::vector<std::pair<std::string, curve::Spline>> made = {{"PolyCurve", poly}, {"BezierLoop", bez}, {"Nurbs", nurbs}};
    for (const auto& [name, s] : made)
    {
        Node* n = g.createNode(*types::find("curve"), name, kNoNode, false);
        g.setValue({n->id(), curve::CurveShape::kCreate, -1}, curve::makeCurveData({s}));
    }
    const fs::path file = tempDir() / "curves.usda";
    run("exportUsd", {{"args", {file.generic_string()}}, {"selectionOnly", false}});
    const std::string text = readAll(file);
    CHECK(text.find("def BasisCurves \"PolyCurve\"") != std::string::npos);
    CHECK(text.find("def NurbsCurves \"Nurbs\"") != std::string::npos);

    fresh();
    run("importUsd", {{"args", {file.generic_string()}}});
    for (const auto& [name, s] : made)
    {
        INFO(name);
        const curve::Splines& back = curve::shapeSplines(graph(), node(name).id());
        REQUIRE(back.size() == 1);
        const int spans = curve::spanCount(s);
        CHECK(curve::spanCount(back[0]) == spans);
        for (int k = 0; k <= 8; ++k)
        {
            const double u = spans * k / 8.0;
            CHECK(near(curve::pointAt(back[0], u), curve::pointAt(s, u), 1e-3));
        }
    }
}

TEST_CASE("usd takes as variants")
{
    fresh();
    Graph& g = graph();
    Node* ball = addMesh("Ball", geo::makeSphere({.radius = 10}));
    run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}});
    anim::setKey(g, {ball->id(), Transform::kTranslate, 0}, 0.0, 0.0);
    anim::setKey(g, {ball->id(), Transform::kTranslate, 0}, 1.0, 100.0);
    run("createTake", {{"name", "Roll"}, {"start", 0.0}, {"end", 1.0}, {"fps", 24.0}});
    run("createTake", {{"name", "Bounce"}, {"start", 0.0}, {"end", 2.0}, {"fps", 24.0}, {"activate", true}});
    anim::setKey(g, {ball->id(), Transform::kTranslate, 1}, 0.0, 0.0);
    anim::setKey(g, {ball->id(), Transform::kTranslate, 1}, 1.0, 50.0);
    anim::setKey(g, {ball->id(), Transform::kTranslate, 1}, 2.0, 0.0);
    run("setActiveTake", {{"args", {"Roll"}}});

    const fs::path file = tempDir() / "takes.usda";
    run("exportUsd", {{"args", {file.generic_string(), "Ball"}}, {"takes", "all"}});
    const std::string text = readAll(file);
    CHECK(text.find("variantSet \"take\"") != std::string::npos);

    auto translateAt = [&](double t) {
        graph().setTime(t);
        graph().evaluate();
        return node("Ball").as<glm::dvec3>(Transform::kTranslate);
    };
    fresh();
    json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    CHECK(r["take"] == "Roll");
    CHECK(r["takes"].size() == 2);
    CHECK(translateAt(0.5).x == doctest::Approx(50.0));
    CHECK(translateAt(0.5).y == doctest::Approx(0.0));
    CHECK(anim::playback().maxTime == doctest::Approx(1.0));

    fresh();
    r = run("importUsd", {{"args", {file.generic_string()}}, {"take", "Bounce"}, {"report", true}});
    CHECK(r["take"] == "Bounce");
    CHECK(translateAt(1.0).y == doctest::Approx(50.0));
    CHECK(translateAt(1.0).x == doctest::Approx(0.0));
    CHECK(anim::playback().maxTime == doctest::Approx(2.0));
    graph().setTime(0.0);
}

TEST_CASE("usd split layers")
{
    buildDecorated();
    const auto faces = faceData("House");
    const fs::path file = tempDir() / "split.usda";
    json r = run("exportUsd", {{"args", {file.generic_string(), "Group"}}, {"splitLayers", true}, {"report", true}});
    CHECK(r["files"].size() == 4);
    for (const char* part : {"split.geometry.usda", "split.materials.usda", "split.anim.usda"})
        CHECK(fs::exists(tempDir() / part));
    CHECK(readAll(file).find("subLayers") != std::string::npos);
    CHECK(readAll(tempDir() / "split.geometry.usda").find("def Mesh") != std::string::npos);
    CHECK(readAll(tempDir() / "split.materials.usda").find("def Material") != std::string::npos);
    fresh();
    run("importUsd", {{"args", {file.generic_string()}}});
    compareFaces(faces, faceData("House"));
}

TEST_CASE("usd LOD groups as Unreal's LOD variant set")
{
    fresh();
    Graph& g = graph();
    Node* group = g.createNode(*types::find("lodGroup"), "Rock", kNoNode, false);
    g.setValue({group->id(), LodGroup::kScreenSizes, -1}, lod::formatSizes({1.0, 0.25}));
    addMesh("Rock_hi", geo::makeSphere({.radius = 50, .subdivAxis = 24, .subdivHeight = 12}), group->id());
    addMesh("Rock_lo", geo::makeSphere({.radius = 50, .subdivAxis = 8, .subdivHeight = 4}), group->id());
    const fs::path file = tempDir() / "lods.usda";
    run("exportUsd", {{"args", {file.generic_string(), "Rock"}}});
    const std::string text = readAll(file);
    CHECK(text.find("variantSet \"LOD\"") != std::string::npos);
    CHECK(text.find("def Mesh \"LOD1\"") != std::string::npos);

    fresh();
    json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    INFO(r.dump());
    Node& rock = node("Rock");
    CHECK(lod::isGroup(rock));
    const std::vector<NodeId> levels = lod::levels(graph(), rock.id());
    REQUIRE(levels.size() == 2);
    CHECK(graph().node(levels[0])->name() == "Rock_hi");
    CHECK(graph().node(levels[1])->name() == "Rock_lo");
    CHECK(outMesh("Rock_hi")->numFaces() > outMesh("Rock_lo")->numFaces());
    CHECK(lod::parseSizes(rock.as<std::string>(LodGroup::kScreenSizes)) == std::vector<double>{1.0, 0.25});
}

TEST_CASE("usd round trip: morphs on a skinned mesh, and a node parented to a joint")
{
    buildSkinned();
    Graph& g = graph();
    const NodeId morph = morphs::create(g, node("Tube").id(), "Morph");
    auto bulge = std::make_shared<MorphDeltas>();
    for (uint32_t p = 0; p < 20; ++p)
    {
        bulge->indices.push_back(p);
        bulge->deltas.push_back({0, 0, 4.0});
    }
    const NodeId target = morphs::addTarget(g, morph, "Bulge", bulge, 0.0);
    anim::setKey(g, {target, morphs::kWeight, -1}, 0.0, 0.0);
    anim::setKey(g, {target, morphs::kWeight, -1}, 1.0, 1.0);
    Node* socket = g.createNode(*types::find("transform"), "SOCKET_hand", node("bone_c").id(), false);
    g.setValue({socket->id(), Transform::kTranslate, -1}, glm::dvec3(5, 10, 0));
    std::vector<glm::dmat4> socketBefore;
    for (int f = 0; f <= 24; f += 6)
    {
        g.setTime(f / 24.0);
        socketBefore.push_back(world("SOCKET_hand"));
    }
    g.setTime(0.0);
    const std::vector<Frame> before = sample();

    const fs::path file = tempDir() / "skinned_morph.usda";
    json r = run("exportUsd", {{"args", {file.generic_string()}}, {"selectionOnly", false}, {"report", true}});
    INFO(r.dump());
    const std::string text = readAll(file);
    CHECK(text.find("def BlendShape \"Bulge\"") != std::string::npos);
    CHECK(text.find("uniform token[] blendShapes = [\"Bulge\"]") != std::string::npos);

    fresh();
    r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    INFO(r.dump());
    CHECK(morphs::find(graph(), node("Tube").id()) != kNoNode);
    compareFrames(before, sample(), 1e-3);
    size_t k = 0;
    for (int f = 0; f <= 24; f += 6)
    {
        graph().setTime(f / 24.0);
        CHECK(near(world("SOCKET_hand"), socketBefore[k++], 1e-3));
    }
    graph().setTime(0.0);
}

extern bool gUsdLoadedWithExtension;

TEST_CASE("usd: the extension loads without OpenUSD, which loads on the first command")
{
    CHECK_FALSE(gUsdLoadedWithExtension);
    fresh();
    run("polyCube", {{"name", "Lazy"}});
    const fs::path path = fs::temp_directory_path() / "lila_usd_tests" / "lazy.usda";
    run("exportUsd", {{"args", {path.string(), "Lazy"}}});
    CHECK(fs::exists(path));
}
