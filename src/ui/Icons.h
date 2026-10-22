#pragma once

#include <QIcon>
#include <QStringList>

namespace lila::ui
{
/**
 * A Tabler outline icon from resources/icons/tabler, rendered in the UI greys: light grey normally, brighter on hover
 * (QIcon::Active) and when checked, dim when disabled. Cached by name; a null icon when the name is unknown.
 */
QIcon icon(const QString& name);

/** Every bundled icon name, sorted. */
const QStringList& iconNames();
} // namespace lila::ui
