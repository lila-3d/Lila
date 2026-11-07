#include "bake/BakePipeline.h"

#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeCache.h"
#include "bake/BakeCommon.h"
#include "bake/BakeNodes.h"
#include "bake/Maps.h"
#include "bake/Naming.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Mesh.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshData.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "vcs/SourceControl.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace lila::bake
{
namespace fs = std::filesystem;
using T = ArgType;

namespace
{
using detail::findNode;
using detail::projectArg;
using detail::shortName;
using scene::Graph;
using scene::Node;
using scene::NodeId;

/** 64-bit FNV-1a: stable across runs and builds, which a hash stored in a manifest needs. */
struct Hasher
{
    uint64_t h = 0xcbf29ce484222325ull;

    void bytes(const void* data, size_t n)
    {
        // Mesh payloads run to hundreds of MB: those go through the word-at-a-time parallel hash.
        if (n >= 4096)
        {
            h = hashBytes(data, n, h);
            return;
        }
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 0x100000001b3ull;
    }
    void str(std::string_view s)
    {
        u64(s.size());
        bytes(s.data(), s.size());
    }
    void u64(uint64_t v) { bytes(&v, sizeof v); }
    void json(const nlohmann::json& j)
    {
        u64(uint64_t(j.type()));
        switch (j.type())
        {
        case nlohmann::json::value_t::object:
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                str(it.key());
                json(it.value());
            }
            break;
        case nlohmann::json::value_t::array:
            u64(j.size());
            for (const nlohmann::json& v : j)
                json(v);
            break;
        case nlohmann::json::value_t::binary:
            u64(j.get_binary().subtype());
            u64(j.get_binary().size());
            bytes(j.get_binary().data(), j.get_binary().size());
            break;
        case nlohmann::json::value_t::string: str(j.get_ref<const std::string&>()); break;
        default: str(j.dump()); break;
        }
    }
};

/** Mesh digests by mesh pointer: the DG hands back the same mesh until it changes, so repeat saves hash nothing. */
uint64_t meshDigest(const scene::MeshPtr& mesh)
{
    static std::mutex mutex;
    static std::map<const geo::Mesh*, std::pair<std::weak_ptr<const geo::Mesh>, uint64_t>> cache;
    if (!mesh)
        return 0;
    {
        std::scoped_lock lock(mutex);
        auto it = cache.find(mesh.get());
        if (it != cache.end() && !it->second.first.expired())
            return it->second.second;
    }
    Hasher hs;
    hs.json(scene::meshToJsonPacked(*mesh));
    std::scoped_lock lock(mutex);
    std::erase_if(cache, [](const auto& e) { return e.second.first.expired(); });
    cache[mesh.get()] = {mesh, hs.h};
    return hs.h;
}

void hashMeshNode(Hasher& hs, Graph& g, const std::string& name)
{
    hs.str(name);
    Node* n = findNode(g, name);
    if (!n || !n->type().isA("mesh"))
        return hs.u64(0);
    scene::MeshPtr m = scene::modifiers::exportMesh(g, n->id());
    if (!m)
        m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
    hs.u64(meshDigest(m));
    const scene::PlugValue w = g.value({n->id(), scene::Transform::kWorldMatrix, -1});
    if (std::holds_alternative<glm::dmat4>(w))
        hs.bytes(&std::get<glm::dmat4>(w), sizeof(glm::dmat4));
}

/** The last result of a project: this session's, else its manifest on disk. */
json lastOrManifest(const Node& project)
{
    json r = lastResult(project.id());
    if (r.is_object() && r.contains("outputs"))
        return r;
    return readManifest(project);
}

/** The scene file's changelist when it is opened in a numbered one, else empty (the default). */
std::string sceneChangelist(const std::shared_ptr<vcs::Provider>& provider)
{
    const fs::path& scene = io::currentPath();
    if (scene.empty() || !provider || !provider->supportsChangelists())
        return {};
    try
    {
        const std::vector<vcs::FileStatus> st = provider->status({scene});
        if (!st.empty() && st[0].checkedOutByMe && st[0].changelist != "default")
            return st[0].changelist;
    }
    catch (const std::exception& e)
    {
        log::warn("Bake: can't read the scene's changelist: {}", e.what());
    }
    return {};
}

bool readOnly(const fs::path& f)
{
    std::error_code ec;
    const fs::file_status st = fs::status(f, ec);
    return !ec && fs::exists(st) && (st.permissions() & fs::perms::owner_write) == fs::perms::none;
}
} // namespace

