#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <QDialog>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lila::ui
{
/**
 * Preferences pages, Lila's own included: one tab each in the Preferences window, in registration order. Rows made
 * through the PageBuilder are found by the window's search like the built-in ones, and write their pref the moment
 * they change.
 */
namespace prefPages
{
class LILA_UI_API PageBuilder
{
public:
    explicit PageBuilder(void* page) : mPage(page) {}

    /** Starts a collapsible section; rows after it sit under its header. */
    void section(const std::string& title);
    void toggle(const std::string& label, const std::string& key, bool fallback);
    void number(const std::string& label, const std::string& key, double fallback, double lo, double hi, double step, int decimals = 2);
    /** options: (shown text, stored value) pairs. */
    void choice(const std::string& label, const std::string& key, const std::vector<std::pair<std::string, std::string>>& options,
                const std::string& fallback);
    void text(const std::string& label, const std::string& key, const std::string& fallback, bool fileButton = false);
    /** sRGB "#rrggbb". */
    void color(const std::string& label, const std::string& key, const std::string& fallback);
    /** Any widget as a row; key (a pref pointer) is searched along with the label. */
    void row(const std::string& label, QWidget* field, const std::string& key = {});
    /** The prefs file the page's keys are in. */
    std::string file() const;

    void* mPage; // the window's own page, opaque outside it
};

struct Page
{
    std::string id;
    std::string title;
    std::string file = "general"; // the prefs file its keys are in
    std::function<void(PageBuilder&)> build;
};

LILA_UI_API Registry<Page>& registry();
LILA_UI_API void add(Page page, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t add(ext::Context& ctx, Page page);
LILA_UI_API bool remove(const std::string& id);
} // namespace prefPages

/**
 * Settings window. Every control writes its pref the moment it changes (prefs are live), so there's no OK/Apply:
 * close it when done. Anything not shown here is still editable in the JSON files.
 */
class LILA_UI_API PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    static void open(QWidget* parent);

private:
    /** Backs up the user's prefs for every page's file, then clears them so the defaults apply. */
    void resetAll();
    void restoreBackup();
    /** Reopens the window in place so every field shows the prefs as they now are. */
    void rebuild();

    /** The prefs files the pages edit. */
    std::set<std::string> mFiles;
};
} // namespace lila::ui
