#pragma once

#include "core/Export.h"

#include "uv/Chart.h"
#include "uv/Unwrap.h"

#include <span>
#include <string_view>

namespace lila::uv
{
enum class MirrorUvMode : uint8_t
{
    Flip,  // the partner island is the mirror image, laid beside its driver
    Stack, // the partner island shares the driver's uvs exactly (texture shared, wound the other way)
};

struct SymmetryUvOptions
{
    /** 0, 1 or 2: the mesh's persistent symmetry layer for that axis (built when missing). */
    int axis = 0;
    MirrorUvMode mode = MirrorUvMode::Flip;
    UnwrapOptions unwrap;
    /** Seams are mirrored across the plane first, so islands pair up. */
    bool mirrorSeams = true;
    /** Pack the result into 0-1 instead of keeping islands near where they were. */
    bool pack = false;
};

struct SymmetryUvResult
{
    size_t islands = 0;
    size_t pairs = 0;         // islands solved once and mirrored to their partner
    size_t selfSymmetric = 0; // islands across the plane, made mirror symmetric
    size_t asymmetric = 0;    // no partner (seams or topology not symmetric): solved alone
    Index seamsAdded = 0;
};

/**
 * Symmetry-aware unwrap of the faces (all when empty): islands on the positive side are solved and
 * copied to their mirror partners (flipped or stacked), and islands that straddle the plane are solved
 * and made exactly mirror symmetric about a vertical line in uv.
 */
LILA_SCENE_API SymmetryUvResult unwrapSymmetric(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const SymmetryUvOptions& opts = {});

/**
 * Makes a chart's uvs mirror symmetric: partner[v] is the vertex mirroring v (itself on the plane). Fits
 * the best mirror line, averages each pair across it, and turns the chart so the line is vertical.
 * Returns the largest correction applied, in uv units.
 */
LILA_SCENE_API double symmetrizeChart(Chart& chart, std::span<const Index> partner);
} // namespace lila::uv
