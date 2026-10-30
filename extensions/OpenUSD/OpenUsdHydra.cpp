// Hydra viewport renderers: one per render delegate on USD's plugin path, found by reading the plugInfo.json files, so
// listing them never loads Pixar's libraries. Picking one loads OpenUSDCore, which opens the session.

#include "OpenUsd.h"
#include "UsdBackend.h"

#include <lila/Log.h>
#include <lila/ViewportRenderers.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace lila;

namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

struct Delegate
{
    std::string plugin;
    std::string name;
};

bool wildcard(std::string_view pattern, std::string_view s)
{
    if (pattern.empty())
        return s.empty();
    if (pattern[0] == '*')
        return wildcard(pattern.substr(1), s) || (!s.empty() && wildcard(pattern, s.substr(1)));
    return !s.empty() && (pattern[0] == '?' || pattern[0] == s[0]) && wildcard(pattern.substr(1), s.substr(1));
}

/** The paths a plugInfo Includes entry names: '*' and '?' match within one path component. */
void expand(const fs::path& at, const std::vector<std::string>& parts, size_t i, std::vector<fs::path>& out)
{
    std::error_code ec;
    if (i == parts.size())
    {
        if (fs::exists(at, ec))
            out.push_back(at);
        return;
    }
    if (parts[i].find_first_of("*?") == std::string::npos)
    {
        expand(at / fs::path(parts[i]), parts, i + 1, out);
        return;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(at, ec))
        if (wildcard(parts[i], e.path().filename().string()))
            expand(e.path(), parts, i + 1, out);
}

/** plugInfo.json allows '#' comment lines, which JSON doesn't. */
json readPlugInfo(const fs::path& file)
{
    std::ifstream in(file);
    std::stringstream text;
    for (std::string line; std::getline(in, line);)
    {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] != '#')
            text << line << '\n';
    }
    return json::parse(text.str(), nullptr, false);
}

void scan(const fs::path& file, std::set<fs::path>& seen, std::map<std::string, Delegate>& out, int depth = 0)
{
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(file, ec);
    if (depth > 8 || !seen.insert(canonical).second)
        return;
    const json info = readPlugInfo(file);
    if (!info.is_object())
        return;
    if (const auto inc = info.find("Includes"); inc != info.end() && inc->is_array())
        for (const json& pattern : *inc)
        {
            if (!pattern.is_string())
                continue;
            std::string p = pattern.get<std::string>();
            const bool dir = !p.empty() && (p.back() == '/' || p.back() == '\\');
            std::vector<std::string> parts;
            for (const fs::path& part : fs::path(p))
                if (!part.empty() && part != "/" && part != "\\")
                    parts.push_back(part.string());
            std::vector<fs::path> found;
            expand(fs::path(p).is_absolute() ? fs::path(p).root_path() : file.parent_path(), parts, 0, found);
            for (const fs::path& f : found)
                scan(dir || fs::is_directory(f, ec) ? f / "plugInfo.json" : f, seen, out, depth + 1);
        }
    const auto plugins = info.find("Plugins");
    if (plugins == info.end() || !plugins->is_array())
        return;
    for (const json& plugin : *plugins)
    {
        const json types = plugin.value("Info", json::object()).value("Types", json::object());
        if (!types.is_object())
            continue;
        for (const auto& [type, desc] : types.items())
        {
            const json bases = desc.is_object() ? desc.value("bases", json::array()) : json::array();
            if (std::find(bases.begin(), bases.end(), json("HdRendererPlugin")) == bases.end())
                continue;
            // Storm calls itself "GL", from before it ran on Vulkan and Metal too.
            std::string name = type == "HdStormRendererPlugin" ? "Storm" : desc.value("displayName", type);
            out.emplace(type, Delegate{type, name});
        }
    }
}

/** Every render delegate on the plugin path: the package's own plugins, then PXR_PLUGINPATH_NAME's. */
std::vector<Delegate> delegates(const fs::path& packageRoot)
{
    std::vector<fs::path> roots = {packageRoot / "plugin" / "usd", packageRoot / "usd"};
    if (const char* env = std::getenv("PXR_PLUGINPATH_NAME"))
    {
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::stringstream list(env);
        for (std::string entry; std::getline(list, entry, sep);)
            if (!entry.empty())
                roots.push_back(fs::path(entry));
    }
    std::set<fs::path> seen;
    std::map<std::string, Delegate> found;
    for (const fs::path& root : roots)
    {
        std::error_code ec;
        if (fs::is_directory(root, ec))
        {
            if (fs::exists(root / "plugInfo.json", ec))
                scan(root / "plugInfo.json", seen, found);
            for (const fs::directory_entry& e : fs::directory_iterator(root, ec))
                for (const fs::path& f : {e.path() / "plugInfo.json", e.path() / "resources" / "plugInfo.json"})
                    if (fs::exists(f, ec))
                        scan(f, seen, found);
        }
        else if (fs::exists(root, ec))
            scan(root, seen, found);
    }
    std::vector<Delegate> out;
    for (auto& [type, d] : found)
        out.push_back(std::move(d));
    std::stable_partition(out.begin(), out.end(), [](const Delegate& d) { return d.plugin == "HdStormRendererPlugin"; });
    return out;
}

std::string idOf(const Delegate& d)
{
    std::string id = "hydra";
    for (char c : d.name)
        if (std::isalnum(static_cast<unsigned char>(c)))
            id += c;
    return id == "hydra" ? "hydra" + d.plugin : id;
}
} // namespace

namespace lila::usd
{
void registerHydra(ext::Context& ctx)
{
    namespace vr = ui::viewportRenderers;
    for (const Delegate& d : delegates(ctx.root()))
    {
        const std::string doc =
            d.plugin == "HdStormRendererPlugin"
                ? "Pixar's Storm through Hydra, on Vulkan: the scene's meshes, materials (as UsdPreviewSurface), lights and cameras, "
                  "and the prims of live USD stages Lila has no objects for, drawn as usdview draws them."
                : "The " + d.name + " Hydra render delegate (" + d.plugin + "): the scene and live USD stages, refined progressively.";
        vr::add(ctx, {.id = idOf(d),
                      .name = "Hydra: " + d.name,
                      .doc = doc,
                      .open = [d](int panel) -> std::unique_ptr<vr::Session> {
                          auto open = reinterpret_cast<BackendHydraOpen>(backendSymbol("lilaHydraOpen"));
                          if (!open)
                              throw CommandError("Hydra: OpenUSDCore has no Hydra support");
                          std::string error;
                          void* session = open(d.plugin.c_str(), panel, &error);
                          if (!session)
                          {
                              log::error("Hydra: {}: {}", d.name, error);
                              return nullptr;
                          }
                          return std::unique_ptr<vr::Session>(static_cast<vr::Session*>(session));
                      }});
    }
}
} // namespace lila::usd
