#include "bake/Targets.h"

#include "bake/Maps.h"
#include "bake/Naming.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Workspace.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>

namespace lila::bake
{
namespace fs = std::filesystem;

json TargetProfile::toJson() const
{
    json ms = json::array();
    for (const TargetMap& m : maps)
    {
        json j = m.settings.toJson();
        if (!m.slot.empty())
            j["slot"] = m.slot;
        if (m.optional)
            j["optional"] = true;
        ms.push_back(j);
    }
    json j = {{"name", name}, {"label", label}, {"version", kSettingsVersion}, {"output", output}, {"maps", ms}};
    if (!doc.empty())
        j["doc"] = doc;
    if (preset)
        j["preset"] = true;
    if (!backend.empty())
        j["backend"] = backend;
    if (!groupDefaults.empty())
        j["groupDefaults"] = groupDefaults;
    if (!pipeline.empty())
        j["pipeline"] = pipeline;
    if (!file.empty())
        j["file"] = paths::toUtf8(file);
    return j;
}

TargetProfile TargetProfile::fromJson(const json& j)
{
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string())
        throw std::invalid_argument("a target needs a name");
    TargetProfile t;
    t.name = j["name"].get<std::string>();
    t.label = j.value("label", t.name);
    t.doc = j.value("doc", std::string());
    if (j.contains("output") && j["output"].is_object())
        t.output = j["output"];
    if (!j.contains("maps") || !j["maps"].is_array())
        throw std::invalid_argument("target " + t.name + " has no maps list");
    for (const json& m : j["maps"])
    {
        TargetMap tm;
        tm.settings = MapSettings::fromJson(m);
        if (m.is_object())
        {
            tm.slot = m.value("slot", std::string());
            tm.optional = m.value("optional", false);
        }
        t.maps.push_back(std::move(tm));
    }
    t.preset = j.value("preset", false);
    t.backend = j.value("backend", std::string());
    if (j.contains("groupDefaults") && j["groupDefaults"].is_object())
        t.groupDefaults = j["groupDefaults"];
    if (j.contains("pipeline"))
    {
        if (!j["pipeline"].is_object())
            throw std::invalid_argument("target " + t.name + ": pipeline must be an object");
        for (const char* k : {"autoAssign", "rebakeOnSave"})
            if (j["pipeline"].contains(k))
            {
                if (!j["pipeline"][k].is_boolean())
                    throw std::invalid_argument("target " + t.name + ": pipeline." + k + " must be true or false");
                t.pipeline[k] = j["pipeline"][k];
            }
    }
    // Validate the output keys now, so a bad file fails when it is listed rather than when it is baked.
    OutputSettings::fromJson(t.output);
    withGroupDefaults({}, t.groupDefaults);
    return t;
}

std::vector<fs::path> targetDirs()
{
    std::vector<fs::path> dirs = {paths::resourcesDir() / "bake" / "targets", paths::userConfigDir() / "bake" / "targets"};
    if (Workspace::get().isOpen())
        dirs.push_back(Workspace::get().root() / ".lila" / "bake" / "targets");
    return dirs;
}

namespace
{
std::vector<TargetProfile> targetsIn(const fs::path& dir)
{
    std::vector<TargetProfile> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".json")
            files.push_back(e.path());
    std::ranges::sort(files);
    for (const fs::path& f : files)
    {
        try
        {
            std::ifstream in(f);
            TargetProfile t = TargetProfile::fromJson(json::parse(in));
            t.file = f;
            out.push_back(std::move(t));
        }
        catch (const std::exception& e)
        {
            log::warn("bake target {}: {}", paths::toUtf8(f), e.what());
        }
    }
    return out;
}

fs::path scopeDir(const std::string& scope)
{
    if (scope.empty() || scope == "user")
        return paths::userConfigDir() / "bake" / "targets";
    if (scope != "workspace")
        throw std::invalid_argument("scope must be user or workspace");
    if (!Workspace::get().isOpen())
        throw std::invalid_argument("no workspace is open");
    return Workspace::get().root() / ".lila" / "bake" / "targets";
}

const std::vector<const char*>& groupFields()
{
    static const std::vector<const char*> f = {"frontal", "rear", "cageOffset", "skew", "aoScope", "ignoreBackfaces"};
    return f;
}
} // namespace

std::vector<TargetProfile> targets()
{
    std::map<std::string, TargetProfile> byName;
    for (const fs::path& dir : targetDirs())
        for (TargetProfile& t : targetsIn(dir))
            byName[t.name] = std::move(t);
    std::vector<TargetProfile> out;
    for (auto& [k, v] : byName)
        out.push_back(std::move(v));
    return out;
}

std::optional<TargetProfile> findTarget(std::string_view name)
{
    for (TargetProfile& t : targets())
        if (t.name == name || t.file.stem().string() == name)
            return std::move(t);
    return std::nullopt;
}

