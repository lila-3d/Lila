#include <doctest/doctest.h>

#include "TestUtil.h"
#include "bake/HeightNormal.h"
#include "ops/Command.h"

#include <cmath>
#include <filesystem>

using namespace lila;
using namespace baketest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
image::Image filled(int w, int h, glm::vec4 c)
{
    image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
    for (size_t i = 0; i < img.rgba.size(); i += 4)
        for (int k = 0; k < 4; ++k)
            img.rgba[i + size_t(k)] = c[k];
    return img;
}

glm::vec3 normalAt(const image::Image& img, int x, int y)
{
    const glm::vec4 t = texel(img, x, y);
    return glm::normalize(glm::vec3(t) * 2.0f - 1.0f);
}

/** A 32 x 32 height with a raised square from texel 8 to 23 (1 inside, 0 outside). */
image::Image step()
{
    image::Image h = filled(32, 32, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            h.rgba[(size_t(y) * 32 + size_t(x)) * 4] = 1.0f;
    return h;
}
} // namespace

TEST_CASE("height normals: a step's walls tilt away from it; flats face up")
{
    const image::Image n = bake::heightNormals(step(), {0.1, 0.1}, true);
    CHECK(normalAt(n, 2, 2).z == doctest::Approx(1.0f));
    CHECK(normalAt(n, 16, 16).z == doctest::Approx(1.0f));
    // Left wall leans -u, right wall +u; the top wall (row 8, v up) leans +v.
    CHECK(normalAt(n, 8, 16).x < -0.5f);
    CHECK(normalAt(n, 23, 16).x > 0.5f);
    CHECK(normalAt(n, 16, 8).y > 0.5f);
    // DirectX stores green the other way.
    CHECK(normalAt(bake::heightNormals(step(), {0.1, 0.1}, false), 16, 8).y < -0.5f);
}

TEST_CASE("detail normals: combine gives the normal back, and steep walls take the height's normal")
{
    const image::Image h = step();
    // A normal map that knows the tops and bottoms but missed the walls, with a little tilt everywhere for detail.
    const glm::vec3 tilt = glm::normalize(glm::vec3(0.15f, -0.1f, 1.0f));
    const image::Image flat = filled(32, 32, glm::vec4(tilt * 0.5f + 0.5f, 1.0f));

    const image::Image kept = bake::detailNormals(flat, h, {0.1, 0.1}, 0.0, true);
    const image::Image back = bake::combineNormals(kept, h, {0.1, 0.1}, true);
    for (int y = 0; y < 32; y += 3)
        for (int x = 0; x < 32; x += 3)
            CHECK(angleDeg(normalAt(back, x, y), tilt) < 1.0f);

    const image::Image faded = bake::detailNormals(flat, h, {0.1, 0.1}, 30.0, true);
    const image::Image rebuilt = bake::combineNormals(faded, h, {0.1, 0.1}, true);
    const image::Image shape = bake::heightNormals(h, {0.1, 0.1}, true);
    // Flat ground keeps the detail; on the wall the height's normal wins.
    CHECK(angleDeg(normalAt(rebuilt, 2, 2), tilt) < 1.0f);
    CHECK(angleDeg(normalAt(rebuilt, 8, 16), normalAt(shape, 8, 16)) < 1.0f);
    CHECK(angleDeg(normalAt(rebuilt, 8, 16), tilt) > 40.0f);
}

TEST_CASE("detailNormal bake: walls the projection misses come back from the height")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_detail_normal");
    // A 6 cm block with near vertical sides on the 100 cm plane, from x and z -19 to 19.
    auto f = [](double x, double z) { return std::abs(x) < 19.0 && std::abs(z) < 19.0 ? 6.0 : 0.0; };
    const std::string low = addMesh("block_low", plane(1));
    const std::string high = addMesh("block_high", displaced(200, f));
    const std::string p = project(dir, 64, 1, "exr");
    ops::execute("bakeGroup", {{"args", {"block"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 10.0}, {"rear", 10.0}});
    ops::execute("setBakeMap", {{"args", {"detailNormal"}}, {"project", p}, {"options", {{"fade", 30.0}, {"greenUp", true}}}});
    ops::execute("setBakeMap", {{"args", {"normal"}}, {"project", p}, {"options", {{"greenUp", true}}}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "height", "detailNormal"}}});
    const json& dOut = outputFor(r, "detailNormal");
    const json& hOut = outputFor(r, "height");
    CHECK(dOut["meta"]["uvSize"][0].get<double>() == doctest::Approx(100.0).epsilon(0.01));

    const image::Image detail = readImage(dOut), height = readImage(hOut), normal = readImage(outputFor(r, "normal"));
    // Heights as cm through the height map's range, over the 100 cm UV square.
    const double depth = 2.0 * hOut["meta"]["heightRange"].get<double>();
    const image::Image rebuilt = bake::combineNormals(detail, height, glm::dvec2(depth / 100.0), true);
    // Texel 20 (centre x = -18 cm) is the first on the block's top, texel 19 (x = -19.5 cm) is on the ground: the wall lies between.
    const int wall = 20;
    INFO("baked normal " << angleDeg(normalAt(normal, wall, 32), {0, 0, 1}) << ", rebuilt " << angleDeg(normalAt(rebuilt, wall, 32), {0, 0, 1}));
    CHECK(angleDeg(normalAt(normal, wall, 32), {0, 0, 1}) < 20.0f);
    CHECK(normalAt(rebuilt, wall, 32).x < -0.6f);
    CHECK(angleDeg(normalAt(detail, wall, 32), {0, 0, 1}) < 5.0f);
    // Flat ground and the block's top: the detail is the baked normal.
    for (const glm::ivec2 t : {glm::ivec2(4, 4), glm::ivec2(32, 32)})
        CHECK(angleDeg(normalAt(detail, t.x, t.y), normalAt(normal, t.x, t.y)) < 2.0f);
}
