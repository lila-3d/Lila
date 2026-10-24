#include <doctest/doctest.h>

#include "render/Renderer.h"
#include "ui/Camera.h"

#include <glm/gtc/constants.hpp>

#include <bit>
#include <cmath>
#include <cstdlib>
#include <random>

using namespace lila;
using namespace lila::render;

namespace
{
constexpr int kSize = 96;

bool gpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for stylised shading tests");
    return false;
}

/** A UV sphere of radius 50 at the origin, with tangents. */
std::shared_ptr<RenderMesh> sphere()
{
    auto mesh = std::make_shared<RenderMesh>();
    constexpr int kAround = 48, kUp = 24;
    for (int j = 0; j <= kUp; ++j)
        for (int i = 0; i <= kAround; ++i)
        {
            const float u = float(i) / kAround, v = float(j) / kUp;
            const float phi = u * glm::two_pi<float>(), theta = v * glm::pi<float>();
            const glm::vec3 n(std::sin(theta) * std::cos(phi), -std::cos(theta), std::sin(theta) * std::sin(phi));
            MeshVertex vert{};
            vert.pos = n * 50.0f;
            vert.normal = n;
            vert.tangent = glm::vec4(-std::sin(phi), 0.0f, std::cos(phi), 1.0f);
            vert.uv = {u, v};
            mesh->vertices.push_back(vert);
        }
    for (int j = 0; j < kUp; ++j)
        for (int i = 0; i < kAround; ++i)
        {
            const uint32_t a = uint32_t(j * (kAround + 1) + i), b = a + 1, c = a + kAround + 1, d = c + 1;
            mesh->indices.insert(mesh->indices.end(), {a, c, b, b, c, d});
        }
    mesh->boundsMin = glm::vec3(-50.0f);
    mesh->boundsMax = glm::vec3(50.0f);
    return mesh;
}

/** A smooth tileable stroke sheet: wavy normal tilt, shade and per-stroke random values. */
TexturePtr strokeSheet()
{
    auto t = std::make_shared<TextureImage>();
    t->width = t->height = 64;
    t->srgb = false;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> any(0, 255);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const float a = std::sin(float(x) * glm::two_pi<float>() / 16.0f), b = std::cos(float(y) * glm::two_pi<float>() / 8.0f);
            t->rgba.push_back(uint8_t(128 + 100 * a));
            t->rgba.push_back(uint8_t(128 + 100 * b));
            t->rgba.push_back(uint8_t(128 + 90 * a * b));
            t->rgba.push_back(uint8_t(any(rng)));
        }
    return t;
}

FrameState frame(const Material& mat, int msaa = 1)
{
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = msaa;
    s.lighting = FrameState::Lighting::Studio;
    s.background = glm::vec3(0.0f);
    ui::Camera cam;
    cam.frame(glm::dvec3(0.0), 55.0, 1.0);
    s.view = cam.view();
    s.proj = cam.proj(1.0);
    s.eye = cam.eye();
    MeshDraw d;
    d.mesh = sphere();
    d.materials = {mat};
    s.meshes.push_back(d);
    return s;
}

std::vector<uint8_t> renderFrame(const FrameState& s)
{
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, kSize, kSize, bgra, &err), err);
    REQUIRE(bgra.size() == size_t(kSize) * kSize * 4);
    return bgra;
}

int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int m = 0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

double meanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += std::abs(int(a[i]) - int(b[i]));
    return sum / double(a.size());
}

/** BGRA at (x, y). */
glm::ivec4 at(const std::vector<uint8_t>& bgra, int x, int y)
{
    const uint8_t* p = &bgra[(size_t(y) * kSize + size_t(x)) * 4];
    return {p[0], p[1], p[2], p[3]};
}

int litPixels(const std::vector<uint8_t>& bgra)
{
    int n = 0;
    for (size_t i = 0; i < bgra.size(); i += 4)
        n += bgra[i] + bgra[i + 1] + bgra[i + 2] > 6;
    return n;
}

Material base()
{
    Material m;
    m.baseColor = glm::vec3(0.5f);
    m.roughness = 0.5f;
    return m;
}

Material painterly(float strength)
{
    Material m = base();
    m.style.model = ShadingModel::Painterly;
    m.style.painterlyStrength = strength;
    m.style.strokeScale = 25.0f;
    m.style.strokeMap = strokeSheet();
    return m;
}
} // namespace

TEST_CASE("Packed style blocks put each model's inputs where mesh.slang reads them")
{
    MaterialStyle s;
    s.model = ShadingModel::Painterly;
    s.strokeScale = 20.0f;
    s.paletteCount = 9;
    auto block = packStyle(s, 7, glm::vec3(-1.0f), glm::vec3(2.0f), true);
    CHECK(block[0] == 1u);
    CHECK(block[1] == 7u);
    CHECK(std::bit_cast<float>(block[4]) == doctest::Approx(0.05f));
    CHECK(std::bit_cast<float>(block[15]) == doctest::Approx(4.0f)); // palette count clamps to 4
    CHECK(std::bit_cast<float>(block[27]) == doctest::Approx(2.0f)); // Z up
    CHECK(std::bit_cast<float>(block[29]) == doctest::Approx(2.0f));
}

