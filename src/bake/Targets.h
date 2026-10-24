#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * Target profiles: JSON files naming the maps a downstream tool needs, their conventions, bit depth and file names.
 * Applying one sets a bake project's maps and output naming in one go (bakeForTarget).
 *
 *   {
 *     "name": "substancePainter", "label": "Smart Material Inputs", "version": 1,
 *     "doc": "...",
 *     "output": {"name": "{material}_{map}", "format": "png", "bits": 16},
 *     "maps": [
 *       {"map": "normal", "suffix": "normal", "slot": "Normal", "options": {"greenUp": true}},
 *       {"map": "bentNormal", "suffix": "bentnormals", "slot": "BentNormals", "optional": true},
 *       ...
 *     ]
 *   }
 *
 * "output" holds any OutputSettings keys (folder, resolution and supersampling are usually left to the project). Each map
 * entry is a MapSettings plus "slot" (the tool's name for it) and "optional" (baked only when asked). Maps that no
 * registered MapDef implements are skipped with a warning. An optional "pipeline": {"autoAssign", "rebakeOnSave"} sets
 * those project options when applied; keys it leaves out keep the project's.
 *
 * A saved preset (saveBakeTarget) is a target with "preset": true, the full output, every map, and optionally
 * "backend" and "groupDefaults": {"frontal", "rear", "cageOffset", "skew", "aoScope", "ignoreBackfaces" (GroupSettings
 * keys), "modifiers" (the bake-only stack each high gets), "self" (lows bake onto themselves)}. It never names meshes.
 *
 * Search order, later wins on the same name: built-in <resources>/bake/targets, the user's <config>/bake/targets, the
 * workspace's .lila/bake/targets.
 */
namespace lila::bake
{
struct TargetMap
{
    MapSettings settings;
    std::string slot;
    bool optional = false;
};

struct LILA_SCENE_API TargetProfile
{
    std::string name;
    std::string label;
    std::string doc;
    std::filesystem::path file;
    /** Partial OutputSettings JSON. */
    json output = json::object();
    std::vector<TargetMap> maps;
    /** Saved from a project's settings rather than shipped: selecting it in the Baker applies it. */
    bool preset = false;
    /** The backend bakes for it run on; empty leaves the caller's. */
    std::string backend;
    /** Empty leaves the project's group defaults alone. */
    json groupDefaults = json::object();
    /** ProjectSettings pipeline options it sets: autoAssign, rebakeOnSave. Absent keys leave the project's. */
    json pipeline = json::object();

    json toJson() const;
    /** Throws std::invalid_argument when name or maps are missing or malformed. */
    static TargetProfile fromJson(const json& j);
};

/** Built-in, user, workspace folders, in that order (missing ones included). */
LILA_SCENE_API std::vector<std::filesystem::path> targetDirs();
/** Every readable target, by name; later folders replace earlier ones. Unreadable files are logged and skipped. */
LILA_SCENE_API std::vector<TargetProfile> targets();
LILA_SCENE_API std::optional<TargetProfile> findTarget(std::string_view name);

/**
 * settings with the target's output keys and map list (replacing the project's maps). Optional maps are included when
 * includeOptional. Map ids with no registered MapDef go to *skipped (as "map (slot)") and are left out.
 */
LILA_SCENE_API ProjectSettings applyTarget(const ProjectSettings& settings, const TargetProfile& target, bool includeOptional,
                                           std::vector<std::string>* skipped);

/** True for a profile shipped with Lila (the first targetDirs folder); those can't be deleted. */
LILA_SCENE_API bool isBuiltinTarget(const TargetProfile& t);

/** A group's settings as group defaults: its projection fields, the stack its first high carries, and self. No meshes. */
LILA_SCENE_API json groupDefaultsOf(const GroupSettings& s);
/** The stack the defaults give each new high; empty when they name none. */
LILA_SCENE_API json defaultStack(const json& defaults);
/** A new group with the defaults filled in: its projection fields, the stack on highs without one, a self high when it has none. */
LILA_SCENE_API GroupSettings withGroupDefaults(GroupSettings s, const json& defaults);
/**
 * An existing group moved from one set of defaults to another. A field (or a high's stack) takes the new value only when
 * it still holds the old default, so settings made on that group by hand survive; force takes them all. A group with
 * no highs gains a self high when the new defaults turn self on.
 */
LILA_SCENE_API GroupSettings rebaseGroupDefaults(GroupSettings s, const json& before, const json& after, bool force);

/** Writes a target file into the user (scope "user") or workspace ("workspace") targets folder; returns its path. Throws. */
LILA_SCENE_API std::filesystem::path saveTarget(const TargetProfile& t, const std::string& scope, bool overwrite);
/** Deletes a user or workspace target's file (the one in scope when given, else the one that wins); returns it. Throws. */
LILA_SCENE_API std::filesystem::path deleteTarget(const std::string& name, const std::string& scope);
} // namespace lila::bake
