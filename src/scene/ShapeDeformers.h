#pragma once

#include "scene/Deformers.h"

#include <memory>

/** Shape and handle deformers (ShapeDeformers.cpp): kelvinlet, arap, laplacianDeform, bbw, the simple set and tension, plus the cage's other coordinates. */
namespace lila::scene::deformers
{
void registerShapeTypes();
void registerShapeCommands();

/** Re-captures the bind-time state of a deformer from this file. False for types it does not own. */
bool rebindShape(Graph& g, DeformerNode& d);

/** The cage attrs past mean value coordinates: coordinates (enum), poissonRatio, bulge. */
std::vector<AttrDef> cageParams();

/** Evaluation cache of a cage in a mode other than mean value. */
struct CageCache;

/** True when the cage evaluates through cageDeform: a mode other than mean value, or a binding made by bindCageCoordinates. */
bool cageUsesCoordinates(const DeformerNode& cage);
void cageDeform(const DeformerNode& cage, std::vector<glm::dvec3>& points, std::shared_ptr<CageCache>& cache);
/** cageDeform as a GPU program (scene/DeformEval.h): the coordinates go to the GPU once, the per-frame terms each run. */
bool cageGpuProgram(const DeformerNode& cage, size_t points, deformEval::Program& out, std::shared_ptr<CageCache>& cache);
/** Binds a cage in its coordinates mode (any mode, mean value included), keeping the rest points so modes can switch live. */
void bindCageCoordinates(Graph& g, DeformerNode& cage);
} // namespace lila::scene::deformers
