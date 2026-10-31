#include "ui/MenuSets.h"

#include "core/PrefStore.h"
#include "ui/Menus.h"

#include <QAction>

#include <algorithm>
#include <cctype>
#include <utility>

namespace lila::ui::menuSets
{
namespace
{
constexpr const char* kHiddenProperty = "lilaHiddenByMenuSet";

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
} // namespace

std::string forShelf(const std::string& shelf)
{
    json custom = PrefStore::get().value("general", "/menuSets/shelves", json::object());
    if (custom.is_object() && custom.contains(shelf) && custom[shelf].is_string())
        return custom[shelf].get<std::string>();

    static const std::pair<const char*, const char*> kShelves[] = {
        {"general", "Modeling"}, {"modeling", "Modeling"}, {"clay", "Modeling"},    {"sculpt", "Modeling"},
        {"curves", "Modeling"},  {"uv", "Modeling"},       {"rigging", "Rigging"},  {"rig", "Rigging"},
        {"animation", "Animation"}, {"puppeteer", "Animation"}, {"level", "Level"}, {"effects", "Level"},
    };
    std::string key = lower(shelf);
    for (const auto& [name, set] : kShelves)
        if (key == name)
            return set;
    for (const char* set : {"Modeling", "Rigging", "Animation", "Level"})
        if (key == lower(set))
            return set;
    return {};
}

bool enabled()
{
    return PrefStore::get().value<bool>("general", "/menuSets/enabled", true);
}

bool shows(const menus::Menu& menu, const std::string& set)
{
    if (!enabled() || menu.sets.empty())
        return true;
    return std::find(menu.sets.begin(), menu.sets.end(), set) != menu.sets.end();
}

void setHiddenBySet(QAction* row, bool hidden)
{
    row->setProperty(kHiddenProperty, hidden);
    row->setVisible(!hidden);
}

bool reachable(const QAction* row)
{
    return row->isVisible() || row->property(kHiddenProperty).toBool();
}
} // namespace lila::ui::menuSets
