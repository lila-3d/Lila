#pragma once

#include "core/Export.h"

#include "io/SceneHistory.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lila::io::recovery
{
namespace fs = std::filesystem;

/**
 * Unclean-exit detection. A GUI session writes <userConfigDir>/sessions/<pid>.json while it runs and removes it on a
 * clean exit; the file also records each scene the session had open and the autosave folders it used:
 *   {"pid": 1234, "started": <unix ms>, "scenes": [{"path": "X:/a/scene.lila", "folders": ["..."]}]}
 * A file whose process is gone (or whose pid now belongs to a younger process) is a session that crashed.
 */
struct SessionScene
{
    fs::path scene; // empty for an untitled scene
    std::vector<fs::path> folders;
};

struct Session
{
    fs::path file;
    int64_t pid = 0;
    int64_t startedMs = 0;
    std::vector<SessionScene> scenes;
};

LILA_SCENE_API fs::path sessionsDir();
LILA_SCENE_API int64_t currentPid();
LILA_SCENE_API int64_t nowMs();

/** True when pid is a live process that started no later than startedMs (so it is the one that wrote the file). */
LILA_SCENE_API bool processAlive(int64_t pid, int64_t startedMs);

using AliveFn = std::function<bool(int64_t pid, int64_t startedMs)>;
/** Session files in dir whose process is gone. A file carrying selfPid is always stale: it predates this process. */
LILA_SCENE_API std::vector<Session> staleSessions(const fs::path& dir, int64_t selfPid, const AliveFn& alive = processAlive);

/** Writes this process's session file and keeps it up to date as the scene and workspace change. */
LILA_SCENE_API void beginSession(const fs::path& dir = sessionsDir());
/** Records that the session had scene open with autosaves going to folder. */
LILA_SCENE_API void noteScene(const fs::path& scene, const fs::path& folder);
/** Removes this process's session file; call on a clean exit. */
LILA_SCENE_API void endSession();

/** An autosave or emergency save newer than the scene's saved file, written during a session that crashed. */
struct Candidate
{
    fs::path scene;       // where it belongs; empty for an untitled scene
    fs::path file;        // the autosave or emergency save
    history::Kind kind = history::Kind::Autosave;
    int64_t fileTime = 0; // unix seconds
    int64_t savedTime = 0; // the scene file's, 0 when it does not exist
};

/** Newest recoverable file per scene across sessions, newest first. */
LILA_SCENE_API std::vector<Candidate> findRecoverable(const std::vector<Session>& sessions);

/** Deletes the session files, once their recovery has been offered. */
LILA_SCENE_API void forget(const std::vector<Session>& sessions);

/** Hooks io::emergencySave() into the crash handler. */
LILA_SCENE_API void installCrashSave();
} // namespace lila::io::recovery
