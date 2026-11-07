#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

/**
 * Named sets of bar arrangements (the viewport header, the tool box, the outliner row) for switching between setups,
 * sharing them as files and binding one to a layout. A preset holds only the bars it changes; applying it puts the rest
 * back to Lila's default. Editing the bars while a saved preset is current updates that preset.
 */
namespace lila::ui::barPresets
{
inline constexpr const char* kHeader = "viewportHeader";
inline constexpr const char* kFileSuffix = ".lilabars";
/** The binding that applies Lila's default bars with a layout. */
inline constexpr const char* kDefaultBinding = ":default";

/** The bars a preset can hold, in editor order. */
LILA_UI_API const std::vector<std::string>& bars();
LILA_UI_API std::string barLabel(const std::string& bar);
/** The live arrangement of every bar that differs from the default: {bar: entries}. */
LILA_UI_API nlohmann::json capture();
/** Sets every bar from bars, defaulting the ones it leaves out. */
LILA_UI_API void applyBars(const nlohmann::json& bars);

LILA_UI_API std::vector<std::string> names();
/** The current preset, empty for Lila's default. */
LILA_UI_API std::string current();
/** Applies a saved preset, or the default for an empty name. False for an unknown name. */
LILA_UI_API bool activate(const std::string& name);
/** Saves the live bars as name and makes it current. */
LILA_UI_API void save(const std::string& name);
LILA_UI_API void remove(const std::string& name);
/** Writes the preset to a file. False on failure, with the reason in error. */
LILA_UI_API bool exportTo(const std::string& name, const std::filesystem::path& file, std::string& error);
/** Reads a preset file and saves it under its own name, made unique. Returns that name, or empty with error set. */
LILA_UI_API std::string importFrom(const std::filesystem::path& file, std::string& error);
/** Called after the user edits a bar: keeps the current preset in step. */
LILA_UI_API void edited();

/** The preset applied with a layout, or empty. */
LILA_UI_API std::string boundTo(const std::string& layout);
/** Binds a preset to a layout; an empty preset unbinds. */
LILA_UI_API void bind(const std::string& layout, const std::string& preset);

/** Applies a layout's bound preset whenever that layout is applied. Once. */
LILA_UI_API void install();
} // namespace lila::ui::barPresets
