#pragma once

#include "core/Export.h"

#include <QString>
#include <QStringList>

namespace lila::ui::storage
{
/** Where a script editor tab saves to. */
enum class Location
{
    Unsaved,
    Scene,   // inside the scene file
    Project, // <workspace>/.lila/scripts
    File,    // anywhere on disk
    Global,  // <user prefs>/scripts
};

LILA_UI_API QString locationName(Location loc);
/** Tabler icon name for the tab. */
LILA_UI_API QString iconName(Location loc);

/** <workspace>/.lila/scripts, or empty when no workspace is open. Not created. */
LILA_UI_API QString projectDir();
/** <user prefs dir>/scripts. Not created. */
LILA_UI_API QString globalDir();

/** Project or Global for files under those folders, File otherwise; Unsaved for an empty path. */
LILA_UI_API Location locationOfPath(const QString& path, const QString& projectDir, const QString& globalDir);
LILA_UI_API Location locationOfPath(const QString& path);

/** A script name made safe as a file stem and scene key: trimmed, path separators and reserved characters to '_'. */
LILA_UI_API QString sanitizeName(const QString& name);

/** Script names (file stems) of the .py files in a folder, sorted. */
LILA_UI_API QStringList scriptsIn(const QString& dir);
} // namespace lila::ui::storage
