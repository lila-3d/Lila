#pragma once

#include "render/RenderMesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lila::render
{
struct LineVertex
{
    glm::vec3 pos;
    glm::vec4 color;
};

/**
 * Everything a viewport needs for one frame, copied out of the scene on the main thread. The render thread only
 * ever sees these snapshots, never the live graph. An offline renderer would consume the same data.
 */
/** One component dot: a world-space point and how it shows. */
struct PointVertex
{
    glm::vec3 pos;
    uint32_t state = 0; // PointDraw::Normal, Selected, Mirror or Hidden
};

/** Component dots built once per mesh, selection and view state, so the renderer can keep them on the GPU. */
struct PointCloud
{
    std::vector<PointVertex> points;
    uint64_t key = nextResourceKey();
};

/** Dots drawn as squares of a constant pixel size, depth tested and pulled toward the camera like lines. */
struct PointDraw
{
    enum State : uint32_t
    {
        Normal,
        Selected,
        Mirror, // the mirror image of a selected point
        Hidden,
    };
    std::shared_ptr<const PointCloud> cloud;
    glm::vec4 colors[3]{}; // linear, by state
    float size = 4.0f;     // device pixels
    float selectedSize = 6.0f;
};

/** Pixel-space triangle vertex for the 2D overlay (gizmos, HUD, text). */
struct OverlayVertex
{
    glm::vec2 pos; // pixels, origin top left
    glm::vec2 uv;  // atlas coordinates; u < 0 means a solid fill
    glm::vec4 color;
};

/** Single-channel coverage texture for overlay text. Replace the pointer to upload a new one. */
struct OverlayAtlas
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

/**
 * A reference image on a world-space quad (an image plane). It is drawn in display space: no lighting, exposure, gamma,
 * tonemapping or AO, so the picture's colours stay as authored.
 */
struct ImagePlaneDraw
{
    enum class Depth
    {
        Behind, // under every mesh, no depth test
        Scene,  // depth tested like geometry
        Front,  // over every mesh
    };
    glm::vec3 corners[4]{};               // top left, top right, bottom right, bottom left
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1; v0 is the image's top row
    TexturePtr image;                     // sRGB
    float opacity = 1.0f;
    float brightness = 0.0f; // added in display space
    float contrast = 1.0f;   // around mid gray, in display space
    bool grayscale = false;
    Depth depth = Depth::Behind;
};

struct FrameState
{
    glm::dmat4 view{1.0};
    glm::dmat4 proj{1.0}; // reversed-Z, infinite far
    glm::dvec3 eye{0.0};
    bool zUp = false;
    float gridSpacing = 10.0f; // cm between minor lines
    int gridMajorEvery = 10;
    glm::vec3 background{0.24f, 0.25f, 0.27f};
    /** Grid line colour and the two ground axes (X, then Z or Y when Z-up), in the grid shader's linear values. */
    glm::vec3 gridColor{0.42f};
    glm::vec3 gridAxisX{0.0f};
    glm::vec3 gridAxisZ{0.0f};
    /** Linear object wire colours: selected, preselected, under a selected parent, and the default. */
    glm::vec4 wireSelected{0.92f, 0.92f, 0.92f, 1.0f};
    glm::vec4 wirePreselect{0.75f, 0.75f, 0.75f, 1.0f};
    glm::vec4 wireUnderSelected{0.55f, 0.55f, 0.55f, 1.0f};
    glm::vec4 wireDefault{0.02f, 0.02f, 0.02f, 1.0f};
    std::vector<MeshDraw> meshes;
    float exposure = 1.0f; // linear multiplier
    float gamma = 1.0f;    // display gamma on top of the tonemap, like Maya's viewport gamma
    /** Requested MSAA count (1, 2, 4, 8); the renderer clamps it to the device and rebuilds its targets on change. */
    int msaaSamples = 8;
    bool grid = true;
    bool backfaceCulling = false; // culls back faces even on two-sided materials
    /** Back faces of two-sided materials shade with their normal flipped toward the camera; off, they light from behind. */
    bool twoSidedLighting = true;
    /** Display-encoded rgb mixed over back faces by alpha, to spot reversed normals; alpha 0 is off. */
    glm::vec4 backfaceTint{0.0f};
    bool xray = false;            // shaded meshes see-through
    /** Shaded lighting: the environment (falls back to the studio rig while none is loaded), the studio rig, or flat. */
    enum class Lighting
    {
        Environment,
        Studio,
        Flat
    };
    Lighting lighting = Lighting::Environment;

    // Image-based lighting. An empty path is the built-in neutral studio; changing it loads the new HDRI off-thread.
    std::string envPath;
    float envRotation = 0.0f; // degrees around the up axis
    float envIntensity = 1.0f;
    bool envBackground = true;
    float envBlur = 0.6f;       // 0 sharp, 1 fully diffuse
    float envSaturation = 0.0f; // background only; 0 is gray
    float envBackgroundExposure = 0.6f; // dims the backdrop only, so assets stand out
    float groundShadow = 0.5f;          // soft contact blob under meshes resting on the ground; 0 is off
    bool ambientOcclusion = true;       // screen-space, on indirect light only
    /** AO from the main pass's depth, darkening the whole finished mesh (Maya's way): cheaper, as it skips the prepass. */
    bool aoFast = false;
    float aoRadius = 30.0f;             // world units (cm)
    float aoIntensity = 1.0f;
    // Preview-only post over the shaded scene (not lines or overlays): a painterly anisotropic Kuwahara filter and outlines.
    bool kuwahara = false;
    float kuwaharaRadius = 4.0f; // device pixels
    bool outline = false;
    float outlineWidth = 1.0f;  // device pixels
    glm::vec3 outlineColor{0.01f}; // linear
    std::vector<ImagePlaneDraw> imagePlanes;
    std::vector<LineVertex> lines;
    std::vector<LineVertex> linesOnTop; // like lines, biased toward the camera and writing no depth (edges over faces)
    /** Selected component edges: drawn like linesOnTop, at their own width in device pixels. */
    std::vector<LineVertex> selectedEdges;
    float selectedEdgeWidth = 1.0f;
    /** UV seams over meshes: drawn like linesOnTop, at their own width in device pixels. */
    std::vector<LineVertex> seamLines;
    float seamLineWidth = 1.0f;
    /** Mesh edges on the symmetry plane: drawn like linesOnTop, under the selected edges and hover, at their own width in device pixels. */
    std::vector<LineVertex> symmetryLines;
    float symmetryLineWidth = 1.0f;
    /** Highlighted mesh edges (hard, border, non-manifold...): drawn like linesOnTop, under the symmetry and selected edges, at their own width in device pixels. */
    std::vector<LineVertex> highlightEdges;
    float highlightEdgeWidth = 1.0f;
    /**
     * Curve shapes, drawn like linesOnTop at their own widths in device pixels: each width's dark halo under its lines,
     * plain curves first, then the bold (highlighted) ones over them.
     */
    std::vector<LineVertex> curveHalo, curveLines, curveHaloBold, curveLinesBold;
    float curveWidth = 2.0f, curveBoldWidth = 3.0f, curveHaloWidth = 4.0f, curveHaloBoldWidth = 5.0f;
    std::vector<LineVertex> tris3d;     // world-space triangle list, blended and biased toward the camera, no depth write
    std::vector<PointDraw> points;      // component dots, drawn after tris3d
    std::vector<LineVertex> linesXray;  // world-space lines with no depth test, over every surface (x-ray joints)
    std::vector<LineVertex> overlay; // NDC positions, drawn last with no depth test (marquee, HUD)
    std::vector<OverlayVertex> overlayTris; // drawn after everything else
    std::shared_ptr<const OverlayAtlas> atlas;
};
} // namespace lila::render
