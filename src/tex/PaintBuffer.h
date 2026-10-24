#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "brush/Brush.h"

#include <glm/glm.hpp>

#include <vector>

/**
 * The projection paint buffer (Mari's paint buffer): a screen-space RGBA image at its own resolution that the user
 * paints, erases, blurs, smudges and clones in 2D, places images and stencils on, and moves, scales, rotates and warps
 * before projecting it into every visible texel (a "project" stroke, tex/PaintStrokes.h). Pixels hold straight colour
 * in rgb and coverage in a, row 0 at the top, like the paint pages. CPU and deterministic.
 */
namespace lila::tex::paint
{
/** Placement of a picture on the screen: moved by offset (pixels), scaled and turned (radians) about its centre. */
struct LILA_SCENE_API Placement
{
    glm::vec2 offset{0.0f};
    float scale = 1.0f;
    float rotation = 0.0f;

    bool identity() const { return offset == glm::vec2(0.0f) && scale == 1.0f && rotation == 0.0f; }
    /** Where a point of the picture (pixels, centre at `center`) lands. */
    glm::vec2 apply(const glm::vec2& p, const glm::vec2& center) const;
    /** The picture point that lands at p. */
    glm::vec2 invert(const glm::vec2& p, const glm::vec2& center) const;
};

/** A greyscale mask pinned to the screen: painting goes through it. */
struct LILA_SCENE_API Stencil
{
    image::Image image;
    Placement place;
    bool invert = false;
    /** Shown over the view at this opacity; it masks fully whatever this is. */
    float opacity = 0.5f;
    /** Repeat the image past its edges instead of masking everything outside it. */
    bool tile = false;

    bool active() const { return image.width > 0; }
    /** 0 to 1 at a buffer pixel of a w x h buffer (the stencil image spans the buffer's height at scale 1). */
    float at(const glm::vec2& p, int w, int h) const;
};

/** A brush stamp in the buffer, pixels. */
struct BufferDab
{
    glm::vec2 center{0.0f};
    float radius = 10.0f;
    /** Per dab amount, 0 to 1. */
    float opacity = 1.0f;
    /** For smudge: where the previous dab was. */
    glm::vec2 previous{0.0f};
};

class LILA_SCENE_API ProjectionBuffer
{
public:
    /** Resizes and clears. */
    void resize(int width, int height);
    int width() const { return mWidth; }
    int height() const { return mHeight; }
    /** RGBA floats of the content (before placement and warp). */
    const std::vector<float>& pixels() const { return mPixels; }
    /** Whether any pixel has coverage. */
    bool empty() const;
    void clear();
    /** Bumped by every change, for display caching. */
    uint64_t version() const { return mVersion; }

    void paint(const BufferDab& d, const brush::FalloffTable& f, const glm::vec3& color, const Stencil* stencil = nullptr);
    void erase(const BufferDab& d, const brush::FalloffTable& f, const Stencil* stencil = nullptr);
    /** Blurs what is under the dab over kernel pixels. */
    void blur(const BufferDab& d, const brush::FalloffTable& f, float kernel, const Stencil* stencil = nullptr);
    /** Drags what was at the previous dab to this one. */
    void smudge(const BufferDab& d, const brush::FalloffTable& f, const Stencil* stencil = nullptr);
    /** Copies source (an image of the same size, e.g. the view's channel captured) from offset pixels away. */
    void clone(const BufferDab& d, const brush::FalloffTable& f, const image::Image& source, const glm::vec2& offset, const Stencil* stencil = nullptr);

    /**
     * Lays an image (rgb, alpha as coverage) over the content into rect (x, y, w, h, pixels; empty covers the buffer
     * keeping the image's aspect), times opacity.
     */
    void place(const image::Image& img, const glm::vec4& rect = glm::vec4(0.0f), float opacity = 1.0f);

    /** How the content shows: placement, then a warp grid of (warpCells + 1)^2 control point offsets (pixels). */
    Placement placement;
    int warpCells = 4;
    std::vector<glm::vec2> warp;

    bool transformed() const;
    /** The control point (i, j) of the warp grid, i across. */
    glm::vec2& warpPoint(int i, int j);
    /** Where control point (i, j) sits on the buffer before its offset. */
    glm::vec2 warpRest(int i, int j) const;
    /** The content as it shows (placement and warp applied, bilinear), what a commit projects. */
    image::Image resolved() const;
    /** Applies placement and warp to the content and resets them. */
    void bake();

private:
    template <class F> void forDab(const BufferDab& d, const brush::FalloffTable& f, const Stencil* stencil, F&& fn);
    glm::vec2 warpOffset(const glm::vec2& p) const;

    int mWidth = 0, mHeight = 0;
    std::vector<float> mPixels;
    uint64_t mVersion = 1;
};

/** Bilinear sample (over premultiplied colour, returned straight) of an RGBA image at a pixel position (centres at +0.5), zero outside. */
LILA_SCENE_API glm::vec4 sampleImage(const image::Image& img, const glm::vec2& p);
} // namespace lila::tex::paint