std::string inputHash(Graph& g, NodeId project)
{
    LILA_ZONE();

    Node* p = g.node(project);
    if (!p || !isProject(*p))
        return {};
    Hasher hs;
    json ps = projectSettings(*p).toJson();
    for (const char* k : {"rebakeOnSave", "autoAssign", "target"})
        ps.erase(k);
    hs.str(p->name());
    hs.json(ps);
    for (NodeId gid : groupsOf(g, project))
    {
        Node* gn = g.node(gid);
        if (!gn)
            continue;
        const GroupSettings s = groupSettings(*gn);
        hs.str(gn->name());
        hs.json(s.toJson());
        for (const std::string& l : s.lows)
            hashMeshNode(hs, g, l);
        for (const HighEntry& h : s.highs)
            if (!h.self)
                hashMeshNode(hs, g, h.node);
        if (!s.cage.empty())
            hashMeshNode(hs, g, s.cage);
    }
    return std::format("{:016x}", hs.h);
}

json readManifest(const Node& project)
{
    const fs::path file = resolveFolder(projectSettings(project).output.folder) / paths::fromUtf8(fileSafe(project.name()) + "_bake.json");
    std::ifstream f(file);
    if (!f)
        return nullptr;
    json j = json::parse(f, nullptr, false);
    return j.is_discarded() ? json() : j;
}

bool needsBake(Graph& g, NodeId project, std::string* reason)
{
    auto because = [&](std::string why) {
        if (reason)
            *reason = std::move(why);
        return true;
    };
    Node* p = g.node(project);
    if (!p || !isProject(*p))
        return because("not a bake project");
    const json m = readManifest(*p);
    if (!m.is_object())
        return because("never baked");
    if (m.value("inputHash", std::string()) != inputHash(g, project))
        return because("inputs changed");
    for (const json& o : m.value("outputs", json::array()))
        if (!fs::exists(paths::fromUtf8(o.value("file", std::string()))))
            return because("missing " + o.value("file", std::string()));
    if (reason)
        *reason = "up to date";
    return false;
}

std::string preferredBackend()
{
    for (const std::string& b : backendNames())
        if (b != "cpu")
            return b;
    return "cpu";
}

std::string materialChannel(std::string_view map)
{
    static const std::map<std::string, std::string, std::less<>> table = {
        {"normal", "normal"},       {"ao", "ambientOcclusion"},  {"occlusion", "ambientOcclusion"}, {"orm", "orm"},
        {"mro", "orm"},             {"baseColor", "baseColor"},  {"albedo", "baseColor"},           {"diffuse", "baseColor"},
        {"roughness", "roughness"}, {"metallic", "metallic"},    {"metalness", "metallic"},         {"emissive", "emissive"},
        {"height", "height"},       {"detailNormal", "normal"},
    };
    auto it = table.find(map);
    return it == table.end() ? std::string() : it->second;
}

std::string mapFromFileName(std::string_view fileName)
{
    std::string stem(paths::fromUtf8(std::string(fileName)).stem().string());
    // A UDIM or frame number after the map token, as in T_Rock_N.1001.
    if (const size_t dot = stem.find('.'); dot != std::string::npos)
        stem.resize(dot);
    const size_t cut = stem.find_last_of("_- ");
    const std::string token = cut == std::string::npos ? stem : stem.substr(cut + 1);
    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    };
    for (const MapDef* d : allMaps())
        if (!materialChannel(d->id).empty() && (lower(d->id) == lower(token) || (!d->suffix.empty() && lower(d->suffix) == lower(token))))
            return d->id;
    return scene::materials::guessTextureChannel(std::string(fileName));
}

