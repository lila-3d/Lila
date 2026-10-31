#pragma once

#include "core/Export.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lila::ui::environments
{
/** Pref value (general "/viewport/env/path") that picks the built-in procedural studio. */
inline constexpr const char* kProcedural = "procedural";
/** Environment intensity for HDRIs without their own (general prefs), and the per-HDRI overrides keyed by hdriKey(). */
inline constexpr const char* kIntensity = "/viewport/env/intensity";
inline constexpr const char* kIntensityByHdri = "/viewport/env/intensityByHdri";

/** HDRIs shipped in resources/hdri, sorted by name. */
LILA_UI_API std::vector<std::filesystem::path> builtIn();

/** Menu label for an HDRI file: "kloofendal_48d_partly_cloudy_puresky_2k.hdr" reads "Kloofendal 48d Partly Cloudy (Pure Sky)". */
LILA_UI_API std::string displayName(const std::filesystem::path& hdri);

/**
 * The env path pref as the renderer takes it: empty for the procedural studio. An empty pref is the default HDRI,
 * and a bare file name refers to one of the built-in HDRIs.
 */
LILA_UI_API std::string resolve(const std::string& pref);

/** The env path pref as an intensity map key: the pref itself, with empty spelled as the default HDRI's file name. */
LILA_UI_API std::string hdriKey(const std::string& pref);

/** Intensity of the current environment: its own if it was adjusted, else the global one. */
LILA_UI_API float intensity();
/** Sets the current environment's own intensity, or clears it (back to the global one) when value is empty. */
LILA_UI_API void setIntensity(std::optional<float> value);
/** Whether the current environment has its own intensity. */
LILA_UI_API bool hasOwnIntensity();
} // namespace lila::ui::environments
