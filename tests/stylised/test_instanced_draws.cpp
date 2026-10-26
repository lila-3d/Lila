#include <doctest/doctest.h>

#include "render/Renderer.h"
#include "ui/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::render;

namespace
{
constexpr int kSize = 128;

bool gpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for instanced draw tests");
    return false;
}

/** A closed box, half size h, with face normals and wire edges. */
std::shared_ptr<RenderMesh> box(float h)
{
    auto m = std::make_shared<RenderMesh>();
    for (int axis = 0; axis < 3; ++axis)
        for (int sign = -1; sign <= 1; sign += 2)
        {
            glm::vec3 n(0.0f);
            n[axis] = float(sign);
            const glm::vec3 u = axis == 0 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 v = glm::cross(n, u);
            const uint32_t base = uint32_t(m->vertices.size());
            for (int c = 0; c < 4; ++c)
            {
                MeshVertex vert{};
                vert.pos = (n + u * ((c & 1) ? 1.0f : -1.0f) + v * ((c & 2) ? 1.0f : -1.0f)) * h;
                vert.normal = n;
                vert.tangent = glm::vec4(u, 1.0f);
                m->vertices.push_back(vert);
            }
            m->indices.insert(m->indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
            m->edgePoints.insert(m->edgePoints.end(), {m->vertices[base].pos, m->vertices[base + 1].pos, m->vertices[base + 1].pos,
                                                       m->vertices[base + 3].pos});
        }
    m->boundsMin = glm::vec3(-h);
    m->boundsMax = glm::vec3(h);
    return m;
}

FrameState frameAt(double frameRadius)
{
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.lighting = FrameState::Lighting::Studio;
    s.background = glm::vec3(0.0f);
    ui::Camera cam;
    cam.frame(glm::dvec3(0.0), frameRadius, 1.0);
    s.view = cam.view();
    s.proj = cam.proj(1.0);
    s.eye = cam.eye();
    return s;
}

std::vector<uint8_t> renderFrame(const FrameState& s, Renderer::FrameStats* stats = nullptr, int repeat = 1)
{
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, kSize, kSize, bgra, &err, stats, repeat), err);
    return bgra;
}

int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int m = 0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

/** Mean x of the lit pixels, in pixels, or -1 when nothing is lit. */
double litCentroidX(const std::vector<uint8_t>& bgra)
{
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            const uint8_t* p = &bgra[(size_t(y) * kSize + x) * 4];
            if (p[0] + p[1] + p[2] > 6)
            {
                sum += x;
                ++n;
            }
        }
    return n ? sum / n : -1.0;
}

/** A grid of side x side boxes, every draw sharing one mesh or each with its own copy of it. */
FrameState gridOfBoxes(int side, bool shareMesh)
{
    FrameState s = frameAt(side * 10.0);
    auto mesh = box(3.0f);
    for (int i = 0; i < side * side; ++i)
    {
        MeshDraw d;
        d.mesh = mesh;
        if (!shareMesh)
        {
            auto copy = std::make_shared<RenderMesh>(*mesh);
            copy->key = RenderMesh::nextKey();
            d.mesh = copy;
        }
        d.world = glm::translate(glm::mat4(1.0f), glm::vec3((i % side - side / 2) * 10.0f, 0.0f, (i / side - side / 2) * 10.0f));
        s.meshes.push_back(d);
    }
    return s;
}
} // namespace

