#include "fbx/Fbx.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/FbxAxes.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Morphs.h"
#include "scene/Rig.h"
#include "scene/RotationCurve.h"
#include "scene/Skin.h"

#include <ufbx.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace lila::fbx
{
namespace
{
using geo::Index;
using geo::kInvalid;

glm::dvec3 axisVector(ufbx_coordinate_axis a)
{
    glm::dvec3 v(0.0);
    if (a >= UFBX_COORDINATE_AXIS_POSITIVE_X && a <= UFBX_COORDINATE_AXIS_NEGATIVE_Z)
        v[int(a) / 2] = (int(a) % 2) ? -1.0 : 1.0;
    return v;
}

const char* axisLabel(ufbx_coordinate_axis a)
{
    static const char* kNames[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    return a >= UFBX_COORDINATE_AXIS_POSITIVE_X && a <= UFBX_COORDINATE_AXIS_NEGATIVE_Z ? kNames[int(a)] : "?";
}

/** The declared axes turned onto the canonical declaration for their up axis. */
glm::dmat3 fileToCanonical(const ufbx_coordinate_axes& axes, bool* zUp)
{
    glm::dvec3 r = axisVector(axes.right), u = axisVector(axes.up), f = axisVector(axes.front);
    if (glm::dot(r, r) == 0.0 || glm::dot(u, u) == 0.0 || glm::dot(f, f) == 0.0 || glm::dot(r, u) != 0.0 || glm::dot(u, f) != 0.0)
    {
        r = {1, 0, 0};
        u = {0, 1, 0};
        f = {0, 0, 1};
    }
    *zUp = u.z != 0.0;
    const glm::dmat3 decl(r, u, f);
    const glm::dmat3 canon = *zUp ? glm::dmat3({1, 0, 0}, {0, 0, 1}, {0, -1, 0}) : glm::dmat3({1, 0, 0}, {0, 1, 0}, {0, 0, 1});
    return canon * glm::transpose(decl);
}

/**
 * File space to Lila space. The declared axes are first turned onto the canonical declaration for their up axis
 * (Z up: right +X, front -Y; otherwise Y up: right +X, front +Z), which is then read as the ue5 or ue6 profile.
 */
glm::dmat3 fileToLila(const ufbx_coordinate_axes& axes, math::UpAxis lilaUp)
{
    bool zUp = false;
    const glm::dmat3 toCanon = fileToCanonical(axes, &zUp);
    const glm::dmat3 canonToLila = glm::transpose(profileBasis(zUp ? AxisProfile::Ue5 : AxisProfile::Ue6, lilaUp));
    return canonToLila * toCanon;
}

glm::dmat4 toGlm(const ufbx_matrix& m)
{
    glm::dmat4 out(1.0);
    for (int c = 0; c < 4; ++c)
        out[c] = glm::dvec4(m.cols[c].x, m.cols[c].y, m.cols[c].z, c == 3 ? 1.0 : 0.0);
    return out;
}

std::string toString(const ufbx_string& s) { return std::string(s.data, s.length); }

/** A valid Lila node name from an FBX one; collision and socket prefixes survive as they are. */
std::string safeName(std::string_view raw, const char* fallback)
{
    std::string s;
    for (char c : raw)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        s.push_back(ok ? c : '_');
    }
    if (s.empty())
        return fallback;
    if (s[0] >= '0' && s[0] <= '9')
        s.insert(s.begin(), '_');
    return s;
}

/** Hard where the corner normals of an edge's two faces disagree. Smoothing groups land here, since ufbx keeps only a flag per face. */
void hardEdgesFromNormals(geo::Mesh& m)
{
    std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e))
            continue;
        Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
        auto at = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
        for (int i = 0; i < 2 && !hard[e]; ++i)
        {
            Index p = m.edgePoint(e, i);
            hard[e] = glm::dot(normals[at(c0, p)], normals[at(c1, p)]) < 0.9999f;
        }
    }
}

struct MeshBuild
{
    geo::Mesh mesh;
    size_t badFaces = 0;
    /** FBX control point to Lila point, kInvalid for a point dropped as loose. */
    std::vector<Index> pointOf;
};

/** Slot name per FBX material element id: the name of the material node the slot binds to. */
using SlotNames = std::unordered_map<uint32_t, std::string>;

