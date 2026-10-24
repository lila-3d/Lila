#include "tex/Library.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/NodeDef.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace lila::tex::library
{
namespace
{
namespace fs = std::filesystem;

constexpr const char* kPrefs = "materialLibrary";
constexpr const char* kSceneTier = "scene";

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> stringList(const json& j)
{
    std::vector<std::string> out;
    if (j.is_array())
        for (const json& v : j)
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

json readJsonFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::invalid_argument("can't read " + paths::toUtf8(p));
    std::stringstream ss;
    ss << in.rdbuf();
    try
    {
        return json::parse(ss.str());
    }
    catch (const std::exception& e)
    {
        throw std::invalid_argument(paths::toUtf8(p) + " isn't JSON: " + e.what());
    }
}

Entry entryOf(const json& doc, std::string id, std::string tier, fs::path file)
{
    if (doc.value("format", "") != "lilamat" || !doc.contains("graph"))
        throw std::invalid_argument("not a .lilamat material");
    Entry e;
    e.id = std::move(id);
    e.tier = std::move(tier);
    e.file = std::move(file);
    e.meta = Meta::fromJson(doc.value("meta", json::object()));
    if (e.meta.name.empty())
        e.meta.name = doc.value("name", e.id.substr(e.id.rfind('/') + 1));
    return e;
}

using Tier = std::map<std::string, Entry, std::less<>>;

void scanDir(const fs::path& dir, const std::string& tier, const std::string& owner, Tier& into, std::vector<std::string>& errors)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".lilamat")
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        fs::path rel = fs::relative(f, dir, ec);
        rel.replace_extension();
        std::string id = rel.generic_string();
        try
        {
            Entry e = entryOf(readJsonFile(f), id, tier, f);
            e.owner = owner;
            into[id] = std::move(e);
        }
        catch (const std::exception& ex)
        {
            errors.push_back(paths::toUtf8(f) + ": " + ex.what());
            log::warn("material library: {}: {}", paths::toUtf8(f), ex.what());
        }
    }
}

struct State
{
    bool loaded = false;
    Tier builtin, packs, user, workspace;
    fs::path workspaceRoot;
    std::vector<std::string> extensions;
    std::vector<std::string> errors;
};

State& state()
{
    static State s;
    return s;
}

std::vector<std::string> loadedExtensions()
{
    try
    {
        return extensions::ExtensionManager::get().loaded();
    }
    catch (const std::exception&)
    {
        return {};
    }
}

State& loaded()
{
    State& s = state();
    fs::path ws = Workspace::get().isOpen() ? Workspace::get().root() : fs::path();
    std::vector<std::string> exts = loadedExtensions();
    if (s.loaded && ws == s.workspaceRoot && exts == s.extensions)
        return s;
    s = {};
    s.loaded = true;
    s.workspaceRoot = ws;
    s.extensions = exts;
    scanDir(paths::resourcesDir() / "materials", "builtin", {}, s.builtin, s.errors);
    for (const std::string& id : exts)
        if (ext::Context* ctx = extensions::contextOf(id))
            scanDir(ctx->resources() / "materials", "extension", id, s.packs, s.errors);
    scanDir(tierDir("user"), "user", {}, s.user, s.errors);
    scanDir(tierDir("workspace"), "workspace", {}, s.workspace, s.errors);
    return s;
}

/** The scene's texture sets as library entries. */
Tier sceneTier()
{
    Tier out;
    scene::Graph& g = scene::Graph::current();
    for (const std::string& name : setNames(g))
    {
        Entry e;
        e.id = std::string(kSceneTier) + "/" + name;
        e.tier = kSceneTier;
        e.meta.name = name;
        e.meta.category = "Scene";
        if (scene::Node* n = findSet(g, name))
        {
            try
            {
                e.meta.description = readGraph(*n).description;
            }
            catch (const std::exception&)
            {
            }
        }
        out[e.id] = std::move(e);
    }
    return out;
}

bool matchesCategory(const std::string& have, const std::string& want)
{
    if (want.empty())
        return true;
    std::string h = lower(have), w = lower(want);
    return h == w || (h.size() > w.size() && h.starts_with(w) && h[w.size()] == '/');
}

json prefPresets() { return PrefStore::get().value(kPrefs, "/presets", json::object()); }
} // namespace

