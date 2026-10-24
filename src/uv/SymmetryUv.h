#pragma once

#include "core/Export.h"

#include "geo/Symmetry.h"
#include "uv/Chart.h"
#include "uv/Unwrap.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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
    /** Where the plane lies along the axis (the object's pivot). */
    double offset = 0.0;
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
 * Copies the faces' seams (all faces when empty) across the plane and opens closed islands, re-mirroring the cuts, so
 * the islands pair up. Returns the cuts the opening added.
 */
LILA_SCENE_API Index symmetricSeams(geo::Mesh& mesh, const geo::SymmetryMap& map, std::span<const Index> faces);

/**
 * Makes a chart's uvs mirror symmetric: partner[v] is the vertex mirroring v (itself on the plane). Fits
 * the best mirror line, averages each pair across it, and turns the chart so the line is vertical.
 * Returns the largest correction applied, in uv units.
 */
LILA_SCENE_API double symmetrizeChart(Chart& chart, std::span<const Index> partner);

/**
 * How a mesh's uv corners pair up across its symmetry plane, and how each island's uvs map onto its partner island's,
 * fitted from the uvs at build time. Islands are split wherever the uvs disagree.
 */
struct UvMirror
{
    /** Per corner capacity: the corner mirroring it (itself on the plane in a face that mirrors to itself), or kInvalid. */
    std::vector<Index> partner;
    /** Per corner capacity: +1 or -1 for the side it is on, 0 on the centre line of an island spanning the plane or unpaired. */
    std::vector<int8_t> side;
    /** Per corner capacity: its island, -1 for none. */
    std::vector<int> island;
    /** Per island: the island its partners are in (itself when it spans the plane), -1 for none. */
    std::vector<int> islandPartner;
    /** Per island: partner uv = linear * uv + offset; for a spanning island, the reflection across its centre line. */
    std::vector<glm::dmat2> linear;
    std::vector<glm::dvec2> offset;
    /** Per island: the fit had enough spread; otherwise linear is a plain U flip. */
    std::vector<uint8_t> fitted;
    /** Corner count per island. */
    std::vector<size_t> islandSize;

    bool empty() const { return islandPartner.empty(); }
};

LILA_SCENE_API UvMirror buildUvMirror(const geo::Mesh& mesh, const geo::SymmetryMap& map, std::string_view uvSet);

/** Refits linear, offset and fitted to other uvs (per corner capacity; missing ones read as zero). */
LILA_SCENE_API void fitUvMirror(UvMirror& mirror, std::span<const glm::vec2> uv);

/** The side most of the corners are on, +1 when tied or none are on a side. */
LILA_SCENE_API int uvMirrorDrive(const UvMirror& mirror, std::span<const Index> corners);

/** A corner that copies another's uv change through a linear map: uv = before + linear * (source uv - source before). */
struct UvFollow
{
    Index corner = kInvalid;
    Index source = kInvalid;
    glm::dmat2 linear{1.0};
};

/**
 * Rules for an edit that moved the corners: every partnered corner off the driving side follows its partner mirrored,
 * and centre-line corners keep only the part of their move along the line. A spanning island moved whole keeps its
 * move, so it stays symmetric about its moved line.
 */
LILA_SCENE_API std::vector<UvFollow> uvFollowRules(const UvMirror& mirror, std::span<const Index> moved, int drive);

/** Applies the rules (in order) to uv, given the uvs before the edit. Returns how many corners changed. */
LILA_SCENE_API size_t applyUvFollow(std::span<const UvFollow> rules, std::span<const glm::vec2> before, std::span<glm::vec2> uv);

/**
 * After an edit that re-solved the changed corners' islands (unwrap, relax, straighten): each partner island becomes the
 * driving island mapped through the fit from before, or flipped in place when there was none, and spanning islands
 * are made mirror symmetric about their own centre line. Returns how many corners changed.
 */
LILA_SCENE_API size_t symmetrizeUvIslands(const UvMirror& mirror, std::span<const Index> changed, int drive, std::span<glm::vec2> uv);
} // namespace lila::uv
