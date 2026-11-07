#include "ui/MenuSets.h"

#include "core/PrefStore.h"
#include "ui/Menus.h"

#include <QAction>

#include <algorithm>

namespace lila::ui::menuSets
{
namespace
{
constexpr const char* kHiddenProperty = "lilaHiddenByMenuSet";
} // namespace

bool enabled()
{
    return !PrefStore::get().value<bool>("general", kAllMenusPref, false);
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
