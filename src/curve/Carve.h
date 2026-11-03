#pragma once

#include "core/Export.h"
#include "geo/Mesh.h"

#include <string>

/**
 * Carving a closed cutter into a surface where they overlap, open surfaces (terrain) as well as closed ones. Only the
 * faces near the cutter go through the boolean, re-centred on it, so km-wide meshes carve as fast and as exactly as
 * small ones; everything else, and the patch's border, keeps its points exactly.
 */
namespace lila::curve
{
struct CarveOptions
{
    /** World up: the side of the surface the cutter comes from. Faces turned further than facingAngle from it aren't carved. */
    glm::dvec3 up{0.0, 1.0, 0.0};
    double facingAngle = 100.0;
    /** Material slot the cut's walls take; empty takes the surface's first. */
    std::string material;
    /** New edges sharper than this many degrees are marked hard. */
    double hardAngle = 30.0;
};

struct CarveResult
{
    geo::Mesh mesh;
    /** Faces of the surface that went through the boolean. */
    size_t patchFaces = 0;
    /** Border points of the patch that found no partner to weld back to (0 when the cut stayed inside it). */
    size_t unwelded = 0;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Cuts the cutter (closed, world space) out of the surface (its own space, placed by toWorld). The result is in the surface's space. */
LILA_SCENE_API CarveResult carve(const geo::Mesh& surface, const glm::dmat4& toWorld, const geo::Mesh& cutterWorld, const CarveOptions& o = {});
} // namespace lila::curve
