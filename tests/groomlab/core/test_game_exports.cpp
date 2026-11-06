// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Game exports: compressed PNGs, glTF 2.0 cards (a .glb parsed back and checked the way a
// validator would) and the strands copied for an engine's importer (scaled, resampled).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/export_view.h"
#include "groomlab/gltf_writer.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "groomlab/png.h"
#include "groomlab/random.h"
#include "groomlab/texture.h"
#include "groomlab/usd_writer.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::string tempFile(const std::string& name) { return (fs::temp_directory_path() / name).u8string(); }

std::vector<std::uint8_t> readBytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeBytes(const std::string& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

// A PNG's texels as 0-255 values through the texture loader (stb_image), `channels` of each.
std::vector<int> decodePng(const std::uint8_t* png, std::size_t size, int* width, int* height, int channels,
                           const std::string& name) {
    static int serial = 0;
    const std::string path = tempFile("groomlab_png_" + std::to_string(++serial) + "_" + name + ".png");
    writeBytes(path, png, size);
    const TexturePtr t = loadTexture(path);
    *width = t->width();
    *height = t->height();
    std::vector<int> out;
    for (int y = 0; y < *height; ++y) {
        for (int x = 0; x < *width; ++x) {
            // Texel centres; v = 0 is the bottom row, image rows are stored top down.
            const auto c = t->sample((x + 0.5f) / static_cast<float>(*width), 1.0f - (y + 0.5f) / static_cast<float>(*height));
            // Gray loads into r, g and b; gray + alpha keeps its alpha in a.
            const int picks[4][4] = {{0}, {0, 3}, {0, 1, 2}, {0, 1, 2, 3}};
            for (int k = 0; k < channels; ++k) out.push_back(static_cast<int>(std::lround(c[static_cast<std::size_t>(picks[channels - 1][k])] * 255.0f)));
        }
    }
    fs::remove(path);
    return out;
}

TEST(GameExports, PngRoundTripsEveryChannelCountAndCompresses) {
    for (int channels = 1; channels <= 4; ++channels) {
        const int w = 37, h = 23;  // odd sizes: rows that don't line up with anything
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w * h * channels));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < channels; ++c) {
                    // A gradient, noise and an empty band: every filter gets used.
                    const std::size_t i = static_cast<std::size_t>((y * w + x) * channels + c);
                    pixels[i] = y < 5 ? 0 : static_cast<std::uint8_t>((x * 7 + y * 3 + c * 50 + ((x * y) % 13) * 11) & 0xFF);
                }
            }
        }
        const std::vector<std::uint8_t> png = encodePng(w, h, channels, pixels);
        int dw = 0, dh = 0;
        const std::vector<int> back = decodePng(png.data(), png.size(), &dw, &dh, channels, std::to_string(channels));
        ASSERT_EQ(dw, w);
        ASSERT_EQ(dh, h);
        ASSERT_EQ(back.size(), pixels.size());
        for (std::size_t i = 0; i < pixels.size(); ++i) ASSERT_EQ(back[i], pixels[i]) << "channels " << channels << " at " << i;
    }
    // A mostly empty atlas (what card maps are) shrinks many times over.
    std::vector<std::uint8_t> atlas(512u * 512u * 4u, 0);
    for (std::size_t i = 0; i < 20000; ++i) atlas[(i * 7919u) % atlas.size()] = static_cast<std::uint8_t>(i);
    const std::vector<std::uint8_t> png = encodePng(512, 512, 4, atlas);
    EXPECT_LT(png.size(), atlas.size() / 5);
    int dw = 0, dh = 0;
    EXPECT_EQ(decodePng(png.data(), png.size(), &dw, &dh, 4, "atlas"), std::vector<int>(atlas.begin(), atlas.end()));
    EXPECT_THROW(encodePng(2, 2, 3, {1, 2, 3}), std::invalid_argument);
    EXPECT_THROW(encodePng(2, 2, 5, std::vector<std::uint8_t>(20)), std::invalid_argument);
}