json Meta::toJson() const
{
    json j = {{"name", name},       {"category", category}, {"tags", tags},     {"physical", physical}, {"scaleCm", scaleCm},
              {"tileable", tileable}, {"seedParam", seedParam}, {"variationParams", variationParams}};
    for (auto [k, v] : {std::pair{"author", &author}, {"licence", &licence}, {"version", &version}, {"description", &description}})
        if (!v->empty())
            j[k] = *v;
    return j;
}

Meta Meta::fromJson(const json& j)
{
    Meta m;
    if (!j.is_object())
        return m;
    auto str = [&](const char* k, std::string& into) {
        if (auto it = j.find(k); it != j.end() && it->is_string())
            into = it->get<std::string>();
    };
    str("name", m.name);
    str("category", m.category);
    str("physical", m.physical);
    str("seedParam", m.seedParam);
    str("author", m.author);
    str("licence", m.licence);
    if (m.licence.empty())
        str("license", m.licence);
    str("version", m.version);
    str("description", m.description);
    if (auto it = j.find("version"); it != j.end() && it->is_number())
        m.version = it->dump();
    m.tags = stringList(j.value("tags", json::array()));
    m.variationParams = stringList(j.value("variationParams", json::array()));
    if (auto it = j.find("scaleCm"); it != j.end() && it->is_number() && it->get<double>() > 0.0)
        m.scaleCm = it->get<double>();
    if (auto it = j.find("tileable"); it != j.end() && it->is_boolean())
        m.tileable = it->get<bool>();
    m.physical = lower(m.physical);
    if (m.physical.empty())
        m.physical = "dielectric";
    return m;
}

json Entry::toJson() const
{
    json j = meta.toJson();
    j["id"] = id;
    j["tier"] = tier;
    if (!owner.empty())
        j["owner"] = owner;
    if (!file.empty())
        j["file"] = paths::toUtf8(file);
    j["favourite"] = isFavourite(id);
    return j;
}

std::vector<Entry> all()
{
    State& s = loaded();
    std::map<std::string, const Entry*, std::less<>> byId;
    for (auto& [k, e] : s.builtin)
        byId[k] = &e;
    for (auto& [k, e] : s.packs)
        byId[k] = &e;
    for (const Entry* e : extensionMaterials().all())
        byId[e->id] = e;
    for (auto& [k, e] : s.user)
        byId[k] = &e;
    for (auto& [k, e] : s.workspace)
        byId[k] = &e;
    Tier sc = sceneTier();
    for (auto& [k, e] : sc)
        byId[k] = &e;
    std::vector<Entry> out;
    out.reserve(byId.size());
    for (auto& [k, e] : byId)
        out.push_back(*e);
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.meta.category != b.meta.category ? a.meta.category < b.meta.category : lower(a.meta.name) < lower(b.meta.name);
    });
    return out;
}

std::optional<Entry> find(std::string_view id)
{
    for (Entry& e : all())
        if (e.id == id)
            return std::move(e);
    return std::nullopt;
}

json document(const Entry& e)
{
    if (e.tier == kSceneTier)
    {
        scene::Node* n = findSet(scene::Graph::current(), e.id.substr(std::string(kSceneTier).size() + 1));
        if (!n)
            throw std::invalid_argument("no texture set for " + e.id);
        return {{"format", "lilamat"}, {"version", 1}, {"name", n->name()}, {"meta", e.meta.toJson()}, {"graph", readGraph(*n).toJson()},
                {"settings", readSettings(*n).toJson()}};
    }
    if (!e.document.is_null())
        return e.document;
    return readJsonFile(e.file);
}

Graph graphOf(const json& document) { return Graph::fromJson(document.at("graph")); }

SetSettings settingsOf(const json& document)
{
    return document.contains("settings") ? SetSettings::fromJson(document["settings"]) : SetSettings::makeDefault();
}

std::vector<Entry> search(const Query& q)
{
    std::vector<std::string> words;
    std::istringstream in(lower(q.text));
    for (std::string w; in >> w;)
        words.push_back(w);
    std::vector<Entry> out;
    for (Entry& e : all())
    {
        if (!matchesCategory(e.meta.category, q.category))
            continue;
        if (!q.physical.empty() && lower(q.physical) != e.meta.physical)
            continue;
        if (!q.tier.empty() && q.tier != e.tier)
            continue;
        if (!q.tag.empty() && std::none_of(e.meta.tags.begin(), e.meta.tags.end(), [&](const std::string& t) { return lower(t) == lower(q.tag); }))
            continue;
        if (q.favouritesOnly && !isFavourite(e.id))
            continue;
        std::string hay = lower(e.id + " " + e.meta.name + " " + e.meta.category + " " + e.meta.description + " " + e.meta.physical);
        for (const std::string& t : e.meta.tags)
            hay += " " + lower(t);
        if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return hay.find(w) != std::string::npos; }))
            out.push_back(std::move(e));
    }
    return out;
}

