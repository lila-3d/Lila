#pragma once

#include "scene/Plug.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
void registerExporter(std::string format, ExportFn fn);
void unregisterExporter(std::string_view format);
/** A copy, so the caller can run it without holding the registry lock. Empty when none is registered. */
ExportFn findExporter(std::string_view format);
std::vector<std::string> exporterFormats();
} // namespace lila::link
