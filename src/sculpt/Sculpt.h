#pragma once

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "render/RenderMesh.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/**
 * Sculpting on a dense mesh: brushes, mask and freeze layers, exact mirror strokes, and the GPU copy kept in step with
 * only the vertices each dab touched. Everything is in the mesh's object space.
 */
namespace lila::sculpt
{
using geo::Index;

enum class Brush : uint8_t
{
    Draw,
    ClayStrips,
    Inflate,
    Grab,
    Smooth,
    Flatten,
    Pinch,
    Crease,
    Scrape,
    Mask,
    Relax,
    Freeze,
    Count
};

enum class Falloff : uint8_t
{
    Smooth,
    Sphere,
    Root,
    Sharp,
    Linear,
    Constant,
    Count
};

std::string_view brushName(Brush b);
std::optional<Brush> brushFromName(std::string_view name);
std::string_view falloffName(Falloff f);
std::optional<Falloff> falloffFromName(std::string_view name);
/** Weight at t = distance / radius: 1 at the centre, 0 from the rim on. */
double falloffWeight(Falloff f, double t);

struct BrushSettings
{
    Brush brush = Brush::Draw;
    Falloff falloff = Falloff::Smooth;
    /** 0 to 1. */
    double strength = 0.5;
    /** Ctrl: dig instead of add, unmask, unfreeze, magnify instead of pinch. */
    bool invert = false;
    /** Leave points facing away from the view, so a stroke doesn't reach through thin parts. */
    bool frontFacesOnly = false;
};

/** One brush stamp. */
struct Dab
{
    glm::dvec3 center{0.0};
    /** Surface normal under the cursor, unit length. */
    glm::dvec3 normal{0.0, 1.0, 0.0};
    double radius = 1.0;
    double pressure = 1.0;
    /** Into the screen; zero when unknown. */
    glm::dvec3 viewDir{0.0};
    /** Grab: how far the cursor moved since the last dab. */
    glm::dvec3 delta{0.0};
    /** Direction of travel, for clay strips' square footprint; zero when unknown. */
    glm::dvec3 direction{0.0};
};

/** What a stroke or mask operation changed, enough to undo and redo it. */
struct StrokeRecord
{
    std::vector<Index> points;
    std::vector<glm::dvec3> before, after;
    std::vector<float> maskBefore, maskAfter;
    std::vector<uint8_t> frozenBefore, frozenAfter;

    bool empty() const { return points.empty(); }
};

enum class MaskOp : uint8_t
{
    Clear,
    Fill,
    Invert,
    Blur,
};

enum class Layer : uint8_t
{
    Mask,
    Freeze,
};

/**
 * A mesh being sculpted. Topology never changes; positions, the soft mask (1 = fully masked) and the freeze flags
 * (frozen points never move) do. Strokes run in parallel and are deterministic for the same dabs.
 */
class SculptMesh
{
public:
    SculptMesh() = default;
    explicit SculptMesh(geo::Mesh mesh);

    const geo::Mesh& mesh() const { return mMesh; }
    std::span<const glm::vec3> normals() const { return mNormals; }
    std::span<const float> mask() const { return mMask; }
    std::span<const uint8_t> frozen() const { return mFrozen; }
    const geo::MeshBvh& bvh() const { return mBvh; }
    std::span<const Index> neighbours(Index p) const
    {
        return {mAdj.data() + mAdjOffsets[p], mAdj.data() + mAdjOffsets[p + 1]};
    }

    /** Mirror strokes across the plane, matching points within tolerance. nullopt turns symmetry off. */
    void setSymmetry(const std::optional<geo::SymmetryPlane>& plane, double tolerance);
    bool symmetric() const { return !mSymmetry.empty(); }
    const geo::SymmetryMap& symmetry() const { return mSymmetry; }

    bool raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const;
    /** Smooth normal at a hit, blended from the point normals. */
    glm::dvec3 normalAt(const geo::BvhHit& hit) const;

    void beginStroke(const BrushSettings& settings);
    void dab(const Dab& d);
    /** Finishes the stroke; the record holds every point it changed. */
    StrokeRecord endStroke();
    bool stroking() const { return mStroking; }

    /** Puts back the record's before (undo) or after (redo) state. */
    void apply(const StrokeRecord& r, bool undo);
    StrokeRecord maskOp(MaskOp op, Layer layer = Layer::Mask, int iterations = 1);
    void setPositions(std::span<const glm::dvec3> positions);
    void setMask(std::span<const float> mask);

    /** One vertex per point, smooth shaded, edited in place as the mesh changes. */
    std::shared_ptr<const render::RenderMesh> renderMesh();

    /** Changes whenever positions do. */
    uint64_t version() const { return mVersion; }
    /** Changes whenever the mask or freeze layers do. */
    uint64_t maskVersion() const { return mMaskVersion; }

private:
    struct Side
    {
        Dab dab;
        std::vector<Index> points;
        std::vector<double> weights; // falloff only
        glm::dvec3 areaNormal{0.0};
        glm::dvec3 areaCenter{0.0};
    };

    void gather(Side& s, bool forGrab);
    void computeArea(Side& s) const;
    void touch(Index p);
    void addDisplacement(const Side& s);
    void addMaskChange(const Side& s);
    void symmetrizeChanges();
    void commitChanges();
    void refresh(std::span<const Index> moved);
    void updateNormals(std::span<const Index> points);
    void writeVertices(std::span<const Index> points);
    glm::dvec3 originalPosition(Index p) const;
    glm::dvec3 projectToOriginal(Index p, const glm::dvec3& at) const;
    double pointWeight(Index p, double falloff) const;

    geo::Mesh mMesh;
    geo::MeshBvh mBvh;
    std::vector<glm::vec3> mNormals;
    std::vector<float> mMask;
    std::vector<uint8_t> mFrozen;
    std::vector<uint32_t> mAdjOffsets;
    std::vector<Index> mAdj;
    std::vector<uint32_t> mTriOffsets;
    std::vector<Index> mTris; // triangles around each point
    geo::SymmetryMap mSymmetry;

    BrushSettings mSettings;
    bool mStroking = false;
    std::vector<Side> mGrab;
    // Per stroke: the state before a point was first touched.
    std::vector<uint32_t> mStrokeStamp;
    std::vector<uint32_t> mRecordIndex;
    uint32_t mStroke = 0;
    StrokeRecord mRecord;

    // Per dab: pending changes.
    std::vector<uint32_t> mDabStamp;
    uint32_t mDabId = 0;
    std::vector<Index> mChanged;
    std::vector<glm::dvec3> mDisp;
    std::vector<float> mMaskDelta;
    std::vector<int8_t> mFreezeSet; // -1 clear, 1 set, 0 untouched

    std::vector<uint32_t> mGatherStamp;
    uint32_t mGatherId = 0;

    std::shared_ptr<render::RenderMesh> mRender;
    uint64_t mVersion = 1;
    uint64_t mMaskVersion = 1;
};
} // namespace lila::sculpt
