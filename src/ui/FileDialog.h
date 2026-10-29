#pragma once

#include "core/Export.h"

#include <QDialog>
#include <QStringList>

#include <memory>

namespace lila::ui
{
/**
 * Lila's own file browser, the same on every platform: the current project, bookmarks, recent folders, drives with usage, a
 * breadcrumb path bar and type-to-filter. The fileDialog:: functions show it, or the system dialog when the
 * kNativePref preference is on.
 */
class LILA_UI_API FileDialog : public QDialog
{
public:
    enum class Mode
    {
        OpenFile,
        OpenFiles,
        SaveFile,
        Directory,
    };

    /** `filter` takes QFileDialog's form, "Images (*.png *.jpg);;All files (*)". `start` is a folder or a file in one. */
    FileDialog(QWidget* parent, Mode mode, const QString& caption, const QString& start, const QString& filter);
    ~FileDialog() override;

    /** Shows `dir`; an empty path shows the drives. False when it isn't a folder. */
    bool navigate(const QString& dir);
    QString currentDir() const;
    void setFileName(const QString& name);
    QString fileName() const;
    void setNameFilter(const QString& text);
    /** The type filter, one of the entries of the filter string. */
    QString selectedFilter() const;
    void selectFilter(const QString& filter);
    /** Folders and files the current view lists after filtering. */
    int folderCount() const;
    int fileCount() const;
    /** The chosen paths once accepted. */
    QStringList selectedFiles() const;
    /** Validates the name field and accepts, as the Open/Save button does; false when the dialog stays open. */
    bool tryAccept();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

namespace fileDialog
{
/** On, the functions below show the platform's dialog instead of Lila's. */
inline constexpr const char* kNativePref = "/fileDialog/native";
/** Hidden files and dot-files in the list; off by default. */
inline constexpr const char* kShowHiddenPref = "/fileDialog/showHidden";

LILA_UI_API QString getOpenFileName(QWidget* parent, const QString& caption = {}, const QString& dir = {},
                                    const QString& filter = {}, QString* selectedFilter = nullptr);
LILA_UI_API QStringList getOpenFileNames(QWidget* parent, const QString& caption = {}, const QString& dir = {},
                                         const QString& filter = {}, QString* selectedFilter = nullptr);
LILA_UI_API QString getSaveFileName(QWidget* parent, const QString& caption = {}, const QString& dir = {},
                                    const QString& filter = {}, QString* selectedFilter = nullptr);
LILA_UI_API QString getExistingDirectory(QWidget* parent, const QString& caption = {}, const QString& dir = {});
} // namespace fileDialog

/**
 * The file dialog's file operations. Files tracked by Perforce or git move and delete through source control so their
 * history follows; everything else goes through the file system, and deleting only ever sends to the Recycle Bin or
 * Trash. Each group can be turned off in prefs, or locked off from a studio folder.
 */
namespace fileOps
{
/** Rename and new folder. */
inline constexpr const char* kAllowEditPref = "/fileDialog/allowEdit";
/** Cut, copy, paste and drag-and-drop. */
inline constexpr const char* kAllowMovePref = "/fileDialog/allowMove";
inline constexpr const char* kAllowDeletePref = "/fileDialog/allowDelete";

LILA_UI_API bool allowed(const char* pref);
/** Renames or moves one file or folder to `to`, which must not exist. Returns an error, empty on success. */
LILA_UI_API QString move(const QString& from, const QString& to);
/** Moves into dir, keeping names. Returns the new paths; problems are joined into *error. */
LILA_UI_API QStringList moveInto(const QStringList& paths, const QString& dir, QString* error);
/** Copies into dir, as "name (2)" when the name is taken. Returns the new paths; problems are joined into *error. */
LILA_UI_API QStringList copyInto(const QStringList& paths, const QString& dir, QString* error);
/** How many of paths (folders counted by their files) source control tracks. */
LILA_UI_API int trackedCount(const QStringList& paths);
/** Tracked files become pending deletes in source control; the rest go to the Recycle Bin or Trash. */
LILA_UI_API QString remove(const QStringList& paths);
/** Shows the file selected in Explorer on Windows; elsewhere, for a missing file, or when another file manager owns folders, opens its folder. */
LILA_UI_API void reveal(const QString& file);
} // namespace fileOps
} // namespace lila::ui
