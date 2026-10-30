#include "Helpers.h"

#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Materials.h"
#include "scene/Morphs.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

#include <cstring>

using namespace gltftest;

namespace
{
/** Builds a glTF document and its single buffer by hand. */
struct Builder
{
    std::string bin;
    json doc = {{"asset", {{"version", "2.0"}, {"generator", "lila tests"}}},
                {"bufferViews", json::array()},
                {"accessors", json::array()}};

    int view(const void* p, size_t bytes)
    {
        while (bin.size() % 4)
            bin.push_back('\0');
        doc["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}});
        bin.append(static_cast<const char*>(p), bytes);
        return int(doc["bufferViews"].size()) - 1;
    }
    template <class T> int view(const std::vector<T>& v) { return view(v.data(), v.size() * sizeof(T)); }

    int accessor(json a)
    {
        doc["accessors"].push_back(std::move(a));
        return int(doc["accessors"].size()) - 1;
    }
    int floats(const std::vector<float>& v, const char* type, size_t comps, bool minMax = false)
    {
        json a = {{"bufferView", view(v)}, {"componentType", 5126}, {"count", v.size() / comps}, {"type", type}};
        if (minMax)
        {
            json lo = json::array(), hi = json::array();
            for (size_t c = 0; c < comps; ++c)
            {
                float mn = 1e30f, mx = -1e30f;
                for (size_t i = c; i < v.size(); i += comps)
                {
                    mn = std::min(mn, v[i]);
                    mx = std::max(mx, v[i]);
                }
                lo.push_back(mn);
                hi.push_back(mx);
            }
            a["min"] = lo;
            a["max"] = hi;
        }
        return accessor(std::move(a));
    }

    /** As .gltf with the buffer as a data URI. */
    fs::path write(const fs::path& path)
    {
        json d = doc;
        if (!bin.empty())
            d["buffers"] = json::array({{{"byteLength", bin.size()}, {"uri", "data:application/octet-stream;base64," + base64(bin)}}});
        writeAll(path, d.dump(1));
        return path;
    }
};

std::vector<float> quadPositions() { return {0, 0, 0, 1, 0, 0, 1, 0, -1, 0, 0, -1}; }

json importReport(const fs::path& file)
{
    run("newScene");
    return run("importGltf", {{"args", {file.string()}}, {"report", true}});
}

MeshPtr meshOf(const std::string& name) { return node(name).as<MeshPtr>(MeshNode::kOutMesh); }

bool hasEntry(const json& list, const std::string& text)
{
    for (const json& s : list)
        if (s.get<std::string>().find(text) != std::string::npos)
            return true;
    return false;
}
} // namespace

TEST_CASE("sparse positions, quantized UVs, byte indices, data URIs and KHR_texture_transform")
{
    Builder b;
    // Positions given only as sparse values over zeros.
    const std::vector<float> pos = quadPositions();
    std::vector<uint8_t> sparseIdx = {0, 1, 2, 3};
    const int idxView = b.view(sparseIdx);
    const int valView = b.view(pos);
    const int position = b.accessor({{"componentType", 5126},
                                     {"count", 4},
                                     {"type", "VEC3"},
                                     {"min", {0, 0, -1}},
                                     {"max", {1, 0, 0}},
                                     {"sparse", {{"count", 4}, {"indices", {{"bufferView", idxView}, {"componentType", 5121}}}, {"values", {{"bufferView", valView}}}}}});
    // KHR_mesh_quantization: normalized unsigned short UVs.
    std::vector<uint16_t> uv = {0, 0, 65535, 0, 65535, 65535, 0, 65535};
    const int texcoord = b.accessor({{"bufferView", b.view(uv)}, {"componentType", 5123}, {"normalized", true}, {"count", 4}, {"type", "VEC2"}});
    std::vector<uint8_t> idx = {0, 1, 2, 0, 2, 3};
    const int indices = b.accessor({{"bufferView", b.view(idx)}, {"componentType", 5121}, {"count", 6}, {"type", "SCALAR"}});
    const std::string png = pngBytes(tempDir("fixtures") / "tex.png", 2, 2, [](int x, int y) { return glm::vec4(float(x), float(y), 0, 1); });
    b.doc["images"] = json::array({{{"uri", "data:image/png;base64," + base64(png)}}});
    b.doc["textures"] = json::array({{{"source", 0}}});
    json transform = {{"offset", {0.5, 0.0}}, {"scale", {0.5, 0.5}}};
    json baseTex = {{"index", 0}};
    baseTex["extensions"]["KHR_texture_transform"] = transform;
    json tiled = {{"name", "Tiled"}};
    tiled["pbrMetallicRoughness"]["baseColorTexture"] = baseTex;
    b.doc["materials"] = json::array({tiled});
    b.doc["meshes"] = json::array({{{"name", "Quad"},
                                    {"primitives", json::array({{{"attributes", {{"POSITION", position}, {"TEXCOORD_0", texcoord}}},
                                                                 {"indices", indices},
                                                                 {"material", 0}}})}}});
    b.doc["nodes"] = json::array({{{"name", "Quad"}, {"mesh", 0}}});
    b.doc["scenes"] = json::array({{{"nodes", {0}}}});
    b.doc["scene"] = 0;
    b.doc["extensionsUsed"] = {"KHR_mesh_quantization", "KHR_texture_transform"};
    b.doc["extensionsRequired"] = {"KHR_mesh_quantization"};
    const fs::path file = b.write(tempDir("fixtures") / "sparse.gltf");

    json r = importReport(file);
    CHECK(r["errors"].empty());
    CHECK(r["warnings"].empty());
    MeshPtr m = meshOf("Quad");
    REQUIRE(m);
    CHECK(m->numPoints() == 4);
    CHECK(m->numFaces() == 2);
    std::span<const glm::vec2> uvs = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(uvs.size() == m->cornerCapacity());
    for (geo::Index c = 0; c < m->cornerCapacity(); ++c)
    {
        const glm::dvec3 p = m->position(m->cornerPoint(c));
        // File vertex (x, -z) in metres had uv (x, -z); the transform maps u to 0.5 + 0.5u and v to 0.5v, then v flips.
        const double u = p.x / 100.0, v = -p.z / 100.0;
        CHECK(uvs[c].x == doctest::Approx(0.5 + 0.5 * u).epsilon(1e-4));
        CHECK(uvs[c].y == doctest::Approx(1.0 - 0.5 * v).epsilon(1e-4));
    }
    CHECK(m->materialNames() == std::vector<std::string>{"Tiled"});
    const std::string map = node("Tiled").as<std::string>(material::kBaseColorMap);
    CHECK(fs::exists(fs::path(map)));
    CHECK(r["images"].size() == 1);
}

TEST_CASE("CUBICSPLINE, STEP and LINEAR channels evaluate like glTF")
{
    Builder b;
    b.doc["nodes"] = json::array({{{"name", "N"}}});
    b.doc["scenes"] = json::array({{{"nodes", {0}}}});
    const int t2 = b.floats({0.0f, 1.0f}, "SCALAR", 1, true);
    const int t3 = b.floats({0.0f, 0.5f, 1.0f}, "SCALAR", 1, true);
    // Cubic translation: in tangent, value, out tangent per key (metres, per second).
    const int cubic = b.floats({0, 0, 0, /**/ 0, 0, 0, /**/ 2, 0, 0, /**/ 0, 0, 0, /**/ 1, 1, 0, /**/ 0, 0, 0}, "VEC3", 3);
    const int steps = b.floats({1, 1, 1, 2, 2, 2, 3, 3, 3}, "VEC3", 3);
    const glm::quat half = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    const int rot = b.floats({0, 0, 0, 1, half.x, half.y, half.z, half.w}, "VEC4", 4);
    b.doc["animations"] = json::array({{{"name", "Move"},
                                        {"samplers", json::array({{{"input", t2}, {"output", cubic}, {"interpolation", "CUBICSPLINE"}},
                                                                  {{"input", t3}, {"output", steps}, {"interpolation", "STEP"}},
                                                                  {{"input", t2}, {"output", rot}, {"interpolation", "LINEAR"}}})},
                                        {"channels", json::array({{{"sampler", 0}, {"target", {{"node", 0}, {"path", "translation"}}}},
                                                                  {{"sampler", 1}, {"target", {{"node", 0}, {"path", "scale"}}}},
                                                                  {{"sampler", 2}, {"target", {{"node", 0}, {"path", "rotation"}}}}})}}});
    const fs::path file = b.write(tempDir("fixtures") / "channels.gltf");
    json r = importReport(file);
    CHECK(r["take"] == "Move");
    CHECK(r["end"].get<double>() == doctest::Approx(1.0));
    Graph& g = graph();
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
    {
        CAPTURE(t);
        g.setTime(t);
        g.evaluate();
        const Node& n = node("N");
        // Hermite between (0, out 2) and (1, in 0) for x; y from 0 to 1 with zero tangents.
        const double s2 = t * t, s3 = s2 * t;
        const double x = (s3 - 2 * s2 + t) * 2.0 + (-2 * s3 + 3 * s2);
        const double y = -2 * s3 + 3 * s2;
        const glm::dvec3 tr = n.as<glm::dvec3>(Transform::kTranslate);
        CHECK(tr.x == doctest::Approx(x * 100.0).epsilon(1e-6));
        CHECK(tr.y == doctest::Approx(y * 100.0).epsilon(1e-6));
        const double step = t < 0.5 ? 1.0 : t < 1.0 ? 2.0 : 3.0;
        CHECK(n.as<glm::dvec3>(Transform::kScale).x == doctest::Approx(step));
        // Slerp a quarter turn about Y.
        const glm::dmat3 rotNow(n.as<glm::dmat4>(Transform::kMatrix));
        const glm::dvec3 xAxis = glm::normalize(rotNow[0]);
        const double angle = glm::degrees(std::atan2(-xAxis.z, xAxis.x));
        CHECK(angle == doctest::Approx(90.0 * t).epsilon(1e-3));
    }
}

TEST_CASE("morph targets with names, default weights and a weights channel")
{
    Builder b;
    const int position = b.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, "VEC3", 3, true);
    const int d0 = b.floats({0, 0, 0, 0, 0, 0, 0, 0, 1}, "VEC3", 3, true);
    const int d1 = b.floats({0, 0, 0, 1, 0, 0, 0, 0, 0}, "VEC3", 3, true);
    b.doc["meshes"] = json::array({{{"name", "Tri"},
                                    {"primitives", json::array({{{"attributes", {{"POSITION", position}}},
                                                                 {"targets", json::array({{{"POSITION", d0}}, {{"POSITION", d1}}})}}})},
                                    {"weights", {0.25, 0.0}},
                                    {"extras", {{"targetNames", {"Push", "Pull"}}}}}});
    b.doc["nodes"] = json::array({{{"name", "Tri"}, {"mesh", 0}}});
    b.doc["scenes"] = json::array({{{"nodes", {0}}}});
    const int times = b.floats({0.0f, 1.0f}, "SCALAR", 1, true);
    const int w = b.floats({0.0f, 0.0f, 1.0f, 0.5f}, "SCALAR", 1);
    b.doc["animations"] = json::array({{{"samplers", json::array({{{"input", times}, {"output", w}}})},
                                        {"channels", json::array({{{"sampler", 0}, {"target", {{"node", 0}, {"path", "weights"}}}}})}}});
    const fs::path file = b.write(tempDir("fixtures") / "morph.gltf");
    json r = importReport(file);
    REQUIRE(r["morphs"].size() == 1);
    Graph& g = graph();
    const NodeId morph = g.find(r["morphs"][0].get<std::string>())->id();
    const std::vector<NodeId> targets = morphs::targetNodes(g, morph);
    REQUIRE(targets.size() == 2);
    CHECK(g.node(targets[0])->as<std::string>(morphs::kMorphName) == "Push");
    CHECK(g.node(targets[1])->as<std::string>(morphs::kMorphName) == "Pull");
    g.setTime(0.5);
    g.evaluate();
    const std::vector<double> weights = morphs::weights(g, morph);
    REQUIRE(weights.size() == 2);
    CHECK(weights[0] == doctest::Approx(0.5));
    CHECK(weights[1] == doctest::Approx(0.25));
    MeshPtr m = meshOf("Tri");
    double topZ = 0.0, rightX = 0.0;
    for (const glm::dvec3& p : m->positions())
    {
        if (p.y > 50.0)
            topZ = p.z;
        if (p.x > 50.0)
            rightX = p.x;
    }
    CHECK(topZ == doctest::Approx(50.0));
    CHECK(rightX == doctest::Approx(125.0));
}