TEST(GameExports, DeflateInStretchesGivesTheSerialBytes) {
    // Runs, noise, repeats at several periods and long matches crossing every stretch boundary.
    std::vector<std::uint8_t> data(300000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::size_t band = (i / 7001) % 4;
        data[i] = band == 0   ? 0
                  : band == 1 ? static_cast<std::uint8_t>(hash64(i) & 0xFF)
                  : band == 2 ? static_cast<std::uint8_t>((i % 37) * 5)
                              : static_cast<std::uint8_t>((i % 1000) < 600 ? 7 : hash64(i / 3) & 0x3);
    }
    const std::vector<std::uint8_t> serial = zlibCompress(data, data.size());  // one stretch: the serial parse
    for (const std::size_t stretch : {std::size_t(50), std::size_t(257), std::size_t(1000), std::size_t(4099), std::size_t(65536)}) {
        EXPECT_EQ(zlibCompress(data, stretch), serial) << "stretches of " << stretch;
    }
    EXPECT_EQ(zlibCompress(data), serial);
    EXPECT_EQ(zlibCompress({}, 16), zlibCompress({}));

    // An image over several default stretches (a megabyte each) still reads back exactly.
    const int w = 700, h = 500;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if ((i / 4096) % 3 == 1) pixels[i] = static_cast<std::uint8_t>(hash64(i / 2) & 0xFF);
        if ((i / 4096) % 3 == 2) pixels[i] = static_cast<std::uint8_t>(i % 251);
    }
    const std::vector<std::uint8_t> png = encodePng(w, h, 4, pixels);
    int dw = 0, dh = 0;
    EXPECT_EQ(decodePng(png.data(), png.size(), &dw, &dh, 4, "stretches"), std::vector<int>(pixels.begin(), pixels.end()));
    // And several at once give the bytes of each alone.
    const std::vector<std::uint8_t> gray(static_cast<std::size_t>(w) * h, 9);
    const auto both = encodePngs({{w, h, 4, &pixels}, {w, h, 1, &gray}});
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0], png);
    EXPECT_EQ(both[1], encodePng(w, h, 1, gray));
    EXPECT_THROW(encodePngs({{w, h, 3, &gray}}), std::invalid_argument);
}

// ---- A .glb read back and checked ------------------------------------------------

struct Glb {
    json doc;
    std::vector<std::uint8_t> bin;
};

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

Glb parseGlb(const std::vector<std::uint8_t>& bytes) {
    Glb g;
    EXPECT_GE(bytes.size(), 20u);
    if (bytes.size() < 20) return g;
    EXPECT_EQ(le32(&bytes[0]), 0x46546C67u);  // "glTF"
    EXPECT_EQ(le32(&bytes[4]), 2u);
    EXPECT_EQ(le32(&bytes[8]), bytes.size());
    const std::uint32_t jsonLength = le32(&bytes[12]);
    EXPECT_EQ(le32(&bytes[16]), 0x4E4F534Au);  // "JSON"
    EXPECT_EQ(jsonLength % 4, 0u);
    g.doc = json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLength);
    const std::size_t at = 20 + jsonLength;
    if (at < bytes.size()) {
        const std::uint32_t binLength = le32(&bytes[at]);
        EXPECT_EQ(le32(&bytes[at + 4]), 0x004E4942u);  // "BIN\0"
        EXPECT_EQ(binLength % 4, 0u);
        EXPECT_EQ(at + 8 + binLength, bytes.size());
        g.bin.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8), bytes.end());
    }
    return g;
}

int components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    ADD_FAILURE() << "accessor type " << type;
    return 1;
}

std::vector<float> floatsOf(const Glb& g, int accessor) {
    const json& a = g.doc["accessors"][accessor];
    EXPECT_EQ(a["componentType"], 5126);
    const json& view = g.doc["bufferViews"][a["bufferView"].get<int>()];
    const std::size_t count = a["count"].get<std::size_t>() * static_cast<std::size_t>(components(a["type"]));
    std::vector<float> out(count);
    std::memcpy(out.data(), g.bin.data() + view["byteOffset"].get<std::size_t>() + a.value("byteOffset", std::size_t(0)),
                count * sizeof(float));
    return out;
}

