#pragma once

#include "UsdCommon.h"

#include <lila/Graph.h>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>

#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * Lila materials (Unreal's default lit, scene/Materials.h) as UsdPreviewSurface networks: UsdUVTexture per map,
 * reading its UV set through one UsdPrimvarReader_float2, packed ORM/MRO maps as one texture feeding occlusion,
 * roughness and metallic from its channels, DirectX normal maps as a texture with a negative green scale (USD's normal
 * input is OpenGL style), emissive intensity as the emissive texture's scale and masked opacity from the base color
 * texture's alpha. Settings with no UsdPreviewSurface input (blend mode, two-sided, the stylised models) also go in the
 * material's "lila" customData.
 */
namespace lila::usd
{
/** The primvar a Lila UV set is written as: st, st1, st2, ... (Unreal's names). */
std::string uvPrimvarName(int set);
/** The Lila UV set a primvar name means: st and st0 are 0, stN is N; -1 for any other name. */
int uvSetOfPrimvar(std::string_view name);

enum class TextureMode
{
    Relative, // relative to the file being written where possible
    Absolute,
    Copy,     // copied into <stem>_textures beside the file
    Package,  // usdz: collected for the package, referenced by their path inside it
};

class MaterialWriter
{
public:
    MaterialWriter(scene::Graph& graph, pxr::UsdStagePtr stage, pxr::SdfPath scope, const fs::path& file, TextureMode mode);

    /** The material for a slot, written on first use; a slot without a Lila material gets a plain default one. */
    pxr::UsdShadeMaterial material(const std::string& slot);
    /** True when the slot's Lila material is two-sided. */
    bool twoSided(const std::string& slot) const;

    /** Package mode: texture files on disk and their paths inside the package. */
    const std::vector<std::pair<fs::path, std::string>>& packaged() const { return mPackaged; }
    std::vector<std::string> warnings;

private:
    std::string assetPath(const std::string& lilaPath);

    scene::Graph& mGraph;
    pxr::UsdStagePtr mStage;
    pxr::SdfPath mScope;
    fs::path mFile;
    TextureMode mMode;
    std::map<std::string, pxr::UsdShadeMaterial> mDone;
    std::map<std::string, std::string> mAssets; // resolved Lila path -> asset path written
    std::set<std::string> mPackagedNames;
    std::vector<std::pair<fs::path, std::string>> mPackaged;
};

class MaterialReader
{
public:
    MaterialReader(scene::Graph& graph, const fs::path& file) : mGraph(graph), mFile(file) {}

    /**
     * The Lila slot name for a USD material: a material node made from it on first use (or the scene's material of
     * that name, reused as it is, as FBX import does).
     */
    std::string slot(const pxr::UsdShadeMaterial& m);
    /** Marks the material behind slot two-sided, as a doubleSided mesh using it asks. */
    void setTwoSided(const std::string& slot);

    std::vector<std::string> created;
    std::vector<std::string> warnings;

private:
    std::string texturePath(const pxr::UsdShadeShader& tex, const std::string& material);

    scene::Graph& mGraph;
    fs::path mFile;
    std::map<pxr::SdfPath, std::string> mSlots;
    std::set<std::string> mMade;
    std::set<std::string> mOwnSettings; // made from Lila's customData
    int mExtracted = 0;
};
} // namespace lila::usd
