#pragma once

#include <doctest/doctest.h>

#include "core/ImageIO.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <cgltf.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace gltftest
{
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lila;
using namespace lila::scene;

inline fs::path tempDir(const std::string& sub)
{
    fs::path dir = fs::temp_directory_path() / "lila_gltf_tests" / sub;
    fs::create_directories(dir);
    return dir;
}

inline std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline void writeAll(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline Graph& graph() { return Graph::current(); }

inline Node& node(const std::string& name)
{
    Node* n = graph().find(name);
    REQUIRE_MESSAGE(n, name);
    return *n;
}

inline Node* addNode(Graph& g, const char* type, const std::string& name, NodeId parent = kNoNode)
{
    return g.createNode(*types::find(type), name, parent, false);
}

inline Node* addMesh(Graph& g, const std::string& name, geo::Mesh m, NodeId parent = kNoNode)
{
    Node* n = addNode(g, "mesh", name, parent);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n;
}

inline void setTrs(Graph& g, Node* n, glm::dvec3 t, glm::dvec3 r = glm::dvec3(0.0), glm::dvec3 s = glm::dvec3(1.0))
{
    g.setValue({n->id(), Transform::kTranslate, -1}, t);
    g.setValue({n->id(), Transform::kRotate, -1}, r);
    g.setValue({n->id(), Transform::kScale, -1}, s);
}

inline glm::dmat4 world(const std::string& name) { return node(name).as<glm::dmat4>(Transform::kWorldMatrix); }

inline bool nearMat(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-4)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0, std::abs(b[c][r])))
                return false;
    return true;
}

inline std::string base64(std::string_view bytes)
{
    static constexpr char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8) | uint8_t(bytes[i + 2]);
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], k[v & 63]};
    }
    if (i < bytes.size())
    {
        uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
        if (i + 1 < bytes.size())
            v |= uint32_t(uint8_t(bytes[i + 1])) << 8;
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

/** A small RGBA PNG; texel (x, y) from fn. */
template <class Fn> inline std::string pngBytes(const fs::path& scratch, int w, int h, Fn&& fn)
{
    std::vector<float> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            glm::vec4 c = fn(x, y);
            for (int k = 0; k < 4; ++k)
                rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(k)] = c[k];
        }
    image::writePng(scratch, w, h, rgba, 8);
    return readAll(scratch);
}

/** A file parsed with cgltf, buffers loaded. */
struct Parsed
{
    cgltf_data* data = nullptr;
    explicit Parsed(const fs::path& p)
    {
        cgltf_options opts{};
        const std::string s = p.string();
        if (cgltf_parse_file(&opts, s.c_str(), &data) != cgltf_result_success)
        {
            data = nullptr;
            return;
        }
        if (cgltf_load_buffers(&opts, data, s.c_str()) != cgltf_result_success || cgltf_validate(data) != cgltf_result_success)
        {
            cgltf_free(data);
            data = nullptr;
        }
    }
    ~Parsed() { cgltf_free(data); }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;

    const cgltf_node* find(const std::string& name) const
    {
        for (size_t i = 0; i < data->nodes_count; ++i)
            if (data->nodes[i].name && name == data->nodes[i].name)
                return &data->nodes[i];
        return nullptr;
    }
    const cgltf_material* material(const std::string& name) const
    {
        for (size_t i = 0; i < data->materials_count; ++i)
            if (data->materials[i].name && name == data->materials[i].name)
                return &data->materials[i];
        return nullptr;
    }
};

inline const cgltf_accessor* attribute(const cgltf_primitive& p, cgltf_attribute_type type, int index = 0)
{
    for (size_t a = 0; a < p.attributes_count; ++a)
        if (p.attributes[a].type == type && p.attributes[a].index == index)
            return p.attributes[a].data;
    return nullptr;
}
} // namespace gltftest
