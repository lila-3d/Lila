#pragma once

#include "UsdCommon.h"
#include "UsdMaterialX.h"

#include <lila/Graph.h>

#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * Lila materials (Unreal's default lit, scene/Materials.h) as UsdPreviewSurface networks: UsdUVTexture per map,
 * reading its UV set through one UsdPrimvarReader_float2, packed ORM/MRO maps as one texture feeding occlusion,
 * roughness and metallic from its channels, DirectX normal maps as a texture with a negative green scale (USD's normal
 * input is OpenGL style), emissive intensity as the emissive texture's scale and masked opacity from the base color
 * texture's alpha. Each texture's wrap goes in its wrapS and wrapT, nearest filtering in Hydra's magFilter and minFilter.
 * Unlit materials write their base color as emission over a black surface that reflects nothing; on import that (marked
 * by Lila) and MaterialX's ND_surface_unlit become unlit materials. Settings with no UsdPreviewSurface input (blend mode,
 * two-sided, the stylised models) also go in the material's "lila" customData.
 *
 * Alongside or instead of that, a MaterialX network (OpenPBR or Standard Surface, see UsdMaterialX.h) under the
 * material's outputs:mtlx:surface. Import reads either, preferring MaterialX unless the material is Lila's own and has
 * a UsdPreviewSurface.
 */
namespace lila::usd
{
/** The primvar a Lila UV set is written as: st, st1, st2, ... (Unreal's names). */
std::string uvPrimvarName(int set);
/** The non-default storable values of a material node as JSON, for what the networks can't hold. */
std::string materialSettings(const scene::Node& n);
/** Specular 0.5 is F0 4%, an IOR of 1.5: Unreal's specular scales F0 as 0.08 * specular. */
double iorOfSpecular(double specular);
double specularOfIor(double ior);
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
    /** Which surfaces material() writes: UsdPreviewSurface, a MaterialX network (unless None), or both. */
    void setShading(bool preview, mtlx::Surface materialX)
    {
        mPreview = preview;
        mMaterialX = materialX;
    }
    /**
     * A slot's material as a MaterialX network (Lila's neutral default for a slot without one). usd names UV sets by
     * their primvars (geompropvalue st1) rather than MaterialX's texcoord index.
     */
    mtlx::Network network(const std::string& slot, mtlx::Surface surface, bool usd);
    /** The path a texture is written as, by the writer's texture mode. */
    std::string assetPath(const std::string& lilaPath);

    /** Package mode: texture files on disk and their paths inside the package. */
    const std::vector<std::pair<fs::path, std::string>>& packaged() const { return mPackaged; }
    /** Every material written so far, by slot. */
    const std::map<std::string, pxr::UsdShadeMaterial>& written() const { return mDone; }
    std::vector<std::string> warnings;

private:
    void writePreview(const pxr::UsdShadeMaterial& m, const scene::Node* n, const std::string& slot);
    /** The surface of an unlit material: its base color as emission over a black surface that reflects nothing. */
    void writeUnlit(const pxr::UsdShadeMaterial& m, pxr::UsdShadeShader surface, const scene::Node& n, const std::string& slot,
                    const std::function<pxr::UsdShadeShader(const std::string&, pxr::GfVec4f)>& texture);

    scene::Graph& mGraph;
    pxr::UsdStagePtr mStage;
    pxr::SdfPath mScope;
    fs::path mFile;
    TextureMode mMode;
    bool mPreview = true;
    mtlx::Surface mMaterialX = mtlx::Surface::None;
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
    /** The slot each USD material read so far became. */
    const std::map<pxr::SdfPath, std::string>& slots() const { return mSlots; }

    /**
     * Lila's own settings for materials whose stage has no customData to hold them, by material prim name: the slot and
     * the attrs JSON a .mtlx document Lila wrote keeps on its material nodes.
     */
    std::map<std::string, std::pair<std::string, std::string>> external;

    std::vector<std::string> created;
    std::vector<std::string> warnings;
    /** What the networks hold that no Lila setting maps to. */
    std::vector<std::string> skipped;

private:
    std::string texturePath(const pxr::UsdShadeShader& tex, const std::string& material);
    /** Fills a material from a MaterialX surface (UsdMaterialX.cpp); own is true for one Lila wrote. */
    void readMtlx(const pxr::UsdShadeShader& surface, scene::NodeId id, const std::string& name, bool own);
    /** Lila's own settings, written by its exporters, over what the network implied. Texture paths stay the network's. */
    void applyOwnSettings(scene::NodeId id, const std::string& attrs);

    scene::Graph& mGraph;
    fs::path mFile;
    std::map<pxr::SdfPath, std::string> mSlots;
    std::set<std::string> mMade;
    std::set<std::string> mOwnSettings; // made from Lila's customData
    int mExtracted = 0;
};

namespace mtlx
{
/** Defines the network's shaders under m (info:id their nodedefs) and connects m's outputs:mtlx:surface to its surface. */
void writeUsd(const Network& net, const pxr::UsdShadeMaterial& m);
} // namespace mtlx
} // namespace lila::usd