std::vector<std::string> categories()
{
    std::set<std::string> out;
    for (const Entry& e : all())
    {
        // Every parent of "Metal/Steel/Painted" is a category too, so the browser's tree has its branches.
        const std::string& c = e.meta.category;
        for (size_t p = c.find('/'); p != std::string::npos; p = c.find('/', p + 1))
            out.insert(c.substr(0, p));
        if (!c.empty())
            out.insert(c);
    }
    return {out.begin(), out.end()};
}

std::vector<std::string> tags()
{
    std::set<std::string> out;
    for (const Entry& e : all())
        for (const std::string& t : e.meta.tags)
            out.insert(lower(t));
    return {out.begin(), out.end()};
}

void reload() { state().loaded = false; }

fs::path tierDir(std::string_view tier)
{
    if (tier == "builtin")
        return paths::resourcesDir() / "materials";
    if (tier == "user")
        return PrefStore::get().userDir() / "materials";
    if (tier == "workspace")
        return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / "materials" : fs::path();
    return {};
}

std::vector<std::string> errors() { return loaded().errors; }

Registry<Entry>& extensionMaterials()
{
    static Registry<Entry> r;
    return r;
}

uint64_t registerExtensionMaterial(ext::Context& ctx, Entry e)
{
    e.tier = "extension";
    e.owner = ctx.owner();
    if (e.document.is_null() && !e.file.empty())
        e.document = readJsonFile(e.file);
    std::string id = e.id;
    extensionMaterials().put(id, std::move(e), ctx.owner());
    std::string own = ctx.owner();
    return ctx.track("material:" + id, [id, own] {
        if (const std::string* o = extensionMaterials().ownerOf(id); o && *o == own)
            extensionMaterials().remove(id);
    });
}

size_t registerExtensionDir(ext::Context& ctx, const fs::path& dir)
{
    Tier t;
    std::vector<std::string> errs;
    scanDir(dir, "extension", ctx.owner(), t, errs);
    for (const std::string& e : errs)
        ctx.log(LogLevel::Warning, e);
    for (auto& [id, e] : t)
        registerExtensionMaterial(ctx, std::move(e));
    return t.size();
}

bool isFavourite(std::string_view id)
{
    for (const std::string& f : favourites())
        if (f == id)
            return true;
    return false;
}

void setFavourite(const std::string& id, bool on)
{
    std::vector<std::string> f = favourites();
    std::erase(f, id);
    if (on)
        f.push_back(id);
    PrefStore::get().set(kPrefs, "/favourites", f);
}

std::vector<std::string> favourites() { return stringList(PrefStore::get().value(kPrefs, "/favourites", json::array())); }

json presets(const std::string& id)
{
    json out = json::object();
    if (std::optional<Entry> e = find(id))
    {
        try
        {
            json doc = document(*e);
            if (doc.contains("presets") && doc["presets"].is_object())
                out = doc["presets"];
        }
        catch (const std::exception&)
        {
        }
    }
    json mine = prefPresets();
    if (mine.contains(id) && mine[id].is_object())
        for (auto& [k, v] : mine[id].items())
            out[k] = v;
    return out;
}

void savePreset(const std::string& id, const std::string& name, const json& values)
{
    json all = prefPresets();
    if (!all.is_object())
        all = json::object();
    all[id][name] = values;
    PrefStore::get().set(kPrefs, "/presets", all);
}

bool deletePreset(const std::string& id, const std::string& name)
{
    json all = prefPresets();
    if (!all.contains(id) || !all[id].contains(name))
        return false;
    all[id].erase(name);
    if (all[id].empty())
        all.erase(id);
    PrefStore::get().set(kPrefs, "/presets", all);
    return true;
}

