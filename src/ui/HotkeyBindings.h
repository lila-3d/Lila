#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QKeyCombination>
#include <QString>

#include <array>
#include <optional>
#include <string>
#include <vector>

class QKeyEvent;
class QWidget;

namespace lila::ui::hotkeys
{
/**
 * Read/write view of the "hotkeys" prefs for the editing UI. Hotkeys itself reloads on every prefs change, so all
 * writes here apply live.
 */
struct Binding
{
    enum class Origin
    {
        Default,
        Modified,
        User
    };

    std::string context;
    std::string keyText;   // as written in the JSON (PortableText)
    QKeyCombination chord; // Ctrl/Shift/Alt/Meta only
    nlohmann::json press, hold, release;
    std::array<nlohmann::json, 3> buttons; // key + LMB, MMB, RMB
    Origin origin = Origin::Default;
};

/** A default binding the user has nulled out. */
struct Unbound
{
    std::string context;
    std::string keyText;
    nlohmann::json defaultValue;
};

struct Snapshot
{
    std::vector<std::string> contexts; // "global" first
    std::vector<Binding> bindings;
    std::vector<Unbound> unbound;
};

enum class Slot
{
    Press,
    Hold,
    Release,
    Lmb,
    Mmb,
    Rmb
};

LILA_UI_API Snapshot snapshot();

/** Fills b's targets from a hotkeys.json value. */
LILA_UI_API void split(const nlohmann::json& value, Binding& b);
LILA_UI_API const nlohmann::json& slotTarget(const Binding& b, Slot slot);

LILA_UI_API std::optional<QKeyCombination> parse(const std::string& keyText);
LILA_UI_API std::string toText(QKeyCombination chord);
LILA_UI_API QString displayText(QKeyCombination chord);
LILA_UI_API QString keyName(int qtKey);

/** Chord for a key event, named by US physical position the way Hotkeys matches. Empty for a lone modifier. */
LILA_UI_API std::optional<QKeyCombination> chordFromEvent(QKeyEvent* e);

/** Focus widget's hotkeyContext chain, innermost first, ending in "global". */
LILA_UI_API std::vector<std::string> contextChain(QWidget* focus);

/** Bindings reachable from focus, with inner contexts shadowing outer ones. */
LILA_UI_API std::vector<const Binding*> active(const Snapshot& s, QWidget* focus);

LILA_UI_API const Binding* find(const Snapshot& s, const std::string& context, QKeyCombination chord);

/** "Undo", or "Pivot On / release: Pivot Off" for multi-slot bindings. */
LILA_UI_API std::string describe(const Binding& b);
LILA_UI_API std::string modeText(const Binding& b);
/** Action ids among the binding's targets. */
LILA_UI_API std::vector<std::string> actionIds(const Binding& b);
LILA_UI_API bool targets(const Binding& b, const std::string& actionId);

/** Puts target into one slot of the binding at chord, keeping the binding's other slots. */
LILA_UI_API void assign(const std::string& context, QKeyCombination chord, Slot slot, const nlohmann::json& target);

struct Assignment
{
    std::string context;
    QKeyCombination chord;
    Slot slot = Slot::Press;
    nlohmann::json target;
};
/** assign() for each, in order, written as one prefs change so the hotkeys reload once. */
LILA_UI_API void assignAll(const std::vector<Assignment>& list);
/** Removes the binding; a default gets a null override. */
LILA_UI_API void unbind(const std::string& context, const std::string& keyText);
/** Drops the user override so the default (or nothing) applies again. */
LILA_UI_API void resetToDefault(const std::string& context, const std::string& keyText);
} // namespace lila::ui::hotkeys
