#include "io/References.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Paths.h"
#include "io/SceneFile.h"
#include "io/SceneFormats.h"
#include "scene/Graph.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace lila::io
{
namespace fs = std::filesystem;
using json = nlohmann::json;
using scene::Graph;
using scene::NodeId;

namespace
{
constexpr int kMaxDepth = 16; // guards against files that reference each other

std::vector<Reference> gRefs;
uint32_t gNextId = 1;
std::vector<std::pair<std::string, std::string>> gPendingParents; // referenced node, scene parent

const json kNoArray = json::array();
const json kNoObject = json::object();

/** j[key] without copying it, or `fallback` when j has no such member. */
const json& member(const json& j, const char* key, const json& fallback)
{
    auto it = j.find(key);
    return it == j.end() ? fallback : *it;
}

std::string prefixed(const std::string& ns, const std::string& name)
{
    return ns.empty() ? name : ns + ":" + name;
}

std::string stripNs(const std::string& ns, const std::string& name)
{
    return name.rfind(ns + ":", 0) == 0 ? name.substr(ns.size() + 1) : name;
}

std::vector<NodeId> nodesOf(uint32_t refId)
{
    std::vector<NodeId> out;
    Graph& g = Graph::current();
    for (NodeId id : g.nodes())
        if (g.node(id)->referenceId() == refId)
            out.push_back(id);
    return out;
}

/** Appends the plug names (a Vec3's components by name) for the lock bits in `bits`. */
void appendLockNames(const scene::AttrDef& def, uint8_t bits, json& out)
{
    if (def.type != scene::PlugType::Vec3)
    {
        if (bits & 1u)
            out.push_back(def.name);
        return;
    }
    for (int c = 0; c < 3; ++c)
        if (bits & (1u << c))
            out.push_back(def.childNames[c]);
}

bool isStoredInput(Graph& g, scene::Node* n, int i)
{
    const scene::AttrDef& a = n->type().attrs[size_t(i)];
    return (a.flags & scene::AttrStorable) && (a.flags & scene::AttrInput) && !g.source({n->id(), i, -1});
}

void captureBaseline(Reference& r)
{
    Graph& g = Graph::current();
    r.baseline.clear();
    r.baselineConnections.clear();
    for (NodeId id : nodesOf(r.id))
    {
        scene::Node* n = g.node(id);
        NodeId p = g.parent(id);
        Reference::Baseline& b = r.baseline[stripNs(r.ns, n->name())];
        for (int i = 0; i < int(n->type().attrs.size()); ++i)
        {
            if (isStoredInput(g, n, i))
                b.attrs.emplace(i, n->value(i));
            if (uint8_t mask = n->lockMask(i))
                b.locks.emplace(i, mask);
        }
        if (p)
            b.parent = g.node(p)->name();
    }
    for (auto& [dst, src] : g.connections())
        if (g.node(src.node)->referenceId() == r.id && g.node(dst.node)->referenceId() == r.id)
            r.baselineConnections.insert({g.plugName(src), g.plugName(dst)});
}

std::string uniqueNamespace(std::string wanted)
{
    for (char& c : wanted)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    if (wanted.empty() || std::isdigit(static_cast<unsigned char>(wanted[0])))
        wanted = "ref_" + wanted;
    std::string ns = wanted;
    for (int i = 1; refs::find(ns); ++i)
        ns = wanted + std::to_string(i);
    return ns;
}

/** Scene-owned nodes parented under this reference's nodes, as [child, parent]. */
json sceneChildren(const Reference& r)
{
    Graph& g = Graph::current();
    json out = json::array();
    for (NodeId id : nodesOf(r.id))
        for (NodeId c : g.children(id))
            if (g.node(c)->referenceId() != r.id)
                out.push_back({g.node(c)->name(), g.node(id)->name()});
    return out;
}

void detachChildren(const json& children)
{
    Graph& g = Graph::current();
    for (const json& pair : children)
        if (scene::Node* c = g.find(pair[0].get<std::string>()))
            g.setParent(c->id(), scene::kNoNode, true);
}

void attachChildren(const json& children, std::vector<std::string>& warnings)
{
    Graph& g = Graph::current();
    for (const json& pair : children)
    {
        scene::Node* c = g.find(pair[0].get<std::string>());
        scene::Node* p = g.find(pair[1].get<std::string>());
        if (c && p)
            g.setParent(c->id(), p->id(), true);
        else if (c)
            warnings.push_back(pair[0].get<std::string>() + ": its referenced parent " + pair[1].get<std::string>() + " is gone");
    }
}

/** Reference edits change nodes the user can't otherwise undo into; undo history can't span them. */
struct NoUndo
{
    bool was = UndoStack::get().enabled();
    NoUndo() { UndoStack::get().setEnabled(false); }
    ~NoUndo()
    {
        UndoStack::get().setEnabled(was);
        UndoStack::get().clear();
    }
};
} // namespace

namespace
{
std::string readBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("can't open " + path.string());
    std::string bytes(size_t(in.tellg()), char(0));
    in.seekg(0);
    if (!in.read(bytes.data(), std::streamsize(bytes.size())))
        throw std::runtime_error("can't read " + path.string());
    return bytes;
}
} // namespace

