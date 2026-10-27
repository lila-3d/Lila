#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <string>

namespace lila::uv
{
enum class AlignFit : uint8_t
{
    Rigid,      // rotation and translation
    Similarity, // plus uniform scale
    Stretch,    // plus a different scale per axis (a longer plank, a squatter rock)
};

struct AlignOptions
{
    AlignFit fit = AlignFit::Stretch;
    /** Try the source's principal axes in every orientation, not only the placement given. */
    bool search = true;
    /** Also try mirror images, for left and right pieces. */
    bool mirror = true;
    int iterations = 40;
};

struct AlignResult
{
    /** Takes the moving mesh's object space into the fixed mesh's object space. */
    glm::dmat4 transform{1.0};
    /** Root mean square distance after fitting, as a fraction of the fixed mesh's bounding box diagonal. */
    double residual = 0.0;
    bool searched = false; // a principal-axis start beat the placement given
    bool mirrored = false;
};

/**
 * Fits the moving mesh onto the fixed one by iterative closest points, both ways so neither can collapse onto part
 * of the other. Starts from initial (moving to fixed object space) and, with search, from the principal-axis
 * frames in all 24 orientations (48 with mirror); the placement given wins unless another start fits clearly better.
 */
LILA_SCENE_API AlignResult alignShapes(const geo::Mesh& moving, const geo::Mesh& fixed, const glm::dmat4& initial,
                                       const AlignOptions& opts = {});

struct GuidedUvOptions
{
    std::string sourceSet = "uv0";
    std::string targetSet = "uv0";
    /** Passes pulling the target snugly onto the source before sampling, so slightly different shapes still line up. */
    int wrapIterations = 8;
    /**
     * Faces with nothing like them on the source (a cap where the source is open) become islands of their own, placed
     * beside the rest; off, they join the island next to them.
     */
    bool separateUnmatched = true;
    /** Relax each island (SLIM from the transferred layout) before fitting it back onto the source island. */
    bool relax = true;
    int relaxIterations = 20;
};

struct GuidedUvResult
{
    size_t islands = 0;
    size_t relaxed = 0;  // islands relaxed from the transferred layout
    size_t repaired = 0; // islands whose transferred layout folded in places, filled in around the parts that held
    size_t unfolded = 0; // islands whose transferred layout could not be repaired, so they were unwrapped from scratch
    size_t unmatched = 0; // islands with nothing like them on the source, unwrapped on their own and placed beside the rest
    geo::Index seams = 0;
};

/**
 * The source's UV layout used as a guide for a similar mesh of any topology: each target face takes the source
 * island it lies on, the borders between them become seams, every target island is unwrapped (starting from the
 * transferred uvs) and then fitted by rotation, scale and position onto its source island. toSource takes the
 * target's object space into the source's. Writes "seam" on the target's edges.
 */
LILA_SCENE_API GuidedUvResult guidedUvs(const geo::Mesh& source, geo::Mesh& target, const glm::dmat4& toSource,
                                        const GuidedUvOptions& opts = {});
} // namespace lila::uv
