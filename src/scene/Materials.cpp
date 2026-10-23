#include "scene/Materials.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "core/Paths.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace lila::scene
{
namespace
{
using namespace material;

AttrDef floatAttr(std::string name, double def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}

AttrDef colorAttr(std::string name, glm::dvec3 def, std::string doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable, .doc = std::move(doc)};
    a.childNames = {name + "R", name + "G", name + "B"};
    return a;
}

AttrDef pathAttr(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrFile,
            .doc = std::move(doc)};
}

AttrDef enumAttr(std::string name, std::vector<std::string> names, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
            .enumNames = std::move(names), .doc = std::move(doc)};
}

std::vector<AttrDef> materialAttrs()
{
    std::vector<AttrDef> a(kCount);
    a[kBaseColor] = colorAttr("baseColor", glm::dvec3(0.5), "Linear base color, used when no base color map is set.");
    a[kBaseColorMap] = pathAttr("baseColorMap", "sRGB base color texture; its alpha is the opacity of masked and translucent materials.");
    a[kMetallic] = floatAttr("metallic", 0.0, "0 dielectric, 1 metal.");
    a[kMetallicMap] = pathAttr("metallicMap", "Linear metallic texture (red channel).");
    a[kSpecular] = floatAttr("specular", 0.5, "Dielectric reflectance; 0.5 is 4%, Unreal's default.");
    a[kRoughness] = floatAttr("roughness", 0.4, "0 mirror, 1 fully rough.");
    a[kRoughness].fileDefault = 0.5;
    a[kRoughnessMap] = pathAttr("roughnessMap", "Linear roughness texture (red channel).");
    a[kNormalMap] = pathAttr("normalMap", "Tangent-space normal map.");
    a[kNormalFormat] = enumAttr("normalFormat", {"directx", "opengl"},
                                "Green channel convention: directx (green down, Unreal) or opengl (green up; Flip Green in the editor).");
    a[kAmbientOcclusionMap] = pathAttr("ambientOcclusionMap", "Linear occlusion texture (red channel); darkens ambient light only.");
    a[kOrmMap] = pathAttr("ormMap", "Packed occlusion/roughness/metallic texture, channels per packedLayout. Wins over the separate maps.");
    a[kPackedLayout] = enumAttr("packedLayout", {"orm", "mro"},
                                "Channel order of the packed map: orm (R occlusion, G roughness, B metallic, Unreal) or mro (R metallic, "
                                "G roughness, B occlusion).");
    a[kEmissiveColor] = colorAttr("emissiveColor", glm::dvec3(0.0), "Linear emitted light, used when no emissive map is set.");
    a[kEmissiveMap] = pathAttr("emissiveMap", "sRGB emissive texture.");
    a[kEmissiveIntensity] = floatAttr("emissiveIntensity", 1.0, "Scales the emissive color or map.");
    a[kOpacity] = floatAttr("opacity", 1.0, "Masked and translucent only; multiplies the base color map's alpha.");
    a[kOpacityMaskClip] = floatAttr("opacityMaskClipValue", 0.3333, "Masked only: opacity below this is cut away.");
    a[kBlendMode] = enumAttr("blendMode", {"opaque", "masked", "translucent"}, "How opacity is used.");
    a[kTwoSided] = {.name = "twoSided", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
                    .doc = "Draw back faces too (with flipped normals). One-sided materials cull them, as in Unreal."};
    a[kUvSet] = {.name = "uvSet", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
                 .doc = "UV set the textures use (0 or 1)."};
    return a;
}

Graph& graph() { return Graph::current(); }

Node& materialArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("material"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a material");
    return *n;
}

geo::Mesh baseMesh(Graph& g, const PlugRef& p)
{
    g.evaluate();
    const MeshPtr& m = std::get<MeshPtr>(g.storedValue(p));
    return m ? *m : geo::Mesh();
}

