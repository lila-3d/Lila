#include "Helpers.h"

#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "scene/Morphs.h"
#include "scene/Skin.h"

#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>

#include <cstring>

using namespace gltftest;

namespace
{
/** World position, uv0 and normal of every corner. */
struct Corner
{
    glm::dvec3 p;
    glm::dvec2 uv;
    glm::dvec3 n;
};

std::vector<Corner> meshCorners(const std::string& name)
{
    MeshPtr m = node(name).as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    const glm::dmat4 w = world(name);
    std::vector<glm::vec3> computed;
    std::span<const glm::vec3> n = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (n.size() != m->cornerCapacity())
    {
        computed = geo::cornerNormals(*m);
        n = computed;
    }
    std::span<const glm::vec2> uv = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::vector<Corner> out;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
            m->forEachFaceCorner(f, [&](geo::Index c) {
                out.push_back({glm::dvec3(w * glm::dvec4(m->position(m->cornerPoint(c)), 1.0)), glm::dvec2(uv[c]), glm::normalize(glm::dvec3(n[c]))});
            });
    return out;
}

/** Every corner of a has a corner in b at the same place with nearly the same uv and normal. */
void sameCorners(const std::vector<Corner>& a, const std::vector<Corner>& b, double posTol, double uvTol, double nTol)
{
    double worstP = 0.0, worstUv = 0.0, worstN = 0.0;
    for (const Corner& c : a)
    {
        double best = 1e30;
        const Corner* match = nullptr;
        for (const Corner& d : b)
        {
            const double score = glm::length(c.p - d.p) + glm::length(c.uv - d.uv) * 100.0 + glm::length(c.n - d.n) * 10.0;
            if (score < best)
            {
                best = score;
                match = &d;
            }
        }
        REQUIRE(match);
        worstP = std::max(worstP, glm::length(c.p - match->p));
        worstUv = std::max(worstUv, glm::length(c.uv - match->uv));
        worstN = std::max(worstN, glm::length(c.n - match->n));
    }
    MESSAGE("worst position " << worstP << " cm, uv " << worstUv << ", normal " << worstN);
    CHECK(worstP < posTol);
    CHECK(worstUv < uvTol);
    CHECK(worstN < nTol);
}

geo::Mesh paintedCube()
{
    geo::Mesh m = geo::makeCube({.width = 100, .height = 60, .depth = 40});
    m.materialNames() = {"M_A", "M_B"};
    std::span<int32_t> mat = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (size_t f = 0; f < mat.size(); ++f)
        mat[f] = int32_t(f % 2);
    std::span<glm::vec4> col = geo::colors::add(m, "Paint");
    for (size_t c = 0; c < col.size(); ++c)
        col[c] = glm::vec4(float(c % 3) / 2.0f, 0.25f, 1.0f, 1.0f);
    return m;
}
} // namespace

TEST_CASE("draco: a static mesh round-trips compressed")
{
    run("newScene");
    Graph& g = graph();
    Node* box = addMesh(g, "Box", paintedCube());
    setTrs(g, box, {10, 20, 30}, {15, 30, 45});
    g.evaluate();
    const std::vector<Corner> before = meshCorners("Box");
    const size_t points = node("Box").as<MeshPtr>(MeshNode::kOutMesh)->numPoints();

    const fs::path file = tempDir("draco") / "box.glb";
    const fs::path plain = tempDir("draco") / "box_plain.glb";
    run("exportGltf", {{"args", {file.string(), "Box"}}, {"draco", true}});
    run("exportGltf", {{"args", {plain.string(), "Box"}}});
    {
        Parsed p(file);
        REQUIRE(p.data);
        REQUIRE(p.data->extensions_required_count == 1);
        CHECK(std::string(p.data->extensions_required[0]) == "KHR_draco_mesh_compression");
        const cgltf_mesh* mesh = p.find("Box")->mesh;
        REQUIRE(mesh->primitives_count == 2);
        for (size_t i = 0; i < mesh->primitives_count; ++i)
        {
            const cgltf_primitive& prim = mesh->primitives[i];
            CHECK(prim.has_draco_mesh_compression);
            const cgltf_accessor* pos = attribute(prim, cgltf_attribute_type_position);
            REQUIRE(pos);
            CHECK(pos->buffer_view == nullptr);
            CHECK(pos->has_min);
            CHECK(attribute(prim, cgltf_attribute_type_color, 0));
            CHECK(attribute(prim, cgltf_attribute_type_tangent));
        }
    }
    CHECK(fs::file_size(file) < fs::file_size(plain));

    run("newScene");
    json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["errors"].empty());
    CHECK(r["warnings"].empty());
    CHECK(node("Box").as<MeshPtr>(MeshNode::kOutMesh)->numPoints() == points);
    CHECK(node("Box").as<MeshPtr>(MeshNode::kOutMesh)->materialNames() == std::vector<std::string>{"M_A", "M_B"});
    // 14 bits over a metre is about 0.006 cm, 12-bit UVs about 2.5e-4, 10-bit normals about 0.002.
    sameCorners(before, meshCorners("Box"), 0.02, 1e-3, 1e-2);
}

