#include "collada/Collada.h"

#include "collada/ColladaInternal.h"
#include "collada/Xml.h"
#include "core/Profile.h"
#include "fbx/ImportCommon.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"
#include "scene/Light.h"
#include "scene/Materials.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace lila::collada
{
namespace
{
using fbx::ExportNode;
using fbx::ExportScene;
using fbx::ExportTake;
using geo::Index;
namespace lights = scene::lights;

std::string num(double v)
{
    if (std::abs(v) < 1e-12)
        return "0";
    return std::format("{:.9g}", v);
}

/** Row-major, as COLLADA lists matrices. */
void appendMatrix(std::string& out, const glm::dmat4& m)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            if (!out.empty())
                out += ' ';
            out += num(m[c][r]);
        }
}

std::string matrixText(const glm::dmat4& m)
{
    std::string s;
    appendMatrix(s, m);
    return s;
}

template <class T> std::string joined(const std::vector<T>& v)
{
    std::string s;
    s.reserve(v.size() * 8);
    for (const T& x : v)
    {
        if (!s.empty())
            s += ' ';
        if constexpr (std::is_floating_point_v<T>)
            s += num(double(x));
        else
            s += std::to_string(x);
    }
    return s;
}

/** An xs:ID from a name: letters, digits, '_', '-' and '.', not starting with a digit. */
std::string idFrom(std::string_view name)
{
    std::string id;
    for (char c : name)
        id += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' ? c : '_';
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0])) || id[0] == '-' || id[0] == '.')
        id.insert(0, "_");
    return id;
}

struct Ids
{
    std::set<std::string> used;
    std::string make(std::string_view name)
    {
        std::string base = idFrom(name), id = base;
        for (int k = 1; !used.insert(id).second; ++k)
            id = std::format("{}_{}", base, k);
        return id;
    }
};

std::string vec3Text(const glm::dvec3& v) { return std::format("{} {} {}", num(v.x), num(v.y), num(v.z)); }

std::string rotateText(int axis, double degrees)
{
    return std::format("{} {} {} {}", axis == 0 ? 1 : 0, axis == 1 ? 1 : 0, axis == 2 ? 1 : 0, num(degrees));
}

/** Rotate elements in the order a node lists them: the last-applied axis first, as COLLADA multiplies left to right. */
std::array<int, 3> rotateListing(scene::RotateOrder order)
{
    static const std::array<int, 3> kApplied[6] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
    const std::array<int, 3>& a = kApplied[std::clamp(int(order), 0, 5)];
    return {a[2], a[1], a[0]};
}

/** A node's transform as Maya's elements, file axes. */
struct Elements
{
    glm::dvec3 t{0.0}, jo{0.0}, r{0.0}, s{1.0};
    bool joint = false;
    scene::RotateOrder order = scene::RotateOrder::XYZ;
    glm::dmat3 pre{1.0};
};

struct MeshOut
{
    std::string geometryId;
    std::vector<std::string> slots; // material symbol per polylist
    bool hasUv = false;
};

struct Exporter
{
    const gltf::ExportSnapshot& snap;
    const ExportScene& scene;
    const ExportOptions& opts;
    fs::path outDir;
    std::vector<std::string>& warnings;
    xml::Writer w;
    Ids ids;
    glm::dmat3 B{1.0};
    glm::dmat4 K{1.0}, Kinv{1.0};
    std::vector<std::string> nodeId;     // per node
    std::vector<MeshOut> meshes;         // per node, geometry written for meshes
    std::vector<std::string> controller; // per node, skin controller id
    std::vector<std::string> lightId, cameraId;
    std::map<std::string, std::string> materialId; // slot -> material id
    std::map<std::string, std::string> imageId;    // absolute path -> image id
    std::vector<Elements> elements;                // per node, the rest transform
    double timelineStart = 0.0, timelineEnd = -1.0;

    /** Maya's elements of file-space local m of node i, rotation angles near ref. */
    Elements decomposeElements(size_t i, const glm::dmat4& m, const glm::dvec3& ref) const
    {
        Elements e = elements[i];
        const fbx::Trs trs = fbx::decomposeWith(m, e.pre, e.order, ref);
        e.t = trs.t;
        e.r = trs.r;
        e.s = trs.s;
        return e;
    }

    glm::dmat4 fileLocal(size_t i, const glm::dmat4& local) const
    {
        const ExportNode& n = scene.nodes[i];
        glm::dmat4 m = K * local * Kinv;
        // Cameras and lights look down -Z in both, so their frames turn with the axes and their children turn back.
        auto turned = [](const ExportNode& e) { return e.camera || e.light; };
        if (turned(n))
            m = m * glm::dmat4(B);
        if (n.parent >= 0 && turned(scene.nodes[size_t(n.parent)]))
            m = glm::transpose(glm::dmat4(B)) * m;
        return m;
    }

    void asset();
    void images();
    void effectsAndMaterials();
    void geometries();
    void controllers();
    void lightsAndCameras();
    void animations();
    void visualScene();
    void node(size_t i, const std::vector<std::vector<size_t>>& children);
    std::string imageFor(const std::string& path);
    void writeMesh(size_t i);
};

