#include <doctest/doctest.h>

#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/Maps.h"
#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <filesystem>
#include <set>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
fs::path tempDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / ("lila_bakemaps_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

scene::Graph& graph() { return scene::Graph::current(); }

std::string addMesh(const std::string& name, geo::Mesh m)
{
    scene::Graph& g = graph();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n->path();
}

/** A 100 cm plane in XZ facing +Y; uv0 spans [uvMin, uvMax] (u along +x, v along -z). */
geo::Mesh plane(int subdiv = 1, glm::vec2 uvMin = {0.0f, 0.0f}, glm::vec2 uvMax = {1.0f, 1.0f}, double size = 100.0)
{
    geo::PlaneOptions o;
    o.width = o.depth = size;
    o.subdivX = o.subdivZ = subdiv;
    geo::Mesh m = geo::makePlane(o);
    for (glm::vec2& uv : m.cornerAttrs().get<glm::vec2>("uv0"))
        uv = uvMin + uv * (uvMax - uvMin);
    return m;
}

template <class F> geo::Mesh transformed(geo::Mesh m, F f)
{
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        m.setPosition(p, f(m.position(p)));
    return m;
}

template <class F> geo::Mesh displaced(int subdiv, F f)
{
    return transformed(plane(subdiv), [&](glm::dvec3 q) { return glm::dvec3(q.x, f(q.x, q.z), q.z); });
}

geo::Mesh lifted(geo::Mesh m, double y)
{
    return transformed(std::move(m), [&](glm::dvec3 q) { return q + glm::dvec3(0.0, y, 0.0); });
}

image::Image readImage(const json& out)
{
    std::string err;
    image::Image img = image::read(fs::path(out["file"].get<std::string>()), &err);
    INFO(err);
    REQUIRE(img.width > 0);
    return img;
}

glm::vec4 texel(const image::Image& img, int x, int y)
{
    const float* p = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
    return {p[0], p[1], p[2], p[3]};
}

const json& outputFor(const json& result, const std::string& map)
{
    for (const json& o : result["outputs"])
        if (o["map"] == map)
            return o;
    FAIL("no output for " << map);
    static json none;
    return none;
}

void newScene()
{
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
}

std::string project(const fs::path& dir, int res, int ss = 1, const std::string& format = "exr", const std::string& name = "{group}_{map}")
{
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p},         {"folder", dir.generic_string()}, {"resolution", res},
                                   {"supersampling", ss}, {"format", format},                {"name", name},
                                   {"padding", 4}});
    return p;
}

void group(const std::string& p, const std::string& name, const std::string& low, const std::vector<std::string>& highs, double frontal = 3.0,
           double rear = 3.0)
{
    ops::execute("bakeGroup", {{"args", {name}}, {"project", p}, {"lows", {low}}, {"highs", highs}, {"frontal", frontal}, {"rear", rear}});
}

void setMap(const std::string& p, const std::string& map, const json& options)
{
    ops::execute("setBakeMap", {{"args", {map}}, {"project", p}, {"options", options}});
}

float angleDeg(glm::vec3 a, glm::vec3 b) { return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f))); }

/** World xz of texel (x, y) on the 100 cm plane with uv 0-1. */
glm::dvec2 planeXz(int x, int y, int res)
{
    const double u = (x + 0.5) / res, v = 1.0 - (y + 0.5) / res;
    return {u * 100.0 - 50.0, 50.0 - v * 100.0};
}

double decodeCurvature(const image::Image& img, const json& out, int x, int y)
{
    return (texel(img, x, y).r - 0.5) * 2.0 * out["meta"]["curvatureRange"].get<double>();
}
} // namespace

