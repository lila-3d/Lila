#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::io
{
enum class SceneFormat
{
    Binary, // .lila
    Ascii,  // .lilaa
    Json,   // legacy v1 .lila files
};

/** Sniffs the first bytes of a file. */
LILA_SCENE_API SceneFormat detectFormat(std::string_view bytes);

/**
 * Binary .lila: an 8-byte magic, then chunks {fourcc, flags, crc32, rawSize, storedSize, payload}. "JSON" holds the
 * scene document with every large numeric array swapped for {"$blob": n, "shape", "dtype"}; a "BLOB" chunk and the
 * "BLB+" chunks after it hold that array as raw little-endian values of its dtype (f64 when absent). Payloads are zstd
 * compressed and checksummed, so a damaged chunk only loses its array. Packed arrays (core/PackedArray.h) in the
 * document go straight into blobs.
 */
LILA_SCENE_API std::string encodeBinary(nlohmann::json doc);
/** encodeBinary, handing the file to write() piece by piece as chunks finish compressing. */
LILA_SCENE_API void encodeBinary(nlohmann::json doc, const std::function<void(std::string_view)>& write);
/**
 * The document back as json. Large arrays inside meshes stay packed (see core/PackedArray.h), which meshFromJson reads
 * directly; packed::unpackAll gives plain json.
 */
LILA_SCENE_API nlohmann::json decodeBinary(std::string_view bytes, std::vector<std::string>* warnings);
/** Only the document chunk: large arrays stay {"$blob": n} placeholders. For reading metadata cheaply. */
LILA_SCENE_API nlohmann::json decodeBinaryHeader(std::string_view bytes);

/**
 * ASCII .lilaa: one fact per line, stable order, so it diffs and merges well and a damaged line only loses itself.
 *
 *   lila 2 ascii
 *   upAxis y
 *   node locator Target
 *     parent Rig
 *     translate 0 10 0
 *   connect Driver.translateX Target.translateY
 *   select Target
 */
LILA_SCENE_API std::string encodeAscii(const nlohmann::json& doc);
LILA_SCENE_API nlohmann::json decodeAscii(std::string_view text, std::vector<std::string>* warnings);
} // namespace lila::io
