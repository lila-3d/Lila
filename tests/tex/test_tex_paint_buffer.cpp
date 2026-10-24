#include "TexTest.h"

#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "tex/BrushPresets.h"
#include "tex/PaintBuffer.h"
#include "tex/PaintParticles.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"

using namespace textest;
using namespace lila::tex::paint;

namespace
{
const float* px(const image::Image& img, int x, int y) { return &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4]; }

brush::FalloffTable smooth() { return brush::FalloffTable(brush::Falloff::fromName("smooth")); }

/** Two quads side by side with their own UV islands, and a third sharing the second's uvs along an edge. */
std::shared_ptr<const PaintMesh> twoIslands()
{
    static geo::Mesh m = [] {
        const std::vector<glm::dvec3> pts = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}, {20, 0, 0}, {20, 0, 10}};
        const std::vector<geo::Index> sizes = {4, 4}, idx = {0, 3, 2, 1, 1, 2, 5, 4};
        geo::Mesh mesh = geo::Mesh::fromPolygons(pts, sizes, idx);
        auto uv = mesh.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (geo::Index f = 0; f < 2; ++f)
            for (geo::Index c : mesh.faceCorners(f))
            {
                const glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
                // The face's own island: face 1 is moved apart in u.
                uv[c] = {float(0.05 + (p.x - 10.0 * f) / 25.0 + 0.5 * f), float(0.1 + p.z / 25.0)};
            }
        return mesh;
    }();
    return PaintMesh::build(m, std::make_shared<const geo::MeshBvh>(m), "");
}
} // namespace

TEST_CASE("projection buffer: painting, erasing, placement and warps")
{
    ProjectionBuffer b;
    b.resize(64, 32);
    CHECK(b.empty());
    b.paint({{16.0f, 16.0f}, 6.0f, 1.0f, {}}, smooth(), {1.0f, 0.0f, 0.0f});
    CHECK_FALSE(b.empty());
    const image::Image a = b.resolved();
    CHECK(px(a, 16, 16)[3] > 0.95f);
    CHECK(px(a, 16, 16)[0] == doctest::Approx(1.0f));
    CHECK(px(a, 40, 16)[3] == 0.0f);

    // Moved 20 pixels right: the dab shows there, and baking keeps it there.
    b.placement.offset = {20.0f, 0.0f};
    CHECK(b.transformed());
    const image::Image moved = b.resolved();
    CHECK(px(moved, 36, 16)[3] > 0.9f);
    CHECK(px(moved, 16, 16)[3] < 0.05f);
    b.bake();
    CHECK_FALSE(b.transformed());
    CHECK(px(b.resolved(), 36, 16)[3] > 0.9f);

    // A warp point dragged moves the content near it.
    b.warpPoint(2, 2) = {8.0f, 0.0f};
    CHECK(b.resolved().rgba != b.pixels());

    b.erase({{36.0f, 16.0f}, 8.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("constant")));
    b.bake();
    CHECK(px(b.resolved(), 36, 16)[3] < 0.05f);

    // Through a stencil: only where the stencil is white.
    ProjectionBuffer s;
    s.resize(32, 32);
    Stencil st;
    // Black on the left half, white on the right: 8 rows span the buffer's height, so a texel is 4 pixels.
    st.image = {64, 8, {}};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 64; ++x)
            for (int c = 0; c < 4; ++c)
                st.image.rgba.push_back(c == 3 || x >= 32 ? 1.0f : 0.0f);
    s.paint({{16.0f, 16.0f}, 20.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("constant")), glm::vec3(1.0f), &st);
    const image::Image si = s.resolved();
    CHECK(px(si, 4, 16)[3] < 0.05f);
    CHECK(px(si, 28, 16)[3] > 0.95f);

    // Placing a picture covers the buffer with it.
    ProjectionBuffer p;
    p.resize(16, 16);
    p.place({1, 1, {0.2f, 0.4f, 0.6f, 1.0f}});
    CHECK(px(p.resolved(), 8, 8)[1] == doctest::Approx(0.4f));
}

TEST_CASE("projection buffer: blur, smudge and clone")
{
    ProjectionBuffer b;
    b.resize(32, 32);
    b.paint({{8.0f, 16.0f}, 4.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("constant")), {0.0f, 1.0f, 0.0f});
    const float before = px(b.resolved(), 13, 16)[3];
    b.blur({{8.0f, 16.0f}, 8.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("constant")), 3.0f);
    CHECK(px(b.resolved(), 13, 16)[3] > before);
    b.smudge({{14.0f, 16.0f}, 4.0f, 1.0f, {8.0f, 16.0f}}, brush::FalloffTable(brush::Falloff::fromName("constant")));
    CHECK(px(b.resolved(), 14, 16)[3] > 0.5f);
    const image::Image src = b.resolved();
    ProjectionBuffer c;
    c.resize(32, 32);
    c.clone({{24.0f, 16.0f}, 3.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("constant")), src, {-16.0f, 0.0f});
    CHECK(px(c.resolved(), 24, 16)[1] > 0.5f);
}