TEST_CASE("curvature: 0.5 on flat, and the bump's mean curvature from the mesh and from the normals")
{
    newScene();
    const fs::path dir = tempDir("curv");
    const double A = 3.0, k = glm::two_pi<double>() / 100.0;
    auto f = [&](double x, double z) { return A * std::cos(k * x) + A * std::cos(k * z); };
    const std::string low = addMesh("bump_low", plane(1));
    const std::string high = addMesh("bump_high", displaced(200, f));
    const std::string flatHigh = addMesh("flat_high", lifted(plane(8), 1.0));
    const std::string p = project(dir, 64);
    group(p, "bump", low, {high}, 10.0, 10.0);
    setMap(p, "curvature", {{"range", 0.03}});
    setMap(p, "normalToCurvature", {{"range", 0.03}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"curvature", "normalToCurvature"}}});
    const json& meshOut = outputFor(r, "curvature");
    const json& nrmOut = outputFor(r, "normalToCurvature");
    CHECK(meshOut["meta"]["source"] == "mesh");
    CHECK(nrmOut["meta"]["source"] == "normals");
    const image::Image mesh = readImage(meshOut), nrm = readImage(nrmOut);
    const double peak = A * k * k;
    double worstMesh = 0.0, worstNrm = 0.0, sumMesh = 0.0;
    int n = 0;
    for (int y = 3; y < 61; ++y)
        for (int x = 3; x < 61; ++x)
        {
            const glm::dvec2 w = planeXz(x, y, 64);
            const double hx = -A * k * std::sin(k * w.x), hz = -A * k * std::sin(k * w.y);
            const double hxx = -A * k * k * std::cos(k * w.x), hzz = -A * k * k * std::cos(k * w.y);
            // Exact mean curvature of a height field (normal up, convex positive), and its small-slope -laplacian / 2.
            const double exact = -((1 + hz * hz) * hxx + (1 + hx * hx) * hzz) / (2.0 * std::pow(1 + hx * hx + hz * hz, 1.5));
            const double lap = -(hxx + hzz) / 2.0;
            const double em = std::abs(decodeCurvature(mesh, meshOut, x, y) - exact);
            worstMesh = std::max(worstMesh, em);
            sumMesh += em;
            worstNrm = std::max(worstNrm, std::abs(decodeCurvature(nrm, nrmOut, x, y) - lap));
            ++n;
        }
    INFO("peak " << peak << " mesh worst " << worstMesh << " normals worst " << worstNrm);
    CHECK(worstMesh < 0.1 * peak);
    CHECK(sumMesh / n < 0.03 * peak);
    CHECK(worstNrm < 0.1 * peak);
    // Convex crests are bright, troughs dark.
    CHECK(texel(mesh, 32, 32).r > 0.65f);
    CHECK(texel(mesh, 0, 0).r < 0.35f);

    // Flat: exactly neutral, even with the auto range.
    ops::execute("bakeGroup", {{"args", {"bump"}}, {"highs", {flatHigh}}});
    setMap(p, "curvature", json::object());
    const json flat = ops::execute("bake", {{"project", p}, {"maps", {"curvature", "normalToCurvature"}}});
    for (const char* map : {"curvature", "normalToCurvature"})
    {
        const image::Image img = readImage(outputFor(flat, map));
        for (int y = 0; y < 64; y += 5)
            for (int x = 0; x < 64; x += 5)
                CHECK(texel(img, x, y).r == doctest::Approx(0.5f).epsilon(1e-3));
    }
}