std::vector<std::uint32_t> indicesOf(const Glb& g, int accessor) {
    const json& a = g.doc["accessors"][accessor];
    const json& view = g.doc["bufferViews"][a["bufferView"].get<int>()];
    const std::uint8_t* p = g.bin.data() + view["byteOffset"].get<std::size_t>();
    std::vector<std::uint32_t> out(a["count"].get<std::size_t>());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (a["componentType"] == 5123) {
            out[i] = static_cast<std::uint32_t>(p[2 * i]) | (static_cast<std::uint32_t>(p[2 * i + 1]) << 8);
        } else {
            out[i] = le32(p + 4 * i);
        }
    }
    return out;
}

// Checks what the glTF 2.0 spec requires of the file (what a validator would say).
void validate(const Glb& g) {
    const json& d = g.doc;
    ASSERT_EQ(d["asset"]["version"], "2.0");
    ASSERT_TRUE(d.contains("buffers"));
    ASSERT_EQ(d["buffers"].size(), 1u);
    EXPECT_FALSE(d["buffers"][0].contains("uri"));  // the GLB's own buffer
    EXPECT_EQ(d["buffers"][0]["byteLength"].get<std::size_t>(), g.bin.size());
    for (const json& v : d["bufferViews"]) {
        EXPECT_EQ(v["buffer"], 0);
        const std::size_t offset = v["byteOffset"], length = v["byteLength"];
        EXPECT_EQ(offset % 4, 0u);
        EXPECT_GT(length, 0u);
        EXPECT_LE(offset + length, g.bin.size());
        if (v.contains("target")) EXPECT_TRUE(v["target"] == 34962 || v["target"] == 34963);
    }
    for (const json& a : d["accessors"]) {
        ASSERT_LT(a["bufferView"].get<std::size_t>(), d["bufferViews"].size());
        const int type = a["componentType"];
        ASSERT_TRUE(type == 5123 || type == 5125 || type == 5126) << type;
        const std::size_t size = type == 5123 ? 2 : 4;
        const json& view = d["bufferViews"][a["bufferView"].get<int>()];
        EXPECT_LE(a["count"].get<std::size_t>() * size * static_cast<std::size_t>(components(a["type"])),
                  view["byteLength"].get<std::size_t>());
        EXPECT_GT(a["count"].get<std::size_t>(), 0u);
    }
    for (std::size_t i = 0; i < d["accessors"].size(); ++i) {
        if (d["accessors"][i]["componentType"] != 5126) continue;
        for (const float f : floatsOf(g, static_cast<int>(i))) ASSERT_TRUE(std::isfinite(f));
    }
    const std::size_t materials = d.contains("materials") ? d["materials"].size() : 0;
    for (const json& mesh : d["meshes"]) {
        for (const json& p : mesh["primitives"]) {
            const json& attributes = p["attributes"];
            const int position = attributes["POSITION"];
            const std::size_t vertices = d["accessors"][position]["count"];
            for (const auto& [name, index] : attributes.items()) {
                EXPECT_EQ(d["accessors"][index.get<int>()]["count"].get<std::size_t>(), vertices) << name;
            }
            // POSITION carries its exact bounds.
            const std::vector<float> points = floatsOf(g, position);
            for (int c = 0; c < 3; ++c) {
                float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
                for (std::size_t v = 0; v < vertices; ++v) {
                    lo = std::min(lo, points[3 * v + static_cast<std::size_t>(c)]);
                    hi = std::max(hi, points[3 * v + static_cast<std::size_t>(c)]);
                }
                EXPECT_EQ(d["accessors"][position]["min"][c].get<float>(), lo);
                EXPECT_EQ(d["accessors"][position]["max"][c].get<float>(), hi);
            }
            if (attributes.contains("NORMAL")) {
                const std::vector<float> n = floatsOf(g, attributes["NORMAL"]);
                for (std::size_t v = 0; v < vertices; ++v) {
                    EXPECT_NEAR(std::sqrt(n[3 * v] * n[3 * v] + n[3 * v + 1] * n[3 * v + 1] + n[3 * v + 2] * n[3 * v + 2]), 1.0, 1e-3);
                }
            }
            if (attributes.contains("TANGENT")) {
                const std::vector<float> t = floatsOf(g, attributes["TANGENT"]);
                for (std::size_t v = 0; v < vertices; ++v) EXPECT_EQ(std::abs(t[4 * v + 3]), 1.0f);
            }
            const json& indexAccessor = d["accessors"][p["indices"].get<int>()];
            EXPECT_EQ(indexAccessor["type"], "SCALAR");
            EXPECT_EQ(indexAccessor["componentType"], vertices > 65535 ? 5125 : 5123);
            EXPECT_EQ(d["bufferViews"][indexAccessor["bufferView"].get<int>()]["target"], 34963);
            const std::vector<std::uint32_t> indices = indicesOf(g, p["indices"]);
            EXPECT_EQ(indices.size() % 3, 0u);
            for (const std::uint32_t i : indices) ASSERT_LT(i, vertices);
            EXPECT_EQ(p.value("mode", 4), 4);
            if (p.contains("material")) EXPECT_LT(p["material"].get<std::size_t>(), materials);
        }
    }
    const std::size_t textures = d.contains("textures") ? d["textures"].size() : 0;
    for (std::size_t m = 0; m < materials; ++m) {
        const json& mat = d["materials"][m];
        for (const char* slot : {"baseColorTexture", "metallicRoughnessTexture"}) {
            if (mat["pbrMetallicRoughness"].contains(slot)) {
                EXPECT_LT(mat["pbrMetallicRoughness"][slot]["index"].get<std::size_t>(), textures);
            }
        }
        if (mat.contains("occlusionTexture")) EXPECT_LT(mat["occlusionTexture"]["index"].get<std::size_t>(), textures);
        const std::string mode = mat.value("alphaMode", "OPAQUE");
        EXPECT_TRUE(mode == "OPAQUE" || mode == "MASK" || mode == "BLEND");
        if (mode != "MASK") EXPECT_FALSE(mat.contains("alphaCutoff"));
    }
    for (std::size_t t = 0; t < textures; ++t) {
        EXPECT_LT(d["textures"][t]["source"].get<std::size_t>(), d["images"].size());
        EXPECT_LT(d["textures"][t]["sampler"].get<std::size_t>(), d["samplers"].size());
    }
    if (d.contains("images")) {
        for (const json& image : d["images"]) {
            EXPECT_EQ(image["mimeType"], "image/png");
            const json& view = d["bufferViews"][image["bufferView"].get<int>()];
            EXPECT_FALSE(view.contains("target"));
            const std::uint8_t* png = g.bin.data() + view["byteOffset"].get<std::size_t>();
            EXPECT_EQ(png[0], 0x89);
            EXPECT_EQ(png[1], 'P');
        }
    }
    for (const json& node : d["nodes"]) {
        if (node.contains("mesh")) EXPECT_LT(node["mesh"].get<std::size_t>(), d["meshes"].size());
        if (node.contains("children")) {
            for (const json& c : node["children"]) EXPECT_LT(c.get<std::size_t>(), d["nodes"].size());
        }
    }
    ASSERT_LT(d["scene"].get<std::size_t>(), d["scenes"].size());
    for (const json& n : d["scenes"][0]["nodes"]) EXPECT_LT(n.get<std::size_t>(), d["nodes"].size());
}

