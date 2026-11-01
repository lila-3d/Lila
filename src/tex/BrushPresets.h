#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Texture brush presets: `.lilabrush` JSON files in tiers, later winning on the same id: built-in
 * (<resources>/brushes), extension packs (<package>/resources/brushes of loaded extensions), the user's
 * (<config>/brushes) and the workspace's (.lila/brushes). An id is the file's path under its tier folder without the
 * extension ("Filters/blur").
 *
 *   { "format": "lilabrush", "version": 1, "name": "Soft Round", "category": "Paint", "doc": "...",
 *     "brush":     { brush::BrushSettings JSON: radius, strength, spacing, falloff, alpha, jitter, strokeMode... },
 *     "payload":   { tex::paint::Payload JSON: kind, channels, blend, filterRadius, amount, source, tip, normalMode... },
 *     "particles": { tex::paint::ParticleSpec JSON } }
 *
 * Every part is optional: a preset only changes what it names, so "Smudge" can keep the current colours. Files with a
 * "tool" other than "paint" belong to another tool (brush/Presets.h) and are skipped.
 */
namespace lila::tex::brushes
{
using json = nlohmann::json;

inline constexpr const char* kTiers[] = {"builtin", "extension", "user", "workspace"};

struct LILA_SCENE_API Preset
{
    std::string id;
    std::string name;
    std::string category;
    std::string doc;
    std::string tier;
    /** The extension a pack preset came from. */
    std::string owner;
    std::filesystem::path file;
    json brush = json::object();
    json payload = json::object();
    json particles;

    /** The file's document. */
    json toJson() const;
    /** Throws std::invalid_argument when it isn't a .lilabrush document. */
    static Preset fromJson(const json& j);
};

/** Every visible preset (the winner per id), by category then name. Scans on first use. */
LILA_SCENE_API std::vector<Preset> all();
LILA_SCENE_API std::optional<Preset> find(std::string_view id);
/** Rescans every tier on the next lookup. */
LILA_SCENE_API void reload();
/** The folder of a tier ("builtin", "user", "workspace"); empty when there is none. */
LILA_SCENE_API std::filesystem::path tierDir(std::string_view tier);
/** Files that failed to load on the last scan, "path: why". */
LILA_SCENE_API std::vector<std::string> errors();
/** Writes p as <tier>/<id>.lilabrush ("user" or "workspace") and rescans. Returns the file. Throws std::runtime_error. */
LILA_SCENE_API std::filesystem::path save(const Preset& p, std::string_view tier = "user");
} // namespace lila::tex::brushes