TEST_CASE("sphere: curvature 1/r, convexity and cavity, thickness 2r normalised")
{
    newScene();
    const fs::path dir = tempDir("sphere");
    geo::SphereOptions so;
    so.subdivAxis = so.subdivHeight = 32;
    const std::string low = addMesh("ball_low", geo::makeSphere(so));
    geo::IcoSphereOptions io;
    io.subdivisions = 5;
    const std::string high = addMesh("ball_high", geo::makeIcoSphere(io));
    const std::string p = project(dir, 32);
    group(p, "ball", low, {high});
    setMap(p, "curvature", {{"range", 0.04}});
    setMap(p, "convexity", {{"range", 0.04}});
    setMap(p, "cavity", {{"range", 0.04}});
    setMap(p, "thickness", {{"spread", 0.0}, {"rays", 4}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"curvature", "convexity", "cavity", "thickness"}}});
    const image::Image curv = readImage(outputFor(r, "curvature"));
    const image::Image convex = readImage(outputFor(r, "convexity"));
    const image::Image cavity = readImage(outputFor(r, "cavity"));
    const json& thOut = outputFor(r, "thickness");
    const image::Image th = readImage(thOut);
    // Straight through the centre: the diameter, so the auto range is 2r and every texel is white.
    CHECK(thOut["meta"]["thicknessRange"].get<double>() == doctest::Approx(100.0).epsilon(0.01));
    double sumK = 0.0;
    int n = 0;
    for (int y = 4; y < 28; ++y)
        for (int x = 0; x < 32; ++x)
        {
            sumK += decodeCurvature(curv, outputFor(r, "curvature"), x, y);
            ++n;
            CHECK(texel(convex, x, y).r == doctest::Approx(0.5f).epsilon(0.08));
            CHECK(texel(cavity, x, y).r == doctest::Approx(1.0f));
            CHECK(texel(th, x, y).r == doctest::Approx(1.0f).epsilon(0.01));
        }
    CHECK(sumK / n == doctest::Approx(1.0 / 50.0).epsilon(0.03));

    // A fixed distance normalises against it; the cosine spread averages chords of 2r cos(theta), 4r/3.
    setMap(p, "thickness", {{"spread", 0.0}, {"rays", 4}, {"distance", 200.0}});
    const image::Image half = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"thickness"}}}), "thickness"));
    CHECK(texel(half, 10, 16).r == doctest::Approx(0.5f).epsilon(0.01));
    setMap(p, "thickness", {{"rays", 256}, {"distance", 200.0}, {"spread", 180.0}});
    const image::Image spread = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"thickness"}}}), "thickness"));
    double mean = 0.0;
    n = 0;
    for (int y = 4; y < 28; ++y)
        for (int x = 0; x < 32; ++x, ++n)
            mean += texel(spread, x, y).r * 200.0;
    CHECK(mean / n == doctest::Approx(400.0 / 6.0).epsilon(0.03));
}

TEST_CASE("bent normals lean away from a wall and stay put in the open")
{
    newScene();
    const fs::path dir = tempDir("bent");
    const std::string low = addMesh("floor_low", plane(1));
    const std::string high = addMesh("floor_high", lifted(plane(4), 0.5));
    // A huge wall at x = -60 facing +x: it hides almost exactly the -x half of every floor point's hemisphere.
    const std::string wall = addMesh("wall_high", transformed(plane(1, {0, 0}, {1, 1}, 10000.0), [](glm::dvec3 q) {
                                         return glm::dvec3(q.y - 60.0, -q.x, q.z);
                                     }));
    const std::string p = project(dir, 16);
    group(p, "floor", low, {high, wall});
    setMap(p, "bentNormal", {{"rays", 256}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"bentNormal"}}});
    const image::Image img = readImage(outputFor(r, "bentNormal"));
    // E[d | d.x > 0] over the cosine hemisphere is (4 / (3 pi), 0, 2 / 3): 32.5 degrees toward +x (the tangent).
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x)
        {
            const glm::vec3 t = glm::vec3(texel(img, x, y)) * 2.0f - 1.0f;
            INFO(x << "," << y << " " << t.x << " " << t.y << " " << t.z);
            CHECK(t.x > 0.0f);
            CHECK(std::abs(t.y) < 0.08f);
            CHECK(angleDeg(t, glm::vec3(0, 0, 1)) == doctest::Approx(32.5f).epsilon(0.1));
        }

    setMap(p, "bentNormal", {{"rays", 64}, {"scope", "self"}, {"space", "object"}});
    const image::Image open = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"bentNormal"}}}), "bentNormal"));
    CHECK(angleDeg(glm::vec3(texel(open, 8, 8)) * 2.0f - 1.0f, glm::vec3(0, 1, 0)) < 1.0f);
}