MeshBuild convertMesh(const ufbx_mesh& src, const ufbx_node& node, const glm::dmat4& toLila, const SlotNames& slotNames)
{
    LILA_ZONE();

    MeshBuild b;
    geo::Mesh& m = b.mesh;
    const glm::dmat3 lin(toLila);
    const glm::dmat3 normalMat = glm::inverse(glm::transpose(lin));
    const bool mirrored = glm::determinant(lin) < 0.0;

    m.reserve(src.num_vertices, src.num_edges, src.num_faces, src.num_indices);
    for (size_t v = 0; v < src.vertices.count; ++v)
    {
        const ufbx_vec3& p = src.vertices.data[v];
        m.addPoint(glm::dvec3(toLila * glm::dvec4(p.x, p.y, p.z, 1.0)));
    }

    std::vector<Index> cornerOf(src.num_indices, kInvalid);
    std::vector<uint32_t> sourceFace;
    std::vector<Index> pts;
    std::vector<uint32_t> idx;
    for (size_t f = 0; f < src.faces.count; ++f)
    {
        const ufbx_face face = src.faces.data[f];
        if (face.num_indices < 3)
            continue;
        pts.clear();
        idx.clear();
        for (uint32_t k = 0; k < face.num_indices; ++k)
        {
            uint32_t i = face.index_begin + (mirrored ? face.num_indices - 1 - k : k);
            idx.push_back(i);
            pts.push_back(src.vertex_indices.data[i]);
        }
        Index lf = m.addFace(pts);
        if (lf == kInvalid)
        {
            ++b.badFaces;
            continue;
        }
        const Index first = m.faceCorner(lf);
        for (size_t k = 0; k < idx.size(); ++k)
            cornerOf[idx[k]] = first + Index(k);
        sourceFace.push_back(uint32_t(f));
    }

    auto eachCorner = [&](auto&& fn) {
        for (size_t i = 0; i < cornerOf.size(); ++i)
            if (cornerOf[i] != kInvalid)
                fn(i, cornerOf[i]);
    };

    if (src.vertex_normal.exists)
    {
        std::span<glm::vec3> out = m.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        eachCorner([&](size_t i, Index c) {
            ufbx_vec3 n = ufbx_get_vertex_vec3(&src.vertex_normal, i);
            glm::dvec3 v = normalMat * glm::dvec3(n.x, n.y, n.z);
            double len = glm::length(v);
            out[c] = glm::vec3(len > 0.0 ? v / len : v);
        });
    }

    for (size_t s = 0; s < src.uv_sets.count; ++s)
    {
        const ufbx_vertex_vec2& uv = src.uv_sets.data[s].vertex_uv;
        if (!uv.exists)
            continue;
        std::span<glm::vec2> out = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(s)));
        eachCorner([&](size_t i, Index c) {
            ufbx_vec2 t = ufbx_get_vertex_vec2(&uv, i);
            out[c] = glm::vec2(float(t.x), float(t.y));
        });
    }

    for (size_t s = 0; s < src.color_sets.count; ++s)
    {
        const ufbx_vertex_vec4& col = src.color_sets.data[s].vertex_color;
        if (!col.exists)
            continue;
        std::string_view given(src.color_sets.data[s].name.data, src.color_sets.data[s].name.length);
        std::string name = geo::colors::uniqueName(m, given.empty() ? std::string_view("colorSet") : given);
        std::span<glm::vec4> out = geo::colors::add(m, name);
        eachCorner([&](size_t i, Index c) {
            ufbx_vec4 v = ufbx_get_vertex_vec4(&col, i);
            out[c] = glm::vec4(float(v.x), float(v.y), float(v.z), float(v.w));
        });
    }

    // Per-instance materials when the node has them, else the mesh's own.
    const ufbx_material_list& mats = node.materials.count ? node.materials : src.materials;
    if (mats.count)
    {
        for (size_t i = 0; i < mats.count; ++i)
        {
            auto it = mats.data[i] ? slotNames.find(mats.data[i]->element_id) : slotNames.end();
            m.materialNames().push_back(it != slotNames.end() ? it->second : "Material" + std::to_string(i));
        }
        std::span<int32_t> out = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        if (src.face_material.count == src.num_faces)
            for (size_t f = 0; f < sourceFace.size(); ++f)
                out[f] = int32_t(src.face_material.data[sourceFace[f]]);
    }

    auto edgeOf = [&](const ufbx_edge& e) {
        return m.findEdge(src.vertex_indices.data[e.a], src.vertex_indices.data[e.b]);
    };
    if (src.edge_smoothing.count == src.edges.count && src.edges.count)
    {
        std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        for (size_t e = 0; e < src.edges.count; ++e)
            if (Index le = edgeOf(src.edges.data[e]); le != kInvalid)
                hard[le] = !src.edge_smoothing.data[e];
    }
    else if (src.vertex_normal.exists)
    {
        hardEdgesFromNormals(m);
    }
    else if (src.face_smoothing.count == src.num_faces)
    {
        // Flat faces are hard all round.
        std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        for (size_t f = 0; f < sourceFace.size(); ++f)
            if (!src.face_smoothing.data[sourceFace[f]])
                m.forEachFaceCorner(Index(f), [&](Index c) { hard[m.cornerEdge(c)] = true; });
    }
    if (src.edge_crease.count == src.edges.count && src.edges.count)
    {
        std::span<float> crease = m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
        for (size_t e = 0; e < src.edges.count; ++e)
            if (Index le = edgeOf(src.edges.data[e]); le != kInvalid)
                crease[le] = float(src.edge_crease.data[e]);
    }

    m.removeAllLoose();
    b.pointOf.resize(src.vertices.count);
    for (size_t v = 0; v < src.vertices.count; ++v)
        b.pointOf[v] = Index(v);
    if (m.hasDeleted())
    {
        geo::MeshRemap remap;
        m.compact(&remap);
        for (Index& p : b.pointOf)
            p = p < remap.points.size() ? remap.points[p] : kInvalid;
    }
    return b;
}