std::vector<uint32_t> liveFaces(const geo::Mesh& m)
{
    std::vector<uint32_t> out;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

/** Texture attr for a channel name, -1 when unknown. */
int channelAttr(const std::string& channel)
{
    for (const auto& [name, attr] : materials::textureChannels())
        if (name == channel)
            return attr;
    if (channel == "ao" || channel == "occlusion")
        return kAmbientOcclusionMap;
    return -1;
}

json describe(Graph& g, const Node& n)
{
    json out = {{"name", n.name()}};
    const auto& defs = n.type().attrs;
    for (int i = 0; i < kCount; ++i)
    {
        const AttrDef& d = defs[size_t(i)];
        if (d.type == PlugType::Enum)
        {
            int64_t v = n.as<int64_t>(i);
            out[d.name] = v >= 0 && size_t(v) < d.enumNames.size() ? json(d.enumNames[size_t(v)]) : json(v);
        }
        else
            out[d.name] = toJson(n.value(i));
    }
    out["flipGreen"] = NormalFormat(n.as<int64_t>(kNormalFormat)) == NormalFormat::OpenGL;
    json used = json::array();
    g.evaluate();
    for (NodeId id : g.nodes())
    {
        Node* m = g.node(id);
        if (!m->type().isA("mesh"))
            continue;
        const MeshPtr& mesh = m->as<MeshPtr>(MeshNode::kOutMesh);
        if (!mesh)
            continue;
        for (const auto& [slot, count] : materials::slotUsage(*mesh))
            if (slot == n.name())
                used.push_back({{"object", m->name()}, {"faces", count}});
    }
    out["usedBy"] = std::move(used);
    return out;
}

/** Sets flags of createMaterial that name material attrs. */
void applyFlags(Graph& g, Node& n, const Args& a)
{
    const auto& defs = n.type().attrs;
    for (int i = 0; i < kCount; ++i)
    {
        const AttrDef& d = defs[size_t(i)];
        std::string flag = d.name == "emissiveColor" ? "emissive" : d.name;
        if (!a.has(flag))
            continue;
        try
        {
            g.setValue({n.id(), i, -1}, fromJson(d, a.flags[flag]));
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError("createMaterial: " + flag + ": " + e.what());
        }
    }
    if (a.has("flipGreen"))
        g.setValue({n.id(), kNormalFormat, -1}, int64_t(a.get<bool>("flipGreen") ? NormalFormat::OpenGL : NormalFormat::DirectX));
}

/** Every mesh the command targets, with the face ids to assign (empty = whole object). */
std::map<NodeId, std::vector<uint32_t>> assignTargets(Graph& g, const Args& a)
{
    std::map<NodeId, std::vector<uint32_t>> out;
    std::vector<std::string> named;
    for (size_t i = 1; i < a.positional.size(); ++i)
    {
        const json& v = a.positional[i];
        if (v.is_string())
            named.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    named.push_back(x.get<std::string>());
    }

    if (a.has("faces"))
    {
        if (named.size() != 1)
            throw CommandError("assignMaterial: faces= needs exactly one object");
        Node* n = g.find(named[0]);
        if (!n || !n->type().isA("mesh"))
            throw CommandError("assignMaterial: " + named[0] + " is not a mesh");
        std::vector<uint32_t>& ids = out[n->id()];
        for (const json& f : a.flags["faces"])
            ids.push_back(f.get<uint32_t>());
        if (ids.empty())
            out.erase(n->id());
        return out;
    }

    // No objects named: the face selection in face mode, else the selected objects.
    if (named.empty() && components::mode() == ComponentMode::Face)
        for (const auto& [node, ids] : components::selected(ComponentMode::Face))
            if (Node* n = g.node(node); n && n->type().isA("mesh") && !ids.empty())
                out[node] = ids;
    if (!out.empty())
        return out;
    if (named.empty())
        for (NodeId id : g.selection())
            named.push_back(g.node(id)->name());
    for (const std::string& name : named)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError("assignMaterial: no object named " + name);
        // A selected group assigns to every mesh under it, like Maya.
        if (n->type().isA("mesh"))
            out[n->id()];
        for (NodeId d : g.descendants(n->id()))
            if (g.node(d)->type().isA("mesh"))
                out[d];
    }
    return out;
}

/** Rewrites one slot name on every mesh's base mesh. */
void renameSlots(Graph& g, const std::string& from, const std::string& to)
{
    LILA_ZONE();

    g.evaluate();
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n->type().isA("mesh") || n->referenceId())
            continue;
        MeshPtr cage = modifiers::cage(g, id);
        if (!cage || std::ranges::find(cage->materialNames(), from) == cage->materialNames().end())
            continue;
        PlugRef base = modifiers::editPlug(g, id);
        geo::Mesh m = baseMesh(g, base);
        for (std::string& s : m.materialNames())
            if (s == from)
                s = to;
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    }
}
} // namespace

namespace materials
{
Node* find(Graph& g, std::string_view slot)
{
    Node* n = g.find(slot);
    return n && n->type().isA("material") ? n : nullptr;
}

std::vector<Node*> all(Graph& g)
{
    std::vector<Node*> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n->type().isA("material"))
            out.push_back(n);
    return out;
}

Node* create(Graph& g, std::string_view name)
{
    const NodeType* t = types::find("material");
    if (!t)
        throw CommandError("material type not registered");
    // Exact names: a slot only binds to a node of the same name, so the naming policy must not rewrite it.
    return g.createNode(*t, name.empty() ? std::string_view("M_Material") : name, kNoNode, false);
}

std::array<int, 3> packedChannels(PackedLayout layout)
{
    if (layout == PackedLayout::MRO)
        return {2, 1, 0};
    return {0, 1, 2};
}