TEST_CASE("Instanced draws: a shared mesh draws all its placements in one call, pixel for pixel")
{
    if (!gpu())
        return;
    constexpr int kSide = 10;
    Renderer::FrameStats shared, unique;
    const auto a = renderFrame(gridOfBoxes(kSide, true), &shared);
    const auto b = renderFrame(gridOfBoxes(kSide, false), &unique);
    CHECK(shared.meshDrawCalls == 1);
    CHECK(shared.meshInstances == kSide * kSide);
    CHECK(unique.meshDrawCalls == kSide * kSide);
    CHECK(unique.meshInstances == kSide * kSide);
    CHECK(maxDiff(a, b) == 0);
    MESSAGE("100 boxes: " << shared.meshDrawCalls << " draw call shared vs " << unique.meshDrawCalls << " unique");

    // The same placements given as one draw's instance list.
    FrameState listed = frameAt(kSide * 10.0);
    MeshDraw d;
    d.mesh = box(3.0f);
    auto list = std::make_shared<std::vector<glm::mat4>>();
    for (int i = 0; i < kSide * kSide; ++i)
        list->push_back(glm::translate(glm::mat4(1.0f), glm::vec3((i % kSide - kSide / 2) * 10.0f, 0.0f, (i / kSide - kSide / 2) * 10.0f)));
    d.instances = list;
    listed.meshes.push_back(d);
    Renderer::FrameStats listedStats;
    CHECK(maxDiff(renderFrame(listed, &listedStats), a) == 0);
    CHECK(listedStats.meshDrawCalls == 1);
}

TEST_CASE("Instanced draws: placements read their own world matrix")
{
    if (!gpu())
        return;
    // One box moved right of centre: a transposed matrix would lose the translation and leave it in the middle.
    FrameState s = frameAt(40.0);
    MeshDraw d;
    d.mesh = box(5.0f);
    d.world = glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, 0.0f));
    s.meshes.push_back(d);
    const double right = litCentroidX(renderFrame(s));
    s.meshes[0].world = glm::translate(glm::mat4(1.0f), glm::vec3(-20.0f, 0.0f, 0.0f));
    const double left = litCentroidX(renderFrame(s));
    REQUIRE(right >= 0.0);
    REQUIRE(left >= 0.0);
    CHECK(right > kSize * 0.6);
    CHECK(left < kSize * 0.4);
}

TEST_CASE("Instanced draws: wire, selection and translucency per placement")
{
    if (!gpu())
        return;
    FrameState s = gridOfBoxes(3, true);
    for (MeshDraw& d : s.meshes)
        d.flags = DrawShaded | DrawWire;
    s.meshes[4].flags |= DrawSelected;
    const auto shaded = renderFrame(s);
    // The same frame drawn per mesh copy matches, once its selected draw comes last as batches order highlighted wires.
    FrameState unique = gridOfBoxes(3, false);
    for (size_t i = 0; i < unique.meshes.size(); ++i)
        unique.meshes[i].flags = s.meshes[i].flags;
    std::rotate(unique.meshes.begin() + 4, unique.meshes.begin() + 5, unique.meshes.end());
    CHECK(maxDiff(shaded, renderFrame(unique)) == 0);

    // Translucent placements sort one by one, so they match the per-copy frame too.
    for (FrameState* f : {&s, &unique})
        for (MeshDraw& d : f->meshes)
        {
            Material m;
            m.flags |= MatTranslucent;
            m.opacity = 0.5f;
            d.materials = {m};
        }
    Renderer::FrameStats stats;
    CHECK(maxDiff(renderFrame(s, &stats), renderFrame(unique)) == 0);
    CHECK(stats.meshInstances == 9);
}

TEST_CASE("Instanced draws: timing for many placements of one mesh")
{
    if (!gpu())
        return;
    constexpr int kSide = 70; // 4900 boxes
    Renderer::FrameStats shared, unique;
    renderFrame(gridOfBoxes(kSide, true), &shared, 5);
    renderFrame(gridOfBoxes(kSide, false), &unique, 5);
    CHECK(shared.meshDrawCalls == 1);
    CHECK(unique.meshDrawCalls == kSide * kSide);
    MESSAGE("4900 boxes, CPU record ms: instanced " << shared.cpuMs << ", one draw each " << unique.cpuMs << "; GPU ms: " << shared.gpuMs << " vs "
                                                   << unique.gpuMs);
}