TEST_CASE("rounded edges bevel a hard cube within the radius only")
{
    newScene();
    const fs::path dir = tempDir("rounded");
    const std::string low = addMesh("box_low", geo::makeCube());
    const std::string p = project(dir, 128);
    ops::execute("bakeGroup", {{"args", {"box"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"frontal", 2.0}, {"rear", 2.0}});
    const double radius = 20.0;
    setMap(p, "roundedNormal", {{"radius", radius}, {"rays", 64}, {"space", "object"}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"roundedNormal", "position", "alpha"}}});
    const json& posOut = outputFor(r, "position");
    const glm::vec3 lo(posOut["meta"]["boundsMin"][0].get<float>(), posOut["meta"]["boundsMin"][1].get<float>(), posOut["meta"]["boundsMin"][2].get<float>());
    const glm::vec3 hi(posOut["meta"]["boundsMax"][0].get<float>(), posOut["meta"]["boundsMax"][1].get<float>(), posOut["meta"]["boundsMax"][2].get<float>());
    const image::Image pos = readImage(posOut), nrm = readImage(outputFor(r, "roundedNormal")), cover = readImage(outputFor(r, "alpha"));
    int flat = 0, edge = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            if (texel(cover, x, y).r < 1.0f)
                continue;
            const glm::vec3 q = lo + glm::vec3(texel(pos, x, y)) * (hi - lo);
            const glm::vec3 aq = glm::abs(q);
            const int axis = aq.x > aq.y ? (aq.x > aq.z ? 0 : 2) : (aq.y > aq.z ? 1 : 2);
            if (aq[axis] < 49.0f)
                continue;
            glm::vec3 face(0.0f);
            face[axis] = q[axis] > 0.0f ? 1.0f : -1.0f;
            float edgeDist = 1e9f;
            for (int a = 0; a < 3; ++a)
                if (a != axis)
                    edgeDist = std::min(edgeDist, 50.0f - aq[a]);
            const float angle = angleDeg(glm::vec3(texel(nrm, x, y)) * 2.0f - 1.0f, face);
            INFO(q.x << " " << q.y << " " << q.z << " angle " << angle << " edge " << edgeDist);
            if (edgeDist > 1.5 * radius)
            {
                CHECK(angle < 2.0f);
                ++flat;
            }
            else if (edgeDist < 0.2 * radius)
            {
                CHECK(angle > 25.0f);
                CHECK(angle < 65.0f);
                ++edge;
            }
        }
    CHECK(flat > 100);
    CHECK(edge > 10);
}

TEST_CASE("vertex colour bakes a named colour set")
{
    newScene();
    const fs::path dir = tempDir("vcol");
    geo::Mesh hi = lifted(plane(8), 1.0);
    for (const char* set : {"first", "paint"})
    {
        std::span<glm::vec4> c = geo::colors::add(hi, set, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
        if (std::string(set) == "paint")
            for (geo::Index cn = 0; cn < hi.cornerCapacity(); ++cn)
                c[cn] = hi.position(hi.cornerPoint(cn)).x < 0.0 ? glm::vec4(1, 0, 0, 1) : glm::vec4(0, 0, 1, 1);
    }
    const std::string low = addMesh("v_low", plane(1));
    const std::string high = addMesh("v_high", std::move(hi));
    const std::string p = project(dir, 16, 1, "png");
    group(p, "v", low, {high});
    setMap(p, "vertexColor", {{"set", "paint"}});
    const image::Image img = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"vertexColor"}}}), "vertexColor"));
    CHECK(glm::vec3(texel(img, 1, 8)) == glm::vec3(1, 0, 0));
    CHECK(glm::vec3(texel(img, 14, 8)) == glm::vec3(0, 0, 1));
    setMap(p, "vertexColor", {{"set", ""}});
    const image::Image first = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"vertexColor"}}}), "vertexColor"));
    CHECK(glm::vec3(texel(first, 1, 8)) == glm::vec3(0, 1, 0));
}

