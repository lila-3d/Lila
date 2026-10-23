#pragma once

#include "core/Export.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lila::io::history
{
namespace fs = std::filesystem;

enum class Kind : uint8_t
{
    Autosave,
    Emergency,
    Backup,
    Increment,
    Revision, // source control
};

LILA_SCENE_API const char* kindName(Kind k);

struct Version
{
    fs::path path; // empty for a source control revision until it is fetched
    Kind kind = Kind::Backup;
    int64_t time = 0; // unix seconds
    uintmax_t size = 0;
    std::string revision;    // source control revision, for Kind::Revision
    std::string description; // source control user and message
};

/** A trailing _v<digits> on a file stem: "scene_v002" is {"scene", 2, 3}; "scene" is {"scene", 0, 0}. */
struct VersionedStem
{
    std::string base;
    int number = 0;
    int width = 0;
};
LILA_SCENE_API VersionedStem parseVersionedStem(const std::string& stem);

/**
 * Maya-style Increment and Save target: scene.lila -> scene_v002.lila, scene_v002 -> scene_v003, keeping the digit
 * width (scene_v9 -> scene_v10). Skips any number whose .lila or .lilaa already exists.
 */
LILA_SCENE_API fs::path nextIncrement(const fs::path& scene);

/** Local time as 20261022-140312, which sorts chronologically. */
LILA_SCENE_API std::string timestamp(std::chrono::system_clock::time_point t = std::chrono::system_clock::now());

/**
 * History files for a scene live in their own folder under base: base/<dir relative to the workspace>/<stem> for a
 * scene inside workspaceRoot, else base/<stem>.<path hash> so same-named scenes elsewhere never share one. An untitled
 * scene uses base/untitled.
 */
LILA_SCENE_API fs::path folderFor(const fs::path& base, const fs::path& scene, const fs::path& workspaceRoot);

/** The stem history files of a scene are named after; "untitled" for an untitled scene. */
LILA_SCENE_API std::string stemFor(const fs::path& scene);

/**
 * Names inside a history folder: <stem>.<timestamp>[-n].<tag>.lila for autosaves ("autosave") and emergency saves
 * ("emergency"), <stem>.<timestamp>[-n]<scene extension> for backups (empty tag).
 */
struct ParsedName
{
    std::string stamp;
    int counter = 0;
    std::string tag;
};
LILA_SCENE_API std::optional<ParsedName> parseName(const std::string& filename, const std::string& stem);

/** A free name for a new history file; ext is ".lila" or ".lilaa". */
LILA_SCENE_API fs::path uniqueName(const fs::path& folder, const std::string& stem, const std::string& stamp, const std::string& tag,
                    const std::string& ext = ".lila");

/** History files in folder for stem with this tag, oldest first. */
LILA_SCENE_API std::vector<fs::path> entries(const fs::path& folder, const std::string& stem, const std::string& tag);

/** Deletes the oldest entries until keep remain. keep <= 0 keeps everything. Returns how many were removed. */
LILA_SCENE_API int prune(const fs::path& folder, const std::string& stem, const std::string& tag, int keep);

/** <workspace>/.lila/<name> when a workspace is open, else <userConfigDir>/<name>. */
LILA_SCENE_API fs::path baseDir(const char* name);
LILA_SCENE_API fs::path autosaveFolder(const fs::path& scene);
LILA_SCENE_API fs::path backupFolder(const fs::path& scene);

/**
 * Copies scene into folder as a timestamped backup and prunes to keep. A copy, never a move, so a Perforce checkout
 * of the original stays valid. Empty when scene does not exist yet.
 */
LILA_SCENE_API fs::path backup(const fs::path& scene, const fs::path& folder, int keep);

/** Incremental siblings of scene (scene_v002.lila, ...), the unversioned base included, scene itself excluded. */
LILA_SCENE_API std::vector<fs::path> increments(const fs::path& scene);

/** Backups, autosaves, emergency saves and increments of scene on disk, newest first. */
LILA_SCENE_API std::vector<Version> list(const fs::path& scene);

LILA_SCENE_API int64_t unixTime(const fs::path& file);
} // namespace lila::io::history
