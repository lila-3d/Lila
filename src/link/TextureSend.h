#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

/**
 * Sending a bake's textures to Unreal: LilaLink's importTextures updates each texture asset in place (per map sRGB,
 * compression, flip green, LOD group, UDIM sets as virtual textures) and in materialMode "instance" creates or updates a
 * material instance per material slot from a parent, set on the mapped meshes' slots.
 */
namespace lila::link
{
using json = nlohmann::json;

/** How a map imports: {srgb, compression, lodGroup}. compression names: default, normalmap, masks, grayscale, hdr, uncompressed. */
LILA_SCENE_API json textureSettings(std::string_view map, std::string_view colorSpace, std::string_view format, int bits);

/** The texture asset name of a bake file: its stem without the UDIM label and with dots made underscores. */
LILA_SCENE_API std::string textureAssetName(const std::string& file, int udim);

/**
 * importTextures params for a bake result ({outputs: [...]}). options: folder (/Game/...), materialMode (none | instance),
 * materialParent, materialFolder, parameters ({map: parameter name}), lodGroup, virtualTexture, save, and meshes
 * ({Lila mesh path: Unreal static mesh path}) for the instances to be set on. PSD layers are left out (Unreal
 * can't take one layer); warnings lists them.
 */
LILA_SCENE_API json textureImportParams(const json& bakeResult, const json& options, json* warnings = nullptr);

/** Registers unrealSendTextures. */
LILA_SCENE_API void registerTextureCommands();
} // namespace lila::link