TEST_CASE("material transfer: constants, textures, the high's normal map and ORM packing")
{
    newScene();
    const fs::path dir = tempDir("material");
    // A 4 x 4 base colour texture, red on the left half and green on the right.
    image::Image tex{4, 4, {}};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            tex.rgba.insert(tex.rgba.end(), {x < 2 ? 1.0f : 0.0f, x < 2 ? 0.0f : 1.0f, 0.0f, 0.5f});
    image::write(dir / "bc.png", tex, 8);
    // A normal map tilted 30 degrees toward +u, DirectX green.
    const glm::vec3 tilt(std::sin(glm::radians(30.0f)), 0.0f, std::cos(glm::radians(30.0f)));
    image::Image nm{2, 2, {}};
    for (int i = 0; i < 4; ++i)
        nm.rgba.insert(nm.rgba.end(), {tilt.x * 0.5f + 0.5f, 0.5f, tilt.z * 0.5f + 0.5f, 1.0f});
    image::write(dir / "n.png", nm, 16);

    ops::execute("createMaterial", {{"args", {"M_Const"}}, {"baseColor", {0.2, 0.4, 0.6}}, {"roughness", 0.7}, {"metallic", 1.0},
                                    {"emissive", {0.5, 0.0, 0.0}}, {"emissiveIntensity", 2.0}});
    ops::execute("createMaterial", {{"args", {"M_Tex"}}, {"baseColorMap", (dir / "bc.png").generic_string()}, {"blendMode", "masked"},
                                    {"opacity", 0.8}, {"roughness", 0.25}, {"normalMap", (dir / "n.png").generic_string()}});
    geo::Mesh constHigh = lifted(plane(4), 1.0);
    scene::materials::assign(constHigh, "M_Const", {});
    geo::Mesh texHigh = lifted(plane(4), 1.0);
    scene::materials::assign(texHigh, "M_Tex", {});
    const std::string lowA = addMesh("a_low", plane(1));
    const std::string lowB = addMesh("b_low", plane(1));
    const std::string hiA = addMesh("a_high", std::move(constHigh));
    const std::string hiB = addMesh("b_high", std::move(texHigh));
    const std::string p = project(dir, 16, 1, "png", "{group}_{map}");
    group(p, "a", lowA, {hiA});
    group(p, "b", lowB, {hiB});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"baseColor", "roughness", "metallic", "emissive", "materialOpacity", "orm", "normal"}}});

    auto file = [&](const std::string& map, const std::string& grp) {
        for (const json& o : r["outputs"])
            if (o["map"] == map && o["groups"][0] == grp)
                return readImage(o);
        FAIL("no " << map << " for " << grp);
        return image::Image{};
    };
    const float tol = 1.0f / 255.0f;
    const glm::vec3 bc = glm::vec3(texel(file("baseColor", "a"), 8, 8));
    CHECK(bc.r == doctest::Approx(0.4845f).epsilon(0.01));
    CHECK(bc.g == doctest::Approx(0.6650f).epsilon(0.01));
    CHECK(bc.b == doctest::Approx(0.7975f).epsilon(0.01));
    CHECK(std::abs(texel(file("roughness", "a"), 8, 8).r - 0.7f) <= tol);
    CHECK(std::abs(texel(file("metallic", "a"), 8, 8).r - 1.0f) <= tol);
    CHECK(texel(file("emissive", "a"), 8, 8).r == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(std::abs(texel(file("materialOpacity", "a"), 8, 8).r - 1.0f) <= tol);
    const image::Image bcB = file("baseColor", "b");
    CHECK(glm::vec3(texel(bcB, 3, 8)) == glm::vec3(1, 0, 0));
    CHECK(glm::vec3(texel(bcB, 12, 8)) == glm::vec3(0, 1, 0));
    CHECK(std::abs(texel(file("materialOpacity", "b"), 3, 8).r - 0.4f) <= tol);
    CHECK(std::abs(texel(file("roughness", "b"), 8, 8).r - 0.25f) <= tol);

    // ORM: open plane AO, the material's roughness and metallic; its sources aren't written twice.
    const image::Image orm = file("orm", "a");
    CHECK(std::abs(texel(orm, 8, 8).r - 1.0f) <= tol);
    CHECK(std::abs(texel(orm, 8, 8).g - 0.7f) <= tol);
    CHECK(std::abs(texel(orm, 8, 8).b - 1.0f) <= tol);
    CHECK(fs::path(outputFor(r, "orm")["file"].get<std::string>()).filename().string().find("ORM") != std::string::npos);

    // The high's own normal map: same frame as the low, so the bake reproduces it.
    const glm::vec3 nb = glm::vec3(texel(file("normal", "b"), 8, 8)) * 2.0f - 1.0f;
    CHECK(angleDeg(nb, tilt) < 1.0f);
    const glm::vec3 na = glm::vec3(texel(file("normal", "a"), 8, 8)) * 2.0f - 1.0f;
    CHECK(angleDeg(na, glm::vec3(0, 0, 1)) < 0.5f);

    // A custom pack: only it is written; its sources stay hidden.
    setMap(p, "pack", {{"channels", json::array({json{{"map", "roughness"}}, 0.25, json{{"map", "metallic"}, {"invert", true}}, json{{"map", "baseColor"}, {"channel", 1}}})}});
    const json packed = ops::execute("bake", {{"project", p}, {"groups", {"a"}}, {"maps", {"pack"}}});
    REQUIRE(packed["outputs"].size() == 1);
    const glm::vec4 pk = texel(readImage(packed["outputs"][0]), 8, 8);
    CHECK(std::abs(pk.r - 0.7f) <= tol);
    CHECK(std::abs(pk.g - 0.25f) <= tol);
    CHECK(std::abs(pk.b - 0.0f) <= tol);
    CHECK(pk.a == doctest::Approx(0.665f).epsilon(0.01));
    CHECK(packed["outputs"][0]["meta"]["channels"].size() == 4);
}