TEST(GameExports, FeatherCardsCarryTheirSpineIntoGltf) {
    // Feathers whose vanes lift: spine cards (three vertices a station, two quads a segment) go
    // out as they are, their per-half normals with them, TEXCOORD_1's x 0 on the left edge, 0.5 on
    // the spine and 1 on the right edge (|2x - 1|: 0 on the rachis, 1 at the vanes' edges).
    MeshPtr plane = test::makePlane(8, 4.0f);
    ExternalInputs ext;
    ext.set("scalp", plane, 1);
    auto shafts = std::make_shared<StrandSet>();
    for (int f = 0; f < 3; ++f) {
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 10; ++i) pts.push_back({0.5f + 0.1f * static_cast<float>(i), 0.05f, 0.6f + 0.8f * static_cast<float>(f)});
        shafts->addStrand(static_cast<StrandId>(10 + f), pts, {}, plane->closestPoint(pts[0]).root);
    }
    auto propagate = OperatorRegistry::instance().create(op::kPropagate);
    propagate->params().set("count", std::int64_t{16});
    propagate->params().set("lift", 25.0);
    propagate->params().set("asymmetry", 0.3);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr s = valueAs<StrandsPtr>(propagate->evaluate(ctx, {Value(shafts)}));
    CardSettings settings;
    settings.featherSegments = 3;
    const CardMesh cards = buildCards(*s, plane.get(), settings);
    ASSERT_EQ(cards.cards.size(), 3u);
    for (const CardMesh::Card& c : cards.cards) ASSERT_TRUE(c.spine);
    const CardTextures textures = bakeCardTextures(*s, cards, CardBakeSettings{64});

    GltfDocument doc;
    CardGltfSettings gltf;
    gltf.name = "feathers";
    addCardsToGltf(doc, cards, textures, gltf);
    const Glb g = parseGlb(encodeGlb(doc));
    validate(g);
    const json& p = g.doc["meshes"][0]["primitives"][0];
    EXPECT_EQ(indicesOf(g, p["indices"]).size(), 3u * 3u * 4u * 3u);  // 3 cards x 4 triangles a segment x 3
    const std::vector<float> normals = floatsOf(g, p["attributes"]["NORMAL"]);
    const std::vector<float> uv1 = floatsOf(g, p["attributes"]["TEXCOORD_1"]);
    ASSERT_EQ(uv1.size(), 2 * cards.vertexCount());
    // TEXCOORD_1 the unique world-AO layout: the spine (the middle column) between its edges.
    bool folded = false;
    for (const CardMesh::Card& c : cards.cards) {
        for (int k = 0; k < c.stations; ++k) {
            const std::size_t row = c.firstVertex + 3 * static_cast<std::size_t>(k);
            const float left = uv1[2 * row], spine = uv1[2 * (row + 1)], right = uv1[2 * (row + 2)];
            EXPECT_GT((spine - left) * (right - spine), 0.0f) << k;
            for (std::size_t v = row; v < row + 3; ++v) {
                const Vec3f n(normals[3 * v], normals[3 * v + 1], normals[3 * v + 2]);
                EXPECT_NEAR(n.dot(cards.normals[v]), 1.0f, 1e-4f);  // Y up already: as they are
            }
            folded = folded || cards.normals[row].dot(cards.normals[row + 2]) < 0.95f;  // the halves apart
        }
    }
    EXPECT_TRUE(folded);
}

