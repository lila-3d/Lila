#pragma once

#include "core/Export.h"
#include "groom/Groom.h"
#include "curve/Spline.h"
#include "geo/Mesh.h"

#include <functional>
#include <string>
#include <vector>

/** Hair cards: textured strips along hair curves or clumps of them, laid out in a texture atlas, for game hair. */
namespace lila::groom
{
enum class CardSource : uint8_t
{
    Curves, // one card per curve
    Clumps, // curves gathered into clumps by their roots, one card per clump
    Guides, // one card per guide curve; the curves nearest each guide size it
};

enum class CardFacing : uint8_t
{
    SurfaceNormal, // flat to the scalp under it (needs a surface; else up)
    CurveFrame,    // rotation minimising frames along the curve
    UpVector,      // facing the up vector
};

struct CardOptions
{
    CardSource source = CardSource::Curves;
    int clumps = 20;
    double width = 1.0;
    /** Width from the spread of a clump's curves (times widthScale) instead of width. */
    bool autoWidth = false;
    double widthScale = 1.0;
    /** Width along the card, t 0 root to 1 tip; null keeps it constant. */
    std::function<double(double)> taper;
    /** Quads along each card; 0 follows the curve's own point count. */
    int segments = 8;
    int widthSegments = 1;
    CardFacing facing = CardFacing::SurfaceNormal;
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** Turn about the curve, radians: tilt everywhere, twist growing from root to tip. */
    double tilt = 0.0;
    double twist = 0.0;
    /** Lift off the scalp along the card's facing, cm. */
    double offset = 0.0;
    /** Atlas: columns x rows cells; variations uses the first that many (0: all), picked at random by seed. */
    int columns = 4;
    int rows = 1;
    int variations = 0;
    uint32_t seed = 1;
    bool flipU = false;
    bool flipV = false;
    /** Gap kept inside each cell, as a fraction of it. */
    double padding = 0.0;
    /** A back face for each card (separate points, so normals stay clean). */
    bool doubleSided = false;
};

struct CardResult
{
    geo::Mesh mesh;
    int cards = 0;
    /** Atlas cell of each card. */
    std::vector<int> cells;
    std::vector<std::string> warnings;
};

/** Cards for the curves (and guides for CardSource::Guides); scalp optional, for facing and attaching. Quads, uv0. */
LILA_SCENE_API CardResult hairCards(const Splines& curves, const Splines& guides, const groom::Surface* scalp, const CardOptions& o);
} // namespace lila::groom
