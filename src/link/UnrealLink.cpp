#include "link/UnrealLink.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "link/Exporters.h"
#include "link/TextureSetSend.h"
#include "scene/Graph.h"
#include "scene/LodGroup.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/ObjectSets.h"
#include "scene/Take.h"
#include "seq/MeshSequence.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace lila::link
{
namespace
{
constexpr const char* kPrefFile = "unreal";
constexpr int kHelloTimeoutMs = 3000;
constexpr int kImportTimeoutMs = 10 * 60 * 1000;
constexpr int kImportIdleTimeoutMs = 60 * 1000;

const char* const kOptionKeys[] = {
    "skeleton", "importMaterials", "importTextures", "combineMeshes", "autoCollision", "generateLightmapUVs",
    "normals",  "lodGroup",        "existing",       "nanite",        "importAnimations", "reapplyOptions", "save",
    "materialFolder", "keepMaterials", "lodScreenSizes",
};

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path projectRoot(const fs::path& p)
{
    return p.extension() == ".uproject" ? p.parent_path() : p;
}

bool sameDir(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    fs::path ca = fs::weakly_canonical(projectRoot(a), ec);
    fs::path cb = fs::weakly_canonical(projectRoot(b), ec);
    auto norm = [](const fs::path& p) {
        std::string s = lower(p.lexically_normal().generic_string());
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        return s;
    };
    return norm(ca) == norm(cb);
}

bool matchesProject(const Instance& i, const std::string& project)
{
    if (project.empty())
        return true;
    if (lower(i.project) == lower(project))
        return true;
    return !i.projectDir.empty() && sameDir(i.projectDir, fs::path(project));
}

std::string fileSafe(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
            c = '_';
    return out.empty() ? "unnamed" : out;
}

/** A mapping's node (a full path, or a name in older mappings) made file safe, without the leading '|'. */
std::string nodeFileStem(const std::string& node)
{
    return fileSafe(node.starts_with('|') ? node.substr(1) : node);
}

/** The node a mapping names; null when it is gone or the name has become ambiguous. */
scene::Node* mappedNode(const std::string& node)
{
    try
    {
        return node.empty() ? nullptr : scene::Graph::current().find(node);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

bool sameMappedNode(const std::string& a, const std::string& b)
{
    if (a == b)
        return true;
    scene::Node* n = mappedNode(a);
    return n && n == mappedNode(b);
}

void writeMappings(const json& list)
{
    PrefStore::get().set(kPrefFile, "/mappings", list, PrefScope::Workspace);
}

void reportImport(const std::string& what, const RpcResult& r)
{
    if (!r.ok)
    {
        log::error("Unreal import of {} failed: {}", what, r.error);
        return;
    }
    const json& res = r.result;
    for (const json& w : res.value("warnings", json::array()))
        log::warn("Unreal: {}: {}", what, w.is_string() ? w.get<std::string>() : w.dump());
    json errors = res.value("errors", json::array());
    for (const json& e : errors)
        log::error("Unreal: {}: {}", what, e.is_string() ? e.get<std::string>() : e.dump());
    if (errors.empty())
        log::info("Unreal: {} -> {}", what, res.value("assets", json::array()).dump());
}

/** One frame of a meshSequence mapping: a static mesh import at <folder>/<base>_<index>. */
json frameMapping(const json& m, const std::string& contentPath)
{
    json fm = m;
    fm["kind"] = "static";
    fm["contentPath"] = contentPath;
    return fm;
}

/** Imports the frames in order, points the Niagara renderer at them, then drops frames past the end. Worker thread. */
void syncSequence(RpcClient& client, const json& m, const std::vector<fs::path>& files, const json& materials, progress::Scope& scope)
{
    LILA_ZONE();

    const std::string node = m.value("node", std::string());
    const SequenceAssets names = sequenceAssets(m, int(files.size()));
    json meshes = json::array();
    for (size_t i = 0; i < files.size(); ++i)
    {
        scope.setStatus(std::format("Unreal: sending {} frame {} of {}", node, i + 1, files.size()));
        RpcResult r = client.call("importAsset", importParams(files[i], frameMapping(m, names.contentPaths[i]), materials), importCallOptions());
        if (!r.ok || !r.result.value("errors", json::array()).empty())
        {
            mainq::post([what = names.contentPaths[i], r] { reportImport(what, r); });
            return;
        }
        meshes.push_back(names.objectPaths[i]);
    }
    mainq::post([node, n = files.size(), folder = names.folder] { log::info("Unreal: {} -> {} frame(s) in {}", node, n, folder); });

    std::string system = m.value("niagaraSystem", std::string());
    if (!system.empty())
    {
        scope.setStatus("Unreal: updating " + system);
        json params = {{"system", system}, {"meshes", meshes}};
        if (m.contains("niagaraEmitter") && m["niagaraEmitter"].is_string())
            params["emitter"] = m["niagaraEmitter"];
        if (m.contains("niagaraRenderer") && m["niagaraRenderer"].is_number_integer())
            params["renderer"] = m["niagaraRenderer"];
        if (m.value("save", false))
            params["save"] = true;
        RpcResult r = client.call("setNiagaraMeshes", params, importCallOptions());
        mainq::post([system, r] {
            if (!r.ok)
                log::error("Unreal: setting the meshes of {} failed: {}", system, r.error);
            else
                log::info("Unreal: {} renderer(s) of {} now use the frames", r.result.value("renderers", 0), system);
        });
    }

    RpcResult listed = client.call("listAssets", {{"path", names.folder}, {"recursive", false}, {"class", "StaticMesh"}}, kHelloTimeoutMs);
    if (!listed.ok)
        return;
    json surplus = json::array();
    for (const std::string& p : surplusFrames(listed.result, names.base, int(files.size())))
        surplus.push_back(p);
    if (surplus.empty())
        return;
    RpcResult r = client.call("deleteAssets", {{"paths", surplus}}, importCallOptions());
    mainq::post([node, surplus, r] {
        if (!r.ok)
            log::error("Unreal: removing surplus frames of {} failed: {}", node, r.error);
        else
            log::info("Unreal: removed {} frame(s) of {} past the end of the range: {}", surplus.size(), node, surplus.dump());
    });
}
} // namespace

SequenceAssets sequenceAssets(const json& mapping, int count)
{
    SequenceAssets out;
    std::string path = mapping.value("contentPath", std::string());
    const size_t dot = path.find('.', path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
    if (dot != std::string::npos)
        path.resize(dot);
    if (path.ends_with('/'))
    {
        out.folder = path.substr(0, path.size() - 1);
        std::string node = mapping.value("node", std::string("Mesh"));
        out.base = fileSafe(node.substr(node.rfind('|') == std::string::npos ? 0 : node.rfind('|') + 1));
    }
    else
    {
        const size_t slash = path.rfind('/');
        out.folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
        out.base = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    json seqOpts = mapping.value("sequence", json::object());
    const int padding = seqOpts.is_object() ? seqOpts.value("padding", 3) : 3;
    for (int i = 0; i < count; ++i)
    {
        std::string name = seq::frameName("{name}_{index}", out.base, i, double(i), padding);
        out.contentPaths.push_back(out.folder + "/" + name);
        out.objectPaths.push_back(out.folder + "/" + name + "." + name);
    }
    return out;
}

std::vector<std::string> surplusFrames(const json& listed, const std::string& base, int count)
{
    std::vector<std::string> out;
    if (!listed.is_array())
        return out;
    std::string escaped;
    for (char c : base)
    {
        if (std::string_view("\\^$.|?*+()[]{}").find(c) != std::string_view::npos)
            escaped += '\\';
        escaped += c;
    }
    const std::regex frame("^" + escaped + "_([0-9]+)$");
    for (const json& a : listed)
    {
        std::string path = a.is_object() ? a.value("path", std::string()) : a.is_string() ? a.get<std::string>() : std::string();
        std::string name = path.substr(path.rfind('/') + 1);
        if (size_t dot = name.find('.'); dot != std::string::npos)
            name.resize(dot);
        std::smatch match;
        if (std::regex_match(name, match, frame) && std::stoll(match[1].str()) >= count)
            out.push_back(path);
    }
    return out;
}

seq::SequenceOptions sequenceOptions(const json& mapping, const fs::path& folder)
{
    json s = mapping.value("sequence", json::object());
    if (!s.is_object())
        s = json::object();
    seq::SequenceOptions o = seq::fromJson(s);
    o.playbackRange = s.value("playbackRange", !s.contains("start") && !s.contains("end"));
    o.format = mapping.value("kind", std::string()) == "geometryCache" ? "abc" : "fbx";
    o.folder = folder.generic_string();
    o.pattern = "{name}_{index}";
    o.profile = "ue5";
    return o;
}

seq::VatOptions vertexAnimationOptions(const json& mapping, const fs::path& folder, const std::string& name)
{
    json v = mapping.value("vat", json::object());
    if (!v.is_object())
        throw std::invalid_argument("vat must be a dict");
    seq::VatOptions o = seq::vatFromJson(v, sequenceOptions(mapping, folder));
    o.folder = folder.generic_string();
    o.name = name;
    return o;
}

json vertexAnimationParams(const seq::VatResult& bake, const json& mapping)
{
    json options = json::object();
    for (const char* key : kOptionKeys)
        if (mapping.contains(key) && !mapping[key].is_null())
            options[key] = mapping[key];
    json params = {{"contentPath", mapping.value("contentPath", std::string())},
                   {"fbx", bake.mesh.generic_string()},
                   {"positions", bake.positions.generic_string()},
                   {"metaFile", bake.meta.generic_string()},
                   {"meta", bake.metadata},
                   {"options", options}};
    if (!bake.normals.empty())
        params["normals"] = bake.normals.generic_string();
    // The plugin names the textures T_<asset without SM_>_VATPos / _VATNrm beside the mesh.
    std::string path = mapping.value("contentPath", std::string());
    if (size_t dot = path.find('.', path.rfind('/') == std::string::npos ? 0 : path.rfind('/')); dot != std::string::npos)
        path.resize(dot);
    const size_t slash = path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
    std::string asset = slash == std::string::npos ? path : path.substr(slash + 1);
    if (asset.starts_with("SM_"))
        asset = asset.substr(3);
    auto objectPath = [&](const std::string& suffix) {
        const std::string name = "T_" + asset + suffix;
        return folder + "/" + name + "." + name;
    };
    json& mp = params["meta"]["materialParams"];
    if (mp.is_object())
    {
        mp["PositionTexture"] = objectPath("_VATPos");
        if (!bake.normals.empty())
            mp["NormalTexture"] = objectPath("_VATNrm");
    }
    json v = mapping.value("vat", json::object());
    if (v.is_object() && v.contains("materialParent") && v["materialParent"].is_string() && !v["materialParent"].get<std::string>().empty())
        params["materialParent"] = v["materialParent"];
    if (v.is_object() && v.value("rebuildMaterial", false))
        params["options"]["rebuildMaterial"] = true;
    return params;
}

json toJson(const Instance& i)
{
    return {{"port", i.port},
            {"pid", i.pid},
            {"project", i.project},
            {"projectDir", i.projectDir.generic_string()},
            {"engineVersion", i.engineVersion},
            {"source", i.source}};
}

std::vector<Instance> readPortFiles(const std::vector<fs::path>& projectDirs)
{
    LILA_ZONE();

    std::vector<Instance> out;
    for (const fs::path& dir : projectDirs)
    {
        fs::path file = projectRoot(dir) / "Saved" / "LilaLink" / "port.json";
        std::ifstream in(file);
        if (!in)
            continue;
        json j = json::parse(in, nullptr, false);
        if (!j.is_object() || !j.contains("port") || !j["port"].is_number_integer())
            continue;
        Instance i;
        i.port = static_cast<uint16_t>(j["port"].get<int>());
        i.pid = j.value("pid", int64_t{0});
        i.project = j.value("project", std::string());
        i.projectDir = j.contains("projectDir") && j["projectDir"].is_string() ? fs::path(j["projectDir"].get<std::string>()) : projectRoot(dir);
        i.engineVersion = j.value("engineVersion", std::string());
        i.source = "portFile";
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<fs::path> configuredProjects()
{
    std::vector<fs::path> out;
    json list = PrefStore::get().value(kPrefFile, "/projects", json::array());
    if (!list.is_array())
        return out;
    for (const json& p : list)
    {
        if (!p.is_string())
            continue;
        out.push_back(paths::resolvePath(p.get<std::string>(), {}).lexically_normal());
    }
    return out;
}

std::vector<Instance> discover(bool probe, const std::string& host)
{
    LILA_ZONE();

    std::vector<Instance> out = readPortFiles(configuredProjects());
    if (!probe)
        return out;
    for (int p = kDefaultPort; p < kDefaultPort + kPortSearchCount; ++p)
    {
        uint16_t port = static_cast<uint16_t>(p);
        if (std::ranges::any_of(out, [&](const Instance& i) { return i.port == port; }))
            continue;
        if (TcpSocket::connect(host, port, 150).valid())
            out.push_back({.port = port, .source = "probe"});
    }
    return out;
}

json mappings()
{
    json list = PrefStore::get().value(kPrefFile, "/mappings", json::array());
    return list.is_array() ? list : json::array();
}

void setMapping(const json& m)
{
    LILA_ZONE();

    json list = mappings();
    std::string node = m.value("node", std::string());
    auto it = std::ranges::find_if(list, [&](const json& x) { return x.is_object() && sameMappedNode(x.value("node", std::string()), node); });
    if (it != list.end())
        *it = m;
    else
        list.push_back(m);
    writeMappings(list);
}

bool removeMapping(const std::string& node)
{
    json list = mappings();
    size_t before = list.size();
    json kept = json::array();
    for (const json& x : list)
        if (!x.is_object() || !sameMappedNode(x.value("node", std::string()), node))
            kept.push_back(x);
    if (kept.size() == before)
        return false;
    writeMappings(kept);
    return true;
}

json importParams(const fs::path& file, const json& mapping, const json& materials)
{
    json options = json::object();
    for (const char* key : kOptionKeys)
        if (mapping.contains(key) && !mapping[key].is_null() && !(mapping[key].is_string() && mapping[key].get<std::string>().empty()))
            options[key] = mapping[key];
    json params = {{"file", file.generic_string()},
                   {"contentPath", mapping.value("contentPath", std::string())},
                   {"kind", mapping.value("kind", std::string("static"))},
                   {"options", options}};
    if (materials.is_array())
        params["materials"] = materials;
    return params;
}

json materialParams(scene::Graph& g, const std::vector<scene::NodeId>& roots)
{
    LILA_ZONE();

    g.evaluate();
    std::vector<std::string> slots;
    auto addSlots = [&](scene::NodeId id) {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || n->type().isA("lattice"))
            return;
        const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!mesh)
            return;
        for (const auto& [slot, count] : scene::materials::slotUsage(*mesh))
            if (count > 0 && slot != scene::materials::kDefaultSlot && std::ranges::find(slots, slot) == slots.end())
                slots.push_back(slot);
    };
    for (scene::NodeId root : roots)
    {
        addSlots(root);
        for (scene::NodeId d : g.descendants(root))
            addSlots(d);
    }

    json out = json::array();
    for (const std::string& slot : slots)
        out.push_back(materialInputs(g, slot));
    return out;
}

json materialInputs(scene::Graph& g, const std::string& slot)
{
    json m = {{"name", slot}};
    if (scene::Node* mat = scene::materials::find(g, slot))
    {
        const auto& defs = mat->type().attrs;
        for (int i = 0; i < scene::material::kCount; ++i)
        {
            const scene::AttrDef& d = defs[size_t(i)];
            if (d.type == scene::PlugType::Enum)
            {
                int64_t v = mat->as<int64_t>(i);
                m[d.name] = v >= 0 && size_t(v) < d.enumNames.size() ? json(d.enumNames[size_t(v)]) : json(v);
            }
            else if (i == scene::material::kStrokeMap)
                m[d.name] = fs::path(scene::materials::strokeSheetPath(mat->as<std::string>(i))).generic_string();
            else if (d.flags & scene::AttrFile)
            {
                std::string path = mat->as<std::string>(i);
                if (!path.empty())
                    m[d.name] = fs::path(scene::materials::resolvePath(path)).generic_string();
            }
            else
                m[d.name] = scene::toJson(mat->value(i));
        }
    }
    return m;
}

CallOptions importCallOptions()
{
    PrefStore& prefs = PrefStore::get();
    return {.timeoutMs = prefs.value<int>(kPrefFile, "/importTimeoutMs", kImportTimeoutMs),
            .idleTimeoutMs = prefs.value<int>(kPrefFile, "/importIdleTimeoutMs", kImportIdleTimeoutMs)};
}

fs::path cacheDir()
{
    std::string custom = PrefStore::get().value<std::string>(kPrefFile, "/cacheDir", "");
    if (!custom.empty())
        return paths::resolvePath(custom, {});
    fs::path base = Workspace::get().isOpen() ? Workspace::get().stateDir() : paths::userConfigDir();
    return base / "cache" / "unreal";
}

Link& Link::get()
{
    static Link link;
    return link;
}

Link::~Link()
{
    shutdown();
}

json Link::connect(const ConnectOptions& opts)
{
    LILA_ZONE();

    std::vector<Instance> candidates;
    if (opts.port)
    {
        candidates.push_back({.port = *opts.port, .source = "explicit"});
    }
    else
    {
        for (const Instance& i : discover(true, opts.host))
            if (matchesProject(i, opts.project) || i.source == "probe")
                candidates.push_back(i);
    }
    if (candidates.empty())
        throw std::runtime_error("no Unreal editor with LilaLink found (add its project dir to the \"projects\" list in "
                                 "the unreal prefs, or pass a port)");

    std::string lastError = "no Unreal editor answered";
    for (const Instance& i : candidates)
    {
        auto client = std::make_shared<RpcClient>();
        std::string err;
        if (!client->connect(opts.host, i.port, opts.timeoutMs, &err))
        {
            lastError = err;
            continue;
        }
        RpcResult hello = client->call("hello", json::object(), kHelloTimeoutMs);
        if (!hello.ok || !hello.result.is_object())
        {
            lastError = "port " + std::to_string(i.port) + " did not answer hello: " + hello.error;
            continue;
        }
        // A probed port only counts if hello names the project we asked for.
        Instance named = i;
        named.project = hello.result.value("project", std::string());
        named.projectDir = hello.result.value("projectDir", std::string());
        if (!matchesProject(named, opts.project))
        {
            lastError = "port " + std::to_string(i.port) + " is project " + named.project;
            continue;
        }

        json features = hello.result.value("features", json::array());
        if (!features.is_array() || std::ranges::find(features, json("progress")) == features.end())
            log::warn("The LilaLink plugin in {} is out of date ({}); rebuild it from Lila's ue/LilaLink so imports "
                      "report progress and cannot hang the editor",
                      named.project, hello.result.value("pluginVersion", std::string("unknown version")));

        uint16_t port = i.port;
        client->onDisconnected([port] { mainq::post([port] { log::warn("Unreal editor on port {} disconnected", port); }); });
        json h = hello.result;
        h["port"] = port;
        {
            std::scoped_lock lock(mMutex);
            mClient = client;
            mHello = h;
        }
        log::info("Connected to Unreal {} ({}) on port {}", h.value("project", std::string()),
                  h.contains("engine") ? h["engine"].value("string", std::string()) : std::string(), port);
        return h;
    }
    throw std::runtime_error(lastError);
}

void Link::ensureConnected()
{
    if (!connected())
        connect({});
}

void Link::disconnect()
{
    std::shared_ptr<RpcClient> c;
    {
        std::scoped_lock lock(mMutex);
        c.swap(mClient);
        mHello = json();
    }
    if (c)
        c->disconnect();
}

bool Link::connected() const
{
    std::scoped_lock lock(mMutex);
    return mClient && mClient->connected();
}

json Link::status() const
{
    std::scoped_lock lock(mMutex);
    if (!mClient || !mClient->connected())
        return {{"connected", false}};
    json s = mHello;
    s["connected"] = true;
    return s;
}

std::shared_ptr<RpcClient> Link::client() const
{
    std::scoped_lock lock(mMutex);
    return mClient && mClient->connected() ? mClient : nullptr;
}

void Link::post(std::function<void()> fn)
{
    std::scoped_lock lock(mQueueMutex);
    if (mQuit)
        return;
    if (!mWorker.joinable())
        mWorker = std::thread([this] { workerLoop(); });
    mQueue.push_back(std::move(fn));
    mQueueCv.notify_all();
}

void Link::waitIdle()
{
    std::unique_lock lock(mQueueMutex);
    mQueueCv.wait(lock, [&] { return (mQueue.empty() && !mBusy) || mQuit; });
}

void Link::shutdown()
{
    {
        std::scoped_lock lock(mQueueMutex);
        mQuit = true;
        mQueue.clear();
        mQueueCv.notify_all();
    }
    if (mWorker.joinable())
        mWorker.join();
    disconnect();
}

void Link::workerLoop()
{
    LILA_THREAD_NAME("Unreal sync");

    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(mQueueMutex);
            mBusy = false;
            mQueueCv.notify_all();
            mQueueCv.wait(lock, [&] { return mQuit || !mQueue.empty(); });
            if (mQuit)
                return;
            job = std::move(mQueue.front());
            mQueue.pop_front();
            mBusy = true;
        }
        try
        {
            job();
        }
        catch (const std::exception& e)
        {
            std::string msg = e.what();
            mainq::post([msg] { log::error("Unreal sync: {}", msg); });
        }
    }
}

void onSceneSaved()
{
    syncMappings(false);
}

int syncMappings(bool force)
{
    LILA_ZONE();

    if (!force && !PrefStore::get().value<bool>(kPrefFile, "/autoSync", true))
        return 0;
    json list = mappings();
    if (list.empty())
        return syncTextureSets();

    const bool haveFbx = static_cast<bool>(findExporter("fbx"));
    int skipped = 0;

    struct Job
    {
        json mapping;
        json materials;
        std::vector<scene::NodeId> nodes;
        fs::path file;
        ExportFn exporter;
        /** meshSequence: the frame files, already written; the file above is then the first. */
        std::vector<fs::path> frames;
        /** vertexAnimation: importVertexAnimation params for the bake, already written. */
        json vat;
    };
    std::vector<Job> jobs;
    scene::Graph& graph = scene::Graph::current();
    fs::path dir = cacheDir();
    for (const json& m : list)
    {
        if (!m.is_object())
            continue;
        std::string node = m.value("node", std::string());
        scene::Node* n = mappedNode(node);
        if (!n || m.value("contentPath", std::string()).empty())
            continue;
        // "fbx:skeletal" / "fbx:animation" when registered, so the file matches the mapping's kind.
        const std::string kind = m.value("kind", std::string("static"));
        const bool animated = kind == "meshSequence" || kind == "geometryCache" || kind == "vertexAnimation";
        if (!haveFbx && !animated)
        {
            ++skipped;
            continue;
        }
        ExportFn exporter = findExporter("fbx:" + kind);
        std::vector<scene::NodeId> nodes{n->id()};
        if (scene::objectSets::isSet(*n))
        {
            // A set is one asset: its members export together, keeping where they sit relative to each other.
            nodes = scene::objectSets::nodesOf(graph, n->id());
            if (nodes.empty())
                continue;
            if (ExportFn together = findExporter("fbx:set:" + kind))
                exporter = together;
        }
        if (kind == "vertexAnimation")
        {
            try
            {
                const seq::VatOptions vo = vertexAnimationOptions(m, dir / (nodeFileStem(node) + "_vat"), nodeFileStem(node));
                const seq::VatResult bake = seq::exportVat(graph, seq::meshNodes(graph, nodes), vo);
                Job job{m, json(), nodes, bake.mesh, nullptr, {}};
                job.vat = vertexAnimationParams(bake, m);
                log::info("Unreal sync: {} material params: {}", node, seq::vatMaterialSummary(job.vat["meta"]["materialParams"]));
                jobs.push_back(std::move(job));
            }
            catch (const std::exception& e)
            {
                log::error("Unreal sync: vertex animation bake of {} failed: {}", node, e.what());
            }
            continue;
        }
        json materials = materialParams(graph, nodes);
        if (animated)
        {
            const bool cache = kind == "geometryCache";
            const fs::path folder = cache ? dir : dir / (nodeFileStem(node) + "_frames");
            std::vector<fs::path> files;
            try
            {
                std::error_code ec;
                if (!cache)
                    fs::remove_all(folder, ec);
                seq::SequenceOptions so = sequenceOptions(m, folder);
                so.pattern = cache ? nodeFileStem(node) : "{name}_{index}";
                files = seq::exportSequence(graph, seq::meshNodes(graph, nodes), so);
            }
            catch (const std::exception& e)
            {
                log::error("Unreal sync: export of {} failed: {}", node, e.what());
                continue;
            }
            if (files.empty())
                continue;
            // Geometry caches have no material slots Lila manages; Unreal's Alembic importer makes them.
            Job job{m, cache ? json() : materials, nodes, files.front(), nullptr, {}};
            if (!cache)
                job.frames = files;
            jobs.push_back(std::move(job));
            continue;
        }
        // takes: all | [names] pushes each scene take as its own asset, <contentPath>_<take> (or <folder>/<take>).
        if (m.contains("takes") && !m["takes"].is_null() && (kind == "animation" || kind == "skeletal"))
        {
            TakeExportFn takeExporter = findTakeExporter("fbx:" + kind);
            std::vector<scene::NodeId> chosen = scene::anim::selectTakes(graph, m["takes"], n->id());
            if (!takeExporter)
                log::warn("Unreal sync: no take exporter for {}, {} skipped", kind, node);
            for (scene::NodeId t : takeExporter ? chosen : std::vector<scene::NodeId>{})
            {
                std::string take = graph.node(t)->name();
                json tm = m;
                std::string path = m.value("contentPath", std::string());
                tm["contentPath"] = path.ends_with('/') ? path + take : path + "_" + take;
                ExportFn fn = [takeExporter, take](const std::vector<scene::NodeId>& ns, const fs::path& file, std::string* err) {
                    return takeExporter(ns, file, take, err);
                };
                jobs.push_back({tm, materials, nodes, dir / (nodeFileStem(node) + "_" + fileSafe(take) + ".fbx"), fn});
            }
            continue;
        }
        json mapping = m;
        // FBX carries the levels but not their screen sizes, so the plugin sets them on the mesh after the import.
        if (std::vector<double> sizes = scene::lod::exportScreenSizes(graph, nodes); !sizes.empty() && !mapping.contains("lodScreenSizes"))
            mapping["lodScreenSizes"] = sizes;
        jobs.push_back({mapping, materials, nodes, dir / (nodeFileStem(node) + ".fbx"), exporter ? exporter : findExporter("fbx")});
    }
    if (skipped > 0)
        log::warn("Unreal sync: no FBX exporter registered, {} mapping(s) skipped", skipped);
    if (jobs.empty())
        return syncTextureSets();

    int count = static_cast<int>(jobs.size());
    Link::get().post([jobs = std::move(jobs), dir] {
        LILA_ZONE_N("Unreal sync job");

        Link::get().ensureConnected();
        std::shared_ptr<RpcClient> client = Link::get().client();
        if (!client)
            throw std::runtime_error("not connected");

        std::error_code ec;
        fs::create_directories(dir, ec);
        progress::Scope scope("Sending to Unreal", double(jobs.size()), progress::Silent);
        for (const Job& job : jobs)
        {
            std::string node = job.mapping.value("node", std::string());
            if (!job.vat.is_null())
            {
                scope.setStatus("Unreal: sending vertex animation of " + node);
                RpcResult r = client->call("importVertexAnimation", job.vat, importCallOptions());
                mainq::post([node, r] { reportImport(node, r); });
                scope.step();
                continue;
            }
            if (!job.frames.empty())
            {
                syncSequence(*client, job.mapping, job.frames, job.materials, scope);
                scope.step();
                continue;
            }
            std::string err;
            scope.setStatus("Exporting " + node);
            if (job.exporter && !job.exporter(job.nodes, job.file, &err))
            {
                mainq::post([node, err] { log::error("Unreal sync: export of {} failed: {}", node, err); });
                scope.step();
                continue;
            }
            scope.setStatus("Unreal: sending " + node);
            CallOptions opts = importCallOptions();
            opts.onProgress = [&scope, node](const json& p) {
                scope.setStatus(std::format("Unreal: {} {} ({})", p.value("stage", std::string("working on")), node,
                                            progress::formatElapsed(p.value("elapsed", 0.0))));
            };
            RpcResult r = client->call("importAsset", importParams(job.file, job.mapping, job.materials), opts);
            mainq::post([node, r] { reportImport(node, r); });
            scope.step();
        }
    });
    // After the meshes, so their slots exist when the instances are set on them.
    return count + syncTextureSets();
}
} // namespace lila::link
