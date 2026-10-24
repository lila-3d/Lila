#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

/**
 * A scene's sidecar page store, <scene>.lilatex/: the heavy pixels of its texture sets (flattened paint) as chunk
 * files, so the scene file keeps only the graph and stroke records and stays small and diffable.
 *
 * A chunk holds up to kChunkPages x kChunkPages pages of one layer, channel, UDIM tile and mip level, each page zstd
 * packed on its own, under <layer>/<channel>/<udim>/m<mip>/c<x>_<y>-<content hash>.lpg. A save writes only chunks whose
 * content is new (a file of that name not already there), then removes the chunk files the saved scene no longer refers
 * to. index.json lists what the scene refers to.
 */
namespace lila::tex::store
{
using json = nlohmann::json;
namespace fs = std::filesystem;

/** Pages per side of a chunk. */
inline constexpr int kChunkPages = 8;

/** <scene>.lilatex next to the scene file. */
LILA_SCENE_API fs::path storeFor(const fs::path& scene);

/** The store a save in progress writes to; empty outside saves (autosaves keep pixels inline). */
LILA_SCENE_API fs::path activeStore();

/**
 * Writes a chunk's pages (raw texels, pageBytes each, in pages order) under the active store unless a file with that
 * content is there already, and notes it as referenced by this save. Returns its path relative to the store.
 */
LILA_SCENE_API std::string putChunk(const std::string& keyPath, std::span<const uint32_t> pages, std::span<const uint8_t> raw, size_t pageBytes);
/** Notes a chunk an unchanged baseline still refers to (it is kept). */
LILA_SCENE_API void keepChunk(const std::string& relPath);
/** A chunk's pages back: raw texels in the order written. Throws std::runtime_error when missing or corrupt. */
LILA_SCENE_API std::vector<uint8_t> readChunk(const fs::path& file, size_t pageBytes);

/**
 * Where a store a scene refers to is now: the current scene's store when it has the file, else the store the ref was
 * written in (absolute). Empty when neither has it.
 */
LILA_SCENE_API fs::path resolve(const std::string& storeName, const fs::path& storeAbs, const std::string& relPath);

struct SaveStats
{
    /** Chunk files written, kept as they were, and removed as no longer referred to, by the last save. */
    size_t written = 0;
    size_t kept = 0;
    size_t removed = 0;
    size_t bytesWritten = 0;
};
LILA_SCENE_API SaveStats lastSave();

/** Starts and ends store writes around scene saves (io SceneBeforeSave / SceneSaved). Called by tex::registerTypes. */
LILA_SCENE_API void install();
} // namespace lila::tex::store
