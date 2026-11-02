#pragma once

#include "TailorCore.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace tailor
{
/** Solver constants. Defaults equal ClothSolver.cpp's so the two paths agree; keep them in step. */
struct GpuTuning
{
    float lambdaDecay = 0.98f;    // AVBD warm start between substeps
    float stiffnessDecay = 0.99f;
    float kMax = 1e6f;
    float beta = 1e6f;            // AVBD stiffness ramp per metre of violation, per iteration
    float contactStiffness = 1e4f; // plus 1e3 * mass / h^2 per vertex
    float airDrag = 0.2f;         // 1/s
    float frictionSlip = 1e-5f;   // tangential slip in metres still treated as sticking
    float closedGap = 1e-4f;      // a zero-rest seam closes for good below this gap
    float boundFraction = 0.4f;   // VBD conservative bound: share of the distance to the nearest primitive
    float strengthen = 10.f;      // stiffness multiplier on Strengthened panels
    float foldStiffening = 8.f;   // a painted fold holds its angle against the fabric's own bending (ClothSolver's kFoldStiffening)
    float dampingScale = 0.01f;   // fabric damping to the Rayleigh coefficient, seconds
    int redetectEvery = 3;        // iterations between contact re-detections while vertices hit their bounds
};

/**
 * ClothSolver on the GPU (shaders/cloth_vbd.slang), the same method term for term: per-vertex VBD block solves over
 * graph colours, AVBD hard constraints for seams, pins and strain limits, anisotropic StVK membrane from the pattern,
 * dihedral bending, SDF collider contact and vertex-triangle self contact with conservative displacement bounds.
 *
 * Every call blocks on a lila::gpu session run, so none may come from the render thread itself. Use it only when
 * available(); headless runs and tests use ClothSolver.
 */
class ClothGpu
{
public:
    ClothGpu();
    ~ClothGpu();
    ClothGpu(const ClothGpu&) = delete;
    ClothGpu& operator=(const ClothGpu&) = delete;

    /** True while the renderer is running (lila::gpu::available). */
    static bool available();

    /** Builds the topology (colouring, adjacency, rest shapes) and uploads it. False with err when the GPU fails. */
    bool reset(const ClothMesh& mesh, const Garment& g, const SolverSettings& s, std::string* err = nullptr);
    /** The grids are copied and uploaded on the next step; up to 4. */
    void setColliders(std::vector<const Sdf*> colliders);
    /** World to grid as the 3 rows of a rigid 3x4 transform (identity by default), for colliders that move rigidly. */
    void setColliderTransform(int index, const std::array<float, 12>& worldToGrid);
    void setPins(std::vector<Pin> pins);
    /** Moves every vertex there and zeroes velocities, on the next step (continuing a drape from where it got to). */
    void setPositions(std::vector<Vec3> positions);
    /** One frame of settings.dt: seam closing, then substeps x iterations, then the positions read back. */
    bool step(std::string* err = nullptr);

    const std::vector<Vec3>& positions() const;
    /** Per-vertex stretch ratio (largest principal stretch, averaged over the vertex's triangles), for the fit map. */
    std::vector<float> strainMap() const;
    GpuTuning& tuning();
    /** Graph colours of the last reset: two dispatches each per iteration. */
    int colourCount() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace tailor
