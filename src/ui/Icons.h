#pragma once

#include "core/Export.h"

#include <QIcon>
#include <QStringList>

namespace lila::ui
{
/**
 * A Tabler outline icon from resources/icons/tabler, in the theme's icon colours: iconNormal, iconActive on hover
 * (QIcon::Active) and when checked, iconDisabled when disabled. Cached by name; a null icon when the name is unknown.
 */
LILA_UI_API QIcon icon(const QString& name);

/** Every bundled icon name, sorted. */
LILA_UI_API const QStringList& iconNames();
} // namespace lila::ui