TEST_CASE("EXT_meshopt_compression buffers decode")
{
    const std::vector<float> pos = quadPositions();
    const std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    meshopt_encodeVertexVersion(0);
    std::vector<unsigned char> vbuf(meshopt_encodeVertexBufferBound(4, 12));
    vbuf.resize(meshopt_encodeVertexBuffer(vbuf.data(), vbuf.size(), pos.data(), 4, 12));
    std::vector<unsigned char> ibuf(meshopt_encodeIndexBufferBound(6, 4));
    ibuf.resize(meshopt_encodeIndexBuffer(ibuf.data(), ibuf.size(), idx.data(), 6));

    std::string compressed(vbuf.begin(), vbuf.end());
    while (compressed.size() % 4)
        compressed.push_back('\0');
    const size_t ioff = compressed.size();
    compressed.append(ibuf.begin(), ibuf.end());
    json doc = {{"asset", {{"version", "2.0"}}},
                {"extensionsUsed", {"EXT_meshopt_compression"}},
                {"extensionsRequired", {"EXT_meshopt_compression"}},
                {"buffers", json::array({{{"byteLength", compressed.size()}, {"uri", "data:application/octet-stream;base64," + base64(compressed)}},
                                         {{"byteLength", 72}, {"extensions", {{"EXT_meshopt_compression", {{"fallback", true}}}}}}})},
                {"bufferViews",
                 json::array({{{"buffer", 1},
                               {"byteOffset", 0},
                               {"byteLength", 48},
                               {"byteStride", 12},
                               {"extensions",
                                {{"EXT_meshopt_compression",
                                  {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vbuf.size()}, {"byteStride", 12}, {"count", 4}, {"mode", "ATTRIBUTES"}}}}}},
                              {{"buffer", 1},
                               {"byteOffset", 48},
                               {"byteLength", 24},
                               {"extensions",
                                {{"EXT_meshopt_compression",
                                  {{"buffer", 0}, {"byteOffset", ioff}, {"byteLength", ibuf.size()}, {"byteStride", 4}, {"count", 6}, {"mode", "TRIANGLES"}}}}}}})},
                {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {0, 0, -1}}, {"max", {1, 0, 0}}},
                                           {{"bufferView", 1}, {"componentType", 5125}, {"count", 6}, {"type", "SCALAR"}}})},
                {"meshes", json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", 0}}}, {"indices", 1}}})}}})},
                {"nodes", json::array({{{"name", "Packed"}, {"mesh", 0}}})},
                {"scenes", json::array({{{"nodes", {0}}}})}};
    const fs::path file = tempDir("fixtures") / "meshopt.gltf";
    writeAll(file, doc.dump());
    json r = importReport(file);
    CHECK(r["errors"].empty());
    MeshPtr m = meshOf("Packed");
    REQUIRE(m);
    CHECK(m->numFaces() == 2);
    glm::dvec3 lo(1e9), hi(-1e9);
    for (const glm::dvec3& p : m->positions())
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    CHECK(hi.x == doctest::Approx(100.0));
    CHECK(lo.z == doctest::Approx(-100.0));
}