TEST(GameExports, GltfWritesTextAndBinaryAndZUpScenes) {
    // One quad: a small hand-made card mesh.
    CardMesh cards;
    cards.positions = {{0, 0, 0}, {1, 0, 0}, {1, 0, 2}, {0, 0, 2}};
    cards.normals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
    cards.u = {0, 1, 1, 0};
    cards.v = {0, 0, 1, 1};
    cards.quads = {0, 1, 2, 3};
    CardMesh::Card card;
    card.stations = 2;
    card.across = 1;
    cards.cards.push_back(card);
    CardTextures t;
    t.size = 4;
    t.color.assign(48, 200);
    t.coverage.assign(16, 255);
    t.occlusion.assign(16, 128);
    GltfDocument doc;
    CardGltfSettings settings;
    settings.zUp = true;  // a Z-up scene: z becomes y
    settings.scale = 1.0f;
    settings.alphaMode = GltfMaterial::AlphaMode::Blend;
    addCardsToGltf(doc, cards, t, settings);
    const Glb g = parseGlb(encodeGlb(doc));
    validate(g);
    const std::vector<float> positions = floatsOf(g, g.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]);
    EXPECT_FLOAT_EQ(positions[3 * 2 + 1], 2.0f);   // (1, 0, 2) -> (1, 2, 0)
    EXPECT_FLOAT_EQ(positions[3 * 2 + 2], 0.0f);
    EXPECT_EQ(g.doc["materials"][0]["alphaMode"], "BLEND");
    EXPECT_FALSE(g.doc["materials"][0].contains("alphaCutoff"));
    EXPECT_EQ(g.doc["nodes"].size(), 1u);  // no root name: the mesh node alone

    // .gltf: JSON with a .bin and PNGs beside it.
    const std::string path = tempFile("groomlab_cards_test.gltf");
    const std::vector<std::string> written = writeGltf(path, doc, false);
    ASSERT_EQ(written.size(), 4u);  // the .gltf, its .bin and two PNGs
    std::ifstream in(fs::u8path(path));
    const json text = json::parse(in);
    in.close();
    EXPECT_EQ(text["buffers"][0]["uri"], "groomlab_cards_test.bin");
    EXPECT_EQ(fs::file_size(fs::u8path(tempFile("groomlab_cards_test.bin"))), text["buffers"][0]["byteLength"].get<std::size_t>());
    for (const json& image : text["images"]) {
        EXPECT_FALSE(image.contains("bufferView"));
        const std::vector<std::uint8_t> png = readBytes(tempFile(image["uri"].get<std::string>()));
        ASSERT_GT(png.size(), 8u);
        EXPECT_EQ(png[1], 'P');
    }
    for (const std::string& f : written) fs::remove(fs::u8path(f));
    const std::string glb = tempFile("groomlab_cards_test.glb");
    EXPECT_EQ(writeGltf(glb, doc, true), std::vector<std::string>{glb});
    validate(parseGlb(readBytes(glb)));
    EXPECT_FALSE(fs::exists(fs::u8path(glb + ".tmp")));
    fs::remove(fs::u8path(glb));
}

