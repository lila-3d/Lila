#include "TexTest.h"

#include "core/PackedArray.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"

using namespace textest;
using namespace lila::tex::paint;

namespace
{
Stroke sampleStroke()
{
    Stroke s;
    s.id = 42;
    s.mesh = "|Panel";
    s.brush = {{"falloff", "bell"}, {"strength", 0.75}};
    s.payload.channels["baseColor"] = {0.9f, 0.1f, 0.3f};
    s.payload.channels["roughness"] = glm::vec3(0.123456789f);
    s.payload.blend = Blend::Add;
    s.seed = 9;
    StrokeView v;
    v.objectToClip[1][2] = 0.3333333f;
    v.eye = {1.0f / 3.0f, 2.0f, -7.25f};
    v.width = 640;
    v.height = 480;
    s.view = v;
    for (uint32_t i = 0; i < 20; ++i)
        s.dabs.push_back({i * 3u, 0.1f * float(i) / 3.0f, 0.2f / 7.0f, 1.0f + float(i) / 9.0f, 0.5f, 0.25f, float(i) * 0.1f, i % 2});
    return s;
}

/** Two triangles sharing an edge, as one quad over uv 0.25 to 0.75. */
std::shared_ptr<const PaintMesh> quadMesh()
{
    const std::vector<glm::dvec3> pts = {{0, 0, 0}, {0, 0, 10}, {10, 0, 10}, {10, 0, 0}};
    const std::vector<geo::Index> sizes = {4}, idx = {0, 1, 2, 3};
    static geo::Mesh m = [&] {
        geo::Mesh mesh = geo::Mesh::fromPolygons(pts, sizes, idx);
        auto uv = mesh.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (geo::Index c : mesh.faceCorners(0))
        {
            const glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
            uv[c] = {float(0.25 + p.x / 20.0), float(0.25 + p.z / 20.0)};
        }
        return mesh;
    }();
    return PaintMesh::build(m, std::make_shared<const geo::MeshBvh>(m), "");
}
} // namespace

TEST_CASE("paint strokes: records round trip exactly, packed or plain")
{
    const Stroke s = sampleStroke();
    CHECK(Stroke::fromJson(s.toJson(true)) == s);
    CHECK(Stroke::fromJson(s.toJson(false)) == s);
    // An ASCII scene file unpacks binary arrays and writes the numbers as text.
    json j = s.toJson(true);
    packed::unpackAll(j);
    CHECK(Stroke::fromJson(json::parse(j.dump())) == s);

    // Agents may write dabs as objects.
    json o = s.toJson(false);
    o.erase("triangles");
    o["dabs"] = json::array({{{"triangle", 5}, {"bary", {0.25, 0.5}}, {"radius", 2.0}}});
    const Stroke a = Stroke::fromJson(o);
    REQUIRE(a.dabs.size() == 1);
    CHECK(a.dabs[0].triangle == 5);
    CHECK(a.dabs[0].v == 0.5f);
    CHECK(a.dabs[0].opacity == 1.0f);

    CHECK_THROWS(Stroke::fromJson({{"payload", {{"blend", "nope"}}}}));
    CHECK_THROWS(Stroke::fromJson({{"payload", {{"channels", {{"baseColor", {1, 0, 0}}}}}}, {"dabs", {{{"triangle", 0}, {"bary", {0, 0}}, {"radius", -1}}}}}));
}

TEST_CASE("paint strokes: payloads name what they write, the mask last")
{
    Payload p;
    p.channels["roughness"] = glm::vec3(0.5f);
    p.channels["baseColor"] = glm::vec3(1.0f);
    CHECK(p.written() == std::vector<std::string>{"baseColor", "roughness", "mask"});
    p.mask = false;
    CHECK(p.written() == std::vector<std::string>{"baseColor", "roughness"});
    Payload d = Payload::fromJson({{"kind", "direction"}, {"tilt", 0.3}});
    CHECK(d.written() == std::vector<std::string>{"normal", "mask"});
    CHECK(Payload::fromJson(d.toJson()) == d);
    CHECK_THROWS(Payload::fromJson({{"channels", json::object()}, {"mask", false}}));
}

