#pragma once

#include "core/Export.h"

#include "gn/NodeLib.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

/** Geometry and socket values to bytes and back: simulation state, and what a Bake node writes to disk. */
namespace lila::gn
{
/** Every component with its attributes. Instances keep their references, written once each. */
LILA_SCENE_API json geometryToJson(const Geometry& g);
/** Throws std::invalid_argument on a malformed document. */
LILA_SCENE_API GeometryPtr geometryFromJson(const json& j);

/**
 * A value of a socket type: geometry whole, data as the single value it holds (a field that varies per element is read
 * at its first element), strings as text. Bundles keep their items; closures can't be stored and read back as empty.
 */
LILA_SCENE_API json valueToJson(SocketType t, const Value& v);
LILA_SCENE_API Value valueFromStored(SocketType t, const json& j);

/** Compact binary form of a document holding packed arrays (CBOR, with their element types kept). */
LILA_SCENE_API std::vector<uint8_t> toBytes(const json& j);
/** Throws std::invalid_argument when the bytes aren't such a document. */
LILA_SCENE_API json fromBytes(std::span<const uint8_t> bytes);

LILA_SCENE_API void writeFile(const std::filesystem::path& path, const json& j);
/** Throws std::runtime_error when the file is missing or unreadable. */
LILA_SCENE_API json readFile(const std::filesystem::path& path);
} // namespace lila::gn
