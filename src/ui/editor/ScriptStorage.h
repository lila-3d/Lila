#pragma once

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

QString locationName(Location loc);
/** Tabler icon name for the tab. */
QString iconName(Location loc);

/** <workspace>/.lila/scripts, or empty when no workspace is open. Not created. */
QString projectDir();
/** <user prefs dir>/scripts. Not created. */
QString globalDir();

/** Project or Global for files under those folders, File otherwise; Unsaved for an empty path. */
Location locationOfPath(const QString& path, const QString& projectDir, const QString& globalDir);
Location locationOfPath(const QString& path);

/** A script name made safe as a file stem and scene key: trimmed, path separators and reserved characters to '_'. */
QString sanitizeName(const QString& name);

/** Script names (file stems) of the .py files in a folder, sorted. */
QStringList scriptsIn(const QString& dir);
} // namespace lila::ui::storage