namespace
{
/** Turns on a material's Normals From Height with the depth and UV size the bake measured. */
void useHeightNormals(Graph& g, Node& m, const json& height, const json& detail)
{
    namespace mat = scene::material;
    auto set = [&](int attr, double v) {
        if (m.as<double>(attr) != v)
            g.setValue({m.id(), attr, -1}, v);
    };
    if (!m.as<bool>(mat::kNormalFromHeight))
        g.setValue({m.id(), mat::kNormalFromHeight, -1}, true);
    json hm = height.value("meta", json()), dm = detail.value("meta", json());
    if (!hm.is_object())
        hm = json::object();
    if (!dm.is_object())
        dm = json::object();
    if (const double range = hm.value("heightRange", 0.0); range > 0.0)
        set(mat::kHeightDepth, 2.0 * range);
    if (const json size = dm.value("uvSize", json()); size.is_array() && size.size() == 2)
    {
        set(mat::kHeightUvWidth, size[0].get<double>());
        set(mat::kHeightUvHeight, size[1].get<double>());
    }
}
} // namespace

json assignOutputs(Graph& g, const json& result, bool create, const json& channels)
{
    LILA_ZONE();

    json assigned = json::array(), created = json::array(), skipped = json::array();
    // One file per material and channel: the lowest UDIM of a tiled set.
    struct Pick
    {
        json output;
        std::string material;
        std::string channel;
    };
    std::map<std::string, Pick> picks;
    std::vector<std::string> order;
    const json outputs = result.value("outputs", json::array());
    // A detail normal takes the normal input: the material rebuilds the rest from the height.
    bool detail = false;
    for (const json& o : outputs)
        detail |= o.value("map", std::string()) == "detailNormal" && o.value("layer", std::string()).empty();
    auto heightFor = [&](const std::string& material) -> const json* {
        for (const json& o : outputs)
            if (o.value("map", std::string()) == "height" && o.value("layer", std::string()).empty())
                for (const json& mat : o.value("materials", json::array()))
                    if (mat.is_string() && mat.get<std::string>() == material)
                        return &o;
        return nullptr;
    };
    for (const json& o : outputs)
    {
        const std::string map = o.value("map", std::string()), file = o.value("file", std::string());
        if (map == "normal" && detail && !(channels.is_object() && channels.contains(map)))
        {
            skipped.push_back({{"file", file}, {"map", map}, {"reason", "the detail normal takes the normal input"}});
            continue;
        }
        if (!o.value("layer", std::string()).empty())
        {
            skipped.push_back({{"file", file}, {"map", map}, {"reason", "a layer of a PSD"}});
            continue;
        }
        const std::string channel = channels.is_object() && channels.contains(map) && channels[map].is_string()
                                        ? channels[map].get<std::string>()
                                        : materialChannel(map);
        if (channel.empty())
        {
            skipped.push_back({{"file", file}, {"map", map}, {"reason", "no material input takes it"}});
            continue;
        }
        for (const json& mat : o.value("materials", json::array()))
        {
            const std::string key = mat.get<std::string>() + "|" + channel;
            auto it = picks.find(key);
            if (it == picks.end())
            {
                picks[key] = {o, mat.get<std::string>(), channel};
                order.push_back(key);
            }
            else if (o.value("udim", 1001) < it->second.output.value("udim", 1001))
                it->second.output = o;
        }
    }

    for (const std::string& key : order)
    {
        const Pick& pk = picks[key];
        const std::string file = pk.output.value("file", std::string()), map = pk.output.value("map", std::string());
        Node* m = scene::materials::find(g, pk.material);
        if (!m && create)
        {
            const json meshes = pk.output.value("meshes", json::array());
            bool realSlot = false;
            for (const json& mn : meshes)
                if (Node* n = findNode(g, mn.get<std::string>()); n && n->type().isA("mesh") && pk.material != scene::materials::kDefaultSlot)
                {
                    const scene::MeshPtr mesh = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
                    realSlot |= mesh && std::ranges::find(mesh->materialNames(), pk.material) != mesh->materialNames().end();
                }
            if (realSlot)
            {
                m = scene::materials::create(g, pk.material);
                created.push_back(m->name());
            }
            else if (meshes.size() == 1)
            {
                // A mesh without materials: give it one of its own, on every face.
                const std::string mesh = meshes[0].get<std::string>();
                const std::string name = "M_" + fileSafe(shortName(mesh));
                m = scene::materials::find(g, name);
                if (!m)
                {
                    m = scene::materials::create(g, name);
                    created.push_back(m->name());
                }
                ops::execute("assignMaterial", {{"args", {m->name(), mesh}}});
            }
        }
        if (!m)
        {
            skipped.push_back({{"file", file}, {"map", map}, {"reason", "no material " + pk.material}});
            continue;
        }
        int attr = -1;
        for (const auto& [name, a] : scene::materials::textureChannels())
            if (name == pk.channel)
                attr = a;
        if (attr < 0)
        {
            skipped.push_back({{"file", file}, {"map", map}, {"reason", "unknown channel " + pk.channel}});
            continue;
        }
        std::string stored = paths::storedPath(paths::fromUtf8(file));
        std::replace(stored.begin(), stored.end(), '\\', '/');
        if (!std::holds_alternative<std::string>(m->value(attr)) || std::get<std::string>(m->value(attr)) != stored)
            g.setValue({m->id(), attr, -1}, stored);
        if (pk.channel == "normal")
        {
            const auto fmt = pk.output.value("normalFormat", std::string("directx")) == "opengl" ? scene::material::NormalFormat::OpenGL
                                                                                               : scene::material::NormalFormat::DirectX;
            if (m->as<int64_t>(scene::material::kNormalFormat) != int64_t(fmt))
                g.setValue({m->id(), scene::material::kNormalFormat, -1}, int64_t(fmt));
        }
        if (pk.channel == "orm")
        {
            const auto layout = map == "mro" ? scene::material::PackedLayout::MRO : scene::material::PackedLayout::ORM;
            if (m->as<int64_t>(scene::material::kPackedLayout) != int64_t(layout))
                g.setValue({m->id(), scene::material::kPackedLayout, -1}, int64_t(layout));
        }
        if (map == "detailNormal")
            if (const json* height = heightFor(pk.material))
                useHeightNormals(g, *m, *height, pk.output);
        assigned.push_back({{"material", m->name()}, {"channel", pk.channel}, {"file", file}, {"map", map}});
    }
    return {{"assigned", assigned}, {"created", created}, {"skipped", skipped}};
}