json readSceneFile(const fs::path& path, std::vector<std::string>* warnings)
{
    LILA_ZONE();

    std::string bytes = readBytes(path);
    switch (detectFormat(bytes))
    {
    case SceneFormat::Binary: return decodeBinary(bytes, warnings);
    case SceneFormat::Ascii: return decodeAscii(bytes, warnings);
    case SceneFormat::Json:
    {
        json doc = json::parse(bytes, nullptr, false);
        if (doc.is_discarded())
            throw std::runtime_error(path.string() + " is not valid JSON");
        return doc;
    }
    }
    return {};
}

json readSceneHeader(const fs::path& path)
{
    LILA_ZONE();

    std::string bytes = readBytes(path);
    if (detectFormat(bytes) == SceneFormat::Binary)
        return decodeBinaryHeader(bytes);
    return readSceneFile(path, nullptr);
}

std::vector<NodeId> instantiate(const json& doc, const std::string& ns, uint32_t refId, const fs::path& baseDir,
                                std::vector<std::string>& warnings, int depth)
{
    LILA_ZONE();

    if (depth > kMaxDepth)
    {
        warnings.push_back("references nest too deeply (a file probably references itself); stopped");
        return {};
    }
    Graph& g = Graph::current();
    std::vector<NodeId> created;
    std::map<std::string, std::string> renamed; // name in file -> scene name, should uniquing kick in
    auto resolve = [&](const std::string& n) {
        auto it = renamed.find(n);
        return it == renamed.end() ? prefixed(ns, n) : it->second;
    };

    // A referenced file's own references come in as part of it, under a nested namespace.
    if (refId != 0)
        for (const json& rec : member(doc, "references", kNoArray))
        {
            if (!rec.value("loaded", true))
                continue;
            fs::path p = refs::resolve(rec.value("path", ""), baseDir);
            std::string nested = prefixed(ns, rec.value("namespace", p.stem().string()));
            try
            {
                json sub = readSceneFile(p, &warnings);
                auto ids = instantiate(sub, nested, refId, p.parent_path(), warnings, depth + 1);
                created.insert(created.end(), ids.begin(), ids.end());
                for (const json& e : member(rec, "edits", kNoArray))
                {
                    scene::Node* n = g.find(prefixed(nested, e.value("node", "")));
                    if (!n)
                        continue;
                    for (auto& [attr, value] : member(e, "attrs", kNoObject).items())
                        if (int idx = n->type().fileAttrIndex(attr); idx >= 0)
                            g.setValue({n->id(), idx, -1}, scene::fromJson(n->type().attrs[idx], value));
                }
            }
            catch (const std::exception& e)
            {
                warnings.push_back("nested reference " + p.string() + ": " + e.what());
            }
        }

    // Meshes decode up front, in parallel; the loop below picks them up by their json.
    std::vector<std::pair<const scene::AttrDef*, const json*>> meshItems;
    for (const json& jn : member(doc, "nodes", kNoArray))
        if (const scene::NodeType* t = scene::types::find(jn.value("type", "")))
            for (auto& [attr, value] : member(jn, "attrs", kNoObject).items())
                if (int idx = t->fileAttrIndex(attr); idx >= 0 && t->attrs[idx].type == scene::PlugType::Mesh)
                    meshItems.push_back({&t->attrs[idx], &value});
    std::vector<scene::FromJsonResult> meshValues;
    progress::Scope scope("", 3.0 + double(member(doc, "nodes", kNoArray).size()));
    {
        progress::Scope meshes("Decoding meshes", 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { meshValues = scene::fromJsonAll(meshItems); });
    }
    std::unordered_map<const json*, size_t> meshAt;
    for (size_t i = 0; i < meshItems.size(); ++i)
        meshAt[meshItems[i].second] = i;

    for (const json& jn : member(doc, "nodes", kNoArray))
    {
        scope.step();
        std::string type = jn.value("type", "");
        std::string name = jn.value("name", "");
        const scene::NodeType* t = scene::types::find(type);
        if (!t)
        {
            warnings.push_back("unknown node type '" + type + "' (" + name + ") skipped; is an extension missing?");
            continue;
        }
        NodeId parent = scene::kNoNode;
        if (jn.contains("parent"))
        {
            if (scene::Node* p = g.find(resolve(jn["parent"].get<std::string>())))
                parent = p->id();
            else
                warnings.push_back(name + ": parent " + jn["parent"].get<std::string>() + " not found");
        }
        scene::Node* n = g.createNode(*t, prefixed(ns, name), parent, false);
        if (n->name() != prefixed(ns, name))
            renamed[name] = n->name();
        if (refId)
            g.setReference(n->id(), refId);
        created.push_back(n->id());

        const json& stored = member(jn, "attrs", kNoObject);
        for (size_t i = 0; i < t->attrs.size(); ++i)
            if (const scene::AttrDef& a = t->attrs[i]; a.fileDefault && !stored.contains(a.name))
                g.setValue({n->id(), int(i), -1}, *a.fileDefault);
        for (auto& [attr, value] : stored.items())
        {
            int idx = t->fileAttrIndex(attr);
            if (idx < 0)
            {
                warnings.push_back(name + "." + attr + ": no such attribute, ignored");
                continue;
            }
            try
            {
                if (auto m = meshAt.find(&value); m != meshAt.end())
                {
                    scene::FromJsonResult& r = meshValues[m->second];
                    if (!r.error.empty())
                        throw std::invalid_argument(r.error);
                    g.setValue({n->id(), idx, -1}, std::move(r.value));
                }
                else
                    g.setValue({n->id(), idx, -1}, scene::fromJson(t->attrs[idx], value));
            }
            catch (const std::exception& e)
            {
                warnings.push_back(name + "." + attr + ": " + e.what());
            }
        }
        // After the values, which a lock would otherwise refuse.
        for (const json& plug : member(jn, "locked", kNoArray))
            try
            {
                g.setLocked(g.plug(n->name() + "." + plug.get<std::string>()), true);
            }
            catch (const std::exception& e)
            {
                warnings.push_back(name + ": can't lock " + plug.dump() + ": " + e.what());
            }
    }

    auto mapPlug = [&](const std::string& plug) {
        auto dot = plug.find('.');
        // Plugs on nodes from this document get the namespace; ones already namespaced (nested refs) are absolute
        // within it, so prefix those too.
        std::string node = resolve(plug.substr(0, dot));
        if (dot != std::string::npos)
            if (const scene::Node* n = g.find(node))
            {
                size_t end = plug.find_first_of(".[", dot + 1);
                std::string attr = plug.substr(dot + 1, end == std::string::npos ? std::string::npos : end - dot - 1);
                if (int idx = n->type().fileAttrIndex(attr); idx >= 0 && n->type().attrIndex(attr) < 0)
                    return node + "." + n->type().attrs[idx].name + (end == std::string::npos ? "" : plug.substr(end));
            }
        return node + plug.substr(dot);
    };
    for (const json& c : member(doc, "connections", kNoArray))
    {
        try
        {
            g.connect(g.plug(mapPlug(c[0].get<std::string>())), g.plug(mapPlug(c[1].get<std::string>())));
        }
        catch (const std::exception& e)
        {
            warnings.push_back(std::string("connection skipped: ") + e.what());
        }
    }
    return created;
}