TEST_CASE("Painterly at strength 0 shades exactly like default lit")
{
    if (!gpu())
        return;
    const auto lit = renderFrame(frame(base()));
    REQUIRE(litPixels(lit) > kSize * kSize / 4);
    const auto zero = renderFrame(frame(painterly(0.0f)));
    CHECK(maxDiff(lit, zero) <= 1);
    const auto full = renderFrame(frame(painterly(1.0f)));
    CHECK(meanDiff(lit, full) > 1.0);
    CHECK(litPixels(full) > kSize * kSize / 4);

    SUBCASE("with multisampling")
    {
        CHECK(maxDiff(renderFrame(frame(base(), 4)), renderFrame(frame(painterly(0.0f), 4))) <= 1);
    }
}

TEST_CASE("Painterly strokes follow the mapping and boil with the offset")
{
    if (!gpu())
        return;
    const auto object = renderFrame(frame(painterly(1.0f)));
    // World mapping matches object mapping while the object sits at the origin; moved, only object strokes follow it.
    {
        Material world = painterly(1.0f);
        world.style.strokeMapping = 1;
        FrameState a = frame(painterly(1.0f)), b = frame(world);
        a.meshes[0].world = b.meshes[0].world = glm::mat4(1.0f, 0, 0, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f, 0, 7.0f, 3.0f, 0, 1.0f);
        CHECK(meanDiff(renderFrame(a), renderFrame(b)) > 0.2);
    }
    for (uint32_t mapping : {2u, 3u})
    {
        Material m = painterly(1.0f);
        m.style.strokeMapping = mapping;
        const auto other = renderFrame(frame(m));
        CHECK(litPixels(other) > kSize * kSize / 4);
        CHECK(meanDiff(object, other) > 0.2);
    }
    Material boiled = painterly(1.0f);
    boiled.style.boilOffset = {0.37f, 0.61f};
    CHECK(meanDiff(object, renderFrame(frame(boiled))) > 0.2);
    // Same offset, same frame: boiling is deterministic.
    CHECK(maxDiff(renderFrame(frame(boiled)), renderFrame(frame(boiled))) == 0);
}

TEST_CASE("Toon shading tints its shadow and differs from default lit")
{
    if (!gpu())
        return;
    const auto lit = renderFrame(frame(base()));
    Material m = base();
    m.style.model = ShadingModel::Toon;
    m.style.toonSoftness = 0.01f;
    m.style.rimStrength = 0.0f;
    const auto toon = renderFrame(frame(m));
    CHECK(meanDiff(lit, toon) > 2.0);
    // The default shadow colour is blue-grey: somewhere on the sphere blue clearly beats red.
    int bluish = 0;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            glm::ivec4 p = at(toon, x, y);
            bluish += p.x > p.z + 15;
        }
    CHECK(bluish > 50);
}

TEST_CASE("Foliage grades the base colour from bottom to top")
{
    if (!gpu())
        return;
    Material m = base();
    m.style.model = ShadingModel::Foliage;
    m.style.foliageGradient = 1.0f;
    m.style.foliageBottomColor = glm::vec3(0.02f, 0.05f, 0.02f);
    m.style.foliageTopColor = glm::vec3(0.2f, 0.8f, 0.1f);
    const auto leaf = renderFrame(frame(m));
    CHECK(meanDiff(renderFrame(frame(base())), leaf) > 2.0);
    int green = 0;
    for (int y = 0; y < kSize / 3; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            glm::ivec4 p = at(leaf, x, y);
            green += p.y > p.z + 10 && p.y > p.x + 10;
        }
    CHECK(green > 50);
}

TEST_CASE("Viewport post: Kuwahara and outlines change the shaded view, with and without MSAA")
{
    if (!gpu())
        return;
    for (int msaa : {1, 4})
    {
        CAPTURE(msaa);
        FrameState s = frame(painterly(1.0f), msaa);
        const auto plain = renderFrame(s);
        s.kuwahara = true;
        const auto filtered = renderFrame(s);
        CHECK(meanDiff(plain, filtered) > 0.05);
        CHECK(litPixels(filtered) > kSize * kSize / 4);

        s.kuwahara = false;
        s.outline = true;
        s.outlineWidth = 2.0f;
        s.background = glm::vec3(0.5f);
        FrameState grey = s;
        grey.outline = false;
        const auto noLines = renderFrame(grey);
        const auto lines = renderFrame(s);
        // The silhouette against the grey background turns near-black.
        int dark = 0, darkBefore = 0;
        for (size_t i = 0; i < lines.size(); i += 4)
        {
            dark += lines[i] + lines[i + 1] + lines[i + 2] < 120;
            darkBefore += noLines[i] + noLines[i + 1] + noLines[i + 2] < 120;
        }
        CHECK(dark > darkBefore + 40);
    }
}

