#pragma once

#include "core/Export.h"
#include "ui/Docs.h"

#include <optional>
#include <string>

namespace lila::ui::docs
{
inline constexpr const char* kDocsDock = "docs";

/** The Docs panel, the help.docs / help.updateDocs actions and the openDocs, docsUpdate and docsInfo commands. */
LILA_UI_API void registerDocsUi();

/** Refreshes the cached docs in the background when the docs/autoUpdate pref allows; never blocks or prompts. */
LILA_UI_API void startAutoUpdate();
/** Fetches now; forced ones report to the status line. */
LILA_UI_API void requestUpdate(bool forced);
/** Stops a running fetch and the local server. Call before the application goes away. */
LILA_UI_API void shutdown();

/** The copy being read, if any. */
LILA_UI_API std::optional<Root> currentRoot();

/** Where an open goes. Auto goes to the web browser while Shift is held, so Shift+click on any docs entry point does. */
enum class Route
{
    Auto,
    Panel,
    Browser,
};

/** Overrides Auto for the opens made while it lives, for entry points where Shift already means something else. */
class LILA_UI_API RouteScope
{
public:
    explicit RouteScope(Route route);
    ~RouteScope();
    RouteScope(const RouteScope&) = delete;
    RouteScope& operator=(const RouteScope&) = delete;

private:
    Route mPrevious;
};

/** Shows the Docs panel at a page id ("essentials", "curves?id=pipe", "python/"); empty for home. */
LILA_UI_API void open(const std::string& page = {}, Route route = Route::Auto);
/** Opens the API page for a Python symbol. False, doing nothing, when the docs have no API section. */
LILA_UI_API bool openApi(const std::string& symbol, Route route = Route::Auto);
/** Opens a page in the web browser: the public site, or the local copy served on 127.0.0.1 when that can't be reached. */
LILA_UI_API void openInBrowser(const std::string& page);
} // namespace lila::ui::docs
