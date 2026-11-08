#pragma once

#include "Scatter.h"
#include "Seams.h"

#include <string>
#include <vector>

namespace seamwork
{
/** Contact strips: ribbons along joins lying on one or both surfaces. Lengths are cm. */
struct StripOptions
{
    Side side = Side::Ground;
    double width = 40.0;
    /** Rows across each side. */
    int divisions = 3;
    /** Lift off the surface, against z-fighting. */
    double offset = 0.2;
    /** Distance between rows along the join. */
    double spacing = 20.0;
    /** cm of strip per texture repeat along U. */
    double tile = 100.0;
    /** Kinds taken, bit per Kind. */
    int kinds = 3;
    /** Material slot the faces use; empty for none. */
    std::string material;
    /** Alpha across the strip: 1 at the join fading to the edge (its width is set from width). */
    Band band;
};

/** Where a point drops onto the surface it belongs to; false keeps it where it is. */
using ConformFn = std::function<bool(const glm::dvec3& from, const glm::dvec3& dir, glm::dvec3& hit, glm::dvec3& normal)>;

/**
 * One mesh of strips, every joined curve one ribbon, its rows conformed to the surfaces: UV0 U along the length in tile
 * repeats, V across (0 at the join for one side; 0 wall edge, 0.5 join, 1 ground edge for both), vertex colour alpha the
 * band weight across. Point attribute "side" is 0 on the wall and 1 on the ground.
 */
Mesh buildStrips(const Result& r, const StripOptions& o, const ConformFn& conform, size_t* strips = nullptr);

/** A decal box for decal actors: centre, the projection direction (into the surface), the direction along the join, size. */
struct DecalBox
{
    std::string key;
    glm::dvec3 center{0.0};
    glm::dvec3 project{0.0, -1.0, 0.0};
    glm::dvec3 along{1.0, 0.0, 0.0};
    /** Depth along project, width across, length along. */
    glm::dvec3 size{20.0, 40.0, 100.0};
};

/** Boxes of segment length covering the strips (each side its own row of boxes). */
std::vector<DecalBox> decalBoxes(const Result& r, const StripOptions& o, double segment);
} // namespace seamwork