void Exporter::asset()
{
    w.open("asset");
    w.open("contributor");
    w.leaf("authoring_tool", "Lila");
    w.close();
    w.leaf("created", "2000-01-01T00:00:00Z");
    w.leaf("modified", "2000-01-01T00:00:00Z");
    const double meters = opts.gather.unitScale * 0.01;
    const std::string unitName = std::abs(opts.gather.unitScale - 1.0) < 1e-9     ? "centimeter"
                                 : std::abs(opts.gather.unitScale - 100.0) < 1e-9 ? "meter"
                                 : std::abs(opts.gather.unitScale - 0.1) < 1e-9   ? "millimeter"
                                 : std::abs(opts.gather.unitScale - 2.54) < 1e-9  ? "inch"
                                 : std::abs(opts.gather.unitScale - 30.48) < 1e-9 ? "foot"
                                                                                  : "unit";
    w.empty("unit", {{"name", unitName}, {"meter", num(meters)}});
    w.leaf("up_axis", "Y_UP");
    w.close();
}

std::string Exporter::imageFor(const std::string& path)
{
    if (path.empty())
        return {};
    if (auto it = imageId.find(path); it != imageId.end())
        return it->second;
    const std::string id = ids.make(textOf(pathOf(path).stem()) + "-image");
    imageId[path] = id;
    return id;
}

void Exporter::images()
{
    if (!opts.textures)
        return;
    for (const gltf::MaterialSnapshot& m : snap.materials)
        for (const std::string* p : {&m.baseColorMap, &m.emissiveMap, &m.normalMap})
            imageFor(*p);
    if (imageId.empty())
        return;
    w.open("library_images");
    std::set<std::string> copied;
    for (const auto& [path, id] : imageId)
    {
        fs::path src = pathOf(path), ref = src;
        std::error_code ec;
        if (opts.copyTextures && !outDir.empty())
        {
            fs::path dst = outDir / src.filename();
            if (copied.insert(textOf(dst)).second && !fs::equivalent(src, dst, ec))
            {
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    warnings.push_back(std::format("couldn't copy texture {}: {}", path, ec.message()));
            }
            ref = dst.filename();
        }
        else if (!outDir.empty())
        {
            fs::path rel = fs::relative(src, outDir, ec);
            if (!ec && !rel.empty())
                ref = rel;
        }
        const std::u8string generic = ref.generic_u8string();
        std::string uri(generic.begin(), generic.end());
        if (ref.is_absolute())
            uri = "file:///" + uri;
        w.open("image", {{"id", id}, {"name", id}});
        w.leaf("init_from", encodeUri(uri));
        w.close();
    }
    w.close();
}

void Exporter::effectsAndMaterials()
{
    std::set<std::string> slots;
    for (const ExportNode& n : scene.nodes)
        if (n.mesh)
            for (const auto& [slot, faces] : scene::materials::slotUsage(*n.mesh))
                if (faces)
                    slots.insert(slot);
    if (slots.empty())
        return;
    std::map<std::string, const gltf::MaterialSnapshot*> found;
    for (const gltf::MaterialSnapshot& m : snap.materials)
        found[m.name] = &m;

    std::map<std::string, std::string> effectOf;
    w.open("library_effects");
    for (const std::string& slot : slots)
    {
        const gltf::MaterialSnapshot* m = found.count(slot) ? found[slot] : nullptr;
        const std::string effect = ids.make(slot + "-fx");
        effectOf[slot] = effect;
        w.open("effect", {{"id", effect}});
        w.open("profile_COMMON");
        auto sampled = [&](const std::string& path, const char* tag) -> std::string {
            const std::string image = opts.textures && m ? imageFor(path) : std::string();
            if (image.empty())
                return {};
            const std::string surface = std::format("{}-{}-surface", image, tag), sampler = std::format("{}-{}-sampler", image, tag);
            w.open("newparam", {{"sid", surface}});
            w.open("surface", {{"type", "2D"}});
            w.leaf("init_from", image);
            w.close();
            w.close();
            w.open("newparam", {{"sid", sampler}});
            w.open("sampler2D");
            w.leaf("source", surface);
            w.close();
            w.close();
            return sampler;
        };
        const std::string diffuseSampler = m ? sampled(m->baseColorMap, "diffuse") : std::string();
        const std::string emissionSampler = m ? sampled(m->emissiveMap, "emission") : std::string();
        const std::string bumpSampler = m ? sampled(m->normalMap, "bump") : std::string();
        const int texSet = m ? m->uvSet : 0;
        const std::string texcoord = std::format("TEX{}", texSet);

        w.open("technique", {{"sid", "common"}});
        w.open("lambert");
        auto color = [&](const char* tag, const glm::dvec3& c, const std::string& sampler) {
            w.open(tag);
            if (!sampler.empty())
                w.empty("texture", {{"texture", sampler}, {"texcoord", texcoord}});
            else
                w.leaf("color", std::format("{} {} {} 1", num(c.x), num(c.y), num(c.z)), {{"sid", tag}});
            w.close();
        };
        const glm::dvec3 emissive = m ? m->emissiveColor * m->emissiveIntensity : glm::dvec3(0.0);
        color("emission", emissive, emissionSampler);
        color("diffuse", m ? m->baseColor : glm::dvec3(0.5), diffuseSampler);
        if (m && m->opacity < 1.0)
        {
            w.open("transparent", {{"opaque", "A_ONE"}});
            w.leaf("color", std::format("1 1 1 {}", num(m->opacity)));
            w.close();
            w.open("transparency");
            w.leaf("float", "1");
            w.close();
        }
        w.close(); // lambert
        if (!bumpSampler.empty() || (m && m->twoSided))
        {
            w.open("extra");
            w.open("technique", {{"profile", "OpenCOLLADAMaya"}});
            if (!bumpSampler.empty())
            {
                w.open("bump");
                w.empty("texture", {{"texture", bumpSampler}, {"texcoord", texcoord}});
                w.close();
            }
            if (m && m->twoSided)
                w.leaf("double_sided", "1");
            w.close();
            w.close();
        }
        w.close(); // technique
        w.close(); // profile_COMMON
        w.close(); // effect
    }
    w.close();

    w.open("library_materials");
    for (const std::string& slot : slots)
    {
        const std::string id = ids.make(slot + "-material");
        materialId[slot] = id;
        w.open("material", {{"id", id}, {"name", slot}});
        w.empty("instance_effect", {{"url", "#" + effectOf[slot]}});
        w.close();
    }
    w.close();
}

