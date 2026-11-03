#pragma once

#include "core/Export.h"
#include "geo/Mesh.h"
#include "groom/Groom.h"

#include <memory>

namespace lila::clay
{
class Mesher;
class Field;
} // namespace lila::clay

/**
 * Shell: every lock as a chain of flattened round cones in a Clay field, the locks smooth-unioned into one watertight
 * mass and meshed with surface nets. A single cohesive surface over the hair, ready for retopology.
 */
namespace lila::groom
{
struct ShellOptions
{
    double voxelSize = 0.3;
    /** Smooth union radius between locks, cm: larger fills the gaps between them. */
    double blend = 0.6;
    /** Grows every lock, cm. */
    double inflate = 0.0;
    /** Round cones per lock; 0 uses one per resolved ring. */
    int segments = 16;
    /** Leave strays out of the shell. */
    bool skipStrays = true;
    /** Newton steps that move each vertex onto the surface. */
    int projectSteps = 1;
    /** Sample on the GPU when a sampler is registered. */
    bool gpu = false;
};

/** The Clay field of resolved locks (groom/Locks.h resolveLocks). */
LILA_SCENE_API clay::Field shellField(const Splines& resolved, const ShellOptions& o);

/** One-off shell mesh. Colour set "groomShell": r is v along the lock (0 root, 1 tip), g a per-lock hash, blended across joins. */
LILA_SCENE_API geo::Mesh shellMesh(const Splines& resolved, const ShellOptions& o);

/** Keeps a mesher between updates, so a change re-meshes only the bricks it touched. */
class LILA_SCENE_API ShellBuilder
{
public:
    ShellBuilder();
    ~ShellBuilder();
    std::shared_ptr<const geo::Mesh> update(const Splines& resolved, const ShellOptions& o);

private:
    std::unique_ptr<clay::Mesher> mMesher;
    std::shared_ptr<const geo::Mesh> mLastIn, mLastOut;
};
} // namespace lila::groom