TEST(GameExports, GltfIndicesWidenPastSixtyFiveThousandVertices) {
    for (const std::size_t quads : {std::size_t(10), std::size_t(17000)}) {  // 40 and 68000 vertices
        GltfPrimitive p;
        for (std::size_t q = 0; q < quads; ++q) {
            const auto x = static_cast<float>(q);
            const auto base = static_cast<std::uint32_t>(p.positions.size());
            p.positions.insert(p.positions.end(), {{x, 0, 0}, {x + 1, 0, 0}, {x + 1, 1, 0}, {x, 1, 0}});
            p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        GltfDocument doc;
        doc.meshes.push_back({"m", {p}});
        const Glb g = parseGlb(encodeGlb(doc));
        validate(g);
        const json& indices = g.doc["accessors"][g.doc["meshes"][0]["primitives"][0]["indices"].get<int>()];
        EXPECT_EQ(indices["componentType"], quads * 4 > 65535 ? 5125 : 5123);
        EXPECT_EQ(indicesOf(g, g.doc["meshes"][0]["primitives"][0]["indices"]).back(), quads * 4 - 1);
        EXPECT_FALSE(g.doc.contains("materials"));
    }
}

TEST(GameExports, GltfRefusesBrokenDocuments) {
    GltfPrimitive p;
    p.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    p.indices = {0, 1, 2};
    GltfDocument ok;
    ok.meshes.push_back({"m", {p}});
    EXPECT_NO_THROW(encodeGlb(ok));
    GltfDocument nan = ok;
    nan.meshes[0].primitives[0].positions[1].x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(encodeGlb(nan), std::invalid_argument);
    GltfDocument range = ok;
    range.meshes[0].primitives[0].indices[2] = 3;
    EXPECT_THROW(encodeGlb(range), std::invalid_argument);
    GltfDocument partial = ok;
    partial.meshes[0].primitives[0].indices.push_back(0);
    EXPECT_THROW(encodeGlb(partial), std::invalid_argument);
    GltfDocument normals = ok;
    normals.meshes[0].primitives[0].normals = {{0, 1, 0}};
    EXPECT_THROW(encodeGlb(normals), std::invalid_argument);
    GltfDocument material = ok;
    material.meshes[0].primitives[0].material = 0;
    EXPECT_THROW(encodeGlb(material), std::invalid_argument);
    material.materials.push_back({});
    material.materials[0].baseColorTexture = 0;
    EXPECT_THROW(encodeGlb(material), std::invalid_argument);
    material.textures.push_back({"t", {1, 2, 3}});
    EXPECT_THROW(encodeGlb(material), std::invalid_argument);  // not a PNG
    EXPECT_THROW(encodeGlb(GltfDocument{}), std::invalid_argument);
}

TEST(GameExports, TangentsFollowUAndCarryHandedness) {
    // A quad in the XZ plane facing +Y: U along +X, V along -Z, then V along +Z (mirrored).
    const std::vector<Vec3f> positions = {{0, 0, 0}, {1, 0, 0}, {1, 0, -1}, {0, 0, -1}};
    const std::vector<Vec3f> normals(4, Vec3f(0, 1, 0));
    const std::vector<std::uint32_t> indices = {0, 1, 2, 0, 2, 3};
    const auto t = meshTangents(positions, normals, {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, indices);
    for (const auto& v : t) {
        EXPECT_NEAR(v[0], 1.0f, 1e-5f);
        const Vec3f b = Vec3f(0, 1, 0).cross(Vec3f(v[0], v[1], v[2])) * v[3];
        EXPECT_NEAR(b.z, -1.0f, 1e-5f);  // +V runs toward -Z
    }
    const auto mirrored = meshTangents(positions, normals, {{0, 1}, {1, 1}, {1, 0}, {0, 0}}, indices);
    EXPECT_EQ(mirrored[0][3], -t[0][3]);
}

TEST(GameExports, ExportCopyScalesAndResamples) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}, {0, 3, 0}}, {0.5f, 0.5f, 0.1f});
    s.addStrand(2, {{2, 0, 0}, {2, 2, 0}});
    s.addStrandChannel(kClumpIdChannel, -1.0f) = {3.0f, 4.0f};
    s.addPointChannel("colorR", 0.0f) = {0.0f, 0.5f, 1.0f, 0.2f, 0.4f};
    const StrandSet copy = exportCopy(s, 0.5f, 4);
    ASSERT_EQ(copy.strandCount(), 2u);
    EXPECT_EQ(copy.uniformPointCount(), 4u);
    EXPECT_EQ(copy.ids(), s.ids());
    EXPECT_EQ(*copy.strandChannel(kClumpIdChannel), *s.strandChannel(kClumpIdChannel));
    EXPECT_NEAR(copy.strandPoints(0)[1].y, 0.5f, 1e-6f);  // every 1 of 3 units, halved
    EXPECT_NEAR(copy.strandPoints(0)[3].y, 1.5f, 1e-6f);
    EXPECT_NEAR(copy.widths()[0], 0.25f, 1e-6f);
    EXPECT_NEAR(copy.widths()[3], 0.05f, 1e-6f);
    EXPECT_NEAR(copy.widths()[4], 0.5f, 1e-6f);  // no widths given: 1, scaled
    EXPECT_NEAR((*copy.pointChannel("colorR"))[3], 1.0f, 1e-6f);
    EXPECT_NEAR((*copy.pointChannel("colorR"))[7], 0.4f, 1e-6f);
    const StrandSet same = exportCopy(s, 1.0f, 0);
    EXPECT_EQ(same.positions(), s.positions());
    EXPECT_EQ(same.pointCount(), 5u);
}