struct Trs
{
    glm::dvec3 t{0.0}, r{0.0}, s{1.0};
    bool sheared = false;
};

Trs decompose(const glm::dmat4& m)
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, q, out.t, skew, persp))
        return out;
    out.sheared = glm::length(skew) > 1e-6;
    out.r = scene::matrixToEuler(glm::mat3_cast(q), scene::RotateOrder::XYZ);
    return out;
}

/** TRS of a Lila-space local matrix with the pre-rotation (jointOrient) taken out, Euler angles near ref. */
Trs decomposeWith(const glm::dmat4& m, const glm::dmat3& pre, scene::RotateOrder order, const glm::dvec3& ref)
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, q, out.t, skew, persp))
        return out;
    out.sheared = glm::length(skew) > 1e-6;
    out.r = eulerNear(glm::transpose(pre) * glm::dmat3(glm::mat3_cast(q)), order, ref);
    return out;
}

glm::dvec3 vec3Of(const ufbx_vec3& v) { return {v.x, v.y, v.z}; }

/** How one file node maps onto its Lila transform: jointOrient and rotate order through the axis conversion. */
struct NodeFrame
{
    scene::NodeId id = scene::kNoNode;
    bool joint = false;
    glm::dmat3 pre{1.0};
    scene::RotateOrder order = scene::RotateOrder::XYZ;
    Trs rest;
};

std::string attribName(ufbx_element_type t)
{
    switch (t)
    {
    case UFBX_ELEMENT_CAMERA: return "camera";
    case UFBX_ELEMENT_LIGHT: return "light";
    case UFBX_ELEMENT_BONE: return "joint";
    case UFBX_ELEMENT_LINE_CURVE: return "curve";
    case UFBX_ELEMENT_NURBS_CURVE: return "NURBS curve";
    case UFBX_ELEMENT_NURBS_SURFACE: return "NURBS surface";
    case UFBX_ELEMENT_LOD_GROUP: return "LOD group";
    case UFBX_ELEMENT_MARKER: return "marker";
    default: return "attribute";
    }
}

/** Path of a texture map, preferring one that exists on disk; empty without a texture. */
std::string texturePath(const ufbx_material_map& map)
{
    if (!map.texture)
        return {};
    const ufbx_texture& t = *map.texture;
    for (const ufbx_string* s : {&t.filename, &t.absolute_filename, &t.relative_filename})
    {
        std::string path = toString(*s);
        std::error_code ec;
        if (!path.empty() && fs::exists(fs::path(reinterpret_cast<const char8_t*>(path.c_str())), ec))
            return path;
    }
    std::string path = toString(t.filename);
    return path.empty() ? toString(t.absolute_filename) : path;
}

/**
 * A material node per FBX material, named like the slot it binds to. Materials the scene already has are reused
 * as they are (Unreal does the same with existing material assets); a name taken by another kind of node gets a
 * unique variant, and the slots follow it.
 */
