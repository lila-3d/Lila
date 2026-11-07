#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class QWidget;

/**
 * What the viewport header shows, as data the user edits: {"left": [...], "right": [...]}. An entry is a built-in
 * item's id ("componentMode", "snap"...), "|" for a separator, {"action": id} for a command button, or
 * {"menu": title, "icon": name, "items": [action ids, "|"]} for a menu of commands. Saved in the general prefs.
 */
namespace lila::ui::viewportHeader
{
inline constexpr const char* kPref = "/viewport/header";
inline constexpr const char* kSeparator = "|";

struct Builtin
{
    const char* id;
    const char* label;
    const char* icon;
    const char* doc;
};

/** The items only the header can draw (segmented strips, popovers), in the editor's order. */
LILA_UI_API const std::vector<Builtin>& builtins();
LILA_UI_API const Builtin* builtin(const std::string& id);

LILA_UI_API nlohmann::json defaultConfig();
/** The saved layout, or the default. */
LILA_UI_API nlohmann::json config();
LILA_UI_API void setConfig(const nlohmann::json& config);

/** Display name and icon of an entry, for the editor and for menus. */
LILA_UI_API std::string entryLabel(const nlohmann::json& entry);
LILA_UI_API std::string entryIcon(const nlohmann::json& entry);

/** Opens, or raises, the bar editor on a bar (the viewport header when empty; an icon bar's name otherwise). */
LILA_UI_API void openEditor(QWidget* anchor = nullptr, const std::string& bar = {});

/** viewport.customizeHeader, window.customizeToolBox and viewport.resetHeader; installs bar presets. */
LILA_UI_API void registerActions();
} // namespace lila::ui::viewportHeader