ProjectSettings applyTarget(const ProjectSettings& settings, const TargetProfile& target, bool includeOptional, std::vector<std::string>* skipped)
{
    ProjectSettings p = settings;
    json out = p.output.toJson();
    for (auto it = target.output.begin(); it != target.output.end(); ++it)
        out[it.key()] = it.value();
    p.output = OutputSettings::fromJson(out);
    p.maps.clear();
    for (const TargetMap& m : target.maps)
    {
        if (m.optional && !includeOptional)
            continue;
        if (!findMap(m.settings.map))
        {
            if (skipped)
                skipped->push_back(m.slot.empty() ? m.settings.map : m.settings.map + " (" + m.slot + ")");
            continue;
        }
        p.maps.push_back(m.settings);
    }
    if (!target.groupDefaults.empty())
        p.groupDefaults = target.groupDefaults;
    p.autoAssign = target.pipeline.value("autoAssign", p.autoAssign);
    p.rebakeOnSave = target.pipeline.value("rebakeOnSave", p.rebakeOnSave);
    p.target = target.name;
    return p;
}

bool isBuiltinTarget(const TargetProfile& t)
{
    std::error_code ec;
    return !t.file.empty() && fs::equivalent(t.file.parent_path(), targetDirs().front(), ec);
}

json groupDefaultsOf(const GroupSettings& s)
{
    const json all = s.toJson();
    json d = json::object();
    for (const char* k : groupFields())
        d[k] = all[k];
    d["modifiers"] = s.highs.empty() || !s.highs.front().modifiers.is_array() ? json::array() : s.highs.front().modifiers;
    d["self"] = std::ranges::any_of(s.highs, [](const HighEntry& h) { return h.self; });
    return d;
}

json defaultStack(const json& defaults)
{
    return defaults.is_object() && defaults.contains("modifiers") && defaults["modifiers"].is_array() ? defaults["modifiers"] : json::array();
}

GroupSettings withGroupDefaults(GroupSettings s, const json& defaults)
{
    if (!defaults.is_object() || defaults.empty())
        return s;
    json j = s.toJson();
    for (const char* k : groupFields())
        if (defaults.contains(k))
            j[k] = defaults[k];
    s = GroupSettings::fromJson(j);
    const json stack = defaultStack(defaults);
    for (HighEntry& h : s.highs)
        if (h.modifiers.empty())
            h.modifiers = stack;
    if (defaults.value("self", false) && s.highs.empty())
        s.highs.push_back(HighEntry{.self = true, .modifiers = stack});
    return s;
}

GroupSettings rebaseGroupDefaults(GroupSettings s, const json& before, const json& after, bool force)
{
    if (!after.is_object() || after.empty())
        return s;
    const json old = before.is_object() ? before : json::object();
    const json base = GroupSettings{}.toJson();
    json j = s.toJson();
    for (const char* k : groupFields())
        if (after.contains(k) && (force || j[k] == old.value(k, base[k])))
            j[k] = after[k];
    s = GroupSettings::fromJson(j);
    if (after.contains("modifiers"))
    {
        const json oldStack = defaultStack(old);
        for (HighEntry& h : s.highs)
            if (force || h.modifiers == oldStack)
                h.modifiers = defaultStack(after);
    }
    if (after.value("self", false) && s.highs.empty())
        s.highs.push_back(HighEntry{.self = true, .modifiers = defaultStack(after)});
    return s;
}

fs::path saveTarget(const TargetProfile& t, const std::string& scope, bool overwrite)
{
    if (t.name.empty())
        throw std::invalid_argument("a preset needs a name");
    if (const auto existing = findTarget(t.name); existing && isBuiltinTarget(*existing) && !overwrite)
        throw std::invalid_argument(t.name + " is a built-in target; pick another name, or overwrite=True to shadow it");
    const fs::path dir = scopeDir(scope);
    const fs::path file = dir / (fileSafe(t.name) + ".json");
    std::error_code ec;
    if (fs::exists(file, ec) && !overwrite)
        throw std::invalid_argument("a preset named " + t.name + " already exists (" + paths::toUtf8(file) + "); overwrite=True replaces it");
    fs::create_directories(dir, ec);
    json j = t.toJson();
    j.erase("file");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << j.dump(2) << '\n';
    if (!out)
        throw std::invalid_argument("can't write " + paths::toUtf8(file));
    return file;
}

fs::path deleteTarget(const std::string& name, const std::string& scope)
{
    std::optional<TargetProfile> victim;
    if (scope.empty())
        victim = findTarget(name);
    else
        for (TargetProfile& t : targetsIn(scopeDir(scope)))
            if (t.name == name || t.file.stem().string() == name)
                victim = std::move(t);
    if (!victim)
        throw std::invalid_argument("no " + (scope.empty() ? std::string() : scope + " ") + "target named " + name);
    if (isBuiltinTarget(*victim))
        throw std::invalid_argument(name + " is built in and can't be deleted");
    std::error_code ec;
    if (!fs::remove(victim->file, ec))
        throw std::invalid_argument("can't delete " + paths::toUtf8(victim->file) + (ec ? ": " + ec.message() : std::string()));
    return victim->file;
}
} // namespace lila::bake