void Exporter::writeMesh(size_t i)
{
    const ExportNode& e = scene.nodes[i];
    const geo::Mesh& m = *e.mesh;
    const glm::dmat4 toFile = K * e.bake;
    const glm::dmat3 nm = glm::inverse(glm::transpose(glm::dmat3(toFile)));
    const bool mirrored = glm::determinant(glm::dmat3(toFile)) < 0.0;
    MeshOut& out = meshes[i];
    out.geometryId = ids.make(e.name + "-mesh");
    const std::string& gid = out.geometryId;

    std::vector<int32_t> pointIndex(m.pointCapacity(), -1);
    std::vector<double> pos;
    int32_t points = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            pointIndex[p] = points++;
            const glm::dvec3 v = glm::dvec3(toFile * glm::dvec4(m.position(p), 1.0));
            pos.insert(pos.end(), {v.x, v.y, v.z});
        }

    std::vector<glm::vec3> computed;
    std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (opts.gather.normals && normals.size() != m.cornerCapacity())
    {
        computed = geo::cornerNormals(m);
        normals = computed;
    }
    const bool writeNormals = opts.gather.normals && normals.size() == m.cornerCapacity();
    std::vector<std::span<const glm::vec2>> uvs;
    for (int s = 0; s < 8; ++s)
    {
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(s));
        if (uv.size() != m.cornerCapacity())
            break;
        uvs.push_back(uv);
    }
    out.hasUv = !uvs.empty();
    std::vector<std::span<const glm::vec4>> colors;
    if (opts.vertexColors)
        for (const std::string& set : geo::colors::sets(m))
            if (std::span<const glm::vec4> c = geo::colors::values(m, set); c.size() == m.cornerCapacity())
                colors.push_back(c);

    // Each corner attribute is its own indexed source, shared values written once.
    struct Indexed
    {
        std::vector<double> values;
        std::unordered_map<std::string, int32_t> of;
        int comps = 0;
        int32_t add(const double* v)
        {
            std::string key(reinterpret_cast<const char*>(v), sizeof(double) * size_t(comps));
            auto [it, added] = of.emplace(key, int32_t(values.size() / size_t(comps)));
            if (added)
                values.insert(values.end(), v, v + comps);
            return it->second;
        }
    };
    Indexed nrmIdx{{}, {}, 3};
    std::vector<Indexed> uvIdx(uvs.size(), Indexed{{}, {}, 2});
    std::vector<Indexed> colIdx(colors.size(), Indexed{{}, {}, 4});
    std::vector<int32_t> cornerNormal(m.cornerCapacity(), 0);
    std::vector<std::vector<int32_t>> cornerUv(uvs.size(), std::vector<int32_t>(m.cornerCapacity(), 0));
    std::vector<std::vector<int32_t>> cornerCol(colors.size(), std::vector<int32_t>(m.cornerCapacity(), 0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        for (Index c : m.faceCorners(f))
        {
            if (writeNormals)
            {
                glm::dvec3 n = nm * glm::dvec3(normals[c]);
                const double len = glm::length(n);
                n = len > 0.0 ? n / len : glm::dvec3(0, 1, 0);
                const double v[3] = {double(float(n.x)), double(float(n.y)), double(float(n.z))};
                cornerNormal[c] = nrmIdx.add(v);
            }
            for (size_t s = 0; s < uvs.size(); ++s)
            {
                const double v[2] = {double(uvs[s][c].x), double(uvs[s][c].y)};
                cornerUv[s][c] = uvIdx[s].add(v);
            }
            for (size_t s = 0; s < colors.size(); ++s)
            {
                const glm::vec4 col = colors[s][c];
                const double v[4] = {col.r, col.g, col.b, col.a};
                cornerCol[s][c] = colIdx[s].add(v);
            }
        }
    }

    // Faces grouped by material slot.
    std::span<const int32_t> faceMat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::map<int32_t, std::vector<Index>> bySlot;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            bySlot[faceMat.size() == m.faceCapacity() ? faceMat[f] : 0].push_back(f);
    geo::Triangulation tris;
    if (opts.triangles)
        tris = geo::triangulate(m);

    w.open("geometry", {{"id", gid}, {"name", e.name}});
    w.open("mesh");
    auto source = [&](const std::string& id, const std::vector<double>& values, std::initializer_list<const char*> params) {
        const size_t stride = params.size();
        w.open("source", {{"id", id}});
        w.leaf("float_array", joined(values), {{"id", id + "-array"}, {"count", std::to_string(values.size())}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-array"}, {"count", std::to_string(values.size() / stride)}, {"stride", std::to_string(stride)}});
        for (const char* p : params)
            w.empty("param", {{"name", p}, {"type", "float"}});
        w.close();
        w.close();
        w.close();
    };
    source(gid + "-positions", pos, {"X", "Y", "Z"});
    if (writeNormals)
        source(gid + "-normals", nrmIdx.values, {"X", "Y", "Z"});
    for (size_t s = 0; s < uvs.size(); ++s)
        source(std::format("{}-map{}", gid, s), uvIdx[s].values, {"S", "T"});
    for (size_t s = 0; s < colors.size(); ++s)
        source(std::format("{}-colors{}", gid, s), colIdx[s].values, {"R", "G", "B", "A"});
    w.open("vertices", {{"id", gid + "-vertices"}});
    w.empty("input", {{"semantic", "POSITION"}, {"source", "#" + gid + "-positions"}});
    w.close();

    for (const auto& [slotIndex, faces] : bySlot)
    {
        std::string symbol;
        if (!m.materialNames().empty())
            symbol = scene::materials::slotName(m, slotIndex);
        out.slots.push_back(symbol);
        std::string p, vcount;
        size_t count = 0;
        auto corner = [&](Index c) {
            if (!p.empty())
                p += ' ';
            p += std::to_string(pointIndex[m.cornerPoint(c)]);
            if (writeNormals)
                p += ' ' + std::to_string(cornerNormal[c]);
            for (size_t s = 0; s < uvs.size(); ++s)
                p += ' ' + std::to_string(cornerUv[s][c]);
            for (size_t s = 0; s < colors.size(); ++s)
                p += ' ' + std::to_string(cornerCol[s][c]);
        };
        for (Index f : faces)
        {
            if (opts.triangles)
            {
                for (Index t = tris.faceFirst[f]; t < tris.triangleCount() && tris.faces[t] == f; ++t, ++count)
                    for (int k = 0; k < 3; ++k)
                        corner(tris.corners[size_t(t) * 3 + size_t(mirrored ? 2 - k : k)]);
                continue;
            }
            std::vector<Index> cs = m.faceCorners(f);
            if (mirrored)
                std::reverse(cs.begin(), cs.end());
            for (Index c : cs)
                corner(c);
            if (!vcount.empty())
                vcount += ' ';
            vcount += std::to_string(cs.size());
            ++count;
        }
        const char* tag = opts.triangles ? "triangles" : "polylist";
        if (symbol.empty())
            w.open(tag, {{"count", std::to_string(count)}});
        else
            w.open(tag, {{"material", idFrom(symbol)}, {"count", std::to_string(count)}});
        int offset = 0;
        w.empty("input", {{"semantic", "VERTEX"}, {"source", "#" + gid + "-vertices"}, {"offset", std::to_string(offset++)}});
        if (writeNormals)
            w.empty("input", {{"semantic", "NORMAL"}, {"source", "#" + gid + "-normals"}, {"offset", std::to_string(offset++)}});
        for (size_t s = 0; s < uvs.size(); ++s)
            w.empty("input", {{"semantic", "TEXCOORD"}, {"source", std::format("#{}-map{}", gid, s)}, {"offset", std::to_string(offset++)},
                              {"set", std::to_string(s)}});
        for (size_t s = 0; s < colors.size(); ++s)
            w.empty("input", {{"semantic", "COLOR"}, {"source", std::format("#{}-colors{}", gid, s)}, {"offset", std::to_string(offset++)},
                              {"set", std::to_string(s)}});
        if (!opts.triangles)
            w.leaf("vcount", vcount);
        w.leaf("p", p);
        w.close();
    }
    w.close(); // mesh
    w.close(); // geometry
}

void Exporter::geometries()
{
    bool any = false;
    for (const ExportNode& n : scene.nodes)
        any = any || (n.mesh && n.mesh->numFaces() > 0);
    if (!any)
        return;
    w.open("library_geometries");
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        if (scene.nodes[i].mesh && scene.nodes[i].mesh->numFaces() > 0)
            writeMesh(i);
    w.close();
}

void Exporter::controllers()
{
    bool any = false;
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        any = any || (!meshes[i].geometryId.empty() && scene.nodes[i].skin && !scene.nodes[i].skin->clusters.empty());
    if (!any)
        return;
    w.open("library_controllers");
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const ExportNode& n = scene.nodes[i];
        if (meshes[i].geometryId.empty() || !n.skin || n.skin->clusters.empty())
            continue;
        const fbx::ExportSkin& skin = *n.skin;
        const std::string id = ids.make(n.name + "-skin");
        controller[i] = id;
        size_t points = 0;
        for (Index p = 0; p < n.mesh->pointCapacity(); ++p)
            points += n.mesh->pointValid(p);

        std::vector<std::string> sids;
        std::string bindPoses;
        for (const fbx::ExportSkin::Cluster& cl : skin.clusters)
        {
            sids.push_back(nodeId[size_t(cl.joint)]);
            appendMatrix(bindPoses, glm::inverse(K * cl.bindWorld * Kinv));
        }
        std::vector<std::vector<std::pair<int, double>>> per(points);
        for (size_t c = 0; c < skin.clusters.size(); ++c)
        {
            const fbx::ExportSkin::Cluster& cl = skin.clusters[c];
            for (size_t k = 0; k < cl.vertices.size() && k < cl.weights.size(); ++k)
                if (cl.vertices[k] >= 0 && size_t(cl.vertices[k]) < points && cl.weights[k] > 0.0)
                    per[size_t(cl.vertices[k])].push_back({int(c), cl.weights[k]});
        }
        std::vector<double> weights;
        std::map<double, int> weightIndex;
        std::string vcount, v;
        for (auto& influences : per)
        {
            if (!vcount.empty())
                vcount += ' ';
            vcount += std::to_string(influences.size());
            for (const auto& [joint, weight] : influences)
            {
                auto [it, added] = weightIndex.emplace(weight, int(weights.size()));
                if (added)
                    weights.push_back(weight);
                if (!v.empty())
                    v += ' ';
                v += std::format("{} {}", joint, it->second);
            }
        }

        w.open("controller", {{"id", id}, {"name", n.name + "Skin"}});
        w.open("skin", {{"source", "#" + meshes[i].geometryId}});
        w.leaf("bind_shape_matrix", matrixText(K * skin.meshBindWorld * Kinv));
        w.open("source", {{"id", id + "-joints"}});
        std::string names;
        for (const std::string& s : sids)
            names += (names.empty() ? "" : " ") + s;
        w.leaf("Name_array", names, {{"id", id + "-joints-array"}, {"count", std::to_string(sids.size())}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-joints-array"}, {"count", std::to_string(sids.size())}, {"stride", "1"}});
        w.empty("param", {{"name", "JOINT"}, {"type", "name"}});
        w.close();
        w.close();
        w.close();
        w.open("source", {{"id", id + "-bind_poses"}});
        w.leaf("float_array", bindPoses, {{"id", id + "-bind_poses-array"}, {"count", std::to_string(sids.size() * 16)}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-bind_poses-array"}, {"count", std::to_string(sids.size())}, {"stride", "16"}});
        w.empty("param", {{"name", "TRANSFORM"}, {"type", "float4x4"}});
        w.close();
        w.close();
        w.close();
        w.open("source", {{"id", id + "-weights"}});
        w.leaf("float_array", joined(weights), {{"id", id + "-weights-array"}, {"count", std::to_string(weights.size())}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-weights-array"}, {"count", std::to_string(weights.size())}, {"stride", "1"}});
        w.empty("param", {{"name", "WEIGHT"}, {"type", "float"}});
        w.close();
        w.close();
        w.close();
        w.open("joints");
        w.empty("input", {{"semantic", "JOINT"}, {"source", "#" + id + "-joints"}});
        w.empty("input", {{"semantic", "INV_BIND_MATRIX"}, {"source", "#" + id + "-bind_poses"}});
        w.close();
        w.open("vertex_weights", {{"count", std::to_string(points)}});
        w.empty("input", {{"semantic", "JOINT"}, {"source", "#" + id + "-joints"}, {"offset", "0"}});
        w.empty("input", {{"semantic", "WEIGHT"}, {"source", "#" + id + "-weights"}, {"offset", "1"}});
        w.leaf("vcount", vcount);
        w.leaf("v", v);
        w.close();
        w.close(); // skin
        w.close(); // controller
    }
    w.close();
}

void Exporter::lightsAndCameras()
{
    bool anyLight = false, anyCamera = false;
    for (const ExportNode& n : scene.nodes)
    {
        anyLight = anyLight || (opts.lights && n.light && (n.light->kind == lights::Kind::Point || n.light->kind == lights::Kind::Spot ||
                                                           n.light->kind == lights::Kind::Directional));
        anyCamera = anyCamera || n.camera;
    }
    if (anyLight)
    {
        w.open("library_lights");
        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            const ExportNode& n = scene.nodes[i];
            if (!opts.lights || !n.light)
                continue;
            const lights::Settings& l = *n.light;
            const char* tag = l.kind == lights::Kind::Point ? "point" : l.kind == lights::Kind::Spot ? "spot" : l.kind == lights::Kind::Directional ? "directional" : nullptr;
            if (!tag)
            {
                warnings.push_back(std::format("{}: area, disc and dome lights have no COLLADA form; left out", n.name));
                continue;
            }
            lightId[i] = ids.make(n.name + "-light");
            const glm::dvec3 c = lights::effectiveColor(l) * l.intensity;
            w.open("light", {{"id", lightId[i]}, {"name", n.name}});
            w.open("technique_common");
            w.open(tag);
            w.leaf("color", std::format("{} {} {}", num(c.x), num(c.y), num(c.z)));
            if (l.kind != lights::Kind::Directional)
            {
                w.leaf("constant_attenuation", "1");
                w.leaf("linear_attenuation", "0");
                w.leaf("quadratic_attenuation", "0");
            }
            if (l.kind == lights::Kind::Spot)
            {
                w.leaf("falloff_angle", num(2.0 * l.outerConeAngle));
                w.leaf("falloff_exponent", "0");
            }
            w.close();
            w.close();
            w.close();
        }
        w.close();
    }
    if (anyCamera)
    {
        w.open("library_cameras");
        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            const ExportNode& n = scene.nodes[i];
            if (!n.camera)
                continue;
            const fbx::ExportCamera& c = *n.camera;
            cameraId[i] = ids.make(n.name + "-camera");
            const double toUnits = 1.0 / opts.gather.unitScale;
            w.open("camera", {{"id", cameraId[i]}, {"name", n.name}});
            w.open("optics");
            w.open("technique_common");
            if (c.ortho)
            {
                w.open("orthographic");
                w.leaf("xmag", num(c.orthoWidth * 0.5 * toUnits));
                w.leaf("aspect_ratio", num(c.filmWidth / std::max(c.filmHeight, 1e-9)));
            }
            else
            {
                w.open("perspective");
                const double xfov = glm::degrees(2.0 * std::atan(c.filmWidth * 25.4 * 0.5 / std::max(c.focalLength, 1e-6)));
                w.leaf("xfov", num(xfov));
                w.leaf("aspect_ratio", num(c.filmWidth / std::max(c.filmHeight, 1e-9)));
            }
            w.leaf("znear", num(c.nearClip * toUnits));
            w.leaf("zfar", num(c.farClip * toUnits));
            w.close();
            w.close();
            w.close();
            w.close();
        }
        w.close();
    }
}

void Exporter::animations()
{
    std::vector<const ExportTake*> takes;
    if (scene.take)
        takes.push_back(&*scene.take);
    for (const ExportTake& t : scene.extraTakes)
        takes.push_back(&t);
    bool any = false;
    for (const ExportTake* t : takes)
        for (const auto& l : t->locals)
            any = any || !l.empty();
    if (!any)
        return;

    // Takes follow each other on one timeline; each is a clip over its own stretch.
    struct Clip
    {
        std::string name;
        double start, end;
        std::vector<std::string> animations;
    };
    std::vector<Clip> clips;
    double offset = 0.0;
    std::vector<std::vector<double>> times(scene.nodes.size());
    std::vector<std::vector<glm::dmat4>> values(scene.nodes.size());
    for (const ExportTake* t : takes)
    {
        const double shift = clips.empty() ? 0.0 : offset - t->start;
        Clip clip{t->name.empty() ? "Take 001" : t->name, t->start + shift, t->end + shift, {}};
        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            const bool sampled = i < t->locals.size() && !t->locals[i].empty();
            // A joint the take leaves still holds its pose for the clip, so the target rig gets a value for it.
            if (!sampled && !scene.nodes[i].joint)
                continue;
            for (size_t k = 0; k < t->times.size(); ++k)
            {
                times[i].push_back(t->times[k] + shift);
                values[i].push_back(fileLocal(i, sampled && k < t->locals[i].size() ? t->locals[i][k] : scene.nodes[i].local));
            }
        }
        offset = clip.end + 1.0 / std::max(t->fps, 1.0);
        clips.push_back(std::move(clip));
    }

    struct Track
    {
        std::string id, target, param, type;
        size_t stride;
        std::vector<double> values;
    };
    w.open("library_animations");
    std::vector<std::string> animationIds;
    auto writeTrack = [&](const std::vector<double>& t, const Track& tr) {
        const std::string& id = tr.id;
        const std::string n = std::to_string(t.size());
        std::string interp;
        for (size_t k = 0; k < t.size(); ++k)
            interp += k ? " LINEAR" : "LINEAR";
        w.open("animation", {{"id", id}});
        w.open("source", {{"id", id + "-input"}});
        w.leaf("float_array", joined(t), {{"id", id + "-input-array"}, {"count", n}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-input-array"}, {"count", n}, {"stride", "1"}});
        w.empty("param", {{"name", "TIME"}, {"type", "float"}});
        w.close();
        w.close();
        w.close();
        w.open("source", {{"id", id + "-output"}});
        w.leaf("float_array", joined(tr.values), {{"id", id + "-output-array"}, {"count", std::to_string(tr.values.size())}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-output-array"}, {"count", n}, {"stride", std::to_string(tr.stride)}});
        if (tr.stride == 3)
            for (const char* axis : {"X", "Y", "Z"})
                w.empty("param", {{"name", axis}, {"type", "float"}});
        else
            w.empty("param", {{"name", tr.param}, {"type", tr.type}});
        w.close();
        w.close();
        w.close();
        w.open("source", {{"id", id + "-interpolations"}});
        w.leaf("Name_array", interp, {{"id", id + "-interpolations-array"}, {"count", n}});
        w.open("technique_common");
        w.open("accessor", {{"source", "#" + id + "-interpolations-array"}, {"count", n}, {"stride", "1"}});
        w.empty("param", {{"name", "INTERPOLATION"}, {"type", "Name"}});
        w.close();
        w.close();
        w.close();
        w.open("sampler", {{"id", id + "-sampler"}});
        w.empty("input", {{"semantic", "INPUT"}, {"source", "#" + id + "-input"}});
        w.empty("input", {{"semantic", "OUTPUT"}, {"source", "#" + id + "-output"}});
        w.empty("input", {{"semantic", "INTERPOLATION"}, {"source", "#" + id + "-interpolations"}});
        w.close();
        w.empty("channel", {{"source", "#" + id + "-sampler"}, {"target", tr.target}});
        w.close();
        animationIds.push_back(id);
    };
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        if (times[i].empty() || nodeId[i].empty())
            continue;
        if (opts.bakeTransforms)
        {
            Track tr{ids.make(nodeId[i] + ".transform"), nodeId[i] + "/transform", "TRANSFORM", "float4x4", 16, {}};
            for (const glm::dmat4& m : values[i])
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        tr.values.push_back(m[c][r]);
            writeTrack(times[i], tr);
            continue;
        }
        // Maya's layout: translate, each rotate angle and scale as their own curves. Every channel goes out, constant
        // ones too, as a keyed Maya rig writes them, so the target rig gets a value for each.
        Track t{ids.make(nodeId[i] + ".translate"), nodeId[i] + "/translate", "", "float", 3, {}};
        Track s{ids.make(nodeId[i] + ".scale"), nodeId[i] + "/scale", "", "float", 3, {}};
        std::array<Track, 3> r;
        for (int a = 0; a < 3; ++a)
        {
            const std::string axis(1, "XYZ"[a]);
            r[size_t(a)] = Track{"", nodeId[i] + "/rotate" + axis + ".ANGLE", "ANGLE", "float", 1, {}};
        }
        glm::dvec3 ref = elements[i].r;
        for (const glm::dmat4& m : values[i])
        {
            const Elements e = decomposeElements(i, m, ref);
            ref = e.r;
            for (int a = 0; a < 3; ++a)
            {
                t.values.push_back(e.t[a]);
                s.values.push_back(e.s[a]);
                r[size_t(a)].values.push_back(e.r[a]);
            }
        }
        writeTrack(times[i], t);
        for (int a : {0, 1, 2})
        {
            r[size_t(a)].id = ids.make(nodeId[i] + ".rotate" + std::string(1, "XYZ"[a]));
            writeTrack(times[i], r[size_t(a)]);
        }
        writeTrack(times[i], s);
    }
    w.close();
    timelineStart = clips.front().start;
    timelineEnd = clips.back().end;

    // One take is the whole timeline, as Maya writes it; several are clips over their stretches.
    if (clips.size() < 2)
        return;
    w.open("library_animation_clips");
    for (const Clip& c : clips)
    {
        w.open("animation_clip", {{"id", ids.make(c.name)}, {"name", c.name}, {"start", num(c.start)}, {"end", num(c.end)}});
        for (const std::string& id : animationIds)
            w.empty("instance_animation", {{"url", "#" + id}});
        w.close();
    }
    w.close();
}

void Exporter::node(size_t i, const std::vector<std::vector<size_t>>& children)
{
    const ExportNode& n = scene.nodes[i];
    if (n.joint)
        w.open("node", {{"id", nodeId[i]}, {"name", n.name}, {"sid", nodeId[i]}, {"type", "JOINT"}});
    else
        w.open("node", {{"id", nodeId[i]}, {"name", n.name}, {"type", "NODE"}});
    if (opts.bakeTransforms)
        w.leaf("matrix", matrixText(fileLocal(i, n.local)), {{"sid", "transform"}});
    else
    {
        const Elements& e = elements[i];
        w.leaf("translate", vec3Text(e.t), {{"sid", "translate"}});
        if (e.joint)
            for (int a : {2, 1, 0})
                w.leaf("rotate", rotateText(a, e.jo[a]), {{"sid", std::string("jointOrient") + "XYZ"[a]}});
        for (int a : rotateListing(e.order))
            w.leaf("rotate", rotateText(a, e.r[a]), {{"sid", std::string("rotate") + "XYZ"[a]}});
        w.leaf("scale", vec3Text(e.s), {{"sid", "scale"}});
    }
    auto bindMaterials = [&]() {
        const MeshOut& mo = meshes[i];
        bool any = false;
        for (const std::string& s : mo.slots)
            any = any || (!s.empty() && materialId.count(s));
        if (!any)
            return;
        w.open("bind_material");
        w.open("technique_common");
        for (const std::string& s : mo.slots)
        {
            if (s.empty() || !materialId.count(s))
                continue;
            w.open("instance_material", {{"symbol", idFrom(s)}, {"target", "#" + materialId[s]}});
            if (mo.hasUv)
                w.empty("bind_vertex_input", {{"semantic", "TEX0"}, {"input_semantic", "TEXCOORD"}, {"input_set", "0"}});
            w.close();
        }
        w.close();
        w.close();
    };
    if (!controller[i].empty())
    {
        w.open("instance_controller", {{"url", "#" + controller[i]}});
        std::set<std::string> roots;
        for (const fbx::ExportSkin::Cluster& cl : n.skin->clusters)
        {
            int top = cl.joint;
            while (scene.nodes[size_t(top)].parent >= 0 && scene.nodes[size_t(scene.nodes[size_t(top)].parent)].joint)
                top = scene.nodes[size_t(top)].parent;
            roots.insert(nodeId[size_t(top)]);
        }
        for (const std::string& r : roots)
            w.leaf("skeleton", "#" + r);
        bindMaterials();
        w.close();
    }
    else if (!meshes[i].geometryId.empty())
    {
        w.open("instance_geometry", {{"url", "#" + meshes[i].geometryId}, {"name", n.name}});
        bindMaterials();
        w.close();
    }
    if (!lightId[i].empty())
        w.empty("instance_light", {{"url", "#" + lightId[i]}});
    if (!cameraId[i].empty())
        w.empty("instance_camera", {{"url", "#" + cameraId[i]}});
    for (size_t c : children[i])
        node(c, children);
    w.close();
}

void Exporter::visualScene()
{
    std::vector<std::vector<size_t>> children(scene.nodes.size());
    std::vector<size_t> roots;
    for (size_t i = 0; i < scene.nodes.size(); ++i)
    {
        const int p = scene.nodes[i].parent;
        if (p >= 0)
            children[size_t(p)].push_back(i);
        else
            roots.push_back(i);
    }
    w.open("library_visual_scenes");
    w.open("visual_scene", {{"id", "VisualSceneNode"}, {"name", "VisualSceneNode"}});
    for (size_t r : roots)
        node(r, children);
    if (timelineEnd >= timelineStart)
    {
        w.open("extra");
        w.open("technique", {{"profile", "FCOLLADA"}});
        w.leaf("start_time", num(timelineStart));
        w.leaf("end_time", num(timelineEnd));
        w.close();
        w.close();
    }
    w.close();
    w.close();
    w.open("scene");
    w.empty("instance_visual_scene", {{"url", "#VisualSceneNode"}});
    w.close();
}
} // namespace

bool writeCollada(const gltf::ExportSnapshot& snap, const fs::path& path, const ExportOptions& opts, std::string* error, std::vector<std::string>* warningsOut)
{
    LILA_ZONE();

    std::vector<std::string> localWarnings;
    Exporter e{snap, snap.scene, opts, path.parent_path(), warningsOut ? *warningsOut : localWarnings};
    e.B = fbx::profileBasis(fbx::AxisProfile::Ue6, snap.scene.upAxis);
    e.K = glm::scale(glm::dmat4(1.0), glm::dvec3(1.0 / opts.gather.unitScale)) * glm::dmat4(e.B);
    e.Kinv = glm::inverse(e.K);
    const size_t n = snap.scene.nodes.size();
    e.nodeId.resize(n);
    e.meshes.resize(n);
    e.controller.resize(n);
    e.lightId.resize(n);
    e.cameraId.resize(n);
    e.ids.used.insert("VisualSceneNode");
    for (size_t i = 0; i < n; ++i)
        e.nodeId[i] = e.ids.make(snap.scene.nodes[i].name);
    e.elements.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const fbx::ExportNode& node = snap.scene.nodes[i];
        Elements& el = e.elements[i];
        const bool turned = node.camera || node.light;
        el.joint = node.joint && !turned;
        el.order = node.rotateOrder;
        if (el.joint)
        {
            el.pre = e.B * glm::dmat3(node.pre) * glm::transpose(e.B);
            el.jo = scene::matrixToEuler(el.pre, scene::RotateOrder::XYZ);
        }
        const glm::dmat4 local = e.fileLocal(i, node.local);
        const fbx::Trs trs = fbx::decomposeWith(local, el.pre, el.order, node.plainTrs ? node.rotate : glm::dvec3(0.0));
        if (trs.sheared && !opts.bakeTransforms)
            e.warnings.push_back(std::format("{}: shear can't be written as Maya's transform elements; export with bakeTransforms to keep it",
                                             node.name));
        el.t = trs.t;
        el.r = trs.r;
        el.s = trs.s;
    }

    e.w.declaration();
    e.w.open("COLLADA", {{"xmlns", "http://www.collada.org/2005/11/COLLADASchema"}, {"version", "1.4.1"}});
    e.asset();
    e.images();
    e.effectsAndMaterials();
    e.geometries();
    e.controllers();
    e.lightsAndCameras();
    e.animations();
    e.visualScene();
    e.w.close();
    std::error_code ec;
    if (!e.outDir.empty())
        fs::create_directories(e.outDir, ec);
    if (!writeText(path, e.w.str()))
    {
        if (error)
            *error = "can't write " + textOf(path);
        return false;
    }
    return true;
}
} // namespace lila::collada