TEST_CASE("UV wireframe skips diagonals; island borders mark seams")
{
    newScene();
    const fs::path dir = tempDir("wire");
    geo::Mesh a = plane(4, {0.0f, 0.0f}, {0.5f, 1.0f});
    geo::Mesh b = plane(1, {0.5f, 0.0f}, {1.0f, 1.0f});
    a.append(transformed(std::move(b), [](glm::dvec3 q) { return q + glm::dvec3(200.0, 0.0, 0.0); }));
    const std::string low = addMesh("w_low", std::move(a));
    const std::string p = project(dir, 64, 4, "png");
    ops::execute("bakeGroup", {{"args", {"w"}}, {"project", p}, {"lows", {low}}, {"self", true}});
    setMap(p, "islandBorder", {{"width", 2.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"wireframe", "islandBorder"}}});
    const image::Image wire = readImage(outputFor(r, "wireframe"));
    const image::Image border = readImage(outputFor(r, "islandBorder"));
    // Island a: 4 x 4 quads of 8 x 16 texels. Grid lines at x = 0, 8, 16, ...; quad centres (and their diagonals) clear.
    CHECK(texel(wire, 8, 20).r > 0.4f);
    CHECK(texel(wire, 4, 24).r == 0.0f);
    CHECK(texel(wire, 12, 40).r == 0.0f);
    CHECK(texel(wire, 20, 16).r > 0.4f);
    // Island b is one quad: only its outline.
    CHECK(texel(wire, 48, 32).r == 0.0f);
    // Borders: island a's inner grid lines are not seams.
    CHECK(texel(border, 8, 20).r == 0.0f);
    CHECK(texel(border, 0, 20).r == 1.0f);
    CHECK(texel(border, 31, 20).r == 1.0f);
    CHECK(texel(border, 16, 32).r == 0.0f);
    CHECK(texel(border, 48, 32).r == 0.0f);
}

