#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/ImageIO.h"
#include "fbx/Fbx.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/TextureDeformers.h"
#include "scene/Transform.h"

#include <algorithm>
#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

MeshPtr outMesh(const std::string& mesh)
{
    Graph& g = Graph::current();
    g.evaluate();
    Node* n = g.find(mesh);
    REQUIRE(n);
    MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return m;
}

std::string tempFile(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_deform_tests";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

image::Image flat(int w, int h, glm::vec4 c)
{
    image::Image img;
    img.width = w;
    img.height = h;
    for (int i = 0; i < w * h; ++i)
        img.rgba.insert(img.rgba.end(), {c.r, c.g, c.b, c.a});
    return img;
}

double plug(const std::string& node, const char* attr)
{
    Graph& g = Graph::current();
    g.evaluate();
    Node* n = g.find(node);
    REQUIRE(n);
    return std::get<double>(g.value({n->id(), n->type().attrIndex(attr), -1}));
}
} // namespace

TEST_CASE("sampleUv is bilinear with v up and wraps; rasterizeByUv fills the islands and pads them")
{
    image::Image img = flat(2, 1, glm::vec4(0.0f));
    img.rgba[4] = 1.0f; // right texel red
    CHECK(deformers::sampleUv(img, {0.25, 0.5}).r == doctest::Approx(0.0));
    CHECK(deformers::sampleUv(img, {0.75, 0.5}).r == doctest::Approx(1.0));
    CHECK(deformers::sampleUv(img, {0.5, 0.5}).r == doctest::Approx(0.5));
    CHECK(deformers::sampleUv(img, {1.25, 0.5}).r == doctest::Approx(0.0));

    image::Image tall = flat(1, 2, glm::vec4(0.0f));
    tall.rgba[0] = 1.0f; // row 0 is the top, v = 1
    CHECK(deformers::sampleUv(tall, {0.5, 0.75}).r == doctest::Approx(1.0));

    run("newScene");
    std::string plane = run("polyPlane", {{"name", "Sheet"}, {"width", 10}, {"depth", 10}, {"subdivisionsX", 2}, {"subdivisionsZ", 2}})[0];
    MeshPtr m = outMesh(plane);
    std::vector<glm::vec4> colours(m->pointCapacity(), glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));
    image::Image r = deformers::rasterizeByUv(*m, "uv0", colours, 32, 32);
    REQUIRE(r.width == 32);
    CHECK(r.rgba[(16 * 32 + 16) * 4 + 1] == doctest::Approx(0.5f));
    CHECK(deformers::rasterizeByUv(*m, "nope", colours, 32, 32).rgba.empty());
}

TEST_CASE("textureDeformer displaces along the normal by an image and is identity at mid level")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"name", "Sheet"}, {"width", 10}, {"depth", 10}, {"subdivisionsX", 3}, {"subdivisionsZ", 3}})[0];
    MeshPtr before = outMesh(plane);
    const std::string file = tempFile("texture_deformer.png");
    image::write(file, flat(8, 8, glm::vec4(1.0f)), 8);
    std::string d = run("textureDeformer", {{"args", {plane}}, {"image", file}, {"strength", 2.0}, {"midLevel", 0.5}})[0];
    MeshPtr after = outMesh(plane);
    REQUIRE(after->pointCapacity() == before->pointCapacity());
    for (geo::Index p = 0; p < after->pointCapacity(); ++p)
    {
        glm::dvec3 moved = after->position(p) - before->position(p);
        CHECK(std::abs(std::abs(moved.y) - 1.0) < 1e-5);
        CHECK(std::abs(moved.x) + std::abs(moved.z) < 1e-6);
    }
    run("setAttr", {{"args", {d + ".midLevel", 1.0}}});
    MeshPtr still = outMesh(plane);
    for (geo::Index p = 0; p < still->pointCapacity(); ++p)
        CHECK(glm::length(still->position(p) - before->position(p)) < 1e-6);

    run("setAttr", {{"args", {d + ".direction", 1}}});
    run("setAttr", {{"args", {d + ".midLevel", 0.0}}});
    MeshPtr vec = outMesh(plane);
    CHECK(glm::length(vec->position(0) - before->position(0) - glm::dvec3(2.0)) < 1e-5);
}

