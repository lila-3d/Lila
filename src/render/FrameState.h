#pragma once

#include "render/RenderMesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::render
{
struct LineVertex
{
    glm::vec3 pos;
    glm::vec4 color;
};

/** World-space lines at one width in device pixels: depth tested like linesOnTop, or over every surface. */
struct LineBatch
{
    std::vector<LineVertex> vertices; // pairs
    float width = 1.0f;
    bool onTop = false;
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
    /** corners are view NDC (x, y; z unused) rather than world points: glued to the screen, untouched by the camera. */
    bool screen = false;
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1; v0 is the image's top row
    TexturePtr image;                     // sRGB
    float opacity = 1.0f;
    float brightness = 0.0f; // added in display space
    float contrast = 1.0f;   // around mid gray, in display space
    bool grayscale = false;
    /** Only the image's edges, in outlineColor (display-encoded): a tracing sheet that leaves what is under it visible. */
    bool outline = false;
    glm::vec3 outlineColor{0.25f, 0.85f, 1.0f};
    Depth depth = Depth::Behind;
};

/** A 3D colour lookup table (an Adobe .cube): display-encoded rgb in, rgb out, red varying fastest. */
struct Lut3D
{
    int size = 0;
    std::vector<float> rgb; // size^3 entries of 3 floats
    uint64_t key = nextResourceKey();
};

/** How to turn the scene's light into the picture: tonemapper, grading, bloom and lens effects (scene::postProcess). */
struct PostSettings
{
    enum Tonemapper
    {
        Neutral, // Khronos PBR Neutral: base colours stay true, the default
        AgX,
        Aces,
        Linear, // clipped
    };
    Tonemapper tonemapper = Neutral;
    int agxLook = 0; // 0 none, 1 punchy, 2 golden
    /** Grading, bloom and lens effects below apply only when on; the tonemapper and auto exposure always do. */
    bool effects = false;
    bool autoExposure = false;
    float exposureCompensation = 0.0f;       // stops on top of the auto exposure
    float exposureMin = -6.0f, exposureMax = 6.0f; // stops the auto exposure may reach
    float exposureSpeed = 3.0f;              // per second; 0 jumps
    float contrast = 1.0f, saturation = 1.0f;
    glm::vec3 whiteBalance{1.0f};            // per channel gains (from temperature and tint)
    glm::vec3 lift{0.0f}, gamma{1.0f}, gain{1.0f};
    float bloom = 0.0f;                      // blend toward the bloom, 0..1
    float bloomRadius = 1.0f;                // tent filter radius in texels per level
    float vignette = 0.0f, grain = 0.0f;
    float chromaticAberration = 0.0f;        // pixels of colour split at the corners
    float distortion = 0.0f;                 // negative barrel, positive pincushion
    float sharpen = 0.0f;
    std::shared_ptr<const Lut3D> lut;
    float lutIntensity = 1.0f;
    /** Off for passes whose values must come out exact, such as object mattes. */
    bool dither = true;
};

/** A light the viewport shades with: the scene's lights its own provider handles, in world space. */
struct LightDraw
{
    enum Kind
    {
        Point,
        Spot,
        Directional,
        Area, // width by height, facing along direction
        Disc, // width is the diameter
    };
    Kind kind = Point;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // where the light travels
    glm::vec3 right{1.0f, 0.0f, 0.0f};      // area lights: the width axis
    glm::vec3 radiance{1.0f};               // colour times intensity, in the shader's units (1 lights like the default environment)
    float range = 0.0f;                     // cm; 0 has no cutoff
    float cosInner = 1.0f, cosOuter = 0.0f; // spot cone
    float width = 0.0f, height = 0.0f;      // cm
    float radius = 0.0f;                    // cm, softens highlights and shadows
    /** The light casts shadows: traced on GPUs with ray queries (any kind), else a shadow map (directional and spot). */
    bool castsShadows = false;
    /** World to the shadow map's clip space (reversed-Z), when it has one. */
    bool shadows = false;
    glm::mat4 shadowViewProj{1.0f};
    float shadowTexel = 1.0f; // world size of a shadow texel at the light's focus, cm, for the normal offset
    /**
     * For contact-hardening shadow maps: the map's near and far distances (cm), the light's size (the sun's angular radius
     * as a tangent, a spot's source radius in cm), and shadow uv per cm across (per cm at 1 cm away for a spot).
     */
    glm::vec4 shadowDepth{0.0f};
    /** Lighting channel bits (at most 20): it lights only meshes that share one. */
    uint32_t lightingChannels = 1;
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
    /** Where the grid and its axes put zero: the scene's display origin, world cm. */
    glm::dvec3 gridOrigin{0.0};
    /** Linear object wire colours: selected, selected secondary (see selectionOrderColors), preselected, under a selected parent, and the default. */
    glm::vec4 wireSelected{0.92f, 0.92f, 0.92f, 1.0f};
    glm::vec4 wireSelectedOther{0.92f, 0.92f, 0.92f, 1.0f};
    glm::vec4 wirePreselect{0.75f, 0.75f, 0.75f, 1.0f};
    glm::vec4 wireUnderSelected{0.55f, 0.55f, 0.55f, 1.0f};
    glm::vec4 wireDefault{0.02f, 0.02f, 0.02f, 1.0f};
    DrawList meshes;
    float exposure = 1.0f; // linear multiplier
    float gamma = 1.0f;    // display gamma on top of the tonemap, like Maya's viewport gamma
    /** Requested MSAA count (1, 2, 4, 8); the renderer clamps it to the device and rebuilds its targets on change. */
    int msaaSamples = 8;
    /** Temporal antialiasing: the scene jitters per frame and blends into a history, which smooths shading and texture shimmer MSAA can't. */
    bool taa = true;
    /** Jittered frames a still view keeps drawing and averaging after it changes, for a supersampled result; 1 stops at once. */
    int accumulate = 16;
    PostSettings post;
    /**
     * Depth of field: the camera's aperture radius (cm; 0 for none) and focus distance along the view. With temporal
     * antialiasing on, each frame looks through another point of the aperture, so a still view or a render converges on
     * true lens blur. blades 0 is a round aperture.
     */
    float lensRadius = 0.0f;
    float focusDistance = 500.0f;
    int apertureBlades = 0;
    float apertureRotation = 0.0f; // degrees
    std::vector<LightDraw> lights;
    /** Up to four shadowed lights trace their shadows against the scene (GPUs with ray queries) instead of shadow maps. */
    bool rtShadows = false;
    /** Exponential height fog, lit by the first directional light as well as its own colour. Lengths cm. */
    struct Fog
    {
        bool on = false;
        glm::vec3 color{0.6f, 0.7f, 0.8f}; // linear, scaled by the environment intensity like ambient light
        float density = 0.0002f;           // per cm at the fog height
        float height = 0.0f;               // where the density is as given
        float falloff = 0.002f;            // per cm upward: density halves every ln2 / falloff
        float start = 0.0f;                // no fog nearer the camera than this
        float sunScatter = 0.5f;           // how much the sun lights the fog
        float anisotropy = 0.6f;           // forward scattering toward the sun, 0 to 0.95
    } fog;
    /** No backdrop: what no surface covers stays clear (alpha 0), for renders composited later. */
    bool transparentBackground = false;
    /**
     * Frames drawn after this one average into it as they are, with no reprojection: the subframes of a render, whose
     * cameras and objects may move between them for motion blur.
     */
    bool accumulateOnly = false;
    bool grid = true;
    bool backfaceCulling = false; // culls back faces of every material; off, every material shows them
    /** Back faces of two-sided materials shade with their normal flipped toward the camera; off, they light from behind. */
    bool twoSidedLighting = true;
    /** Display-encoded rgb mixed over back faces by alpha, to spot reversed normals; alpha 0 is off. */
    glm::vec4 backfaceTint{0.0f};
    bool xray = false;            // shaded meshes see-through
    /** Materials shade with a 50% grey base colour; their maps' alpha, normals, ORM, emission and shading models still apply. */
    bool greyBaseColor = false;
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
    /** Colour of the environment's light on surfaces: 0 lights with its brightness alone (gray), 1 with the HDRI's own tint. */
    float envLightSaturation = 1.0f;
    float envBackgroundExposure = 0.6f; // dims the backdrop only, so assets stand out
    float groundShadow = 0.5f;          // soft contact blob under meshes resting on the ground; 0 is off
    bool ambientOcclusion = true;       // screen-space, on indirect light only
    /** AO from the main pass's depth, darkening the whole finished mesh (Maya's way): cheaper, as it skips the prepass. */
    bool aoFast = false;
    /** AO traced against the scene (on GPUs with ray queries) instead of from the screen: no halos, and occluders off screen count. */
    bool aoRayTraced = false;
    float aoRadius = 30.0f;             // world units (cm)
    float aoIntensity = 1.0f;
    // Preview-only post over the shaded scene (not lines or overlays): a painterly anisotropic Kuwahara filter and outlines.
    bool kuwahara = false;
    float kuwaharaRadius = 4.0f; // device pixels
    bool outline = false;
    float outlineWidth = 1.0f;  // device pixels
    glm::vec3 outlineColor{0.01f}; // linear
    std::vector<ImagePlaneDraw> imagePlanes;
    /**
     * An external viewport renderer draws the scene instead of Lila: its image replaces the backdrop, grid fill, AO,
     * post and shaded meshes, and meshes keep only their wires and selection highlights, depth tested against its depth.
     * On with no colour yet (the first frame has not arrived), the view shows the plain background.
     */
    bool externalScene = false;
    /** Straight alpha over the background, row 0 at the top: RGBA8 sRGB or RGBA16F linear. Stretched to the view. */
    TexturePtr externalColor;
    /** R32F distance along the view axis in cm, 0 or less where nothing was hit; null when the renderer gave none. */
    TexturePtr externalDepth;
    /** externalColor and externalDepth store row 0 at the bottom. */
    bool externalBottomUp = false;
    /** externalColor is premultiplied; the draw divides alpha out. */
    bool externalPremultiplied = false;
    /** Multiplies externalColor's rgb. */
    float externalExposure = 1.0f;
    /**
     * When set, externalDepth holds OpenGL window depth (1 or zero alpha: nothing hit), turned into view distance by
     * z = (x * ndc + y) / (z + w * (1 - ndc)): see viewportRenderers' Frame::glDepthProj.
     */
    std::optional<glm::vec4> externalDepthUnproject;
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
    /** Debug drawing (debug/DebugDraw.h), shared between frames while it doesn't change. Triangles are blended and depth tested. */
    std::shared_ptr<const std::vector<LineBatch>> debugLines;
    std::shared_ptr<const std::vector<LineVertex>> debugTris;
    std::vector<LineVertex> overlay; // NDC positions, drawn last with no depth test (marquee, HUD)
    std::vector<OverlayVertex> overlayTris; // drawn after everything else
    std::shared_ptr<const OverlayAtlas> atlas;
};
} // namespace lila::render