void prepareOutputs(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    std::vector<fs::path> locked;
    for (const fs::path& f : files)
        if (readOnly(f))
            locked.push_back(f);
    if (locked.empty())
        return;
    vcs::SourceControl& sc = vcs::SourceControl::get();
    if (sc.settings().value("autoCheckout", std::string("always")) == "never")
        return;
    for (auto& [provider, group] : sc.group(locked))
    {
        if (!provider)
            continue;
        try
        {
            provider->checkout(group, sceneChangelist(provider));
        }
        catch (const std::exception& e)
        {
            log::warn("Bake: checking out {} file(s) failed: {}", group.size(), e.what());
        }
    }
    for (const fs::path& f : locked)
        if (readOnly(f))
            log::warn("Bake: {} is read only and source control didn't open it", paths::toUtf8(f));
}

void addOutputs(const std::vector<fs::path>& files)
{
    // An untitled scene has no changelist to join, and is no reason to put textures under source control.
    if (files.empty() || io::currentPath().empty() || !vcs::SourceControl::get().settings().value("autoAdd", true))
        return;
    vcs::SourceControl::get().async([files] {
        vcs::SourceControl& sc = vcs::SourceControl::get();
        json added = json::array();
        for (auto& [provider, group] : sc.group(files))
        {
            if (!provider)
                continue;
            std::vector<fs::path> fresh;
            const std::vector<vcs::FileStatus> st = provider->status(group);
            for (size_t i = 0; i < st.size() && i < group.size(); ++i)
                if (st[i].state == vcs::FileState::NotTracked)
                    fresh.push_back(group[i]);
            if (fresh.empty())
                continue;
            provider->add(fresh, sceneChangelist(provider));
            for (const fs::path& f : fresh)
                added.push_back(paths::toUtf8(f));
        }
        return added;
    }, [](const vcs::AsyncResult& r) {
        if (!r.ok)
            log::warn("Bake: adding the textures to source control failed: {}", r.error);
        else if (!r.value.empty())
            log::info("Bake: opened {} new texture(s) for add", r.value.size());
    });
}