TEST_CASE("draco: a primitive built with the encoder decodes, with normalized colors, skin data and an uncompressed morph target")
{
    // A quad, its points in order (sequential encoding) so the morph target accessor lines up.
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 1, 0, -1, 0, 0, -1};
    const std::vector<float> uv = {0, 0, 1, 0, 1, 1, 0, 1};
    const std::vector<uint8_t> color = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128};
    const std::vector<uint8_t> joints = {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const std::vector<float> weights = {0.75f, 0.25f, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    draco::Mesh dm;
    dm.set_num_points(4);
    dm.AddFace({draco::PointIndex(0), draco::PointIndex(1), draco::PointIndex(2)});
    dm.AddFace({draco::PointIndex(0), draco::PointIndex(2), draco::PointIndex(3)});
    auto add = [&](draco::GeometryAttribute::Type type, int comps, draco::DataType dt, bool normalized, size_t size, const void* src) {
        draco::GeometryAttribute ga;
        ga.Init(type, nullptr, uint8_t(comps), dt, normalized, int64_t(size * size_t(comps)), 0);
        const int id = dm.AddAttribute(ga, true, 4);
        for (uint32_t p = 0; p < 4; ++p)
            dm.attribute(id)->SetAttributeValue(draco::AttributeValueIndex(p), static_cast<const uint8_t*>(src) + p * size * size_t(comps));
        return int(dm.attribute(id)->unique_id());
    };
    const int idPos = add(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32, false, 4, pos.data());
    const int idUv = add(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32, false, 4, uv.data());
    const int idCol = add(draco::GeometryAttribute::COLOR, 4, draco::DT_UINT8, true, 1, color.data());
    const int idJoints = add(draco::GeometryAttribute::GENERIC, 4, draco::DT_UINT8, false, 1, joints.data());
    const int idWeights = add(draco::GeometryAttribute::GENERIC, 4, draco::DT_FLOAT32, false, 4, weights.data());
    draco::Encoder enc;
    enc.SetEncodingMethod(draco::MESH_SEQUENTIAL_ENCODING);
    enc.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 16);
    enc.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 14);
    draco::EncoderBuffer buffer;
    REQUIRE(enc.EncodeMeshToBuffer(dm, &buffer).ok());

    std::string bin(buffer.data(), buffer.size());
    while (bin.size() % 4)
        bin.push_back('\0');
    const size_t morphOffset = bin.size();
    const std::vector<float> morph = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    bin.append(reinterpret_cast<const char*>(morph.data()), morph.size() * sizeof(float));
    // The extension's ids must be valid accessor indices for cgltf, so the compressed accessors come first.
    json accessors = json::array();
    for (int i = 0; i < 6; ++i)
        accessors.push_back(nullptr);
    accessors[size_t(idPos)] = {{"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {0, 0, -1}}, {"max", {1, 0, 0}}};
    accessors[size_t(idUv)] = {{"componentType", 5126}, {"count", 4}, {"type", "VEC2"}};
    accessors[size_t(idCol)] = {{"componentType", 5121}, {"normalized", true}, {"count", 4}, {"type", "VEC4"}};
    accessors[size_t(idJoints)] = {{"componentType", 5121}, {"count", 4}, {"type", "VEC4"}};
    accessors[size_t(idWeights)] = {{"componentType", 5126}, {"count", 4}, {"type", "VEC4"}};
    REQUIRE(accessors[5].is_null());
    accessors[5] = {{"componentType", 5123}, {"count", 6}, {"type", "SCALAR"}};
    accessors.push_back({{"bufferView", 1}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {0, 1, 0}}});
    json doc = {{"asset", {{"version", "2.0"}}},
                {"extensionsUsed", {"KHR_draco_mesh_compression"}},
                {"extensionsRequired", {"KHR_draco_mesh_compression"}},
                {"buffers", json::array({{{"byteLength", bin.size()}, {"uri", "data:application/octet-stream;base64," + base64(bin)}}})},
                {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", buffer.size()}},
                                             {{"buffer", 0}, {"byteOffset", morphOffset}, {"byteLength", morph.size() * sizeof(float)}}})},
                {"accessors", accessors}};
    json draco = {{"bufferView", 0},
                  {"attributes", {{"POSITION", idPos}, {"TEXCOORD_0", idUv}, {"COLOR_0", idCol}, {"JOINTS_0", idJoints}, {"WEIGHTS_0", idWeights}}}};
    json prim = {{"attributes", {{"POSITION", idPos}, {"TEXCOORD_0", idUv}, {"COLOR_0", idCol}, {"JOINTS_0", idJoints}, {"WEIGHTS_0", idWeights}}},
                 {"indices", 5},
                 {"targets", json::array({{{"POSITION", 6}}})}};
    prim["extensions"]["KHR_draco_mesh_compression"] = draco;
    doc["meshes"] = json::array({{{"name", "Quad"}, {"primitives", json::array({prim})}, {"weights", {1.0}}}});
    doc["nodes"] = json::array({{{"name", "Quad"}, {"mesh", 0}, {"skin", 0}}, {{"name", "j0"}, {"children", {2}}}, {{"name", "j1"}, {"translation", {1, 0, 0}}}});
    doc["skins"] = json::array({{{"joints", {1, 2}}}});
    doc["scenes"] = json::array({{{"nodes", {0, 1}}}});
    const fs::path file = tempDir("draco") / "fixture.gltf";
    writeAll(file, doc.dump());

    run("newScene");
    json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["errors"].empty());
    CHECK(r["skinClusters"].size() == 1);
    CHECK(r["morphs"].size() == 1);
    Graph& g = graph();
    const NodeId quad = node("Quad").id();
    const NodeId sc = skin::find(g, quad);
    REQUIRE(sc != kNoNode);
    MeshPtr bind = skin::inputMesh(g, sc);
    REQUIRE(bind);
    CHECK(bind->numPoints() == 4);
    WeightTable t = skin::table(g, sc);
    const auto infs = skin::influences(g, sc);
    std::span<const glm::vec2> uvs = bind->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::span<const glm::vec4> cols = geo::colors::values(*bind, geo::colors::sets(*bind).at(0));
    for (geo::Index c = 0; c < bind->cornerCapacity(); ++c)
    {
        // The skin reads the mesh after the morph, which raises point 1 at full weight, so points match by x and z.
        const glm::dvec3 p = bind->position(bind->cornerPoint(c));
        int v = -1;
        for (int k = 0; k < 4; ++k)
            if (glm::length(glm::dvec2(p.x, p.z) - glm::dvec2(pos[k * 3], pos[k * 3 + 2]) * 100.0) < 0.01)
                v = k;
        REQUIRE(v >= 0);
        CHECK(uvs[c].x == doctest::Approx(uv[size_t(v) * 2]).epsilon(1e-3));
        CHECK(uvs[c].y == doctest::Approx(1.0 - uv[size_t(v) * 2 + 1]).epsilon(1e-3));
        for (int k = 0; k < 4; ++k)
            CHECK(cols[c][k] == doctest::Approx(color[size_t(v) * 4 + size_t(k)] / 255.0).epsilon(1e-3));
        float w0 = 0.0f;
        for (const auto& [id, w] : t.points[bind->cornerPoint(c)])
            for (const skin::Influence& inf : infs)
                if (inf.index == id && g.node(inf.joint)->name() == "j0")
                    w0 = w;
        CHECK(w0 == doctest::Approx(weights[size_t(v) * 4] * (joints[size_t(v) * 4] == 0 ? 1.0f : 0.0f) +
                                    weights[size_t(v) * 4 + 1] * (joints[size_t(v) * 4 + 1] == 0 ? 1.0f : 0.0f))
                         .epsilon(1e-3));
    }
    // The morph target moves the second point up a metre, at full weight.
    g.evaluate();
    MeshPtr out = node("Quad").as<MeshPtr>(MeshNode::kOutMesh);
    double top = -1e9;
    for (const glm::dvec3& p : out->positions())
        top = std::max(top, p.y);
    CHECK(top == doctest::Approx(100.0).epsilon(1e-3));
}
