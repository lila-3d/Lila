#pragma once

#include "core/Export.h"

#include "fbx/Fbx.h"
#include "scene/Materials.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * glTF 2.0 import (through cgltf) and export (our own writer), .gltf with external or embedded data and binary .glb.
 *
 * Axes and units. glTF is right-handed Y up, its assets face +Z, and lengths are metres. Those are Lila's own semantic
 * axes for a Y-up scene (forward +Z, right -X, up +Y), so a Y-up scene maps one to one and a Z-up scene turns forward,
 * right and up onto glTF's (fbx::profileBasis for ue6). Every local matrix is conjugated by the axis change, so no
 * conversion rotation lands on the roots, and lengths are scaled between cm and the file's units.
 *
 * Textures. glTF UVs are V-down and Lila's are V-up, so v flips both ways. glTF normal maps are OpenGL style (green
 * up): imported materials keep the file's image with normalFormat "opengl", and DirectX maps are written with their
 * green flipped. glTF packs roughness in G and metallic in B of one texture and occlusion in R of another (or the
 * same) one, which is Unreal's ORM layout, so ORM maps are shared as they are and anything else is repacked.
 */
namespace lila::gltf
{
namespace fs = std::filesystem;

struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Skins become skinClusters on joints; off imports the meshes unskinned. */
    bool skins = true;
    /** Import animation as curves. */
    bool animation = true;
    /** Only this animation (by name, or its index as a string); empty imports them all. */
    std::string take;
    /** Set the scene fps and playback range from the active animation. */
    bool setPlayback = true;
    /** Material nodes from the file's materials; off leaves only the slot names on the faces. */
    bool materials = true;
    /** Where embedded and converted images are written; empty is <file folder>/<file stem>_textures. */
    fs::path textureDir;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every DAG node created, parents first
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins; // skinCluster nodes
    std::vector<scene::NodeId> cameras;
    std::vector<scene::NodeId> lights;
    std::vector<scene::NodeId> morphs;
    std::vector<std::string> materials; // material node names
    std::vector<std::string> images;    // image files written (embedded or converted)
    std::vector<std::string> takes;     // Lila takes made, one per animation when there are several
    std::string take;                   // the animation that is active, empty for none
    double fps = 0.0;
    double start = 0.0, end = 0.0; // seconds
    int curves = 0;
    std::string generator;
    std::vector<std::string> extensionsUsed;
    /** Things in the file that were not imported, one line each. */
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    /** Parts that failed to import (a primitive compressed in a way Lila can't decode). The rest still imports. */
    std::vector<std::string> errors;
};

/** Main thread. Creates the file's default scene (or every root when it has none), undoably. */
LILA_SCENE_API ImportResult importGltf(scene::Graph& graph, const fs::path& path, const ImportOptions& opts = {});

struct ExportOptions
{
    /** What to gather: kind, children, atOrigin, worldSpace, takes, range, influences, deformOnly, unitScale. */
    fbx::ExportOptions gather;
    /** A .glb (one binary file); false writes JSON .gltf. */
    bool binary = false;
    /** Images inside the file: GLB buffer views, or data URIs in a .gltf. False writes them as files next to it. */
    bool embedImages = false;
    /** .gltf only: the geometry buffer as a data URI instead of a .bin next to it. */
    bool embedBuffers = false;
    /** Write texture maps; false writes material factors only. */
    bool textures = true;
    /** Images written as files: copy source textures next to the file; false references them in place (relative URI). */
    bool copyTextures = true;
    /** Morph target normal deltas, from the target shapes' normals. */
    bool morphNormals = true;
    /** KHR_draco_mesh_compression on every mesh primitive (then required to read the file). */
    bool draco = false;
    /** Draco quantization bits per attribute kind; generic covers tangents and skin weights. */
    int dracoPositionBits = 14;
    int dracoNormalBits = 10;
    int dracoUvBits = 12;
    int dracoColorBits = 8;
    int dracoGenericBits = 12;
    /** Draco compression level, 0 fastest to 10 smallest. */
    int dracoLevel = 7;
};

/** A material slot's inputs as the exporter writes them; paths are resolved and absolute. */
struct MaterialSnapshot
{
    std::string name;
    bool found = false; // a material node of that name exists
    glm::dvec3 baseColor{0.5};
    std::string baseColorMap;
    double metallic = 0.0;
    std::string metallicMap;
    double specular = 0.5;
    double roughness = 0.55;
    std::string roughnessMap;
    std::string normalMap;
    bool normalOpenGl = false;
    std::string occlusionMap;
    std::string ormMap;
    bool packedMro = false;
    glm::dvec3 emissiveColor{0.0};
    std::string emissiveMap;
    double emissiveIntensity = 1.0;
    double opacity = 1.0;
    double opacityMaskClip = 0.3333;
    int blendMode = 0; // scene::material::BlendMode
    bool twoSided = false;
    int uvSet = 0;
    int shadingModel = 0;
    /** How each map samples, by its material attr (scene::material::kBaseColorMap, ...); a missing entry is the default. */
    std::map<int, scene::material::TextureSampling> sampling;

    scene::material::TextureSampling samplingOf(int mapAttr) const
    {
        auto it = sampling.find(mapAttr);
        return it == sampling.end() ? scene::material::TextureSampling{} : it->second;
    }

    /** Whether a map's values are sRGB-encoded (its colorSpace, auto resolved). */
    bool srgbOf(int mapAttr) const
    {
        switch (samplingOf(mapAttr).colorSpace)
        {
        case scene::material::TextureColorSpace::Srgb: return true;
        case scene::material::TextureColorSpace::Linear: return false;
        default: return scene::materials::colorMap(mapAttr);
        }
    }
};

/** What an export needs, taken on the main thread. */
struct ExportSnapshot
{
    fbx::ExportScene scene;
    std::vector<MaterialSnapshot> materials; // every slot the exported meshes use
};

/** Main thread: fbx::gatherExport plus the materials of the slots the meshes use. */
LILA_SCENE_API ExportSnapshot gatherExport(scene::Graph& graph, const std::vector<scene::NodeId>& roots, const ExportOptions& opts);

/** Any thread. Writes the .gltf (with its .bin and images) or .glb. Warnings about what could not be written go to warnings. */
LILA_SCENE_API bool writeGltf(const ExportSnapshot& snapshot, const fs::path& path, const ExportOptions& opts, std::string* error,
                              std::vector<std::string>* warnings = nullptr);

/** importGltf, exportGltf and exportWebViewer. */
LILA_SCENE_API void registerCommands();
} // namespace lila::gltf
