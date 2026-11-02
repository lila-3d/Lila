#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * What happens around a bake: content hashes that let unchanged bakes and saves skip, rebake on save, wiring outputs into
 * the lows' materials (bakeAssign), and putting the written textures under source control in the scene's changelist.
 *
 * Commands (registerPipelineCommands): bakeAssign, setBakeProject, bakeAll, bakeUpToDate.
 */
namespace lila::bake
{
/**
 * A hex content hash of everything a full bake of the project reads: its output and map settings, and per group its
 * settings plus every low, high and cage as exported (modifiers applied) with its world matrix. Settings that don't
 * change the textures (rebakeOnSave, autoAssign, target) are left out.
 */
LILA_SCENE_API std::string inputHash(scene::Graph& g, scene::NodeId project);

/** The project's last manifest on disk ({folder}/{project}_bake.json), null when there is none or it doesn't parse. */
LILA_SCENE_API json readManifest(const scene::Node& project);

/** Whether a full bake would write anything new: false only when the manifest's hash matches and every output exists. */
LILA_SCENE_API bool needsBake(scene::Graph& g, scene::NodeId project, std::string* reason = nullptr);

/** A registered GPU backend when there is one, else "cpu". */
LILA_SCENE_API std::string preferredBackend();

/** The material channel (setMaterialTexture's names) a map wires into: normal, ambientOcclusion, orm, baseColor, ...; empty for none. */
LILA_SCENE_API std::string materialChannel(std::string_view map);

/**
 * Wires a bake's outputs ({outputs: [...]} as bake returns) into the materials of the slots they were baked for:
 * normal (with its DirectX or OpenGL convention), ambient occlusion, ORM or MRO packed maps, base colour, roughness,
 * metallic and emissive. A slot with no material gets one (create), named after the slot, or for a mesh with no
 * material, M_<mesh> assigned to the whole mesh. UDIM sets wire their first tile; PSD layers are skipped. Undoable.
 * Returns {assigned: [{material, channel, file, map}], created, skipped: [{file, map, reason}]}.
 */
LILA_SCENE_API json assignOutputs(scene::Graph& g, const json& result, bool create = true, const json& channels = json::object());

/**
 * The bake map id (normal, ao, orm, ...) or, failing that, the material channel a texture file's name says it holds:
 * its last name token against the maps' ids and suffixes, then scene::materials::guessTextureChannel. "" when neither.
 */
LILA_SCENE_API std::string mapFromFileName(std::string_view fileName);

/**
 * What bakeAssign returned when a project's last bake auto-assigned (autoAssign on: every successful, non-dry-run bake
 * assigns as its own command once it lands, on the main thread); null when none has.
 */
LILA_SCENE_API json autoAssigned(scene::NodeId project);

/** Source control: checks out the files that exist and are read only, into the scene file's changelist when it has one. */
LILA_SCENE_API void prepareOutputs(const std::vector<std::filesystem::path>& files);
/**
 * Source control: opens the files not yet tracked for add (per the autoAdd pref), into the scene file's changelist.
 * Nothing for an untitled scene. Async.
 */
LILA_SCENE_API void addOutputs(const std::vector<std::filesystem::path>& files);

/**
 * Save hook: bakes every project with rebakeOnSave whose inputs changed (assigning when autoAssign is on). Logs, never
 * throws. In place, returns [{project, baked, reason}]. With setRebakeAsync on, returns at once with
 * [{project, started, queued, reason}]: one rebake runs off the main thread (bakeAsync) and the rest queue behind it, a save
 * that changed a running rebake's inputs restarts it, and autoAssign runs on the main thread once it lands.
 */
LILA_SCENE_API json onSceneSaved();

/** Whether onSceneSaved bakes off the main thread (the app) or in place (batch, so scripts see the result). Main thread. */
LILA_SCENE_API void setRebakeAsync(bool on);
/** True while an on-save rebake runs or waits its turn. */
LILA_SCENE_API bool rebakesPending();
/** Drops queued rebakes and cancels and joins a running one; no more start. Main thread, before the backends stop. */
LILA_SCENE_API void shutdownRebakes();

LILA_SCENE_API void registerPipelineCommands();
} // namespace lila::bake