namespace
{
/** Rebakes on save in the app: one at a time off the main thread, the rest queued. Main thread only. */
struct Rebakes
{
    bool async = false;
    bool shutdown = false;
    int listener = 0;
    /** Project paths waiting their turn, in save order. */
    std::vector<std::string> pending;
    NodeId running = scene::kNoNode;
    std::string runningPath;
    /** What the running rebake snapshot, so a save that changed nothing leaves it be. */
    std::string runningHash;
    /** Cancelled by a save to start over; any other cancel (bakeCancel) drops the queue too. */
    bool restarting = false;
};

Rebakes& rebakes()
{
    static Rebakes r;
    return r;
}

void startNextRebake()
{
    LILA_ZONE();

    Rebakes& r = rebakes();
    Graph& g = Graph::current();
    while (!r.shutdown && r.running == scene::kNoNode && !bakeRunning() && !r.pending.empty())
    {
        const std::string path = r.pending.front();
        r.pending.erase(r.pending.begin());
        Node* p = findNode(g, path);
        std::string reason;
        if (!p || !isProject(*p) || !projectSettings(*p).rebakeOnSave || !needsBake(g, p->id(), &reason))
            continue;
        try
        {
            log::info("Rebaking {} on save: {}", p->name(), reason);
            r.running = p->id();
            r.runningPath = path;
            r.runningHash = inputHash(g, p->id());
            bakeAsync(g, BakeRequest{.project = p->id(), .backend = preferredBackend()});
        }
        catch (const std::exception& e)
        {
            r.running = scene::kNoNode;
            log::error("Rebake of {} on save failed: {}", p->name(), e.what());
        }
    }
}

void onRebakeEvent(const BakeEvent& e)
{
    Rebakes& r = rebakes();
    if (e.kind != BakeEvent::Kind::Finished || r.shutdown)
        return;
    if (e.project == r.running)
    {
        r.running = scene::kNoNode;
        r.runningPath.clear();
        r.runningHash.clear();
        const bool restart = std::exchange(r.restarting, false);
        if (e.data.value("cancelled", false) && !restart)
            r.pending.clear();
    }
    // Any bake ending frees the backend for the next queued rebake.
    startNextRebake();
}

json rebakeOnSaveAsync(Graph& g)
{
    Rebakes& r = rebakes();
    if (r.shutdown)
        return json::array();
    if (!r.listener)
        r.listener = addBakeListener(onRebakeEvent);
    json out = json::array();
    std::vector<std::pair<size_t, NodeId>> queued;
    for (NodeId id : projects(g))
    {
        Node* p = g.node(id);
        if (!p || !projectSettings(*p).rebakeOnSave)
            continue;
        std::string reason;
        if (!needsBake(g, id, &reason))
        {
            out.push_back({{"project", p->name()}, {"started", false}, {"reason", reason}});
            continue;
        }
        if (id == r.running)
        {
            if (inputHash(g, id) == r.runningHash)
            {
                out.push_back({{"project", p->name()}, {"started", false}, {"reason", "already rebaking"}});
                continue;
            }
            // Its snapshot is stale: stop it, and the queue starts it again once it has.
            r.restarting = true;
            cancelAsyncBake(false);
            reason += ", restarting";
        }
        if (std::ranges::find(r.pending, p->path()) == r.pending.end())
            r.pending.push_back(p->path());
        queued.emplace_back(out.size(), id);
        out.push_back({{"project", p->name()}, {"started", false}, {"reason", reason}});
    }
    startNextRebake();
    for (const auto& [i, id] : queued)
    {
        const bool now = id == r.running && !r.restarting;
        out[i]["started"] = now;
        out[i]["queued"] = !now;
    }
    return out;
}
/** What the last auto-assign returned per project, so bakeAll can report it. */
std::map<NodeId, json>& autoAssigns()
{
    static std::map<NodeId, json> a;
    return a;
}

/** autoAssign: every successful bake of a project with it on runs bakeAssign, on the main thread, as its own command. */
void onAutoAssignEvent(const BakeEvent& e)
{
    if (e.kind != BakeEvent::Kind::Finished || e.data.contains("error") || e.data.value("dryRun", false))
        return;
    Node* p = Graph::current().node(e.project);
    if (!p || !isProject(*p) || !projectSettings(*p).autoAssign)
        return;
    try
    {
        autoAssigns()[e.project] = ops::execute("bakeAssign", {{"project", p->path()}});
    }
    catch (const std::exception& ex)
    {
        autoAssigns()[e.project] = {{"error", ex.what()}};
        log::error("Assigning the bake of {} failed: {}", p->name(), ex.what());
    }
}
} // namespace

