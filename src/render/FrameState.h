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
    std::vector<MeshDraw> meshes;
    float exposure = 1.0f; // linear multiplier
    float gamma = 1.0f;    // display gamma on top of the tonemap, like Maya's viewport gamma
    /** Requested MSAA count (1, 2, 4, 8); the renderer clamps it to the device and rebuilds its targets on change. */
    int msaaSamples = 8;
    bool grid = true;
    bool backfaceCulling = false; // culls back faces even on two-sided materials
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
    float aoRadius = 30.0f;             // world units (cm)
    float aoIntensity = 1.0f;
    std::vector<ImagePlaneDraw> imagePlanes;
    std::vector<LineVertex> lines;
    std::vector<LineVertex> linesOnTop; // like lines, biased toward the camera and writing no depth (edges over faces)
    std::vector<LineVertex> tris3d;     // world-space triangle list, blended and biased toward the camera, no depth write
    std::vector<LineVertex> linesXray;  // world-space lines with no depth test, over every surface (x-ray joints)
    std::vector<LineVertex> overlay; // NDC positions, drawn last with no depth test (marquee, HUD)
    std::vector<OverlayVertex> overlayTris; // drawn after everything else
    std::shared_ptr<const OverlayAtlas> atlas;
};
} // namespace lila::render