TEST_CASE("error map: misses, back faces, surfaces beyond the rays and degenerate frames")
{
    newScene();
    const fs::path dir = tempDir("error");
    // Left quarter: a good high. Next: a back-facing one. Next: one too deep for the rear distance. Right: nothing.
    auto strip = [](double x0, double x1, double y, bool flip) {
        geo::Mesh m = plane(2);
        return transformed(std::move(m), [&](glm::dvec3 q) {
            glm::dvec3 o((q.x + 50.0) / 100.0 * (x1 - x0) + x0, y, q.z);
            return flip ? glm::dvec3(o.x, y, -q.z) : o;
        });
    };
    geo::Mesh good = strip(-50, -25, 1.0, false);
    geo::Mesh back = strip(-25, 0, 1.0, true);
    geo::Mesh deep = strip(0, 25, -20.0, false);
    const std::string low = addMesh("e_low", plane(1));
    const std::string p = project(dir, 32, 1, "png");
    group(p, "e", low, {addMesh("good_high", std::move(good)), addMesh("back_high", std::move(back)), addMesh("deep_high", std::move(deep))});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"error"}}});
    const json& out = outputFor(r, "error");
    const image::Image img = readImage(out);
    CHECK(glm::vec3(texel(img, 4, 16)) == glm::vec3(0, 0, 0));
    CHECK(glm::vec3(texel(img, 12, 16)) == glm::vec3(1, 0, 1));
    CHECK(glm::vec3(texel(img, 20, 16)) == glm::vec3(1, 1, 0));
    CHECK(glm::vec3(texel(img, 28, 16)) == glm::vec3(1, 0, 0));
    const json& counts = out["meta"]["texels"];
    CHECK(counts["ok"].get<int>() > 200);
    CHECK(counts["backface"].get<int>() > 200);
    CHECK(counts["beyondDistance"].get<int>() > 200);
    CHECK(counts["miss"].get<int>() > 200);

    // A sample with no tangent frame is flagged before anything else.
    bake::registerBuiltinMaps();
    const bake::MapDef* def = bake::findMap("error");
    REQUIRE(def);
    bake::BakeInputs in;
    bake::GroupInputs g;
    bake::LowInput l;
    l.soup.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};
    l.soup.uvs = {{0, 0}, {1, 0}, {0, 1}};
    l.soup.materials = {0};
    g.lows.push_back(l);
    const bake::MapSettings ms{.map = "error"};
    const bake::MapContext ctx{ms, in, g, 0, nullptr, 32, 32, nullptr};
    bake::TexelSample s{};
    s.normal[1] = 1.0f;
    bake::HitRecord h{};
    h.flags = bake::kHitValid;
    glm::vec4 o;
    uint8_t v = 0;
    def->cpu(ctx, std::span(&s, 1), std::span(&h, 1), std::span(&o, 1), std::span(&v, 1));
    CHECK(glm::vec3(o) == glm::vec3(0, 1, 1));
    s.tangent[0] = 1.0f;
    def->cpu(ctx, std::span(&s, 1), std::span(&h, 1), std::span(&o, 1), std::span(&v, 1));
    CHECK(glm::vec3(o) == glm::vec3(0, 0, 0));
}

TEST_CASE("derived maps: gradient, normal to height and edge wear")
{
    newScene();
    const fs::path dir = tempDir("derived");
    const double A = 3.0, k = glm::two_pi<double>() / 100.0;
    auto f = [&](double x, double z) { return A * std::cos(k * x) + A * std::cos(k * z); };
    const std::string low = addMesh("bump_low", plane(1));
    const std::string high = addMesh("bump_high", displaced(200, f));
    const std::string p = project(dir, 64);
    group(p, "bump", low, {high}, 10.0, 10.0);
    setMap(p, "gradient", {{"direction", {1.0, 0.0, 0.0}}});
    setMap(p, "edgeWear", {{"range", 0.012}, {"threshold", 0.5}, {"softness", 0.2}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"gradient", "normalToHeight", "edgeWear"}}});

    const image::Image grad = readImage(outputFor(r, "gradient"));
    for (int x = 2; x < 64; x += 6)
        CHECK(texel(grad, x, 20).r == doctest::Approx((x + 0.5) / 64.0).epsilon(0.02));

    const json& hOut = outputFor(r, "normalToHeight");
    const image::Image h = readImage(hOut);
    const double range = hOut["meta"]["heightRange"].get<double>();
    double worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const glm::dvec2 w = planeXz(x, y, 64);
            worst = std::max(worst, std::abs((texel(h, x, y).r - 0.5) * 2.0 * range - f(w.x, w.y)));
        }
    INFO("worst " << worst);
    CHECK(worst < 0.05 * 4.0 * A);

    // Wear on the crests (curvature A k^2 at the centre), none in the troughs.
    const image::Image wear = readImage(outputFor(r, "edgeWear"));
    CHECK(texel(wear, 32, 32).r > 0.9f);
    CHECK(texel(wear, 0, 32).r < 0.05f);
    CHECK(texel(wear, 16, 16).r < 0.05f);
}

TEST_CASE("Painter and Unreal targets bake every map they name")
{
    for (const char* name : {"substancePainter", "unreal"})
    {
        const auto t = bake::findTarget(name);
        REQUIRE(t);
        std::vector<std::string> skipped;
        bake::applyTarget(bake::ProjectSettings::defaults(), *t, true, &skipped);
        INFO(name);
        CHECK(skipped.empty());
    }
}