// ---- Benchmarks (Budget.*: ctest runs them alone). Each prints its time and a hash of what
// it made, so a faster version can be checked to make the very same bytes.

double millisecondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// A groom like the game_cards preset (clumps, color, width, 12 segments, a Cards node at the mid
// LOD with a 2048 atlas) at about 100k strands on a 20 x 20 plane with UVs, carded and laid out.
struct GameCardsGroom {
    MeshPtr scalp;
    StrandsPtr strands;
    std::shared_ptr<CardMesh> cards;
};

std::uint64_t texturesHash(const CardTextures& t) {
    std::uint64_t h = fnv1a(&t.size, sizeof(t.size));
    for (const auto* v : {&t.coverage, &t.depth, &t.tangent, &t.attribute, &t.color, &t.occlusion}) h = fnv1a(v->data(), v->size(), h);
    return h;
}

// Unity's seven card maps (groomLabBakeCards -layout unity) at 2048, as PNGs.
std::vector<std::pair<int, std::vector<std::uint8_t>>> unityMaps(const CardTextures& t) {
    const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
    std::vector<std::uint8_t> baseColor(4 * n), mask(4 * n);
    for (std::size_t i = 0; i < n; ++i) {
        baseColor[4 * i] = t.color[3 * i];
        baseColor[4 * i + 1] = t.color[3 * i + 1];
        baseColor[4 * i + 2] = t.color[3 * i + 2];
        baseColor[4 * i + 3] = t.coverage[i];
        mask[4 * i] = 0;
        mask[4 * i + 1] = t.occlusion[i];
        mask[4 * i + 2] = t.attribute[4 * i + 3];
        mask[4 * i + 3] = 166;
    }
    return {{4, baseColor}, {4, mask}, {1, t.occlusion}, {3, t.tangent}, {1, t.depth}, {4, t.attribute}, {3, cardNormalOpenGl(t)}};
}