TEST_CASE("paint strokes: new payload kinds and images round trip")
{
    Payload p;
    p.kind = "heal";
    p.channels["baseColor"] = glm::vec3(0.0f);
    p.filterRadius = 6.5f;
    p.sourceOffset = {0.25f, -0.125f};
    CHECK(Payload::fromJson(p.toJson()) == p);
    Payload f;
    f.kind = "fill";
    f.channels["roughness"] = glm::vec3(1.0f);
    f.triangles = {1, 4, 9};
    f.fillBy = "island";
    f.source = {{"library", "bricks"}, {"scale", 2.0}};
    CHECK(Payload::fromJson(f.toJson()) == f);
    Payload n;
    n.kind = "normal";
    n.normalMode = "stamp";
    n.stampImage = "stamp.png";
    n.intensity = -0.5f;
    CHECK(Payload::fromJson(n.toJson()) == n);
    CHECK(n.written() == std::vector<std::string>{"normal", "mask"});
    CHECK_THROWS(Payload::fromJson({{"kind", "blur"}}));
    CHECK_THROWS(Payload::fromJson({{"kind", "project"}}));
    CHECK_THROWS(Payload::fromJson({{"kind", "nope"}}));

    std::vector<float> rgba(4 * 3 * 2);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = float(i) / 7.0f;
    auto img = PaintImage::fromRgba(3, 2, rgba);
    const std::vector<float> back = img->rgba();
    for (size_t i = 0; i < rgba.size(); ++i)
        CHECK(back[i] == doctest::Approx(rgba[i]).epsilon(1e-3));
    auto again = PaintImage::fromJson(img->toJson());
    REQUIRE(again);
    CHECK(again->key() == img->key());
    CHECK(again->rgba() == back);

    PaintData d;
    Stroke s;
    s.id = 1;
    s.payload.kind = "project";
    s.payload.image = img->key();
    d.buffers["p"].strokes.push_back(std::make_shared<const Stroke>(s));
    d.images[img->key()] = img;
    d.images["imgunused"] = img;
    d.pruneImages();
    CHECK(d.images.size() == 1);
    auto loaded = std::dynamic_pointer_cast<const PaintData>(PaintData::fromJson(d.toJson()));
    REQUIRE(loaded);
    CHECK(loaded->images.count(img->key()) == 1);
}

TEST_CASE("paint surface: islands, fills and uv lookups")
{
    auto mesh = twoIslands();
    REQUIRE(mesh->triangleCount() == 4);
    const std::vector<uint32_t> id = mesh->islands();
    CHECK(id[0] == id[1]);
    CHECK(id[2] == id[3]);
    CHECK(id[0] != id[2]);
    CHECK(mesh->islandOf(0).size() == 2);
    CHECK(mesh->allTriangles().size() == 4);
    const uint32_t faces[] = {1};
    CHECK(mesh->ofFaces(faces).size() == 2);
    uint32_t tri = 0;
    float u = 0.0f, v = 0.0f;
    REQUIRE(mesh->atUv({0.15f, 0.2f}, tri, u, v));
    CHECK(mesh->islandOf(tri) == mesh->islandOf(0));
    const glm::vec2 back = mesh->uv(tri, u, v);
    CHECK(back.x == doctest::Approx(0.15f));
    CHECK(back.y == doctest::Approx(0.2f));
    CHECK_FALSE(mesh->atUv({0.9f, 0.9f}, tri, u, v));
}

TEST_CASE("particle brushes are deterministic and stay on the surface")
{
    auto mesh = twoIslands();
    ParticleSpec spec = ParticleSpec::fromJson({{"kind", "sparks"}, {"count", 5}, {"rate", 1.0}, {"seed", 3}});
    Dab d{0, 0.3f, 0.3f, 2.0f, 1.0f, 1.0f, 0.0f, 0};
    const auto a = emitParticles(*mesh, d, 4, spec), b = emitParticles(*mesh, d, 4, spec);
    CHECK(a == b);
    CHECK(a.size() > 5);
    for (const Dab& p : a)
        CHECK(std::abs(mesh->point(p).y) < 1e-4f);
    spec.seed = 4;
    CHECK(emitParticles(*mesh, d, 4, spec) != a);
    CHECK_THROWS(ParticleSpec::fromJson({{"kind", "confetti"}}));
}

TEST_CASE("brush presets: the starter set loads")
{
    tex::brushes::reload();
    CHECK(tex::brushes::errors().empty());
    const auto all = tex::brushes::all();
    CHECK(all.size() >= 20);
    for (const tex::brushes::Preset& p : all)
    {
        CAPTURE(p.id);
        CHECK(p.tier == "builtin");
        CHECK_FALSE(p.name.empty());
    }
    CHECK(tex::brushes::find("Filters/blur"));
    CHECK(tex::brushes::find("Blur"));
}
