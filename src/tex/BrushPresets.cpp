#include "tex/BrushPresets.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace lila::tex::brushes
{
namespace
{
namespace fs = std::filesystem;

using Tier = std::map<std::string, Preset, std::less<>>;

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

json readJsonFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::invalid_argument("can't read the file");
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

void scanDir(const fs::path& dir, const std::string& tier, const std::string& owner, Tier& into, std::vector<std::string>& errors)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".lilabrush")
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        fs::path rel = fs::relative(f, dir, ec);
        rel.replace_extension();
        try
        {
            Preset p = Preset::fromJson(readJsonFile(f));
            p.id = rel.generic_string();
            p.tier = tier;
            p.owner = owner;
            p.file = f;
            if (p.name.empty())
                p.name = p.id.substr(p.id.rfind('/') + 1);
            into[p.id] = std::move(p);
        }
        catch (const std::exception& ex)
        {
            errors.push_back(paths::toUtf8(f) + ": " + ex.what());
            log::warn("brush presets: {}: {}", paths::toUtf8(f), ex.what());
        }
    }
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
    const fs::path ws = Workspace::get().isOpen() ? Workspace::get().root() : fs::path();
    std::vector<std::string> exts = loadedExtensions();
    if (s.loaded && ws == s.workspaceRoot && exts == s.extensions)
        return s;
    s = {};
    s.loaded = true;
    s.workspaceRoot = ws;
    s.extensions = exts;
    scanDir(tierDir("builtin"), "builtin", {}, s.builtin, s.errors);
    for (const std::string& id : exts)
        if (ext::Context* ctx = extensions::contextOf(id))
            scanDir(ctx->resources() / "brushes", "extension", id, s.packs, s.errors);
    scanDir(tierDir("user"), "user", {}, s.user, s.errors);
    scanDir(tierDir("workspace"), "workspace", {}, s.workspace, s.errors);
    return s;
}
} // namespace

json Preset::toJson() const
{
    json j = {{"format", "lilabrush"}, {"version", 1}, {"name", name}, {"category", category}, {"brush", brush}, {"payload", payload}};
    if (!doc.empty())
        j["doc"] = doc;
    if (!particles.is_null())
        j["particles"] = particles;
    return j;
}

Preset Preset::fromJson(const json& j)
{
    if (!j.is_object() || j.value("format", "") != "lilabrush")
        throw std::invalid_argument("not a .lilabrush preset");
    Preset p;
    p.name = j.value("name", std::string());
    p.category = j.value("category", std::string());
    p.doc = j.value("doc", std::string());
    p.brush = j.value("brush", json::object());
    p.payload = j.value("payload", json::object());
    if (!p.brush.is_object() || !p.payload.is_object())
        throw std::invalid_argument("brush and payload must be objects");
    if (j.contains("particles") && j["particles"].is_object())
        p.particles = j["particles"];
    return p;
}

std::vector<Preset> all()
{
    State& s = loaded();
    std::map<std::string, const Preset*> win;
    for (const Tier* t : {&s.builtin, &s.packs, &s.user, &s.workspace})
        for (const auto& [id, p] : *t)
            win[id] = &p;
    std::vector<Preset> out;
    for (const auto& [id, p] : win)
        out.push_back(*p);
    std::stable_sort(out.begin(), out.end(), [](const Preset& a, const Preset& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    return out;
}

std::optional<Preset> find(std::string_view id)
{
    State& s = loaded();
    for (const Tier* t : {&s.workspace, &s.user, &s.packs, &s.builtin})
        if (auto it = t->find(id); it != t->end())
            return it->second;
    // By name, for scripts.
    for (const Preset& p : all())
        if (p.name == id)
            return p;
    return std::nullopt;
}

void reload() { state().loaded = false; }

fs::path tierDir(std::string_view tier)
{
    if (tier == "builtin")
        return paths::resourcesDir() / "brushes";
    if (tier == "user")
        return PrefStore::get().userDir() / "brushes";
    if (tier == "workspace")
        return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / "brushes" : fs::path();
    return {};
}

std::vector<std::string> errors() { return loaded().errors; }

fs::path save(const Preset& p, std::string_view tier)
{
    if (tier != "user" && tier != "workspace")
        throw std::runtime_error("brush presets save to the user or workspace tier");
    const fs::path dir = tierDir(tier);
    if (dir.empty())
        throw std::runtime_error("no workspace is open");
    std::string id = p.id.empty() ? p.name : p.id;
    if (id.empty() || id.find("..") != std::string::npos)
        throw std::runtime_error("a brush preset needs a name");
    const fs::path file = dir / paths::fromUtf8(id + ".lilabrush");
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
    out << p.toJson().dump(2) << "\n";
    reload();
    return file;
}
} // namespace lila::tex::brushes