TEST_CASE("a Draco primitive that doesn't decode and has no fallback is an error, the rest still imports, lights too")
{
    Builder b;
    const int position = b.floats(quadPositions(), "VEC3", 3, true);
    std::vector<uint8_t> junk(16, 7);
    const int dracoView = b.view(junk);
    const int dracoPos = b.accessor({{"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {0, 0, -1}}, {"max", {1, 0, 0}}});
    b.doc["meshes"] = json::array(
        {{{"name", "Plain"}, {"primitives", json::array({{{"attributes", {{"POSITION", position}}}}})}},
         {{"name", "Squashed"},
          {"primitives", json::array({{{"attributes", {{"POSITION", dracoPos}}},
                                       {"extensions", {{"KHR_draco_mesh_compression", {{"bufferView", dracoView}, {"attributes", {{"POSITION", 0}}}}}}}}})}}});
    b.doc["extensions"] = {{"KHR_lights_punctual", {{"lights", json::array({{{"name", "Sun"}, {"type", "directional"}}})}}}};
    b.doc["nodes"] = json::array({{{"name", "Plain"}, {"mesh", 0}},
                                  {{"name", "Squashed"}, {"mesh", 1}},
                                  {{"name", "SunNode"}, {"extensions", {{"KHR_lights_punctual", {{"light", 0}}}}}}});
    b.doc["scenes"] = json::array({{{"nodes", {0, 1, 2}}}});
    b.doc["extensionsUsed"] = {"KHR_draco_mesh_compression", "KHR_lights_punctual"};
    b.doc["extensionsRequired"] = {"KHR_draco_mesh_compression"};
    const fs::path file = b.write(tempDir("fixtures") / "draco.gltf");
    json r = importReport(file);
    REQUIRE(r["errors"].size() == 1);
    CHECK(r["errors"][0].get<std::string>().find("Draco") != std::string::npos);
    CHECK(meshOf("Plain"));
    CHECK(graph().find("Squashed"));
    CHECK(graph().find("SunNode")->type().isA("light"));
    CHECK(r["lights"].size() == 1);
}

TEST_CASE("separate metallic-roughness and occlusion images pack into an ORM map; unlit stays unlit")
{
    Builder b;
    const fs::path dir = tempDir("fixtures");
    const std::string mr = pngBytes(dir / "mr.png", 2, 2, [](int, int) { return glm::vec4(0.3f, 0.5f, 1.0f, 1.0f); });
    const std::string ao = pngBytes(dir / "ao.png", 2, 2, [](int, int) { return glm::vec4(0.25f, 0.25f, 0.25f, 1.0f); });
    writeAll(dir / "mr_ext.png", mr);
    writeAll(dir / "ao_ext.png", ao);
    b.doc["images"] = json::array({{{"uri", "mr_ext.png"}}, {{"uri", "ao_ext.png"}}});
    b.doc["textures"] = json::array({{{"source", 0}}, {{"source", 1}}});
    b.doc["materials"] = json::array(
        {{{"name", "Packed"},
          {"pbrMetallicRoughness", {{"metallicFactor", 0.5}, {"roughnessFactor", 1.0}, {"metallicRoughnessTexture", {{"index", 0}}}}},
          {"occlusionTexture", {{"index", 1}}}},
         {{"name", "Flat"}, {"pbrMetallicRoughness", {{"baseColorFactor", {0.2, 0.4, 0.6, 1.0}}}}, {"extensions", {{"KHR_materials_unlit", json::object()}}}}});
    b.doc["extensionsUsed"] = {"KHR_materials_unlit"};
    const int position = b.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, "VEC3", 3, true);
    b.doc["meshes"] = json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", position}}}, {"material", 0}},
                                                                {{"attributes", {{"POSITION", position}}}, {"material", 1}}})}}});
    b.doc["nodes"] = json::array({{{"name", "M"}, {"mesh", 0}}});
    b.doc["scenes"] = json::array({{{"nodes", {0}}}});
    const fs::path file = b.write(dir / "orm.gltf");
    json r = importReport(file);
    CHECK(r["errors"].empty());
    const std::string orm = node("Packed").as<std::string>(material::kOrmMap);
    REQUIRE(!orm.empty());
    image::Image img = image::read(fs::path(orm));
    REQUIRE(img.width == 2);
    CHECK(img.rgba[0] == doctest::Approx(0.25).epsilon(0.02));
    CHECK(img.rgba[1] == doctest::Approx(0.5).epsilon(0.02));
    CHECK(img.rgba[2] == doctest::Approx(0.5).epsilon(0.02));
    CHECK(node("Flat").as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Unlit));
    CHECK(node("Flat").as<glm::dvec3>(material::kBaseColor).b == doctest::Approx(0.6));
    CHECK(node("Flat").as<glm::dvec3>(material::kEmissiveColor) == glm::dvec3(0.0));
    // Both slots of the one mesh: two primitives of the same points weld into one mesh.
    CHECK(meshOf("M")->materialNames() == std::vector<std::string>{"Packed", "Flat"});
}

