#pragma once

#include "gltf/Gltf.h"
#include "gltf/GltfInternal.h"

#include <cgltf.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace lila::gltf
{
using json = nlohmann::json;

/** The import's images: where each lands on disk (embedded ones written out once) and their decoded texels. */
class ImageStore
{
public:
    ImageStore(const cgltf_data& data, const fs::path& gltfPath, const fs::path& textureDir, ImportResult& result);

    /** Path of the image on disk, UTF-8 with forward slashes; empty when it has no data. */
    std::string path(const cgltf_image* image);
    /** Decoded texels, cached; null when the image can't be decoded. */
    const Pixels* pixels(const cgltf_image* image);
    /** Writes texels as <textureDir>/<stem>.png and returns the path; empty on failure. */
    std::string writePng(const std::string& stem, const Pixels& p);
    /** A file stem for images made from this one. */
    std::string stem(const cgltf_image* image);

private:
    bool bytes(const cgltf_image* image, std::string& out);

    const cgltf_data& mData;
    fs::path mDir;
    fs::path mTextureDir;
    std::string mFileStem;
    ImportResult& mResult;
    std::map<const cgltf_image*, std::string> mPaths;
    std::map<const cgltf_image*, std::unique_ptr<Pixels>> mPixels;
};

/** How the faces of one glTF material's primitives are set up. */
struct MaterialSlot
{
    std::string name;
    /** The UV set the textures read. */
    int uvSet = 0;
    /** KHR_texture_transform of the material's textures, glTF UV space: baked into that UV set of its faces. */
    bool transformed = false;
    glm::dmat3 uvTransform{1.0};
};

/** A material node per glTF material (existing ones of the same name are reused as they are), indexed like data.materials. */
std::vector<MaterialSlot> importMaterials(scene::Graph& graph, const cgltf_data& data, ImageStore& images, const ImportOptions& opts,
                                          ImportResult& r);

/** The slot's material inputs, read from the scene (main thread). */
MaterialSnapshot snapshotMaterial(scene::Graph& graph, const std::string& slot);

/** Where the material writer puts textures: it returns a glTF texture index for an image, or -1. */
class TextureSink
{
public:
    virtual ~TextureSink() = default;
    /** A texture showing a source file; PNG and JPEG go as they are, other formats are converted to PNG. */
    virtual int file(const std::string& path, const scene::material::TextureSampling& sampling) = 0;
    /** A texture of a PNG made by `make` (called once per key), named after `name`. */
    virtual int made(const std::string& key, const std::string& name, const std::function<bool(Pixels&)>& make,
                     const scene::material::TextureSampling& sampling) = 0;
};

/** The glTF material object for a snapshot. Records the extensions it uses. */
json materialJson(const MaterialSnapshot& m, TextureSink& textures, const ExportOptions& opts, std::set<std::string>& extensionsUsed,
                  std::vector<std::string>& warnings);
} // namespace lila::gltf
