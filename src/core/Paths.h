#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace lila::paths
{
namespace fs = std::filesystem;

/** %APPDATA%/Lila, ~/.config/Lila, ~/Library/Application Support/Lila. Created on first call. */
fs::path userConfigDir();

fs::path executableDir();

/** Read-only app data: resources/ and python/. Source tree in dev builds, next to the exe when installed. */
fs::path resourcesDir();
fs::path pythonDir();

fs::path fromUtf8(std::string_view utf8);
/** Generic (forward slash) UTF-8 form. */
std::string toUtf8(const fs::path& p);

/** Folder of the open scene file, empty for an untitled scene. io keeps it current. */
const fs::path& currentSceneDir();
void setCurrentSceneDir(const fs::path& dir);

/**
 * path relative to base, lexically, with ".." where it leaves base and case blind on Windows. Empty when no relative
 * form exists: either path is relative, or they sit on different drives / root names.
 */
fs::path relativeTo(const fs::path& path, const fs::path& base);

/**
 * How a file path is written to a scene or workspace file: relative to the workspace root when one is open and on the
 * path's drive, else to sceneDir, ".." allowed, forward slashes. Absolute only when neither has a relative form
 * (another drive) or there is no base. A relative path is taken as stored for the open scene, so it is resolved first and re-expressed against
 * sceneDir, which is where the file being written lives. Empty and URL-like strings pass through.
 */
std::string storedPath(const fs::path& path, const fs::path& sceneDir = currentSceneDir());

/**
 * A stored path made usable: absolute ones as written, relative ones against the workspace root, then sceneDir. The
 * first base where the file exists wins, then the first where its folder does (a sequence pattern names no file),
 * then the first base.
 */
fs::path resolvePath(std::string_view stored, const fs::path& sceneDir = currentSceneDir());
} // namespace lila::paths