SlotNames importMaterials(scene::Graph& graph, const ufbx_scene& fbx)
{
    using namespace scene;
    SlotNames out;
    const bool makeNodes = types::find("material") != nullptr;
    for (size_t i = 0; i < fbx.materials.count; ++i)
    {
        const ufbx_material& fm = *fbx.materials.data[i];
        std::string name = safeName(toString(fm.name), "Material");
        if (!makeNodes || materials::find(graph, name))
        {
            out[fm.element_id] = name;
            continue;
        }
        Node* n = materials::create(graph, name);
        out[fm.element_id] = n->name();
        auto set = [&](int attr, PlugValue v) { graph.setValue({n->id(), attr, -1}, std::move(v)); };
        const ufbx_material_pbr_maps& pbr = fm.pbr;
        if (pbr.base_color.has_value)
        {
            double f = pbr.base_factor.has_value ? pbr.base_factor.value_real : 1.0;
            set(material::kBaseColor, glm::dvec3(pbr.base_color.value_vec4.x, pbr.base_color.value_vec4.y, pbr.base_color.value_vec4.z) * f);
        }
        if (pbr.metalness.has_value)
            set(material::kMetallic, double(pbr.metalness.value_real));
        if (pbr.roughness.has_value)
            set(material::kRoughness, std::clamp(double(pbr.roughness.value_real), 0.0, 1.0));
        if (pbr.emission_color.has_value)
        {
            glm::dvec3 e(pbr.emission_color.value_vec4.x, pbr.emission_color.value_vec4.y, pbr.emission_color.value_vec4.z);
            double f = pbr.emission_factor.has_value ? pbr.emission_factor.value_real : 1.0;
            if (e * f != glm::dvec3(0.0))
                set(material::kEmissiveColor, e * f);
        }
        if (fm.features.double_sided.enabled)
            set(material::kTwoSided, true);
        const std::pair<const ufbx_material_map*, int> maps[] = {
            {&pbr.base_color, material::kBaseColorMap},       {&pbr.metalness, material::kMetallicMap},
            {&pbr.roughness, material::kRoughnessMap},        {&pbr.normal_map, material::kNormalMap},
            {&pbr.ambient_occlusion, material::kAmbientOcclusionMap}, {&pbr.emission_color, material::kEmissiveMap},
            {&pbr.opacity, material::kBaseColorMap},
        };
        for (const auto& [map, attr] : maps)
            if (std::string path = texturePath(*map); !path.empty() && std::get<std::string>(graph.storedValue({n->id(), attr, -1})).empty())
            {
                std::replace(path.begin(), path.end(), '\\', '/');
                set(attr, path);
            }
        // A texture in the opacity slot is the base color's alpha in Unreal terms.
        if (pbr.opacity.texture)
            set(material::kBlendMode, int64_t(material::BlendMode::Masked));
    }
    return out;
}

using WorldFn = std::function<glm::dmat4(const ufbx_matrix&)>;

/**
 * A skinCluster on the mesh from a ufbx skin: the mesh's geometry is its bind shape, the clusters' TransformLink
 * matrices are the joints' bind worlds and the first cluster places the mesh at bind time.
 */
scene::NodeId buildSkin(scene::Graph& g, const ufbx_skin_deformer& skin, scene::NodeId mesh, const std::vector<Index>& pointOf,
                        const std::unordered_map<uint32_t, NodeFrame>& frames, const WorldFn& worldToLila, ImportResult& r)
{
    LILA_ZONE();

    using namespace scene;
    const std::string meshName = g.node(mesh)->path();
    std::vector<std::pair<const ufbx_skin_cluster*, NodeId>> clusters;
    for (size_t i = 0; i < skin.clusters.count; ++i)
    {
        const ufbx_skin_cluster* c = skin.clusters.data[i];
        auto it = c->bone_node ? frames.find(c->bone_node->element_id) : frames.end();
        if (it == frames.end())
        {
            r.warnings.push_back(std::format("{}: cluster {} has no bone, skipped", meshName, toString(c->name)));
            continue;
        }
        clusters.push_back({c, it->second.id});
    }
    if (clusters.empty())
        return kNoNode;

    PlugRef top{mesh, MeshNode::kInMesh, -1};
    Node* sc = g.createNode(*types::find("skinCluster"), "SkinCluster", kNoNode, true);
    NodeId skinId = sc->id();
    Node* source = g.createNode(*types::find("meshSource"), "meshSource", kNoNode, true);
    g.setValue({source->id(), MeshSourceNode::kMesh, -1}, g.storedValue(top));
    g.setValue(top, MeshPtr());
    g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {skinId, SkinClusterNode::kInMesh, -1});
    g.connect({skinId, SkinClusterNode::kOutMesh, -1}, top);
    g.setValue({skinId, SkinClusterNode::kSkinningMethod},
               int64_t(skin.skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION ? 1 : 0));
    g.setValue({skinId, SkinClusterNode::kGeomMatrix},
               worldToLila(ufbx_matrix_mul(&clusters[0].first->bind_to_world, &clusters[0].first->mesh_node_to_bone)));

    MeshPtr bind = skin::inputMesh(g, skinId);
    WeightTable t;
    t.points.resize(bind ? bind->pointCapacity() : 0);
    for (const auto& [c, joint] : clusters)
    {
        uint32_t index = skin::addInfluence(g, skinId, joint);
        for (const skin::Influence& inf : skin::influences(g, skinId))
            if (inf.index == index)
                g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, glm::inverse(worldToLila(c->bind_to_world)));
        for (size_t k = 0; k < c->vertices.count; ++k)
        {
            uint32_t v = c->vertices.data[k];
            Index p = v < pointOf.size() ? pointOf[v] : kInvalid;
            if (p != kInvalid && p < t.points.size() && c->weights.data[k] > 0.0)
                t.points[p].push_back({index, float(c->weights.data[k])});
        }
    }
    size_t maxUsed = 0;
    for (const auto& pt : t.points)
        maxUsed = std::max(maxUsed, pt.size());
    g.setValue({skinId, SkinClusterNode::kMaxInfluences}, int64_t(std::max<size_t>(4, maxUsed)));
    skin::setWeights(g, skinId, t);
    return skinId;
}

