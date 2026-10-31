#include "io/ScenePreview.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "io/References.h"
#include "scene/Node.h"
#include "scene/Plug.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

namespace lila::io
{
namespace
{
using nlohmann::json;
namespace fs = std::filesystem;

constexpr int kSpMaxDepth = 8;

/** One node's attrs as stored, falling back to the type's defaults for the ones the file leaves out. */
struct SpAttrs
{
    const scene::NodeType& type;
    const json& stored;

    const json* find(int idx) const
    {
        const std::string& name = type.attrs[idx].name;
        if (auto it = stored.find(name); it != stored.end())
            return &*it;
        for (auto it = stored.begin(); it != stored.end(); ++it)
            if (type.fileAttrIndex(it.key()) == idx)
                return &*it;
        return nullptr;
    }

    std::optional<scene::PlugValue> value(std::string_view name) const
    {
        const int idx = type.attrIndex(name);
        if (idx < 0)
            return std::nullopt;
        const scene::AttrDef& def = type.attrs[idx];
        if (const json* j = find(idx))
            try
            {
                return scene::fromJson(def, *j);
            }
            catch (const std::exception&)
            {
            }
        return def.fileDefault ? *def.fileDefault : def.defaultValue;
    }

    template <class T> T get(std::string_view name, T fallback) const
    {
        const std::optional<scene::PlugValue> v = value(name);
        const T* t = v ? std::get_if<T>(&*v) : nullptr;
        return t ? *t : fallback;
    }
};

/** Transform::localMatrix from stored values: T * Rpt * Rp * R * Ra * Rp^-1 * Spt * Sp * S * Sp^-1. */
glm::dmat4 spLocal(const SpAttrs& a)
{
    if (!a.type.isA("transform"))
        return glm::dmat4(1.0);
    auto tr = [](const glm::dvec3& v) { return glm::translate(glm::dmat4(1.0), v); };
    auto v3 = [&](const char* name, double fallback) { return a.get<glm::dvec3>(name, glm::dvec3(fallback)); };
    const glm::dvec3 rp = v3("rotatePivot", 0.0), sp = v3("scalePivot", 0.0);
    const auto order = scene::RotateOrder(a.get<int64_t>("rotateOrder", 0));
    return tr(v3("translate", 0.0)) * tr(v3("rotatePivotTranslate", 0.0)) * tr(rp) * scene::eulerToMatrix(v3("rotate", 0.0), order) *
           scene::eulerToMatrix(v3("rotateAxis", 0.0), scene::RotateOrder::XYZ) * tr(-rp) * tr(v3("scalePivotTranslate", 0.0)) * tr(sp) *
           glm::scale(glm::dmat4(1.0), v3("scale", 1.0)) * tr(-sp);
}

/** "|A|B.inMesh" into node and attr; dots before the last '|' belong to names. */
std::pair<std::string, std::string> spSplitPlug(const std::string& plug)
{
    const size_t bar = plug.rfind('|');
    const size_t dot = plug.find('.', bar == std::string::npos ? 0 : bar);
    if (dot == std::string::npos)
        return {plug, {}};
    return {plug.substr(0, dot), plug.substr(dot + 1)};
}

void spAppend(const geo::Mesh& mesh, const glm::dmat4& world, std::vector<glm::vec3>& out)
{
    std::vector<geo::Index> points;
    for (geo::Index f = 0; f < mesh.numFaces(); ++f)
    {
        points.clear();
        mesh.forEachFaceCorner(f, [&](geo::Index c) { points.push_back(mesh.cornerPoint(c)); });
        for (size_t i = 1; i + 1 < points.size(); ++i)
            for (geo::Index p : {points[0], points[i], points[i + 1]})
                out.emplace_back(glm::vec3(world * glm::dvec4(mesh.position(p), 1.0)));
    }
}

void spCollect(const json& doc, const fs::path& baseDir, int depth, std::vector<glm::vec3>& out)
{
    if (depth > kSpMaxDepth || !doc.is_object())
        return;
    struct Entry
    {
        const scene::NodeType* type = nullptr;
        const json* attrs = nullptr;
        std::string path;
        glm::dmat4 world{1.0};
        bool visible = true;
    };
    std::vector<Entry> entries;
    std::unordered_map<std::string, size_t> byPath;
    std::unordered_multimap<std::string, size_t> byName;
    auto lookup = [&](const std::string& ref) -> const Entry* {
        if (auto it = byPath.find(ref.starts_with('|') ? ref : "|" + ref); it != byPath.end())
            return &entries[it->second];
        if (ref.find('|') == std::string::npos && byName.count(ref) == 1)
            return &entries[byName.find(ref)->second];
        return nullptr;
    };

    static const json kEmpty = json::object();
    const json nodes = doc.value("nodes", json::array());
    entries.reserve(nodes.size());
    for (const json& jn : nodes)
    {
        const scene::NodeType* type = scene::types::find(jn.value("type", ""));
        if (!type)
            continue;
        const std::string name = jn.value("name", "");
        Entry e;
        e.type = type;
        auto attrs = jn.find("attrs");
        e.attrs = attrs != jn.end() && attrs->is_object() ? &*attrs : &kEmpty;
        e.path = "|" + name;
        if (auto parent = jn.find("parent"); parent != jn.end() && parent->is_string())
            if (const Entry* p = lookup(parent->get<std::string>()))
            {
                e.world = p->world;
                e.visible = p->visible;
                e.path = p->path + e.path;
            }
        const SpAttrs a{*type, *e.attrs};
        e.world = e.world * spLocal(a);
        e.visible = e.visible && a.get<bool>("visibility", true);
        byPath[e.path] = entries.size();
        byName.emplace(name, entries.size());
        entries.push_back(std::move(e));
    }

    // What feeds each inMesh: another mesh (an instance), a modifier, or a node storing a base mesh.
    std::unordered_map<const Entry*, const Entry*> source;
    for (const json& c : doc.value("connections", json::array()))
    {
        if (!c.is_array() || c.size() < 2 || !c[0].is_string() || !c[1].is_string())
            continue;
        const auto [srcNode, srcAttr] = spSplitPlug(c[0].get<std::string>());
        const auto [dstNode, dstAttr] = spSplitPlug(c[1].get<std::string>());
        if (dstAttr != "inMesh")
            continue;
        if (const Entry* src = lookup(srcNode), *dst = lookup(dstNode); src && dst)
            source[dst] = src;
    }

    std::unordered_map<const Entry*, scene::MeshPtr> decoded;
    auto meshOf = [&](const Entry* e) -> scene::MeshPtr {
        if (auto it = decoded.find(e); it != decoded.end())
            return it->second;
        scene::MeshPtr mesh;
        for (auto it = e->attrs->begin(); it != e->attrs->end() && !mesh; ++it)
            if (const int idx = e->type->fileAttrIndex(it.key()); idx >= 0 && e->type->attrs[idx].type == scene::PlugType::Mesh)
                try
                {
                    const scene::PlugValue v = scene::fromJson(e->type->attrs[idx], *it);
                    if (const auto* m = std::get_if<scene::MeshPtr>(&v))
                        mesh = *m;
                }
                catch (const std::exception&)
                {
                }
        return decoded[e] = mesh;
    };
    for (const Entry& e : entries)
    {
        if (!e.visible || !e.type->isA("mesh"))
            continue;
        const Entry* from = &e;
        for (int hops = 0; hops < 64 && !meshOf(from); ++hops)
        {
            auto it = source.find(from);
            if (it == source.end())
                break;
            from = it->second;
        }
        if (scene::MeshPtr mesh = meshOf(from))
            spAppend(*mesh, e.world, out);
    }

    for (const json& rec : doc.value("references", json::array()))
    {
        if (!rec.is_object() || !rec.value("loaded", true))
            continue;
        const fs::path p = refs::resolve(rec.value("path", ""), baseDir);
        try
        {
            spCollect(readSceneFile(p, nullptr), p.parent_path(), depth + 1, out);
        }
        catch (const std::exception&)
        {
        }
    }
}
} // namespace

std::vector<glm::vec3> previewTriangles(const fs::path& path)
{
    LILA_ZONE();

    std::vector<glm::vec3> out;
    json doc;
    try
    {
        doc = readSceneFile(path, nullptr);
    }
    catch (const std::exception&)
    {
        return out;
    }
    spCollect(doc, path.parent_path(), 0, out);
    if (doc.value("upAxis", "y") == "z")
        for (glm::vec3& p : out)
            p = glm::vec3(p.x, p.z, -p.y);
    return out;
}
} // namespace lila::io
