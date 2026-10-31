#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/PackedArray.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "io/SceneFile.h"
#include "io/SceneFormats.h"
#include "io/ScenePreview.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/MeshData.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json sampleDoc()
{
    json big = json::array();
    for (int i = 0; i < 300; ++i)
        big.push_back({i * 0.5, -i * 1.25, 1e-7 * i});
    json flat = json::array();
    for (int i = 0; i < 100; ++i)
        flat.push_back(i);
    return {
        {"lila", 1},
        {"upAxis", "y"},
        {"units", "cm"},
        {"time", 1.5},
        {"nodes", json::array({
                      {{"name", "Rig"}, {"type", "transform"}},
                      {{"name", "Target"}, {"type", "locator"}, {"parent", "Rig"},
                       {"attrs", {{"translate", {0.1, 10, -3.25}}, {"visibility", false}, {"rotateOrder", 2}, {"note", "a \"quoted\" line"},
                                  {"points", big}, {"ids", flat}}}},
                  })},
        {"connections", json::array({json::array({"Rig.translateX", "Target.translateY"})})},
        {"selection", json::array({"Target", "Rig"})},
        {"future", {{"k", 1}}},
    };
}

/** Numbers come back as doubles from blobs and as ints from text; compare by value. */
bool sameValue(const json& a, const json& b)
{
    if (a.is_number() && b.is_number())
        return std::abs(a.get<double>() - b.get<double>()) <= 1e-12 * std::max(1.0, std::abs(a.get<double>()));
    if (a.type() != b.type() || a.size() != b.size())
        return false;
    if (a.is_array())
    {
        for (size_t i = 0; i < a.size(); ++i)
            if (!sameValue(a[i], b[i]))
                return false;
        return true;
    }
    if (a.is_object())
    {
        for (auto& [k, v] : a.items())
            if (!b.contains(k) || !sameValue(v, b[k]))
                return false;
        return true;
    }
    return a == b;
}
} // namespace

TEST_CASE("binary scene round trip with blobs")
{
    json doc = sampleDoc();
    std::string bytes = io::encodeBinary(doc);
    CHECK(io::detectFormat(bytes) == io::SceneFormat::Binary);
    std::vector<std::string> warnings;
    json back = io::decodeBinary(bytes, &warnings);
    CHECK(warnings.empty());
    CHECK(sameValue(doc, back));
}

TEST_CASE("ascii scene round trip")
{
    json doc = sampleDoc();
    std::string text = io::encodeAscii(doc);
    CHECK(io::detectFormat(text) == io::SceneFormat::Ascii);
    CHECK(text.find("node locator Target") != std::string::npos);
    CHECK(text.find("  translate 0.1 10 -3.25") != std::string::npos);
    std::vector<std::string> warnings;
    json back = io::decodeAscii(text, &warnings);
    CHECK(warnings.empty());
    doc["lila"] = back["lila"];
    CHECK(sameValue(doc, back));
    // Deterministic: encoding twice gives identical bytes.
    CHECK(io::encodeAscii(back) == text);
}

TEST_CASE("ascii survives damaged lines")
{
    std::string text = io::encodeAscii(sampleDoc());
    auto at = text.find("  visibility false");
    REQUIRE(at != std::string::npos);
    text.replace(at, 18, "  visibility @@garbage");
    text += "bogus line here\n";
    std::vector<std::string> warnings;
    json back = io::decodeAscii(text, &warnings);
    CHECK(warnings.size() == 2);
    CHECK(back["nodes"][1]["attrs"]["translate"][1] == 10);
    CHECK(back["selection"].size() == 2);
}

TEST_CASE("binary loses only a damaged blob")
{
    std::string bytes = io::encodeBinary(sampleDoc());
    // Flip a byte near the end: inside the last blob's payload.
    bytes[bytes.size() - 5] ^= 0x5A;
    std::vector<std::string> warnings;
    json back = io::decodeBinary(bytes, &warnings);
    CHECK(!warnings.empty());
    CHECK(back["nodes"][1]["attrs"]["translate"][1] == 10);
}

TEST_CASE("save and open both formats")
{
    ops::execute("newScene", json::object());
    ops::execute("spaceLocator", {{"name", "Saved"}, {"position", {1, 2, 3}}});
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("formats") + ext)).string();
        ops::execute("saveScene", {{"args", {path}}});
        ops::execute("newScene", json::object());
        CHECK(!ops::execute("objExists", {{"args", {"Saved"}}}).get<bool>());
        ops::execute("openScene", {{"args", {path}}});
        CHECK(ops::execute("getAttr", {{"args", {"Saved.ty"}}}).get<double>() == doctest::Approx(2));
    }
}