const ufbx_anim_stack* pickStack(const ufbx_scene& fbx, const std::string& name)
{
    for (size_t i = 0; i < fbx.anim_stacks.count; ++i)
        if (name.empty() || toString(fbx.anim_stacks.data[i]->name) == name)
            return fbx.anim_stacks.data[i];
    return nullptr;
}

/** Nodes whose local transform the stack animates. */
std::set<const ufbx_node*> animatedNodes(const ufbx_anim_stack& stack)
{
    static constexpr const char* kProps[] = {"Lcl Translation", "Lcl Rotation", "Lcl Scaling", "PreRotation", "PostRotation",
                                             "RotationPivot", "ScalingPivot", "RotationOffset", "ScalingOffset"};
    std::set<const ufbx_node*> out;
    for (size_t l = 0; l < stack.layers.count; ++l)
    {
        const ufbx_anim_layer* layer = stack.layers.data[l];
        for (size_t p = 0; p < layer->anim_props.count; ++p)
        {
            const ufbx_anim_prop& ap = layer->anim_props.data[p];
            if (!ap.element || ap.element->type != UFBX_ELEMENT_NODE)
                continue;
            std::string prop = toString(ap.prop_name);
            if (std::any_of(std::begin(kProps), std::end(kProps), [&](const char* k) { return prop == k; }))
                out.insert(reinterpret_cast<const ufbx_node*>(ap.element));
        }
    }
    return out;
}