TEST_CASE("texture samplers become each map's wrap and filter")
{
    Builder b;
    const fs::path dir = tempDir("fixtures");
    writeAll(dir / "px.png", pngBytes(dir / "px_src.png", 2, 2, [](int x, int) { return glm::vec4(float(x), 0.0f, 0.0f, 1.0f); }));
    b.doc["images"] = json::array({{{"uri", "px.png"}}});
    // 9728 NEAREST, 33071 CLAMP_TO_EDGE, 33648 MIRRORED_REPEAT, 10497 REPEAT.
    b.doc["samplers"] = json::array({{{"magFilter", 9728}, {"minFilter", 9984}, {"wrapS", 33071}, {"wrapT", 33648}}, {{"wrapS", 33648}}});
    b.doc["textures"] = json::array({{{"source", 0}, {"sampler", 0}}, {{"source", 0}, {"sampler", 1}}, {{"source", 0}}});
    b.doc["materials"] = json::array({{{"name", "Pixel"},
                                       {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}},
                                       {"normalTexture", {{"index", 1}}},
                                       {"emissiveTexture", {{"index", 2}}},
                                       {"emissiveFactor", {1.0, 1.0, 1.0}}}});
    const int position = b.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, "VEC3", 3, true);
    b.doc["meshes"] = json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", position}}}, {"material", 0}}})}}});
    b.doc["nodes"] = json::array({{{"name", "P"}, {"mesh", 0}}});
    b.doc["scenes"] = json::array({{{"nodes", {0}}}});
    json r = importReport(b.write(dir / "samplers.gltf"));
    CHECK(r["errors"].empty());
    const scene::Node& m = node("Pixel");
    const material::TextureSampling base = materials::sampling(m, material::kBaseColorMap);
    CHECK(base.wrapU == material::TextureWrap::Clamp);
    CHECK(base.wrapV == material::TextureWrap::Mirror);
    CHECK(base.filter == material::TextureFilter::Nearest);
    const material::TextureSampling normal = materials::sampling(m, material::kNormalMap);
    CHECK(normal.wrapU == material::TextureWrap::Mirror);
    CHECK(normal.wrapV == material::TextureWrap::Repeat);
    CHECK(normal.filter == material::TextureFilter::Linear);
    CHECK(materials::sampling(m, material::kEmissiveMap).isDefault());
}

TEST_CASE("a perspective camera keeps its vertical field of view")
{
    Builder b;
    b.doc["cameras"] = json::array({{{"type", "perspective"}, {"perspective", {{"yfov", 0.8}, {"aspectRatio", 1.5}, {"znear", 0.1}, {"zfar", 200.0}}}}});
    b.doc["nodes"] = json::array({{{"name", "Cam"}, {"camera", 0}, {"translation", {0, 1, 5}}}});
    b.doc["scenes"] = json::array({{{"nodes", {0}}}});
    const fs::path file = b.write(tempDir("fixtures") / "camera.gltf");
    json r = importReport(file);
    REQUIRE(r["cameras"].size() == 1);
    const glm::dvec2 fov = camera::angleOfView(node("Cam"), 1.5);
    CHECK(fov.y == doctest::Approx(glm::degrees(0.8)).epsilon(1e-4));
    CHECK(node("Cam").as<double>(Camera::kNearClipPlane) == doctest::Approx(10.0));
    CHECK(node("Cam").as<double>(Camera::kFarClipPlane) == doctest::Approx(20000.0));
    CHECK(node("Cam").as<glm::dvec3>(Transform::kTranslate).z == doctest::Approx(500.0));
}
