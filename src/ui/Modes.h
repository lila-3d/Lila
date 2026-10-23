#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

class QWidget;

/**
 * Modes: a named bundle of the UI a kind of work wants (Puppeteer's animation mode, say). Entering one applies its
 * layout, switches on its hotkey layer and marking-menu set, shows its shelf and names the gizmo provider and selection
 * handler the viewport should use, then runs onEnter; leaving runs onExit. Lila's own is "lila". Switching is a UI
 * action (mode.<id>), never an undo step, and sends events::Event::ModeChanged.
 */
namespace lila::ui::modes
{
inline constexpr const char* kDefaultMode = "lila";

struct Mode
{
    std::string id;
    std::string label;
    std::string doc;
    std::string icon; // Tabler name
    std::string layout; // applied on entering; empty leaves the panels alone
    /**
     * The mode's hotkey layer, shaped like the hotkeys prefs' "contexts". It sits over the contexts it names while the
     * mode is on, slot by slot, so {"viewport": {"Space": {"press": "graph.fullScreen"}}} takes Space's tap and leaves
     * its hold (the hotbox) alone. Registered as hotkeyLayers layer "mode.<id>".
     */
    nlohmann::json hotkeys = nlohmann::json::object();
    std::string markingMenuSet; // see markingMenus: "<set>/<name>" menus win while the mode is on
    std::string shelf;          // shown in the docked shelf on entering
    std::string gizmoProvider;  // the viewport's gizmo provider; empty for the default
    std::string selectionHandler; // the viewport's selection handler; empty for the default
    std::function<void()> onEnter;
    std::function<void()> onExit;
};

LILA_UI_API Registry<Mode>& registry();
/** Adds or replaces, with a mode.<id> action under the same owner. */
LILA_UI_API void add(Mode mode, std::string owner = kLilaOwner);
/** Owned by the extension; unloading it leaves the mode first when it is on. */
LILA_UI_API uint64_t add(ext::Context& ctx, Mode mode);
/** Leaves the mode first when it is on. */
LILA_UI_API bool remove(const std::string& id);

/**
 * Leaves the current mode and enters id. restoring: entering the mode Lila was last in at startup, which keeps the
 * window's own restored layout instead of applying the mode's. False for an unknown id.
 */
LILA_UI_API bool activate(const std::string& id, bool restoring = false);
LILA_UI_API const std::string& current();
/** Null only before the built-in mode is registered. */
LILA_UI_API const Mode* currentMode();

/** The "lila" mode, mode.next / mode.previous, and the mode command. Once; batch mode gets them too. */
LILA_UI_API void registerBuiltins();

/** Enters the mode Lila was in when it last closed (general pref /ui/mode), or the default. */
LILA_UI_API void restore();

/** A compact mode picker for the status bar; follows registrations and switches made elsewhere. */
LILA_UI_API QWidget* createSwitcher(QWidget* parent);
} // namespace lila::ui::modes
