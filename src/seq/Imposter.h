#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

/**
 * Octahedral imposters: a mesh viewed from frames x frames directions, each view a cell of a base colour atlas (alpha
 * is coverage) and a normal atlas (xyz in Unreal space as n * 0.5 + 0.5, alpha the depth through the bounding sphere),
 * plus a card mesh and a JSON whose materialParams feed an imposter material.
 */
namespace lila::seq
{
struct ImposterOptions
{
    std::string folder;
    std::string name = "imposter";
    /** octahedral | hemi-octahedral */
    std::string layout = "hemi-octahedral";
    int frames = 12;
    int resolution = 256;
    /** ue5 | ue6 | generic, as the FBX writer takes it. */
    std::string profile = "ue5";
};

struct ImposterResult
{
    std::filesystem::path baseColor, normal, mesh, meta;
    nlohmann::json metadata;
};

/** Raycasts the atlases and writes every file. Throws std::runtime_error. */
LILA_SCENE_API ImposterResult bakeImposter(const geo::Mesh& mesh, const ImposterOptions& o);

/** The Imposter geometry node and gnBakeImposter. Called by registerCommands. */
void registerImposterNode();
} // namespace lila::seq