TEST_CASE("tension outputs, Unreal curves and the baked mask")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"name", "Brow"}, {"width", 10}, {"depth", 10}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("skinCluster", {{"args", {"root", plane}}});
    std::string t = run("tension", {{"args", {plane}}})[0];
    CHECK(plug(t, "outStretch") == doctest::Approx(0.0));
    CHECK(deformers::tensionCurves(Graph::current()).empty());
    run("setAttr", {{"args", {t + ".stretchCurve", "brow_stretch"}}});
    run("setAttr", {{"args", {t + ".compressCurve", "brow_compress"}}});
    REQUIRE(deformers::tensionCurves(Graph::current()).size() == 2);

    Graph& g = Graph::current();
    const double fps = anim::playback().fps;
    const NodeId root = g.find("root")->id();
    anim::setKey(g, {root, Transform::kScale, 0}, 0.0, 1.0);
    anim::setKey(g, {root, Transform::kScale, 0}, 10.0 / fps, 1.5);
    g.setTime(10.0 / fps);
    CHECK(plug(t, "outStretch") > 0.1);
    CHECK(plug(t, "outCompress") == doctest::Approx(0.0));

    fbx::ExportOptions o;
    o.kind = fbx::ExportKind::Animation;
    fbx::ExportScene s = fbx::gatherExport(g, {g.find(plane)->id(), root}, o);
    auto it = std::find(s.curveNames.begin(), s.curveNames.end(), "brow_stretch");
    REQUIRE(it != s.curveNames.end());
    REQUIRE(s.take);
    const auto& samples = s.take->namedCurves[size_t(it - s.curveNames.begin())];
    REQUIRE(samples.size() > 1);
    CHECK(samples.front() == doctest::Approx(0.0));
    CHECK(*std::max_element(samples.begin(), samples.end()) > 0.1);

    g.setTime(0.0);
    const std::string file = tempFile("tension_mask.png");
    std::filesystem::remove(file);
    json baked = run("bakeTension", {{"args", {plane}}, {"start", 0}, {"end", 10}, {"file", file}, {"resolution", 64}});
    CHECK(baked["frames"] == 11);
    CHECK(baked["maxStretch"].get<double>() > 0.1);
    CHECK(std::filesystem::exists(file));
    CHECK(g.time() == doctest::Approx(0.0));
    MeshPtr base = std::get<MeshPtr>(g.value(modifiers::editPlug(g, g.find(plane)->id())));
    REQUIRE(geo::colors::has(*base, "tensionMask"));
    std::vector<glm::vec4> mask = geo::colors::pointColors(*base, "tensionMask");
    CHECK(std::any_of(mask.begin(), mask.end(), [](const glm::vec4& c) { return c.r > 0.1f; }));
    run("undo");
    base = std::get<MeshPtr>(g.value(modifiers::editPlug(g, g.find(plane)->id())));
    CHECK_FALSE(geo::colors::has(*base, "tensionMask"));
}

TEST_CASE("bakeSurfacePin makes a joint under the dominant influence at the pin")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"name", "Body"}, {"width", 20}, {"depth", 20}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(-10, 0, 0)}, {"name", "hip"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "knee"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "ankle"}});
    run("skinCluster", {{"args", {"hip", plane}}});
    json pins = run("surfacePin", {{"args", {plane}}, {"mode", "nearest"}, {"position", v3(6, 0, 2)}});
    REQUIRE(pins.size() == 1);
    Graph& g = Graph::current();
    g.evaluate();
    const glm::dmat4 pinWorld = g.find(pins[0].get<std::string>())->as<glm::dmat4>(Transform::kWorldMatrix);
    json joints = run("bakeSurfacePin", {{"args", pins}});
    REQUIRE(joints.size() == 1);
    g.evaluate();
    Node* j = g.find(joints[0].get<std::string>());
    REQUIRE(j);
    REQUIRE(g.parent(j->id()) != kNoNode);
    CHECK(g.node(g.parent(j->id()))->name() == "knee");
    const glm::dmat4 jw = j->as<glm::dmat4>(Transform::kWorldMatrix);
    CHECK(glm::length(glm::dvec3(jw[3]) - glm::dvec3(pinWorld[3])) < 1e-6);
    CHECK(glm::length(glm::dvec3(jw[2]) - glm::dvec3(pinWorld[2])) < 1e-6);

    std::string loose = run("polyPlane", {{"name", "Loose"}})[0];
    json loosePin = run("surfacePin", {{"args", {loose}}, {"mode", "nearest"}});
    CHECK_THROWS(run("bakeSurfacePin", {{"args", loosePin}}));
}
