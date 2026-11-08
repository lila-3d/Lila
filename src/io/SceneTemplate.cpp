#include "io/SceneTemplate.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <nlohmann/json.hpp>

namespace lila::io::sceneTemplate
{
using json = nlohmann::json;

fs::path path()
{
    const std::string p = PrefStore::get().value<std::string>("general", kPref, "");
    return p.empty() ? fs::path() : paths::fromUtf8(p);
}

fs::path userPath()
{
    return paths::userConfigDir() / "templates" / "new-scene.lila";
}

bool apply()
{
    const fs::path p = path();
    if (p.empty())
        return false;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
    {
        log::warn("New Scene template {} is missing; started an empty scene", paths::toUtf8(p));
        return false;
    }
    try
    {
        loadVersion(p, p, false, "untitled");
        markClean();
        return true;
    }
    catch (const std::exception& e)
    {
        scene::Graph::current().clear();
        markClean();
        log::warn("New Scene template {} could not be read ({}); started an empty scene", paths::toUtf8(p), e.what());
        return false;
    }
}

fs::path saveCurrent()
{
    const fs::path p = userPath();
    fs::create_directories(p.parent_path());
    saveCopy(p);
    PrefStore::get().set("general", kPref, paths::toUtf8(p));
    return p;
}

void clear()
{
    PrefStore::get().remove("general", kPref);
    std::error_code ec;
    fs::remove(userPath(), ec);
}

void registerCommands()
{
    ops::registerCommand({
        .name = "saveSceneTemplate",
        .doc = "Save the scene as the New Scene template: File > New and launch start from a copy of it, untitled. Later "
               "changes to this scene do not change the template. Not undoable.",
        .undoable = false,
        .run = [](const Args&) -> json { return paths::toUtf8(saveCurrent()); },
    });
    ops::registerCommand({
        .name = "clearSceneTemplate",
        .doc = "Remove your New Scene template, so new scenes start from the studio's or workspace's template, or empty. "
               "Not undoable.",
        .undoable = false,
        .run = [](const Args&) -> json {
            clear();
            const fs::path p = path();
            return p.empty() ? json() : json(paths::toUtf8(p));
        },
    });
    ops::registerCommand({
        .name = "sceneTemplate",
        .doc = "The New Scene template's path, or None when new scenes start empty.",
        .undoable = false,
        .run = [](const Args&) -> json {
            const fs::path p = path();
            return p.empty() ? json() : json(paths::toUtf8(p));
        },
    });
}
} // namespace lila::io::sceneTemplate
