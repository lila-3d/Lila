#include "ui/ExtensionUi.h"

#include "core/Events.h"
#include "core/Log.h"
#include "extensions/ExtensionManager.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Hotkeys.h"
#include "ui/Layouts.h"
#include "ui/MarkingMenu.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/OptionBox.h"
#include "ui/Panels.h"
#include "ui/PreferencesDialog.h"
#include "ui/Shelf.h"

#include <fstream>
#include <sstream>

namespace lila::ui::extensionUi
{
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
json readJson(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false, true);
    if (j.is_discarded() || !j.is_object())
    {
        log::warn("extension resource {} is not a JSON object", path.string());
        return json();
    }
    return j;
}

std::string utf8(const fs::path& p)
{
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

/** Each .json under dir, with its name relative to dir and without the extension. */
template <class Fn> int eachJson(const fs::path& dir, bool recursive, Fn fn)
{
    int n = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return 0;
    auto visit = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file() || e.path().extension() != ".json")
            return;
        json j = readJson(e.path());
        if (j.is_null())
            return;
        fs::path rel = fs::relative(e.path(), dir, ec);
        fn(utf8(rel.replace_extension()), std::move(j));
        ++n;
    };
    if (recursive)
        for (auto& e : fs::recursive_directory_iterator(dir, ec))
            visit(e);
    else
        for (auto& e : fs::directory_iterator(dir, ec))
            visit(e);
    return n;
}

/** Before the registries are swept: leaving an owner's active mode runs its onExit while its actions and layers exist. */
const uint64_t kModesFirst = owners::addCleanup([](std::string_view owner) {
    size_t n = 0;
    for (const std::string& id : modes::registry().owned(owner))
        n += modes::remove(id) ? 1 : 0;
    return n;
});
} // namespace

int loadResources(const std::string& owner, const fs::path& root)
{
    const fs::path res = root / "resources";
    int n = 0;
    n += eachJson(res / "markingMenus", true, [&](const std::string& name, json def) { markingMenus::add(name, std::move(def), owner); });
    n += eachJson(res / "shelves", false, [&](const std::string& name, json def) {
        shelves::add({name, def.value("items", json::array())}, owner);
    });
    n += eachJson(res / "layouts", false, [&](const std::string& name, json def) { layouts::add(name, std::move(def), owner); });
    n += eachJson(res / "hotkeys", false, [&](const std::string& name, json def) {
        hotkeyLayers::add({owner + "." + name, def.value("contexts", json::object()), def.value("mode", std::string())}, owner);
    });
    return n;
}

size_t removeOwned(const std::string& owner)
{
    if (owner.empty() || owner == kLilaOwner)
        return 0;
    return owners::removeOwned(owner);
}

void install()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (const extensions::ExtensionInfo& info : extensions::ExtensionManager::get().available())
        if (info.loaded && info.manifest)
            loadResources(info.name, info.path);
    events::subscribe(events::Event::ExtensionLoaded, [](const events::EventData& e) {
        std::error_code ec;
        if (fs::is_directory(e.path, ec))
            loadResources(e.name, e.path);
    });
    events::subscribe(events::Event::ExtensionUnloaded, [](const events::EventData& e) { removeOwned(e.name); });
}
} // namespace lila::ui::extensionUi