TEST(Budget, UsdFrameOfAMillionPoints) {
    // 100k strands of 10 points, with widths, a melanin color and a per-point channel.
    StrandSet s;
    constexpr std::size_t kStrands = 100000, kPoints = 10;
    s.resizeUniform(kStrands, kPoints);
    std::vector<StrandId>& ids = s.mutableIds();
    std::vector<Vec3f>& points = s.mutablePositions();
    std::vector<float>& widths = s.mutableWidths();
    widths.assign(kStrands * kPoints, 0.0f);
    std::vector<float>& melanin = s.addStrandChannel(kMelaninChannel, 0.0f);
    std::vector<float>& along = s.addPointChannel("along", 0.0f);
    for (std::size_t i = 0; i < kStrands; ++i) {
        ids[i] = hash64(i + 1);
        melanin[i] = unitFloat(hash64(i * 7 + 3));
        const float x = static_cast<float>(i % 316) * 0.0631f, z = static_cast<float>(i / 316) * 0.0631f;
        for (std::size_t k = 0; k < kPoints; ++k) {
            const std::size_t p = i * kPoints + k;
            const float t = static_cast<float>(k) / static_cast<float>(kPoints - 1);
            points[p] = {x + 0.1f * t * unitFloat(hash64(p)), 2.0f * t, z - 0.05f * t * t};
            widths[p] = 0.01f * (1.0f - 0.7f * t);
            along[p] = t;
        }
    }
    const std::string path = tempFile("groomlab_budget_usd.usda");
    const auto t0 = std::chrono::steady_clock::now();
    double frameMs = 0.0;
    {
        UsdCurvesWriter writer(path, "groom", UsdCurvesWriter::Options{});
        writer.writeFrame(1.0, s, nullptr);
        frameMs = millisecondsSince(t0);
        writer.close();
    }
    const double ms = millisecondsSince(t0);
    const std::vector<std::uint8_t> text = readBytes(path);
    std::printf("[ budget ] USD frame of %zu points: %.0f ms (the frame %.0f ms, the layer %.0f ms), %.1f MB (text %016llx)\n",
                s.pointCount(), ms, frameMs, ms - frameMs, static_cast<double>(text.size()) / 1e6,
                static_cast<unsigned long long>(fnv1a(text.data(), text.size())));
    EXPECT_GT(text.size(), s.pointCount() * 20);
    fs::remove(fs::u8path(path));
}

}  // namespace
}  // namespace groomlab