const std::vector<std::pair<std::string, int>>& textureChannels()
{
    static const std::vector<std::pair<std::string, int>> c = {
        {"baseColor", kBaseColorMap}, {"metallic", kMetallicMap},  {"roughness", kRoughnessMap},
        {"normal", kNormalMap},       {"ambientOcclusion", kAmbientOcclusionMap}, {"orm", kOrmMap},
        {"emissive", kEmissiveMap},
    };
    return c;
}

std::string resolvePath(const std::string& path)
{
    return paths::toUtf8(paths::resolvePath(path));
}

int32_t slotIndex(geo::Mesh& mesh, const std::string& name)
{
    auto& names = mesh.materialNames();
    auto it = std::ranges::find(names, name);
    if (it != names.end())
        return int32_t(it - names.begin());
    names.push_back(name);
    return int32_t(names.size() - 1);
}

std::string slotName(const geo::Mesh& mesh, int32_t id)
{
    const auto& names = mesh.materialNames();
    if (id >= 0 && size_t(id) < names.size())
        return names[size_t(id)];
    return names.empty() && id == 0 ? std::string(kDefaultSlot) : "Material" + std::to_string(id);
}

void assign(geo::Mesh& mesh, const std::string& slot, const std::vector<uint32_t>& faces)
{
    LILA_ZONE();

    bool hadAttr = mesh.faceAttrs().has(geo::attr::kMaterial);
    if (faces.empty())
    {
        mesh.materialNames() = {slot};
        std::span<int32_t> ids = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        std::ranges::fill(ids, 0);
        return;
    }
    // Faces without a slot yet keep the default material under an explicit name.
    if (mesh.materialNames().empty())
        mesh.materialNames().push_back(kDefaultSlot);
    else if (!hadAttr)
        mesh.materialNames().resize(1);
    int32_t id = slotIndex(mesh, slot);
    std::span<int32_t> ids = mesh.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (uint32_t f : faces)
        if (f < ids.size() && mesh.faceValid(f))
            ids[f] = id;
    pruneSlots(mesh);
}

void pruneSlots(geo::Mesh& mesh)
{
    std::span<int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    auto& names = mesh.materialNames();
    if (ids.empty())
        return;
    int32_t maxId = 0;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            maxId = std::max(maxId, ids[f]);
    std::vector<uint32_t> count(std::max(size_t(maxId) + 1, names.size()), 0);
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            ++count[size_t(std::max(0, ids[f]))];
    std::vector<int32_t> remap(count.size(), 0);
    std::vector<std::string> kept;
    for (size_t i = 0; i < count.size(); ++i)
        if (count[i])
        {
            remap[i] = int32_t(kept.size());
            kept.push_back(slotName(mesh, int32_t(i)));
        }
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            ids[f] = remap[size_t(std::max(0, ids[f]))];
    names = std::move(kept);
}

std::vector<std::pair<std::string, uint32_t>> slotUsage(const geo::Mesh& mesh)
{
    std::span<const int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::vector<uint32_t> count(std::max<size_t>(mesh.materialNames().size(), 1), 0);
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        size_t id = ids.empty() ? 0 : size_t(std::max(0, ids[f]));
        if (id >= count.size())
            count.resize(id + 1, 0);
        ++count[id];
    }
    std::vector<std::pair<std::string, uint32_t>> out;
    for (size_t i = 0; i < count.size(); ++i)
        if (count[i] || i < mesh.materialNames().size())
            out.push_back({slotName(mesh, int32_t(i)), count[i]});
    return out;
}