namespace
{
uint32_t testCrc(std::string_view s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s)
    {
        c ^= b;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

template <class T> void putRaw(std::string& out, T v)
{
    out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

void putChunk(std::string& out, const char* fourcc, std::string_view payload)
{
    out.append(fourcc, 4);
    putRaw<uint32_t>(out, 0); // stored uncompressed
    putRaw<uint32_t>(out, testCrc(payload));
    putRaw<uint64_t>(out, payload.size());
    putRaw<uint64_t>(out, payload.size());
    out += payload;
}

geo::Mesh layeredMesh()
{
    geo::Mesh m = geo::makeCube({.subdivX = 5, .subdivY = 4, .subdivZ = 3});
    m.setPosition(1, glm::dvec3(0.1, 1.0 / 3.0, -2.0 / 7.0));
    auto ids = m.pointAttrs().getOrAdd<int32_t>("testIds");
    for (size_t i = 0; i < ids.size(); ++i)
        ids[i] = int32_t(i) * 7 - 40;
    auto hard = m.edgeAttrs().getOrAdd<bool>("testHard");
    for (size_t i = 0; i < hard.size(); ++i)
        hard[i] = i % 3 == 0;
    auto tint = m.cornerAttrs().getOrAdd<glm::vec3>("testTint");
    for (size_t i = 0; i < tint.size(); ++i)
        tint[i] = glm::vec3(float(i) / 7.0f, -0.1f, 1e-20f);
    // Holes in the point and face arrays, so stored order and storage order differ.
    m.removeFace(3);
    m.removeFace(10);
    m.removeAllLoose();
    return m;
}
} // namespace

TEST_CASE("binary keeps numeric types and values exactly")
{
    json ints = json::array(), wide = json::array(), floats = json::array(), doubles = json::array();
    for (int i = 0; i < 200; ++i)
    {
        ints.push_back(i * 1000 - 70000);
        wide.push_back(uint64_t(4000000000u) + uint64_t(i));
        floats.push_back(double(float(i) * 0.37f));
        doubles.push_back(1.0 / (i + 3.0));
    }
    json doc = {{"lila", 1}, {"ints", ints}, {"wide", wide}, {"floats", floats}, {"doubles", doubles}};
    std::vector<std::string> warnings;
    json back = io::decodeBinary(io::encodeBinary(doc), &warnings);
    CHECK(warnings.empty());
    CHECK(back == doc);
    CHECK(back["ints"][5].is_number_integer());
    CHECK(back["wide"][5].get<uint64_t>() == 4000000005u);
    CHECK(back["doubles"][7].get<double>() == 1.0 / 10.0);
}

TEST_CASE("packed meshes write the same document as plain ones")
{
    geo::Mesh m = layeredMesh();
    json plain = scene::meshToJson(m);
    json doc = {{"lila", 1}, {"mesh", scene::meshToJsonPacked(m)}};
    std::vector<std::string> warnings;
    json back = io::decodeBinary(io::encodeBinary(doc), &warnings);
    CHECK(warnings.empty());
    CHECK(packed::is(back["mesh"]["points"]));
    json backPlain = back["mesh"];
    packed::unpackAll(backPlain);
    CHECK(backPlain == plain);
    CHECK(scene::meshToJson(*scene::meshFromJson(back["mesh"])) == scene::meshToJson(*scene::meshFromJson(plain)));

    json unpacked = doc;
    packed::unpackAll(unpacked);
    CHECK(unpacked["mesh"] == plain);
}

TEST_CASE("big arrays span several chunks and lose only themselves when damaged")
{
    json big = json::array(), small = json::array();
    for (int i = 0; i < 400000; ++i) // 3.2 MB of doubles: four chunks
        big.push_back(1.0 / (i + 1.0));
    for (int i = 0; i < 100; ++i)
        small.push_back(i * 0.5);
    json doc = {{"lila", 1}, {"big", big}, {"small", small}};
    std::string bytes = io::encodeBinary(doc);
    std::vector<std::string> warnings;
    CHECK(io::decodeBinary(bytes, &warnings) == doc);
    CHECK(warnings.empty());

    // Damage the first continuation part.
    size_t part = bytes.find("BLB+");
    REQUIRE(part != std::string::npos);
    bytes[part + 4 + 4 + 4 + 8 + 8 + 100] ^= 0x5A;
    json back = io::decodeBinary(bytes, &warnings);
    CHECK(!warnings.empty());
    CHECK(back["big"].empty());
    CHECK(back["small"] == small);
}

TEST_CASE("version 2 binary files still load")
{
    std::vector<double> values;
    for (int i = 0; i < 99; ++i)
        values.push_back(i * 0.25 - 3.0);
    json header = {{"lila", 1}, {"points", {{"$blob", 0}, {"shape", {33, 3}}}}, {"tag", "old"}};
    std::string bytes("LILA\0\r\n\x1a", 8);
    putRaw<uint32_t>(bytes, 2);
    putRaw<uint32_t>(bytes, 2);
    putChunk(bytes, "JSON", header.dump());
    putChunk(bytes, "BLOB", std::string_view(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(double)));
    std::vector<std::string> warnings;
    json back = io::decodeBinary(bytes, &warnings);
    CHECK(warnings.empty());
    REQUIRE(back["points"].size() == 33);
    CHECK(back["points"][32][2].get<double>() == values[98]);
    CHECK(back["tag"] == "old");
}

TEST_CASE("scene preview reads stored meshes, transforms and instances without loading")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    auto* box = g.createNode(*scene::types::find("mesh"), "Box", scene::kNoNode, false);
    g.setValue({box->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
    auto* group = g.createNode(*scene::types::find("transform"), "Group", scene::kNoNode, false);
    g.setValue({group->id(), scene::Transform::kTranslate, -1}, glm::dvec3(1000.0, 0.0, 0.0));
    auto* copy = g.createNode(*scene::types::find("mesh"), "Copy", group->id(), false);
    scene::instancing::connectInstance(g, box->id(), copy->id());
    auto* hidden = g.createNode(*scene::types::find("mesh"), "Hidden", scene::kNoNode, false);
    g.setValue({hidden->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
    g.setValue({hidden->id(), scene::Transform::kVisibility, -1}, false);

    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        const auto path = std::filesystem::temp_directory_path() / (std::string("lila_scene_preview") + ext);
        io::save(path);
        const std::vector<glm::vec3> tris = io::previewTriangles(path);
        std::filesystem::remove(path);
        // Two cubes of 6 quads, 2 triangles each.
        REQUIRE(tris.size() == 2 * 12 * 3);
        float maxX = -1e9f;
        for (const glm::vec3& p : tris)
            maxX = std::max(maxX, p.x);
        CHECK(maxX == doctest::Approx(1050.0));
    }
    g.clear();
}
