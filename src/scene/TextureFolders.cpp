#include "scene/TextureFolders.h"

#include "ops/Command.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/TextMatch.h"
#include "core/Workspace.h"
#include "scene/SceneData.h"

#include <algorithm>

namespace lila::scene::textureFolders
{
namespace
{
bool isList(const json& j)
{
    if (!j.is_array())
        return false;
    return std::ranges::all_of(j, [](const json& v) { return v.is_string(); });
}

std::vector<std::string> strings(const json& j)
{
    std::vector<std::string> out;
    for (const json& v : j)
        if (std::string s = v.get<std::string>(); !s.empty())
            out.push_back(std::move(s));
    return out;
}

/** Trims entries and drops empty ones, so a pasted list with blank lines stores cleanly. */
json cleanList(const json& j)
{
    json out = json::array();
    if (!j.is_array())
        return out;
    for (const json& v : j)
    {
        if (!v.is_string())
            continue;
        std::string s = v.get<std::string>();
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b != std::string::npos)
            out.push_back(s.substr(b, e - b + 1));
    }
    return out;
}

json describe()
{
    Rules r = current();
    return {{"root", paths::toUtf8(projectTextures::projectRoot())},
            {"include", r.filter.include},
            {"exclude", r.filter.exclude},
            {"includeFrom", r.includeFrom},
            {"excludeFrom", r.excludeFrom},
            {"workspace", workspaceRules()},
            {"scene", sceneRules()}};
}
} // namespace

Rules resolve(const json& workspace, const json& scene)
{
    Rules r;
    auto pick = [&](const char* key, std::vector<std::string>& out, std::string& from) {
        if (scene.is_object() && scene.contains(key) && isList(scene[key]))
        {
            out = strings(scene[key]);
            from = "scene";
        }
        else if (workspace.is_object() && workspace.contains(key) && isList(workspace[key]))
        {
            out = strings(workspace[key]);
            from = "workspace";
        }
    };
    pick("include", r.filter.include, r.includeFrom);
    pick("exclude", r.filter.exclude, r.excludeFrom);
    return r;
}

json workspaceRules()
{
    json out = json::object();
    json inc = PrefStore::get().value(kPrefFile, kIncludeKey);
    json exc = PrefStore::get().value(kPrefFile, kExcludeKey);
    if (isList(inc))
        out["include"] = inc;
    if (isList(exc))
        out["exclude"] = exc;
    return out;
}

json sceneRules()
{
    json s = data::get(kSceneKey);
    json out = json::object();
    if (!s.is_object())
        return out;
    for (const char* key : {"include", "exclude"})
        if (s.contains(key) && isList(s[key]))
            out[key] = s[key];
    return out;
}

Rules current()
{
    return resolve(workspaceRules(), sceneRules());
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "textureFolders",
        .doc = "Which project images the material editor's texture lists show. Globs relative to the project folder "
               "(the workspace root, else the scene's folder): `*` within a folder name, `**` across folders, a "
               "pattern without '/' matches at any depth, and a folder pattern takes everything under it, e.g. "
               "Textures/**, **/*_old.*, Saved. Empty include takes everything; exclude wins. Kept in the "
               "workspace's .lila/settings.json, or with scene=True in the scene, where each list overrides the "
               "workspace's. With no flags returns the effective lists, where each came from and both levels.",
        .flags = {
            {"include", "i", T::List, nullptr, "Include globs; [] clears them."},
            {"exclude", "e", T::List, nullptr, "Exclude globs; [] clears them."},
            {"scene", "s", T::Bool, false, "Set the scene's override instead of the workspace's lists."},
            {"clear", "c", T::Bool, false, "Remove the lists at that level (with scene, the scene falls back to the workspace)."},
        },
        .run = [](const Args& a) -> json {
            bool scene = a.get<bool>("scene");
            if (scene)
            {
                json s = data::get(kSceneKey);
                if (!s.is_object() || a.get<bool>("clear"))
                    s = json::object();
                if (a.has("include"))
                    s["include"] = cleanList(a.get<json>("include"));
                if (a.has("exclude"))
                    s["exclude"] = cleanList(a.get<json>("exclude"));
                data::set(kSceneKey, s.empty() ? json(nullptr) : s);
            }
            else
            {
                PrefScope scope = Workspace::get().isOpen() ? PrefScope::Workspace : PrefScope::User;
                if (a.get<bool>("clear"))
                {
                    PrefStore::get().remove(kPrefFile, kIncludeKey, scope);
                    PrefStore::get().remove(kPrefFile, kExcludeKey, scope);
                }
                if (a.has("include"))
                    PrefStore::get().set(kPrefFile, kIncludeKey, cleanList(a.get<json>("include")), scope);
                if (a.has("exclude"))
                    PrefStore::get().set(kPrefFile, kExcludeKey, cleanList(a.get<json>("exclude")), scope);
            }
            return describe();
        },
    });

    ops::registerCommand({
        .name = "projectTextures",
        .doc = "Image files (png, tga, jpg, exr, tif, psd, hdr, bmp, dds) under the project folder that pass "
               "textureFolders, as paths relative to it, sorted. Hidden folders (.lila, .git) and build caches are "
               "never listed. The folder is scanned in the background and rescanned when files change; this waits "
               "for a scan in flight.",
        .flags = {
            {"all", "a", T::Bool, false, "Ignore the include and exclude lists."},
            {"match", "m", T::String, "", "Only paths containing every word of this text."},
            {"absolute", "abs", T::Bool, false, "Return absolute paths."},
            {"refresh", "r", T::Bool, false, "Rescan the folder first."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto& index = projectTextures::Index::get();
            index.follow();
            if (a.get<bool>("refresh"))
                index.refresh();
            projectTextures::Index::Snapshot snap = index.snapshot(true);
            std::vector<std::string> files = a.get<bool>("all") ? snap.files : current().filter.apply(snap.files);
            textMatch::Query query(a.get<std::string>("match"));
            json out = json::array();
            for (const std::string& f : files)
                if (query.matches({f}))
                    out.push_back(a.get<bool>("absolute") ? paths::toUtf8(snap.root / paths::fromUtf8(f)) : f);
            return out;
        },
    });
}
} // namespace lila::scene::textureFolders