void registerTypes()
{
    if (types::find("material"))
        return;
    NodeType t;
    t.name = "material";
    t.icon = "palette";
    t.ownAttrs = materialAttrs();
    t.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "createMaterial",
        .doc = "Create a material (Unreal default lit inputs). Faces use it through a slot of the same name. Returns its name.",
        .positionalDoc = "name",
        .flags = {{"baseColor", "bc", T::Vec3, nullptr, "Linear base color."},
                  {"metallic", "", T::Float, nullptr, "0 to 1."},
                  {"roughness", "", T::Float, nullptr, "0 to 1."},
                  {"specular", "", T::Float, nullptr, "Dielectric reflectance, 0.5 default."},
                  {"emissive", "", T::Vec3, nullptr, "Linear emissive color."},
                  {"emissiveIntensity", "", T::Float, nullptr, "Emissive scale."},
                  {"opacity", "", T::Float, nullptr, "Masked/translucent opacity."},
                  {"opacityMaskClipValue", "", T::Float, nullptr, "Masked cutoff."},
                  {"blendMode", "", T::Any, nullptr, "opaque, masked or translucent."},
                  {"twoSided", "", T::Bool, nullptr, "Draw back faces."},
                  {"uvSet", "", T::Int, nullptr, "UV set for textures."},
                  {"normalFormat", "", T::Any, nullptr, "directx (Unreal) or opengl."},
                  {"flipGreen", "", T::Bool, nullptr, "Normal map is OpenGL style (green up); same as normalFormat='opengl'."},
                  {"baseColorMap", "", T::String, nullptr, "Texture path."},
                  {"metallicMap", "", T::String, nullptr, "Texture path."},
                  {"roughnessMap", "", T::String, nullptr, "Texture path."},
                  {"normalMap", "", T::String, nullptr, "Texture path."},
                  {"ambientOcclusionMap", "", T::String, nullptr, "Texture path."},
                  {"ormMap", "", T::String, nullptr, "Packed occlusion/roughness/metallic texture path."},
                  {"packedLayout", "", T::Any, nullptr, "orm or mro: the packed map's channel order."},
                  {"emissiveMap", "", T::String, nullptr, "Texture path."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "M_Material";
            if (!Graph::validName(name))
                throw CommandError("createMaterial: invalid name '" + name + "'");
            Node* n = materials::create(g, name);
            applyFlags(g, *n, a);
            return n->name();
        },
    });

    ops::registerCommand({
        .name = "assignMaterial",
        .doc = "Assign a material to whole objects, to faces= of one object, or (with no objects) to the face selection in "
               "face mode, else the selected objects. Unused slots are dropped.",
        .positionalDoc = "material [objects...]",
        .flags = {{"faces", "f", T::List, nullptr, "Face ids of the one object given."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("assignMaterial: give a material name");
            std::string mat = a.positional[0].get<std::string>();
            // The default slot may be assigned without a node, to clear an assignment.
            if (mat != materials::kDefaultSlot)
                materialArg(g, mat, "assignMaterial");
            auto targets = assignTargets(g, a);
            if (targets.empty())
                throw CommandError("assignMaterial: select meshes or faces first");
            UndoChunk chunk("Assign Material");
            json out = json::array();
            for (auto& [id, faces] : targets)
            {
                PlugRef base = modifiers::editPlug(g, id);
                geo::Mesh m = baseMesh(g, base);
                materials::assign(m, mat, faces);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                out.push_back(g.node(id)->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "listMaterials",
        .doc = "Names of every material; with objects, their slots instead: [{object, slots: [{slot, material, faces}]}].",
        .positionalDoc = "[objects...]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            if (a.positional.empty())
            {
                for (Node* n : materials::all(g))
                    out.push_back(n->name());
                return out;
            }
            g.evaluate();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                    throw CommandError("listMaterials: " + name + " is not a mesh");
                json slots = json::array();
                if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                    for (const auto& [slot, count] : materials::slotUsage(*m))
                        slots.push_back({{"slot", slot}, {"material", materials::find(g, slot) != nullptr}, {"faces", count}});
                out.push_back({{"object", n->name()}, {"slots", std::move(slots)}});
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "materialInfo",
        .doc = "Every input of a material plus the meshes using it: {name, baseColor, ..., flipGreen, usedBy: [{object, faces}]}.",
        .positionalDoc = "material",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("materialInfo: give a material name");
            return describe(g, materialArg(g, a.positional[0].get<std::string>(), "materialInfo"));
        },
    });

    ops::registerCommand({
        .name = "setMaterialTexture",
        .doc = "Set or clear (path='') a texture: channel baseColor, metallic, roughness, normal, ambientOcclusion, orm or "
               "emissive. Stored relative to the workspace root (or the scene folder without one).",
        .positionalDoc = "material",
        .flags = {{"channel", "c", T::String, nullptr, "Texture channel."}, {"path", "p", T::String, "", "Image file; empty clears."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("setMaterialTexture: give a material name");
            Node& n = materialArg(g, a.positional[0].get<std::string>(), "setMaterialTexture");
            std::string channel = a.get<std::string>("channel");
            int attr = channelAttr(channel);
            if (attr < 0)
                throw CommandError("setMaterialTexture: unknown channel '" + channel + "'");
            std::string path = a.get<std::string>("path");
            std::replace(path.begin(), path.end(), '\\', '/');
            g.setValue({n.id(), attr, -1}, paths::storedPath(paths::fromUtf8(path)));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "renameMaterial",
        .doc = "Rename a material and every slot bound to it. Returns the new name.",
        .positionalDoc = "material newName",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.size() != 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
                throw CommandError("renameMaterial: give the material and its new name");
            Node& n = materialArg(g, a.positional[0].get<std::string>(), "renameMaterial");
            std::string from = n.name();
            std::string to = g.rename(n.id(), a.positional[1].get<std::string>());
            if (to != from)
                renameSlots(g, from, to);
            return to;
        },
    });
}
} // namespace materials
} // namespace lila::scene
