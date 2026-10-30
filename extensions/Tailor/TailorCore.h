#pragma once

// Garment core shared by the solver, meshing, blocks and UI. Plain data, no Lila SDK types,
// so the solver and its tests build without the scene.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tailor
{

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;

/** Anisotropic fabric. Stiffness in N/m (membrane) and N*m (bending), density in kg/m^2, lengths in metres. */
struct Fabric
{
    std::string name = "Cotton";
    float stretchWarp = 4000.f;
    float stretchWeft = 3000.f;
    float shear = 300.f;
    float bendWarp = 2e-5f;
    float bendWeft = 2e-5f;
    float bendBias = 1.5e-5f;
    float strainLimit = 0.1f;      // max stretch ratio above rest, hard constraint; 0 = off
    float density = 0.15f;
    float thickness = 0.001f;
    float friction = 0.3f;
    float damping = 0.01f;
    float shrinkWarp = 1.f;        // rest scale multipliers
    float shrinkWeft = 1.f;
};

/** Built-in presets: Cotton, Linen, Denim, Canvas, Wool, Leather, Suede, Silk, Chiffon, Rubber, Burlap, Heavy (chainmail look), Banner. */
const std::vector<Fabric>& fabricPresets();
const Fabric* findFabric(const std::string& name);

enum class PanelState : uint8_t
{
    Active,
    Frozen,       // collides, does not move
    Deactivated,  // ignored by the sim
    Strengthened // stiffened while shaping
};

/** A pattern piece: closed 2D outline (counter-clockwise, metres, pattern space) plus internal lines and holes. */
struct Panel
{
    std::string name;
    std::vector<Vec2> outline;                  // polyline, curves already sampled
    std::vector<std::vector<Vec2>> holes;
    std::vector<std::vector<Vec2>> internalLines; // fold or crease lines, kept as mesh edges
    float grainAngle = 0.f;                    // radians, warp direction in pattern space
    int fabric = 0;                            // index into Garment::fabrics
    int layer = 0;
    PanelState state = PanelState::Active;
    float particleDistance = 0.02f;            // target edge length for meshing

    // 3D placement: pattern-space point (0,0) lands at origin, pattern X along right, Y along up,
    // then the panel is wrapped around a cylinder of wrapRadius about the up axis (0 = flat).
    Vec3 origin{0.f, 0.f, 0.f};
    Vec3 right{1.f, 0.f, 0.f};
    Vec3 up{0.f, 1.f, 0.f};
    float wrapRadius = 0.f;
};

/** A run of outline from vertex index `from` to `to` (inclusive, walking forward, wrapping) on a panel. */
struct SeamSide
{
    int panel = -1;
    int from = 0;
    int to = 0;
};

/** Sews two runs together point to point by arc length. M:N chains are several sides per seam end. */
struct Seam
{
    std::vector<SeamSide> a;
    std::vector<SeamSide> b;
    bool reversed = false;  // walk b backwards
    float foldAngle = 0.f;  // rest dihedral across the seam, radians (0 = flat)
    float shrink = 1.f;     // rest length scale across the seam
};

struct Garment
{
    std::vector<Panel> panels;
    std::vector<Seam> seams;
    std::vector<Fabric> fabrics{Fabric{}};
};

/** Triangle mesh built from a garment, the solver's input. */
struct ClothMesh
{
    std::vector<Vec3> rest3d;              // placed (initial) positions
    std::vector<Vec2> uv;                  // pattern-space positions, metres (the flat pattern)
    std::vector<std::array<int, 3>> tris;
    std::vector<int> triPanel;
    std::vector<int> vertPanel;
    std::vector<std::array<int, 2>> seamPairs; // vertices to be sewn together
    std::vector<float> seamRestLength;     // usually 0
    std::vector<std::array<int, 2>> creaseEdges; // internal-line edges (keep sharp rest angle)
};

/** Mesh every active/frozen panel at its particle distance, place it in 3D and resample seams to matching vertex pairs. */
ClothMesh buildClothMesh(const Garment& g);

/** Distance field collider. Negative inside. Sampled on a uniform grid. */
struct Sdf
{
    Vec3 origin{0.f, 0.f, 0.f};
    float cell = 0.01f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> values;
    float sample(const Vec3& p, Vec3* gradient = nullptr) const;
};

Sdf sdfFromMesh(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, float cell, float padding);

struct SolverSettings
{
    float dt = 1.f / 60.f;
    int substeps = 2;
    int iterations = 10;
    Vec3 gravity{0.f, -9.81f, 0.f};
    float collisionThickness = 0.004f;
    bool selfCollision = true;
    float seamCloseRate = 0.25f; // fraction of remaining seam gap closed per step while sewing
    uint32_t seed = 1;
};

struct Pin
{
    int vertex = -1;
    Vec3 target{};
};

class ClothSolver
{
public:
    void reset(const ClothMesh& mesh, const Garment& g, const SolverSettings& s);
    void setColliders(std::vector<const Sdf*> colliders) { colliders_ = std::move(colliders); }
    void setPins(std::vector<Pin> pins) { pins_ = std::move(pins); }
    void step();
    const std::vector<Vec3>& positions() const { return x_; }
    std::vector<Vec3>& positionsMutable() { return x_; }
    /** Per-vertex stretch ratio (current / rest), for the fit map. */
    std::vector<float> strainMap() const;

private:
    struct Impl;
    std::vector<Vec3> x_, v_, xPrev_;
    std::vector<const Sdf*> colliders_;
    std::vector<Pin> pins_;
    std::shared_ptr<Impl> impl_;
};

} // namespace tailor
