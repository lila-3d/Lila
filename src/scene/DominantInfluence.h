#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <memory>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Which joint owns each face of a skinned mesh, for clicking a mesh to pick its joint (Puppeteer style) and for the
 * hover wash that shows the joint's region.
 */
namespace rig
{
struct DominantInfluenceMap
{
    /** Influence id with the largest summed corner weight, per face id; -1 for dead or unweighted faces. */
    std::vector<int64_t> faceInfluence;
};

/** Cached per skinCluster and rebuilt when its weights or its input mesh change. Null if it has neither. */
LILA_SCENE_API std::shared_ptr<const DominantInfluenceMap> dominantInfluences(Graph& g, NodeId skin);

/** The joint dominating a face of a mesh; kNoNode when the mesh isn't skinned or the face is unweighted. */
LILA_SCENE_API NodeId dominantJoint(Graph& g, NodeId mesh, uint32_t face);

/** Faces of the mesh the joint dominates. */
LILA_SCENE_API std::vector<uint32_t> dominatedFaces(Graph& g, NodeId mesh, NodeId joint);

/** The joint's weight per point id (0 where it has none), for the hover wash; empty when it doesn't influence the mesh. */
LILA_SCENE_API std::vector<float> influenceWeights(Graph& g, NodeId mesh, NodeId joint);

LILA_SCENE_API void clearDominantInfluenceCache();
} // namespace rig

/** dominantInfluence. Called by registerRigCommands. */
LILA_SCENE_API void registerDominantInfluenceCommands();
} // namespace lila::scene
