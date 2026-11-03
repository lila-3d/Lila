#include <doctest/doctest.h>

#include "render/DrawBatches.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using namespace lila::render;

namespace
{
std::shared_ptr<RenderMesh> triangle()
{
    auto m = std::make_shared<RenderMesh>();
    m->vertices.resize(3);
    m->vertices[1].pos = {1, 0, 0};
    m->vertices[2].pos = {0, 1, 0};
    m->indices = {0, 1, 2};
    m->edgePoints = {{0, 0, 0}, {1, 0, 0}};
    return m;
}

MeshDraw drawAt(const std::shared_ptr<RenderMesh>& mesh, float x)
{
    MeshDraw d;
    d.mesh = mesh;
    d.world = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0, 0));
    return d;
}
} // namespace

TEST_CASE("Draw batches: draws sharing a mesh and materials become one instanced batch")
{
    auto shared = triangle();
    FrameState s;
    constexpr int kCopies = 1000;
    for (int i = 0; i < kCopies; ++i)
        s.meshes.push_back(drawAt(shared, float(i)));
    DrawBatches b = batchDraws(s);
    REQUIRE(b.shaded.size() == 1);
    CHECK(b.shaded[0].count == kCopies);
    CHECK(b.wire.empty());
    // Placements keep frame order inside the batch.
    CHECK(b.instances[b.shaded[0].first + 7].world[3].x == doctest::Approx(7.0f));

    // A different mesh, a different material or a different vertex colour display splits the batch.
    s.meshes.push_back(drawAt(triangle(), 0.0f));
    MeshDraw red = drawAt(shared, 1.0f);
    red.materials.push_back({});
    red.materials[0].baseColor = {1, 0, 0};
    s.meshes.push_back(red);
    MeshDraw red2 = red;
    s.meshes.push_back(red2);
    MeshDraw colors = drawAt(shared, 2.0f);
    colors.vertexColors = VertexColorMode::Unlit;
    s.meshes.push_back(colors);
    b = batchDraws(s);
    CHECK(b.shaded.size() == 4);
    CHECK(b.shaded[0].count == kCopies);
    CHECK(b.shaded[2].count == 2);

    // Unshaded and empty draws never reach the shaded batches.
    FrameState w;
    MeshDraw wireOnly = drawAt(shared, 0.0f);
    wireOnly.flags = DrawWire;
    w.meshes.push_back(wireOnly);
    w.meshes.push_back(MeshDraw{});
    b = batchDraws(w);
    CHECK(b.shaded.empty());
    CHECK(b.wire.size() == 1);
}

TEST_CASE("Draw batches: instance lists expand to placements under the draw's world")
{
    auto shared = triangle();
    FrameState s;
    MeshDraw d = drawAt(shared, 100.0f);
    auto list = std::make_shared<std::vector<glm::mat4>>();
    for (int i = 0; i < 5; ++i)
        list->push_back(glm::translate(glm::mat4(1.0f), glm::vec3(0, float(i), 0)));
    d.instances = list;
    s.meshes.push_back(d);
    s.meshes.push_back(drawAt(shared, 0.0f));
    DrawBatches b = batchDraws(s);
    REQUIRE(b.shaded.size() == 1);
    CHECK(b.shaded[0].count == 6);
    const glm::mat4& fourth = b.instances[b.shaded[0].first + 4].world;
    CHECK(fourth[3].x == doctest::Approx(100.0f));
    CHECK(fourth[3].y == doctest::Approx(4.0f));
}

TEST_CASE("Draw batches: wire overlays carry their colour per placement, highlighted last")
{
    auto shared = triangle();
    FrameState s;
    s.wireSelected = glm::vec4(1.0f);
    s.wireDefault = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    MeshDraw selected = drawAt(shared, 0.0f);
    selected.flags = DrawShaded | DrawSelected;
    MeshDraw plain = drawAt(shared, 0.0f);
    plain.flags = DrawShaded | DrawWire;
    MeshDraw tinted = drawAt(shared, 1.0f);
    tinted.flags = DrawShaded | DrawWire;
    tinted.wireColor = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);
    s.meshes = {selected, plain, tinted};
    DrawBatches b = batchDraws(s);
    REQUIRE(b.wire.size() == 1);
    REQUIRE(b.wire[0].count == 3);
    // A selected copy sitting exactly on an unselected one must win the depth tie, so it is drawn last.
    CHECK(b.instances[b.wire[0].first + 2].color == s.wireSelected);
    CHECK(b.instances[b.wire[0].first].color == s.wireDefault);
    CHECK(b.instances[b.wire[0].first + 1].color == tinted.wireColor);

    // A different width or depth mode needs its own draw.
    MeshDraw thick = plain;
    thick.wireWidth = 3.0f;
    MeshDraw xray = plain;
    xray.flags = DrawWire;
    s.meshes.push_back(thick);
    s.meshes.push_back(xray);
    CHECK(batchDraws(s).wire.size() == 3);
}

TEST_CASE("Draw batches: material lists compare field by field")
{
    std::vector<Material> a(2), b(2);
    CHECK(sameMaterials(a, b));
    b[1].style.toonThreshold = 0.5f;
    CHECK_FALSE(sameMaterials(a, b));
    b = a;
    b[0].baseColorMap = std::make_shared<TextureImage>();
    CHECK_FALSE(sameMaterials(a, b));
    a[0].baseColorMap = b[0].baseColorMap;
    CHECK(sameMaterials(a, b));
    CHECK_FALSE(sameMaterials(a, std::vector<Material>(1)));
}