json autoAssigned(NodeId project)
{
    auto it = autoAssigns().find(project);
    return it == autoAssigns().end() ? json() : it->second;
}

void setRebakeAsync(bool on)
{
    rebakes().async = on;
}

bool rebakesPending()
{
    const Rebakes& r = rebakes();
    return r.running != scene::kNoNode || !r.pending.empty();
}

void shutdownRebakes()
{
    Rebakes& r = rebakes();
    r.shutdown = true;
    r.pending.clear();
    if (r.running != scene::kNoNode)
        cancelAsyncBake(true);
    r.running = scene::kNoNode;
}

json onSceneSaved()
{
    LILA_ZONE();

    Graph& g = Graph::current();
    if (rebakes().async)
        return rebakeOnSaveAsync(g);
    json out = json::array();
    for (NodeId id : projects(g))
    {
        Node* p = g.node(id);
        if (!p)
            continue;
        const ProjectSettings ps = projectSettings(*p);
        if (!ps.rebakeOnSave)
            continue;
        std::string reason;
        if (!needsBake(g, id, &reason))
        {
            out.push_back({{"project", p->name()}, {"baked", false}, {"reason", reason}});
            continue;
        }
        try
        {
            log::info("Rebaking {} on save: {}", p->name(), reason);
            bake(g, BakeRequest{.project = id, .backend = preferredBackend()});
            out.push_back({{"project", p->name()}, {"baked", true}, {"reason", reason}});
        }
        catch (const std::exception& e)
        {
            log::error("Rebake of {} on save failed: {}", p->name(), e.what());
            out.push_back({{"project", p->name()}, {"baked", false}, {"reason", e.what()}});
        }
    }
    return out;
}

