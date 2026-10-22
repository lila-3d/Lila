#pragma once

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace lila::render
{
struct MeshVertex
{
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec4 tangent; // w: bitangent sign
    glm::vec2 uv;
    glm::vec2 uv1;
    glm::vec4 color{1.0f}; // linear RGBA from the mesh's current color set; white without one
};

/** A run of triangles sharing one material slot. */
struct MeshSection
{
    uint32_t material = 0; // slot index into MeshDraw::materials
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

inline uint64_t nextResourceKey()
{
    static std::atomic<uint64_t> counter{1};
    return counter++;
}

/**
 * Vertices edited in place while the topology stays (sculpting a dense mesh): the editor writes vertices and marks
 * them dirty under the mutex, and the renderer copies only the dirty ones to its GPU copy before the next frame.
 */
struct DynamicVertices
{
    std::mutex mutex;
    std::vector<MeshVertex> vertices;
    /** Vertex ids written since the renderer last looked; may repeat. */
    std::vector<uint32_t> dirty;
    /** Every vertex changed (or the renderer has not uploaded them yet). */
    bool allDirty = true;

    void write(uint32_t v, const MeshVertex& value)
    {
        vertices[v] = value;
        if (!allDirty)
            dirty.push_back(v);
    }
};

/**
 * GPU-ready triangles, built on the main thread or a worker from a mesh and never changed afterwards. The renderer
 * uploads each one once and keeps it while frames still reference it; a mesh edit makes a new RenderMesh.
 */
struct RenderMesh
{
    /** When set, the vertices live (and change) here and `vertices` stays empty; indices and sections stay fixed. */
    std::shared_ptr<DynamicVertices> dynamic;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;     // triangle list, grouped by section
    std::vector<MeshSection> sections; // empty means one section of slot 0
    std::vector<glm::vec3> edgePoints; // line list for wireframe, original polygon edges only
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    uint64_t key = nextKey();

    static uint64_t nextKey() { return nextResourceKey(); }
};

/**
 * RGBA8 texels, row 0 at the top of the image, never changed once built. The renderer uploads each one once (with a
 * full mip chain) and keeps it while frames reference it; a changed file makes a new TextureImage.
 */
struct TextureImage
{
    int width = 0;
    int height = 0;
    bool srgb = true; // color data; false for linear data (normals, ORM)
    std::vector<uint8_t> rgba;
    uint64_t key = nextResourceKey();
};
using TexturePtr = std::shared_ptr<const TextureImage>;

enum MaterialFlags : uint32_t
{
    MatBaseColorMap = 1 << 0,
    MatNormalMap = 1 << 1,
    MatNormalGreenUp = 1 << 2, // OpenGL-style normal map
    MatMetallicMap = 1 << 3,   // ORM blue
    MatRoughnessMap = 1 << 4,  // ORM green
    MatOcclusionMap = 1 << 5,  // ORM red
    MatEmissiveMap = 1 << 6,
    MatMasked = 1 << 7,
    MatTranslucent = 1 << 8,
    MatTwoSided = 1 << 9,
    MatUv1 = 1 << 10,
};

/** Unreal default lit inputs for one slot. A map flag set means that map replaces its constant. */
struct Material
{
    glm::vec3 baseColor{0.5f}; // linear
    float opacity = 1.0f;
    float metallic = 0.0f;
    float roughness = 0.55f;
    float specular = 0.5f;
    float opacityClip = 0.3333f;
    glm::vec3 emissive{0.0f}; // linear; scales the emissive map when there is one
    uint32_t flags = MatTwoSided;
    TexturePtr baseColorMap; // sRGB, alpha is opacity
    TexturePtr normalMap;    // linear
    TexturePtr ormMap;       // linear: R occlusion, G roughness, B metallic
    TexturePtr emissiveMap;  // sRGB
};

enum MeshDrawFlags : uint32_t
{
    DrawShaded = 1 << 0,
    DrawWire = 1 << 1,
    DrawSelected = 1 << 2,
    DrawUnderSelected = 1 << 3,
    DrawPreselect = 1 << 4, // under the cursor: a click would select it
    DrawWireDepth = 1 << 5, // wire without shading still hides behind other surfaces (templated objects)
};

/** How a draw shows its vertex colors: multiplied into the lit base color, or flat and unlit. */
enum class VertexColorMode : uint8_t
{
    Off,
    Shaded,
    Unlit,
};

/** Every RGB channel, or one channel (alpha included) as grayscale, the way engines use them as masks. */
enum class ColorChannel : uint8_t
{
    Rgb,
    R,
    G,
    B,
    A,
};

struct MeshDraw
{
    std::shared_ptr<const RenderMesh> mesh;
    glm::mat4 world{1.0f};
    /** Used for every section whose slot has no entry in materials. */
    glm::vec3 baseColor{0.5f};
    float roughness = 0.55f;
    std::vector<Material> materials; // per slot
    uint32_t flags = DrawShaded;
    VertexColorMode vertexColors = VertexColorMode::Off;
    ColorChannel colorChannel = ColorChannel::Rgb;
    /** Linear wire color when not selected or preselected; alpha 0 keeps the default near black. */
    glm::vec4 wireColor{0.0f};
};
} // namespace lila::render