TEST_CASE("paint strokes: paint data and baselines round trip")
{
    auto data = std::make_shared<PaintData>();
    Buffer b;
    b.strokes.push_back(std::make_shared<const Stroke>(sampleStroke()));
    b.width = b.height = 512;
    b.clears = 2;
    auto base = std::make_shared<Baseline>();
    Baseline::Tile t;
    t.channel = "baseColor";
    t.udim = 1002;
    t.width = t.height = 256;
    t.pages = {0, 3};
    std::vector<uint8_t> raw(size_t(kPageSize) * kPageSize * 8 * 2);
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = uint8_t((i * 7) ^ (i >> 9));
    t.data = compressPages(raw);
    base->tiles.push_back(t);
    b.baseline = base;
    data->buffers["L3"] = b;
    data->active = "L3";

    json j = data->toJson();
    CHECK(j["kind"] == kDataKind);
    packed::unpackAll(j);
    auto back = std::dynamic_pointer_cast<const PaintData>(PaintData::fromJson(json::parse(j.dump())));
    REQUIRE(back);
    CHECK(back->active == "L3");
    const Buffer* bb = back->buffer("L3");
    REQUIRE(bb);
    CHECK(bb->width == 512);
    CHECK(bb->clears == 2);
    REQUIRE(bb->strokes.size() == 1);
    CHECK(*bb->strokes[0] == sampleStroke());
    REQUIRE(bb->baseline);
    REQUIRE(bb->baseline->tiles.size() == 1);
    CHECK(bb->baseline->tiles[0].pages == std::vector<uint32_t>{0, 3});
    CHECK(decompressPages(bb->baseline->tiles[0].data, raw.size()) == raw);
    CHECK(back->uniqueId() == "paint1");
}

TEST_CASE("paint strokes: external names")
{
    CHECK(imageRef("L3", "baseColor") == "external:paint/L3/baseColor");
    std::string b, c;
    CHECK(parseExternalName(externalName("L3", "mask"), &b, &c));
    CHECK(b == "L3");
    CHECK(c == "mask");
    CHECK_FALSE(parseExternalName("position", &b, &c));
    CHECK_FALSE(parseExternalName("paint/L3", &b, &c));
    const glm::vec3 t = tipFrame({0, 1, 0});
    CHECK(std::abs(glm::dot(t, glm::vec3(0, 1, 0))) < 1e-6f);
    CHECK(std::abs(glm::length(t) - 1.0f) < 1e-6f);
}

TEST_CASE("paint surface: every texel in the uv has one owner, padding the nearest edge")
{
    std::shared_ptr<const PaintMesh> mesh = quadMesh();
    REQUIRE(mesh->triangleCount() == 2);
    SurfaceAtlas atlas(mesh, 1001, 256, 256, 4);
    CHECK(atlas.pagesX() == 2);
    // The quad covers texels 64 to 191 in x and y: all four pages.
    for (uint32_t p = 0; p < 4; ++p)
        CHECK(atlas.pageUsed(p));
    const std::vector<GeoTexel> first = atlas.page(0);
    SurfaceAtlas again(mesh, 1001, 256, 256, 4);
    const std::vector<GeoTexel>& second = again.page(0);
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(GeoTexel)) == 0);

    int inside = 0, padding = 0, empty = 0;
    for (int y = 0; y < kPageSize; ++y)
        for (int x = 0; x < kPageSize; ++x)
        {
            const GeoTexel& t = first[size_t(y) * kPageSize + x];
            const bool in = x >= 64 && y >= 64;
            const bool pad = !in && x >= 60 && y >= 60;
            if (in)
            {
                CHECK(t.triangle != kNoTriangle);
                CHECK(t.flags == 0);
                ++inside;
            }
            else if (pad)
            {
                if (t.triangle != kNoTriangle)
                {
                    CHECK(t.flags == kTexelPadding);
                    // Clamped onto the triangle.
                    CHECK(t.u >= -1e-6f);
                    CHECK(t.v >= -1e-6f);
                    CHECK(t.u + t.v <= 1.0f + 1e-5f);
                    ++padding;
                }
            }
            else if (t.triangle == kNoTriangle)
                ++empty;
        }
    CHECK(inside == 64 * 64);
    CHECK(padding > 4 * 64);
    CHECK(empty > 0);

    // A texel's surface point is where its uv says.
    const GeoTexel& t = first[size_t(100) * kPageSize + 100];
    const glm::vec3 p = mesh->point(t.triangle, t.u, t.v);
    const glm::vec2 uv = atlas.uvOf(100, 100);
    CHECK(std::abs(p.x - (uv.x - 0.25f) * 20.0f) < 1e-3f);
    CHECK(std::abs(p.z - (uv.y - 0.25f) * 20.0f) < 1e-3f);

    // Every page a dab reaches lists it, numbered from first.
    const DabReach near{{0.5f, 0.0f, 0.5f}, 0.4f};
    auto pages = atlas.pagesFor(std::span(&near, 1), 3);
    REQUIRE(!pages.empty());
    for (const auto& [page, dabs] : pages)
        CHECK(dabs == std::vector<uint32_t>{3});
}
