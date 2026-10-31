#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/**
 * Clay: signed-distance primitives blended with a smooth minimum, meshed with surface nets over sparse bricks. The field
 * lives in the clay object's space; every distance is in scene units (cm).
 */
namespace lila::clay
{
enum class Shape : uint8_t
{
    Sphere,    // radius
    Ellipsoid, // extents (semi-axes)
    Box,       // extents (half sizes), rounding
    Capsule,   // length between the end centres along Y, radius at -Y, endRadius at +Y (a round cone when they differ)
    Cylinder,  // radius, length along Y, rounding
    Cone,      // radius at -Y, endRadius at +Y, length along Y, rounding
    Torus,     // radius of the ring, endRadius of the tube, around Y
};

enum class Op : uint8_t
{
    Add,
    Subtract,
    Intersect,
};

struct Primitive
{
    Shape shape = Shape::Sphere;
    glm::dvec3 extents{10.0};
    double radius = 10.0;
    double endRadius = 10.0;
    double length = 20.0;
    double rounding = 0.0;
    glm::vec3 color{1.0f};

    bool operator==(const Primitive&) const = default;
};

/** One entry of a clay tree: a primitive, or a group (no shape) whose children blend first and then join as one. */
struct Item
{
    Op op = Op::Add;
    double blend = 0.0; // smooth radius of the join with what comes before it
    glm::dmat4 matrix{1.0}; // to the parent's space
    std::optional<Primitive> shape;
    std::vector<Item> children;

    bool operator==(const Item&) const = default;
};

struct Bounds
{
    glm::dvec3 min{1e300};
    glm::dvec3 max{-1e300};

    bool operator==(const Bounds&) const = default;
    bool empty() const { return min.x > max.x; }
    void extend(const glm::dvec3& p)
    {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void extend(const Bounds& b)
    {
        min = glm::min(min, b.min);
        max = glm::max(max, b.max);
    }
    Bounds grown(double by) const { return empty() ? *this : Bounds{min - by, max + by}; }
    bool overlaps(const Bounds& b) const
    {
        return !empty() && !b.empty() && glm::all(glm::lessThanEqual(min, b.max)) && glm::all(glm::lessThanEqual(b.min, max));
    }
};

struct Sample
{
    double distance = 1e30; // negative inside
    glm::vec3 color{1.0f};
};

/** Distance from p to the primitive in its own space, before any scale. */
LILA_SCENE_API double distance(const Primitive& prim, const glm::dvec3& p);
/** The primitive's own-space bounds. */
LILA_SCENE_API Bounds localBounds(const Primitive& prim);

/**
 * A clay tree flattened into an evaluation program, primitives carried into the root's space. symmetryX mirrors the
 * whole field across the YZ plane (the union of each side and its reflection), so either side can be modelled.
 */
class LILA_SCENE_API Field
{
public:
    Field() = default;
    explicit Field(const Item& root, bool symmetryX = false);

    Sample eval(const glm::dvec3& p) const;
    bool symmetric() const { return mSymmetric; }
    bool empty() const { return mPrims.empty(); }

    /** Where the surface can be: each top-level added primitive's or group's bounds, grown by the largest blend radius. */
    const std::vector<Bounds>& addBounds() const { return mAddBounds; }

    /** The program cut down to what can change the field near region; values within pad of region match eval(). */
    class LILA_SCENE_API Local
    {
    public:
        Sample eval(const glm::dvec3& p) const;

    private:
        friend class Field;
        const Field* mField = nullptr;
        std::vector<int> mCode; // instruction indices, kEmpty for an intersect that empties its group here
    };
    Local local(const Bounds& region, double pad) const;

    /**
     * The regions where b's field can differ from a's, or nothing when it may differ anywhere (structure, symmetry or
     * an intersect changed).
     */
    static std::optional<std::vector<Bounds>> changes(const Field& a, const Field& b);

private:
    struct Prim
    {
        Primitive shape;
        glm::dmat4 toLocal{1.0};
        double scale = 1.0; // smallest axis scale, so a scaled distance stays a bound
        Bounds bounds;      // root space, grown by the largest blend radius
        bool operator==(const Prim&) const = default;
    };
    enum class Kind : uint8_t
    {
        Prim,
        Begin,
        End,
    };
    struct Instr
    {
        Kind kind = Kind::Prim;
        Op op = Op::Add;
        double blend = 0.0;
        int prim = -1;   // Prim
        int match = -1;  // Begin: its End
        Bounds bounds;   // Prim and Begin: where it can matter
        bool operator==(const Instr&) const = default;
    };
    static constexpr int kEmpty = -1;

    void flatten(const Item& item, const glm::dmat4& parent);
    Sample run(const std::vector<int>* code, const glm::dvec3& p) const;

    std::vector<Prim> mPrims;
    std::vector<Instr> mCode;
    std::vector<Bounds> mAddBounds;
    double mReach = 0.0; // the largest blend radius in the tree
    bool mSymmetric = false;
};

struct MeshOptions
{
    double voxelSize = 1.0;
    /** Newton steps that move each vertex onto the surface; 0 keeps surface nets' cell averages. */
    int projectSteps = 1;
    /** Place vertices by dual contouring, so hard joins and box edges stay sharp; slower. */
    bool sharp = false;
    /** Write the blended primitive colours as the color set "clay". */
    bool colors = true;
    /** Throws past this many bricks rather than run away with a tiny voxel size. */
    size_t maxBricks = 400000;
};

/**
 * Meshes a field with surface nets on a world-aligned lattice, in 8^3-cell bricks that only cover the surface. Keeps
 * each brick's piece, so an update re-meshes only the bricks a change touched.
 */
class LILA_SCENE_API Mesher
{
public:
    Mesher();
    ~Mesher();
    Mesher(Mesher&&) noexcept;
    Mesher& operator=(Mesher&&) noexcept;

    /** The mesh of field, reusing bricks from the last call where the field cannot have changed; the same mesh when nothing did. */
    std::shared_ptr<const geo::Mesh> update(const Field& field, const MeshOptions& options);
    /** Bricks re-meshed by the last update. */
    size_t lastRebuilt() const;
    size_t brickCount() const;

private:
    struct State;
    std::unique_ptr<State> mState;
};

/** One-off mesh of a field, no reuse. */
LILA_SCENE_API geo::Mesh mesh(const Field& field, const MeshOptions& options);
} // namespace lila::clay
