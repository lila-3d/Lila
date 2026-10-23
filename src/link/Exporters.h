#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::link
{
namespace fs = std::filesystem;

/**
 * Writes the given nodes (and whatever they own, at the exporter's discretion) to `path`. Returns false and fills
 * `err` on failure. Called on the link's worker thread, so it must not touch the live scene without its own
 * synchronisation; snapshot what it needs or marshal to the main thread.
 */
using ExportFn = std::function<bool(const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err)>;

/** Keyed by format ("fbx"). Registering again replaces; an empty fn removes. */
LILA_SCENE_API void registerExporter(std::string format, ExportFn fn);
/** For extensions: owned by ctx and removed when it unloads. False, with a log message, if the format is taken. */
LILA_SCENE_API bool registerExporter(ext::Context& ctx, std::string format, ExportFn fn);
LILA_SCENE_API void unregisterExporter(std::string_view format);
/** A copy, so the caller can run it without holding the registry lock. Empty when none is registered. */
LILA_SCENE_API ExportFn findExporter(std::string_view format);
LILA_SCENE_API std::vector<std::string> exporterFormats();
/** The extension that registered format; empty for Lila's own or an unknown format. */
LILA_SCENE_API std::string exporterOwner(std::string_view format);

/** Like ExportFn, writing only the named scene take's animation (one AnimStack), for mappings with `takes`. */
using TakeExportFn =
    std::function<bool(const std::vector<scene::NodeId>& nodes, const fs::path& path, const std::string& take, std::string* err)>;
/** Keyed by format ("fbx:animation"). Registering again replaces; an empty fn removes. */
LILA_SCENE_API void registerTakeExporter(std::string format, TakeExportFn fn);
LILA_SCENE_API TakeExportFn findTakeExporter(std::string_view format);
} // namespace lila::link