namespace refs
{
std::vector<Reference>& all() { return gRefs; }

Reference* find(const std::string& ns)
{
    for (Reference& r : gRefs)
        if (r.ns == ns)
            return &r;
    return nullptr;
}

void clear()
{
    gRefs.clear();
    gPendingParents.clear();
}

fs::path resolve(const std::string& stored, const fs::path& sceneDir)
{
    fs::path p = paths::resolvePath(stored, sceneDir);
    std::error_code ec;
    if (!p.is_absolute() || !fs::exists(p, ec))
        return p;
    fs::path canon = fs::weakly_canonical(p, ec);
    return ec ? p : canon;
}

void load(Reference& r, std::vector<std::string>& warnings)
{
    LILA_ZONE();

    const std::string name = r.path.filename().string();
    progress::Scope scope("Loading reference " + name, 10.0);
    Graph::Batch batch;
    json doc;
    {
        progress::Scope read("Reading reference " + name, 0.0, progress::None, 4.0);
        std::vector<std::string> w;
        progress::runOnWorker([&] { doc = readSceneFile(r.path, &w); });
        warnings.insert(warnings.end(), w.begin(), w.end());
    }
    {
        progress::Scope build("Loading reference " + name, 0.0, progress::None, 6.0);
        instantiate(doc, r.ns, r.id, r.path.parent_path(), warnings);
    }
    captureBaseline(r);
    r.loaded = true;
    applyEdits(r, r.edits, warnings);
    attachChildren(r.children, warnings);
    r.edits = json::array();
    r.children = json::array();
}

Reference& create(const fs::path& path, std::string ns, std::vector<std::string>& warnings)
{
    std::error_code ec;
    Reference r;
    r.id = gNextId++;
    r.path = fs::weakly_canonical(path, ec);
    r.ns = uniqueNamespace(ns.empty() ? path.stem().string() : ns);
    gRefs.push_back(r);
    Reference& ref = gRefs.back();
    try
    {
        load(ref, warnings);
    }
    catch (...)
    {
        gRefs.pop_back();
        throw;
    }
    return ref;
}

json currentEdits(const Reference& r) { return currentEdits(r, paths::currentSceneDir()); }

json currentEdits(const Reference& r, const fs::path& sceneDir)
{
    if (!r.loaded)
        return r.edits;
    Graph& g = Graph::current();
    json edits = json::array();
    for (NodeId id : nodesOf(r.id))
    {
        scene::Node* n = g.node(id);
        std::string rel = stripNs(r.ns, n->name());
        auto bit = r.baseline.find(rel);
        if (bit == r.baseline.end())
            continue;
        const Reference::Baseline& base = bit->second;
        json changed = json::object();
        const auto& defs = n->type().attrs;
        for (int i = 0; i < int(defs.size()); ++i)
        {
            if (!isStoredInput(g, n, i))
                continue;
            const scene::PlugValue& now = n->value(i);
            auto was = base.attrs.find(i);
            // Shared values (meshes) compare by pointer first; only a replaced one is compared by content.
            const bool shared = std::holds_alternative<scene::MeshPtr>(now) || std::holds_alternative<scene::DataPtr>(now);
            if (was != base.attrs.end() && (was->second == now || (shared && scene::toJson(was->second) == scene::toJson(now))))
                continue;
            const scene::AttrDef& def = defs[size_t(i)];
            // Vectors are edited per component, so upstream changes to the other components still come through.
            if (def.type == scene::PlugType::Vec3 && was != base.attrs.end() && std::holds_alternative<glm::dvec3>(was->second) &&
                std::holds_alternative<glm::dvec3>(now))
            {
                const glm::dvec3 a = std::get<glm::dvec3>(was->second), v = std::get<glm::dvec3>(now);
                for (int c = 0; c < 3; ++c)
                    if (a[c] != v[c])
                        changed[def.childNames[c]] = v[c];
            }
            else if ((def.flags & scene::AttrFile) && std::holds_alternative<std::string>(now))
                changed[def.name] = paths::storedPath(paths::fromUtf8(std::get<std::string>(now)), sceneDir);
            else
                changed[def.name] = scene::toJson(now);
        }
        json lock = json::array(), unlock = json::array();
        for (int i = 0; i < int(defs.size()); ++i)
        {
            auto was = base.locks.find(i);
            uint8_t before = was == base.locks.end() ? uint8_t(0) : was->second, now = n->lockMask(i);
            appendLockNames(defs[size_t(i)], uint8_t(now & ~before), lock);
            appendLockNames(defs[size_t(i)], uint8_t(before & ~now), unlock);
        }
        json edit = {{"node", rel}};
        if (!changed.empty())
            edit["attrs"] = changed;
        if (!lock.empty())
            edit["lock"] = std::move(lock);
        if (!unlock.empty())
            edit["unlock"] = std::move(unlock);
        NodeId p = g.parent(id);
        std::optional<std::string> parent = p ? std::optional<std::string>(g.node(p)->name()) : std::nullopt;
        if (parent != base.parent)
            edit["parent"] = parent ? json(*parent) : json(nullptr);
        if (edit.size() > 1)
            edits.push_back(std::move(edit));
    }
    // Connections the scene made onto referenced nodes are saved by the scene itself, not here.
    return edits;
}

void applyEdits(const Reference& r, const json& edits, std::vector<std::string>& warnings)
{
    Graph& g = Graph::current();
    for (const json& e : edits)
    {
        std::string name = prefixed(r.ns, e.value("node", ""));
        scene::Node* n = g.find(name);
        if (!n)
        {
            warnings.push_back("reference edit for " + name + " skipped: the node is no longer in " + r.path.filename().string());
            continue;
        }
        auto setLocks = [&](const char* key, bool locked) {
            for (const json& plug : member(e, key, kNoArray))
                try
                {
                    g.setLocked(g.plug(name + "." + plug.get<std::string>()), locked);
                }
                catch (const std::exception& ex)
                {
                    warnings.push_back(name + ": can't " + key + " " + plug.dump() + ": " + ex.what());
                }
        };
        // Unlocks before the values they free up, locks after the values they would refuse.
        setLocks("unlock", false);
        json attrs = e.value("attrs", json::object());
        for (auto& [attr, value] : attrs.items())
        {
            try
            {
                scene::PlugRef plug = g.plug(name + "." + attr);
                scene::AttrDef def = g.attr(plug);
                if (plug.component >= 0)
                    def.type = scene::PlugType::Float;
                g.setValue(plug, scene::fromJson(def, value));
            }
            catch (const std::exception& ex)
            {
                warnings.push_back(name + "." + attr + ": " + ex.what());
            }
        }
        setLocks("lock", true);
        if (e.contains("parent"))
        {
            NodeId p = scene::kNoNode;
            if (!e["parent"].is_null())
            {
                if (scene::Node* pn = g.find(e["parent"].get<std::string>()))
                    p = pn->id();
                else
                {
                    // A scene node not created yet while the scene loads; resolvePendingParents finishes it.
                    gPendingParents.push_back({name, e["parent"].get<std::string>()});
                    continue;
                }
            }
            try
            {
                g.setParent(n->id(), p, false);
            }
            catch (const std::exception& ex)
            {
                warnings.push_back(name + ": " + ex.what());
            }
        }
    }
}

void unload(Reference& r)
{
    if (!r.loaded)
        return;
    r.edits = currentEdits(r);
    r.children = sceneChildren(r);
    detachChildren(r.children);
    Graph& g = Graph::current();
    for (NodeId id : nodesOf(r.id))
        if (g.node(id) && g.parent(id) == scene::kNoNode)
            g.deleteNode(id);
    for (NodeId id : nodesOf(r.id))
        g.deleteNode(id);
    r.loaded = false;
}

void reload(Reference& r, std::vector<std::string>& warnings)
{
    unload(r);
    load(r, warnings);
}

void remove(const std::string& ns)
{
    Reference* r = find(ns);
    if (!r)
        return;
    json children = sceneChildren(*r);
    detachChildren(children);
    Graph& g = Graph::current();
    for (NodeId id : nodesOf(r->id))
        if (g.node(id) && g.parent(id) == scene::kNoNode)
            g.deleteNode(id);
    for (NodeId id : nodesOf(r->id))
        g.deleteNode(id);
    gRefs.erase(gRefs.begin() + (r - gRefs.data()));
}

void importNodes(const std::string& ns)
{
    Reference* r = find(ns);
    if (!r)
        return;
    Graph& g = Graph::current();
    for (NodeId id : nodesOf(r->id))
        g.setReference(id, 0);
    gRefs.erase(gRefs.begin() + (r - gRefs.data()));
}

json serialize(const fs::path& sceneDir)
{
    json out = json::array();
    for (const Reference& r : gRefs)
    {
        json rec = {{"namespace", r.ns}, {"path", paths::storedPath(r.path, sceneDir)}, {"loaded", r.loaded}};
        json edits = currentEdits(r, sceneDir);
        if (!edits.empty())
            rec["edits"] = std::move(edits);
        json children = r.loaded ? json::array() : r.children;
        if (!children.empty())
            rec["children"] = std::move(children);
        out.push_back(std::move(rec));
    }
    return out;
}

double loadedBytes(const json& records, const fs::path& sceneDir)
{
    double total = 0.0;
    for (const json& rec : records)
        if (rec.value("loaded", true))
        {
            std::error_code ec;
            const auto size = fs::file_size(resolve(rec.value("path", ""), sceneDir), ec);
            total += ec ? 0.0 : double(size);
        }
    return total;
}

void deserialize(const json& records, const fs::path& sceneDir, std::vector<std::string>& warnings, double span)
{
    clear();
    progress::Scope scope("References", loadedBytes(records, sceneDir), progress::None, span);
    for (const json& rec : records)
    {
        Reference r;
        r.id = gNextId++;
        r.path = resolve(rec.value("path", ""), sceneDir);
        r.ns = rec.value("namespace", r.path.stem().string());
        r.edits = rec.value("edits", json::array());
        r.children = rec.value("children", json::array());
        r.loaded = false;
        gRefs.push_back(r);
        if (!rec.value("loaded", true))
            continue;
        std::error_code ec;
        const auto size = fs::file_size(r.path, ec);
        progress::Scope one("", 0.0, progress::None, ec ? 0.0 : double(size));
        try
        {
            load(gRefs.back(), warnings);
        }
        catch (const std::exception& e)
        {
            warnings.push_back("reference " + r.ns + " (" + r.path.string() + ") not loaded: " + e.what());
        }
    }
}

void resolvePendingParents(std::vector<std::string>& warnings)
{
    Graph& g = Graph::current();
    for (const auto& [child, parent] : gPendingParents)
    {
        scene::Node* c = g.find(child);
        scene::Node* p = g.find(parent);
        if (!c || !p)
        {
            warnings.push_back("reference edit for " + child + " skipped: its parent " + parent + " is gone");
            continue;
        }
        try
        {
            g.setParent(c->id(), p->id(), false);
        }
        catch (const std::exception& ex)
        {
            warnings.push_back(child + ": " + ex.what());
        }
    }
    gPendingParents.clear();
}

bool isBaselineConnection(const std::string& src, const std::string& dst)
{
    for (const Reference& r : gRefs)
        if (r.baselineConnections.contains({src, dst}))
            return true;
    return false;
}

void registerCommands()
{
    using T = ArgType;
    auto reportWarnings = [](const std::vector<std::string>& w) {
        for (const std::string& s : w)
            log::warn("{}", s);
    };
    auto need = [](const Args& a, const char* cmd) -> Reference& {
        if (a.positional.empty())
            throw CommandError(std::string(cmd) + ": namespace required");
        Reference* r = find(a.positional[0].get<std::string>());
        if (!r)
            throw CommandError(std::string(cmd) + ": no reference '" + a.positional[0].get<std::string>() + "'");
        return *r;
    };

    ops::registerCommand({
        .name = "createReference",
        .doc = "Reference a scene file (.lila, .lilaa) into this one under a namespace. Returns the namespace. Clears undo.",
        .positionalDoc = "path",
        .flags = {{"namespace", "", T::String, nullptr, "Namespace; defaults to the file name."}},
        .undoable = false,
        .run = [reportWarnings](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("createReference: path required");
            fs::path sceneDir = currentPath().empty() ? fs::path() : currentPath().parent_path();
            fs::path p = resolve(a.positional[0].get<std::string>(), sceneDir);
            if (!fs::exists(p))
                throw CommandError("createReference: no file " + p.string());
            std::vector<std::string> warnings;
            NoUndo guard;
            try
            {
                Reference& r = create(p, a.get<std::string>("namespace"), warnings);
                reportWarnings(warnings);
                return r.ns;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("createReference: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "removeReference",
        .doc = "Remove a reference and its nodes. Clears undo.",
        .positionalDoc = "namespace",
        .undoable = false,
        .run = [need](const Args& a) -> json {
            Reference& r = need(a, "removeReference");
            NoUndo guard;
            remove(r.ns);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "reloadReference",
        .doc = "Re-read a reference's file, keeping local edits. Clears undo.",
        .positionalDoc = "namespace",
        .undoable = false,
        .run = [need, reportWarnings](const Args& a) -> json {
            Reference& r = need(a, "reloadReference");
            std::vector<std::string> warnings;
            NoUndo guard;
            reload(r, warnings);
            reportWarnings(warnings);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "unloadReference",
        .doc = "Take a reference's nodes out of the scene but remember it and its edits. Clears undo.",
        .positionalDoc = "namespace",
        .undoable = false,
        .run = [need](const Args& a) -> json {
            Reference& r = need(a, "unloadReference");
            NoUndo guard;
            unload(r);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "loadReference",
        .doc = "Bring an unloaded reference back. Clears undo.",
        .positionalDoc = "namespace",
        .undoable = false,
        .run = [need, reportWarnings](const Args& a) -> json {
            Reference& r = need(a, "loadReference");
            if (r.loaded)
                return nullptr;
            std::vector<std::string> warnings;
            NoUndo guard;
            load(r, warnings);
            reportWarnings(warnings);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "importReference",
        .doc = "Turn a reference's nodes into ordinary scene nodes and drop the link to the file.",
        .positionalDoc = "namespace",
        .undoable = false,
        .run = [need](const Args& a) -> json {
            Reference& r = need(a, "importReference");
            NoUndo guard;
            importNodes(r.ns);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "references",
        .doc = "Every reference: namespace, path, loaded, and its local edits.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Reference& r : gRefs)
                out.push_back({{"namespace", r.ns}, {"path", r.path.generic_string()}, {"loaded", r.loaded}, {"edits", currentEdits(r)}});
            return out;
        },
    });
}
} // namespace refs
} // namespace lila::io