/** Bakes the chosen take onto each animated node's translate/rotate/scale: one linear key per frame per moving channel. */
void importTake(scene::Graph& g, const ufbx_scene& fbx, const ImportOptions& opts, const glm::dmat4& C, const glm::dmat4& Cinv,
                const std::unordered_map<uint32_t, NodeFrame>& frames, ImportResult& r)
{
    LILA_ZONE();

    using namespace scene;
    const ufbx_anim_stack* stack = pickStack(fbx, opts.take);
    if (!stack)
    {
        r.warnings.push_back(std::format("no take named '{}'", opts.take));
        return;
    }
    r.take = toString(stack->name);
    r.fps = fbx.settings.frames_per_second > 0.0 ? fbx.settings.frames_per_second : 30.0;
    r.start = stack->time_begin;
    r.end = std::max(stack->time_end, stack->time_begin);
    const long long count = std::llround((r.end - r.start) * r.fps);
    std::vector<double> times;
    for (long long f = 0; f <= count; ++f)
        times.push_back(r.start + double(f) / r.fps);

    static constexpr const char* kCurveType[3] = {"animCurveTL", "animCurveTA", "animCurveTU"};
    static constexpr int kAttr[3] = {Transform::kTranslate, Transform::kRotate, Transform::kScale};
    for (const ufbx_node* fn : animatedNodes(*stack))
    {
        auto it = frames.find(fn->element_id);
        if (it == frames.end())
            continue;
        const NodeFrame& nf = it->second;
        std::array<anim::Curve, 9> curves;
        glm::dvec3 ref = nf.rest.r;
        for (double t : times)
        {
            ufbx_transform xf = ufbx_evaluate_transform(stack->anim, fn, t);
            glm::dmat4 local = C * toGlm(ufbx_transform_to_matrix(&xf)) * Cinv;
            Trs trs = decomposeWith(local, nf.pre, nf.order, ref);
            ref = trs.r;
            const glm::dvec3 groups[3] = {trs.t, trs.r, trs.s};
            for (int ch = 0; ch < 9; ++ch)
                curves[size_t(ch)].keys.push_back(
                    {.time = t, .value = groups[ch / 3][ch % 3], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
        }
        const glm::dvec3 rest[3] = {nf.rest.t, nf.rest.r, nf.rest.s};
        // Nodes keyed as rotation curves (joints by default) get one, posed exactly as the file at every frame.
        bool rotationCurve = false;
        const PlugRef rotate{nf.id, Transform::kRotate, -1};
        if (anim::keysAsRotation(g, rotate) && !g.source(rotate))
        {
            bool moves = false;
            for (int ch = 3; ch < 6; ++ch)
                for (const anim::Key& k : curves[size_t(ch)].keys)
                    moves = moves || std::abs(k.value - rest[1][ch % 3]) >= 1e-6;
            if (moves)
            {
                Node* n = g.node(nf.id);
                auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
                auto mode = RotationInterpolation(n->as<int64_t>(Transform::kRotationInterpolation));
                anim::RotationCurve rc = anim::rotationFromEuler({&curves[3], &curves[4], &curves[5]}, rest[1], order,
                                                                 mode == RotationInterpolation::Slerp ? anim::RotInterp::Slerp
                                                                                                      : anim::RotInterp::Smooth);
                NodeId curve = g.createNode(*types::find("animCurveRQ"), n->name() + "_rotate", kNoNode, false)->id();
                anim::setRotationCurve(g, curve, rc);
                g.connect({curve, anim::RotationCurveNode::kOutput, -1}, rotate);
                ++r.curves;
            }
            rotationCurve = true;
        }
        for (int ch = 0; ch < 9; ++ch)
        {
            if (rotationCurve && ch / 3 == 1)
                continue;
            anim::Curve& c = curves[size_t(ch)];
            const double v0 = rest[ch / 3][ch % 3];
            if (std::all_of(c.keys.begin(), c.keys.end(), [&](const anim::Key& k) { return std::abs(k.value - v0) < 1e-6; }))
                continue;
            const PlugRef plug{nf.id, kAttr[ch / 3], ch % 3};
            if (g.source(plug))
                continue;
            std::string name = g.node(nf.id)->name() + "_" + g.attr(plug).childNames[size_t(ch % 3)];
            NodeId curve = g.createNode(*types::find(kCurveType[ch / 3]), name, kNoNode, false)->id();
            anim::setCurve(g, curve, c);
            g.connect({curve, anim::CurveNode::kOutput, -1}, plug);
            ++r.curves;
        }
    }

    if (opts.setPlayback)
    {
        anim::Playback& pb = anim::playback();
        pb.fps = r.fps;
        pb.minTime = pb.animStart = r.start;
        pb.maxTime = pb.animEnd = r.end;
        anim::playbackChanged();
    }
}

struct SceneDeleter
{
    void operator()(ufbx_scene* s) const { ufbx_free_scene(s); }
};
} // namespace

std::optional<ImportAxes> importAxes(const fs::path& path)
{
    LILA_ZONE();

    ufbx_load_opts lo{};
    lo.load_external_files = false;
    lo.ignore_all_content = true;
    ufbx_error err{};
    std::u8string u8 = path.u8string();
    std::unique_ptr<ufbx_scene, SceneDeleter> fbx(ufbx_load_file_len(reinterpret_cast<const char*>(u8.data()), u8.size(), &lo, &err));
    if (!fbx)
        return std::nullopt;
    const ufbx_coordinate_axes& axes = fbx->settings.axes;
    ImportAxes out;
    out.toLila = fileToLila(axes, math::upAxis());
    out.toCanonical = fileToCanonical(axes, &out.zUp);
    return out;
}

ImportResult importFbx(scene::Graph& graph, const fs::path& path, const ImportOptions& opts)
{
    LILA_ZONE();

    using namespace scene;
    ImportResult r;

    ufbx_load_opts lo{};
    lo.load_external_files = false;
    lo.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
    ufbx_error err{};
    std::u8string u8 = path.u8string();
    progress::Scope scope("Importing " + path.filename().string(), 10.0);
    Graph::Batch batch(graph);
    std::unique_ptr<ufbx_scene, SceneDeleter> fbx;
    {
        progress::Scope read("Reading " + path.filename().string(), 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { fbx.reset(ufbx_load_file_len(reinterpret_cast<const char*>(u8.data()), u8.size(), &lo, &err)); });
    }
    if (!fbx)
    {
        r.error = std::string(err.description.data, err.description.length);
        if (r.error.empty())
            r.error = "can't read " + path.string();
        return r;
    }

    const ufbx_coordinate_axes& axes = fbx->settings.axes;
    r.sourceAxes = std::format("right {} up {} front {}", axisLabel(axes.right), axisLabel(axes.up), axisLabel(axes.front));
    r.sourceUnitMeters = fbx->settings.unit_meters > 0.0 ? fbx->settings.unit_meters : 0.01;

    // Conjugating every local matrix by C keeps the hierarchy clean: no conversion rotation lands on the roots.
    const double unitScale = r.sourceUnitMeters / 0.01;
    const glm::dmat4 C = glm::dmat4(fileToLila(axes, math::upAxis())) * glm::scale(glm::dmat4(1.0), glm::dvec3(unitScale));
    const glm::dmat4 Cinv = glm::inverse(C);

    const NodeType* meshType = types::find("mesh");
    const SlotNames slotNames = importMaterials(graph, *fbx);
    const NodeType* xformType = types::find("transform");
    const NodeType* jointType = types::find("joint");
    std::map<uint32_t, MeshPtr> meshCache;
    std::map<uint32_t, std::vector<Index>> pointCache;
    std::unordered_map<uint32_t, NodeFrame> frames; // by ufbx node element id
    std::vector<std::pair<const ufbx_node*, NodeId>> skinned, blended;
    const glm::dmat3 C3 = glm::dmat3(fileToLila(axes, math::upAxis()));
    const AxisMap fileToLilaAxes = axisMap(C3);

    // Skinned bones are joints even when the file calls them nulls.
    std::set<const ufbx_node*> bones;
    if (opts.skins && jointType)
        for (size_t i = 0; i < fbx->skin_clusters.count; ++i)
            if (const ufbx_node* b = fbx->skin_clusters.data[i]->bone_node)
                bones.insert(b);

    progress::Scope build("Building " + path.filename().string(), double(fbx->nodes.count), progress::None, 7.0);
    auto create = [&](auto&& self, const ufbx_node* fn, NodeId parent) -> void {
        build.step();
        const std::string fbxName = toString(fn->name);
        const bool isMesh = fn->mesh && fn->mesh->num_faces > 0;
        const bool isJoint = jointType && !isMesh && (fn->bone || bones.count(fn));
        const NodeType& type = isMesh ? *meshType : isJoint ? *jointType : *xformType;
        Node* n = graph.createNode(type, safeName(fbxName, isMesh ? "Mesh" : isJoint ? "joint" : "Transform"), parent, false);
        r.nodes.push_back(n->id());
        if (parent == opts.parent)
            r.roots.push_back(n->id());

        NodeFrame& nf = frames[fn->element_id];
        nf.id = n->id();
        nf.joint = isJoint;
        const glm::dmat4 local = C * toGlm(fn->node_to_parent) * Cinv;
        if (isJoint)
        {
            // PreRotation is jointOrient, as Maya writes it; the rotate order is carried through the axis swap.
            r.joints.push_back(n->id());
            glm::dvec3 preEuler = vec3Of(ufbx_find_vec3(&fn->props, "PreRotation", ufbx_vec3{0, 0, 0}));
            nf.pre = C3 * glm::dmat3(scene::eulerToMatrix(preEuler, RotateOrder::XYZ)) * glm::transpose(C3);
            nf.order = fileToLilaAxes.order(fromFbxOrder(int(fn->rotation_order)));
            glm::dvec3 lcl = vec3Of(ufbx_find_vec3(&fn->props, "Lcl Rotation", ufbx_vec3{0, 0, 0}));
            nf.rest = decomposeWith(local, nf.pre, nf.order, fileToLilaAxes.rotate(lcl));
            glm::dvec3 jo = scene::matrixToEuler(nf.pre, RotateOrder::XYZ);
            for (int i = 0; i < 3; ++i)
                if (std::abs(jo[i]) < 1e-9)
                    jo[i] = 0.0;
            graph.setValue({n->id(), Joint::kJointOrient, -1}, jo);
            graph.setValue({n->id(), Transform::kRotateOrder, -1}, int64_t(nf.order));
            if (fn->inherit_mode == UFBX_INHERIT_MODE_IGNORE_PARENT_SCALE)
                r.warnings.push_back(std::format("{}: segment scale compensation in the file is not kept", n->path()));
        }
        else
        {
            nf.rest = decompose(local);
        }
        const Trs& trs = nf.rest;
        if (trs.sheared)
            r.warnings.push_back(std::format("{}: shear dropped from its transform", n->path()));
        graph.setValue({n->id(), Transform::kTranslate, -1}, trs.t);
        graph.setValue({n->id(), Transform::kRotate, -1}, trs.r);
        graph.setValue({n->id(), Transform::kScale, -1}, trs.s);
        if (!fn->visible)
            graph.setValue({n->id(), Transform::kVisibility, -1}, false);

        if (isMesh)
        {
            const ufbx_mesh& mesh = *fn->mesh;
            MeshPtr ptr;
            auto cached = meshCache.find(mesh.element_id);
            if (!fn->has_geometry_transform && cached != meshCache.end())
                ptr = cached->second;
            else
            {
                MeshBuild built = convertMesh(mesh, *fn, C * toGlm(fn->geometry_to_node), slotNames);
                if (built.badFaces)
                    r.warnings.push_back(std::format("{}: {} degenerate face(s) skipped", n->path(), built.badFaces));
                ptr = std::make_shared<geo::Mesh>(std::move(built.mesh));
                pointCache[mesh.element_id] = std::move(built.pointOf);
                if (!fn->has_geometry_transform)
                    meshCache[mesh.element_id] = ptr;
            }
            graph.setValue({n->id(), MeshNode::kInMesh, -1}, ptr);
            if (mesh.skin_deformers.count)
            {
                if (opts.skins && types::find("skinCluster"))
                    skinned.push_back({fn, n->id()});
                else
                    r.skipped.push_back(std::format("skin on '{}' (mesh imported unskinned)", fbxName));
            }
            if (mesh.blend_deformers.count)
                blended.push_back({fn, n->id()});
        }
        else if (fn->attrib && fn->attrib_type != UFBX_ELEMENT_EMPTY && !fn->mesh && !isJoint)
        {
            r.skipped.push_back(std::format("{} '{}' (imported as a transform)", attribName(fn->attrib_type), fbxName));
        }

        for (size_t i = 0; i < fn->children.count; ++i)
            self(self, fn->children.data[i], n->id());
    };

    for (size_t i = 0; i < fbx->root_node->children.count; ++i)
        create(create, fbx->root_node->children.data[i], opts.parent);

    graph.evaluate();
    const glm::dmat4 parentWorld =
        opts.parent != kNoNode ? graph.node(opts.parent)->as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
    auto worldToLila = [&](const ufbx_matrix& m) { return parentWorld * C * toGlm(m) * Cinv; };

    for (const auto& [fn, meshId] : skinned)
    {
        NodeId sc = buildSkin(graph, *fn->mesh->skin_deformers.data[0], meshId, pointCache[fn->mesh->element_id], frames, worldToLila, r);
        if (sc != kNoNode)
            r.skins.push_back(sc);
    }

    // Morph targets become a morph with one target per channel: the full-weight shape, the channel's other keyframes as in-betweens.
    for (const auto& [fn, meshId] : blended)
    {
        const std::vector<Index>& pointOf = pointCache[fn->mesh->element_id];
        const glm::dmat3 lin(C * toGlm(fn->geometry_to_node));
        auto deltasOf = [&](const ufbx_blend_shape& shape) {
            auto deltas = std::make_shared<scene::MorphDeltas>();
            for (size_t k = 0; k < shape.num_offsets; ++k)
            {
                uint32_t v = shape.offset_vertices.data[k];
                Index p = v < pointOf.size() ? pointOf[v] : kInvalid;
                if (p == kInvalid)
                    continue;
                const ufbx_vec3& o = shape.position_offsets.data[k];
                deltas->indices.push_back(uint32_t(p));
                deltas->deltas.push_back(lin * glm::dvec3(o.x, o.y, o.z));
            }
            return deltas;
        };
        NodeId morph = scene::morphs::create(graph, meshId, "Morph");
        for (size_t d = 0; d < fn->mesh->blend_deformers.count; ++d)
        {
            const ufbx_blend_deformer& def = *fn->mesh->blend_deformers.data[d];
            for (size_t c = 0; c < def.channels.count; ++c)
            {
                const ufbx_blend_channel& ch = *def.channels.data[c];
                const ufbx_blend_shape* shape = ch.target_shape;
                if (!shape)
                    continue;
                NodeId t = scene::morphs::addTarget(graph, morph, toString(ch.name), deltasOf(*shape), double(ch.weight));
                for (size_t k = 0; k < ch.keyframes.count; ++k)
                {
                    const ufbx_blend_keyframe& kf = ch.keyframes.data[k];
                    const double w = double(kf.target_weight);
                    if (kf.shape && kf.shape != shape && std::abs(w) > 1e-6 && std::abs(w - 1.0) > 1e-6)
                        scene::morphs::setInBetween(graph, t, w, deltasOf(*kf.shape));
                }
            }
        }
    }

    if (opts.animation && fbx->anim_stacks.count)
        importTake(graph, *fbx, opts, C, Cinv, frames, r);
    else if (fbx->anim_curves.count)
        r.skipped.push_back(std::format("animation ({} curve(s))", fbx->anim_curves.count));
    for (size_t i = 0; i < fbx->metadata.warnings.count; ++i)
        r.warnings.push_back(toString(fbx->metadata.warnings.data[i].description));
    r.ok = true;
    return r;
}
} // namespace lila::fbx