void registerPipelineCommands()
{
    if (ops::find("bakeAssign"))
        return;
    events::subscribe(events::Event::Shutdown, [](const events::EventData&) { shutdownRebakes(); });
    addBakeListener(onAutoAssignEvent);

    // One worker: bakes already use every core (or the one GPU), so files bake one after another.
    const json bakeAll = {{"scriptFile", paths::toUtf8(paths::resourcesDir() / "bake" / "bake_all_projects.py")},
                          {"scriptLabel", "Bake all projects"},
                          {"workers", 1},
                          {"save", "overwrite"},
                          {"args", {{"changedOnly", true}, {"assign", false}}}};
    PrefStore::get().setDefaults("batchRun", "bake:presets", {{"presets", {{"Bake all projects", bakeAll}}}});

    const FlagDef projectFlag{"project", "pj", T::String, nullptr, "The bake project; defaults to the scene's only one."};

    ops::registerCommand({
        .name = "bakeAssign",
        .doc = "Wire a bake project's last outputs into the materials of the lows they were baked for: normal (DirectX or OpenGL "
               "as baked), ambient occlusion, ORM/MRO packed maps, base colour, roughness, metallic, emissive. Slots with no "
               "material get one. Returns {assigned, created, skipped}.",
        .flags = {projectFlag,
                  {"create", "cr", T::Bool, true, "Create materials for slots that have none."},
                  {"channels", "ch", T::Any, nullptr, "Override which material channel a map goes to: {\"height\": \"roughness\"}."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(g, a, "bakeAssign");
            const json r = lastOrManifest(project);
            if (!r.is_object() || r.value("outputs", json::array()).empty())
                throw CommandError("bakeAssign: " + project.name() + " has no bake to assign; bake it first");
            return assignOutputs(g, r, a.get<bool>("create", true), a.has("channels") ? a.flags["channels"] : json::object());
        },
    });

    ops::registerCommand({
        .name = "assignTexture",
        .doc = "Wire a texture file into the material on a mesh's face (or its only material): the channel comes from map= or "
               "the file's name (T_Rock_N.png, M_Outfit_ao.png, ..._BaseColor.png). A slot with no material gets one. Returns "
               "{assigned, created, skipped}.",
        .positionalDoc = "file",
        .flags = {nodeFlag({"mesh", "m", T::String, nullptr, "The mesh."}, "mesh"),
                  {"face", "f", T::Int, -1, "A face of the mesh (outMesh id) whose material takes it; -1 for the mesh's first."},
                  {"map", "mp", T::String, nullptr, "A bake map id (normal, ao, orm, ...) or material channel, instead of the name's."},
                  {"normalFormat", "nf", T::String, "directx", "For a normal map: directx or opengl.", {"directx", "opengl"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("assignTexture: give a texture file");
            const std::string file = a.positional[0].get<std::string>();
            Node* n = findNode(g, a.get<std::string>("mesh", ""));
            if (!n || !n->type().isA("mesh"))
                throw CommandError("assignTexture: mesh= must name a mesh");
            const std::string map = a.has("map") ? a.get<std::string>("map") : mapFromFileName(file);
            if (map.empty())
                throw CommandError("assignTexture: can't tell the channel from " + paths::fromUtf8(file).filename().string() + "; give map=");
            const scene::MeshPtr mesh = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
            std::string slot = scene::materials::kDefaultSlot;
            if (mesh)
            {
                const auto ids = mesh->faceAttrs().get<int32_t>(geo::attr::kMaterial);
                const int64_t face = a.get<int64_t>("face", -1);
                if (face >= 0 && size_t(face) < ids.size())
                    slot = scene::materials::slotName(*mesh, ids[size_t(face)]);
                else if (const auto usage = scene::materials::slotUsage(*mesh); !usage.empty())
                    slot = usage.front().first;
            }
            json channels = json::object();
            if (materialChannel(map).empty())
                channels[map] = map;
            const json result = {{"outputs", json::array({{{"file", file},
                                                           {"map", map},
                                                           {"materials", {slot}},
                                                           {"meshes", {n->path()}},
                                                           {"normalFormat", a.get<std::string>("normalFormat", "directx")}}})}};
            UndoChunk chunk("Assign Texture");
            return assignOutputs(g, result, true, channels);
        },
    });

    ops::registerCommand({
        .name = "setBakeProject",
        .doc = "Set a bake project's pipeline options: rebakeOnSave (saving the scene rebakes it when its inputs changed) and "
               "autoAssign (every bake wires its outputs into the lows' materials). Returns them.",
        .flags = {projectFlag,
                  {"rebakeOnSave", "rs", T::Bool, nullptr, "Rebake on save when the inputs' hash changed."},
                  {"autoAssign", "aa", T::Bool, nullptr, "Run bakeAssign after every bake."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(g, a, "setBakeProject");
            ProjectSettings s = projectSettings(project);
            if (a.has("rebakeOnSave"))
                s.rebakeOnSave = a.get<bool>("rebakeOnSave");
            if (a.has("autoAssign"))
                s.autoAssign = a.get<bool>("autoAssign");
            setProjectSettings(g, project.id(), s);
            return {{"rebakeOnSave", s.rebakeOnSave}, {"autoAssign", s.autoAssign}};
        },
    });

    ops::registerCommand({
        .name = "bakeUpToDate",
        .doc = "Whether a bake project's textures match its inputs: {upToDate, reason, hash, bakedHash}. reason is never baked, "
               "inputs changed, missing <file> or up to date.",
        .flags = {projectFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(g, a, "bakeUpToDate");
            std::string reason;
            const bool needs = needsBake(g, project.id(), &reason);
            const json m = readManifest(project);
            return {{"upToDate", !needs},
                    {"reason", reason},
                    {"hash", inputHash(g, project.id())},
                    {"bakedHash", m.is_object() ? m.value("inputHash", std::string()) : std::string()}};
        },
    });

    ops::registerCommand({
        .name = "bakeAll",
        .doc = "Bake every bake project in the scene, one after another. changedOnly=True skips projects whose inputs match "
               "their last bake. Projects with autoAssign (or assign=True) wire their outputs into materials. Returns [{project, "
               "baked, reason, outputs, misses, seconds}].",
        .flags = {{"changedOnly", "co", T::Bool, false, "Skip projects that are up to date."},
                  {"assign", "as", T::Bool, false, "Run bakeAssign after each bake."},
                  {"backend", "be", T::String, nullptr, "Where to bake; the GPU when one is registered, else cpu."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            const std::string backend = a.has("backend") ? a.get<std::string>("backend") : preferredBackend();
            for (NodeId id : projects(g))
            {
                Node* p = g.node(id);
                std::string reason = "requested";
                if (a.get<bool>("changedOnly") && !needsBake(g, id, &reason))
                {
                    out.push_back({{"project", p->name()}, {"baked", false}, {"reason", reason}});
                    continue;
                }
                try
                {
                    const BakeResult r = bake(g, BakeRequest{.project = id, .backend = backend});
                    uint64_t misses = 0;
                    for (const GroupStats& s : r.groups)
                        misses += s.misses;
                    json entry = {{"project", p->name()}, {"baked", true},         {"reason", reason},
                                  {"outputs", r.outputs.size()}, {"misses", misses}, {"seconds", r.seconds}};
                    if (projectSettings(*p).autoAssign)
                        entry["assign"] = autoAssigned(id);
                    else if (a.get<bool>("assign"))
                        entry["assign"] = ops::execute("bakeAssign", {{"project", p->path()}});
                    out.push_back(std::move(entry));
                }
                catch (const progress::Cancelled&)
                {
                    throw CommandError("bakeAll: cancelled");
                }
                catch (const std::exception& e)
                {
                    out.push_back({{"project", p->name()}, {"baked", false}, {"reason", std::string("failed: ") + e.what()}});
                }
            }
            return out;
        },
    });
}
} // namespace lila::bake
