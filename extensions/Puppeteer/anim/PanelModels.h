#pragma once

// What the Mixer and Anim Bank panels show, built from the listAnimLayers and listTakes results. No Qt, so the tests
// check it directly.

#include "KeyMath.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace puppeteer::anim
{
struct LayerRow
{
    std::string name;
    bool base = false;
    bool active = false;
    bool mute = false;
    bool solo = false;
    bool lock = false;
    double weight = 1.0;
    std::string mode = "additive";
    size_t members = 0;
};

/** The layer stack as the Mixer lists it: the top layer first, the base animation last. */
inline std::vector<LayerRow> mixerRows(const nlohmann::json& layers)
{
    std::vector<LayerRow> out;
    if (!layers.is_array())
        return out;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        const nlohmann::json& l = *it;
        LayerRow r;
        r.name = l.value("name", std::string());
        r.base = l.value("base", false);
        r.active = l.value("active", false);
        r.mute = l.value("mute", false);
        r.solo = l.value("solo", false);
        r.lock = l.value("lock", false);
        r.weight = l.value("weight", 1.0);
        r.mode = l.value("mode", std::string("additive"));
        r.members = l.contains("members") && l["members"].is_array() ? l["members"].size() : 0;
        out.push_back(r);
    }
    return out;
}

struct TakeRow
{
    std::string name;
    std::string root;
    std::string folder;
    bool active = false;
    bool loop = false;
    double start = 0.0, end = 0.0, fps = 30.0;
};

struct Folder
{
    std::string name;
    std::string path;
    std::vector<Folder> folders;
    std::vector<TakeRow> takes;
};

/**
 * The Anim Bank's tree: folders (sorted by name) before takes (in list order). With root set, only that character's
 * takes and whole-scene takes.
 */
inline Folder bankTree(const nlohmann::json& takes, const std::string& root = {})
{
    Folder top;
    if (!takes.is_array())
        return top;
    for (const nlohmann::json& t : takes)
    {
        TakeRow r;
        r.name = t.value("name", std::string());
        r.root = t.value("root", std::string());
        r.folder = normalFolder(t.value("folder", std::string()));
        r.active = t.value("active", false);
        r.loop = t.value("loop", false);
        r.start = t.value("start", 0.0);
        r.end = t.value("end", 0.0);
        r.fps = t.value("fps", 30.0);
        if (!root.empty() && !r.root.empty() && r.root != root)
            continue;
        Folder* f = &top;
        for (const std::string& path : folderChain(r.folder))
        {
            const std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
            auto it = std::find_if(f->folders.begin(), f->folders.end(), [&](const Folder& c) { return c.name == name; });
            if (it == f->folders.end())
            {
                f->folders.push_back({name, path, {}, {}});
                std::sort(f->folders.begin(), f->folders.end(), [](const Folder& a, const Folder& b) { return a.name < b.name; });
                it = std::find_if(f->folders.begin(), f->folders.end(), [&](const Folder& c) { return c.name == name; });
            }
            f = &*it;
        }
        f->takes.push_back(r);
    }
    return top;
}
} // namespace puppeteer::anim