namespace
{
/** A top-level param input of the graph by name, or null. */
GraphInput* paramInput(Graph& g, std::string_view name)
{
    for (GraphInput& in : g.inputs)
        if (in.param && in.name == name)
            return &in;
    return nullptr;
}

/** Sets "Node.param" on a top-level node; false when there is no such node. */
bool setNodeValue(Graph& g, const std::string& key, const json& v)
{
    auto dot = key.find('.');
    GNode* n = dot == std::string::npos ? nullptr : g.node(key.substr(0, dot));
    if (!n)
        return false;
    n->values[key.substr(dot + 1)] = v;
    return true;
}

json nodeValue(const Graph& g, const std::string& key)
{
    auto dot = key.find('.');
    const GNode* n = dot == std::string::npos ? nullptr : g.node(key.substr(0, dot));
    if (!n)
        return nullptr;
    std::string id = key.substr(dot + 1);
    if (auto it = n->values.find(id); it != n->values.end())
        return *it;
    if (const NodeDef* d = findNode(n->type))
    {
        if (const ParamDecl* p = d->param(id))
            return p->defaultValue;
        if (const PortDecl* in = d->input(id))
            return in->defaultValue;
    }
    return nullptr;
}
} // namespace

std::vector<std::string> applyValues(Graph& g, const json& values)
{
    std::vector<std::string> missed;
    if (!values.is_object())
        return missed;
    for (auto& [key, v] : values.items())
    {
        // An exposed parameter: its default and every value it targets, so the graph reads it whether used as a node or not.
        if (GraphInput* in = paramInput(g, key))
        {
            in->defaultValue = v;
            for (const std::string& t : in->targets)
                setNodeValue(g, t, v);
            continue;
        }
        if (!setNodeValue(g, key, v))
            missed.push_back(key);
    }
    return missed;
}

json captureValues(const Graph& g, const Meta& m)
{
    json out = json::object();
    std::vector<std::string> keys = m.variationParams;
    if (!m.seedParam.empty())
        keys.insert(keys.begin(), m.seedParam);
    for (const std::string& key : keys)
    {
        if (const GraphInput* in = paramInput(const_cast<Graph&>(g), key))
        {
            json v = in->targets.empty() ? json() : nodeValue(g, in->targets.front());
            out[key] = v.is_null() ? in->defaultValue : v;
            continue;
        }
        if (json v = nodeValue(g, key); !v.is_null())
            out[key] = v;
    }
    return out;
}

double tilesFor(double scaleCm, double uvPerCm)
{
    if (scaleCm <= 0.0 || uvPerCm <= 0.0)
        return 1.0;
    return 1.0 / (uvPerCm * scaleCm);
}

void setTiling(Graph& g, double tilesX, double tilesY)
{
    if (GNode* t = g.node(kTilingNode))
    {
        t->values["tilesX"] = tilesX;
        t->values["tilesY"] = tilesY;
        return;
    }
    const GNode* outNode = g.firstOfType(kMaterialOutputType);
    if (!outNode)
        throw std::invalid_argument("the graph has no Material Output");
    const std::string out = outNode->name;
    const glm::dvec2 at = outNode->position;
    std::string srcNode, srcSocket;
    if (std::vector<GLink> m = g.linksInto(out, "material"); !m.empty())
    {
        srcNode = m.front().fromNode;
        srcSocket = m.front().fromSocket;
        std::erase_if(g.links, [&](const GLink& l) { return l.toNode == out && l.toSocket == "material"; });
    }
    else
    {
        // Direct channel links and values move onto a Material node, so one tiling node covers every channel.
        GNode bundle{g.uniqueNodeName("LibraryChannels"), "material", at + glm::dvec2(-420.0, 0.0), json::object()};
        GNode* o = g.node(out);
        for (const auto& [ch, type] : standardChannels())
        {
            if (auto it = o->values.find(ch); it != o->values.end())
                bundle.values[ch] = *it;
            for (GLink& l : g.links)
                if (l.toNode == out && l.toSocket == ch)
                    l.toNode = bundle.name;
        }
        srcNode = bundle.name;
        srcSocket = "material";
        g.nodes.push_back(std::move(bundle));
    }
    g.nodes.push_back({kTilingNode, kTilingType, at + glm::dvec2(-200.0, 0.0), json{{"tilesX", tilesX}, {"tilesY", tilesY}}});
    g.links.push_back({srcNode, srcSocket, kTilingNode, "input"});
    g.links.push_back({kTilingNode, "material", out, "material"});
}
} // namespace lila::tex::library
