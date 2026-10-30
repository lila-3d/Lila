#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <QDialog>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>

namespace lila::ui
{
namespace fileDialog
{
struct Context;
}

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
    const fileDialog::Context& context() const;
    /** The side panel's section titles and the paths one lists, in order. */
    QStringList sectionTitles() const;
    QStringList sectionPaths(const QString& title) const;
    /** As clicking a side panel item, or double-clicking it when `accept`. */
    void pickItem(const QString& path, bool accept);

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

/** What a dialog was opened for. Recent files are kept per key, and side sections read it to decide what to list. */
struct Context
{
    /** "import", "export", "scene"; the caption when no ContextScope is active. */
    QString key;
    /** Plural noun for the recent files section, "Imports" makes "Recent Imports". */
    QString noun;
    QString caption;
    FileDialog::Mode mode = FileDialog::Mode::OpenFile;
    /** The filter string the dialog was given. */
    QString filter;
};

/** Gives the dialogs shown while it lives a context key, so several call sites share one recent list. */
class LILA_UI_API ContextScope
{
public:
    ContextScope(QString key, QString noun);
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    QString mKey, mNoun;
};

/** One row in a side section. Clicking a file shows it in its folder and picks it; double-clicking accepts it. */
struct Item
{
    QString path;
    /** Empty shows the file name. */
    QString label;
    QString tooltip;
    /** Icon name; empty picks one from the path. */
    QString icon;
};

/**
 * A collapsible section in the panel on the dialog's right, built each time a dialog opens. `items` returns nothing
 * to leave the section out for that context. `%1` in the title becomes the context's noun. Sections an extension adds
 * while loading are removed when it unloads.
 */
struct Section
{
    QString title;
    std::function<QList<Item>(const Context&)> items;
};
LILA_UI_API Registry<Section>& sections();
LILA_UI_API void addSection(std::string id, Section section, std::string owner = kLilaOwner);

/** Files accepted in dialogs with this context key, newest first, missing ones included. */
LILA_UI_API QStringList recentFiles(const QString& key);
/** Records a file as if a dialog with this key had picked it, for files chosen another way (drag and drop). */
LILA_UI_API void addRecentFile(const QString& key, const QString& path);
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
