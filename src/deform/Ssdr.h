#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * Smooth skinning decomposition with rigid bones (Le and Deng 2012), on EA's Dem Bones: linear skin weights fitted so
 * linear blend skinning reproduces sampled frames of any deformation, with optional free helper bones.
 */
namespace lila::deform
{
struct SsdrProblem
{
    /** The bind shape the weights skin. */
    std::vector<glm::dvec3> rest;
    /** Faces as point ids, for the weight smoothness term. */
    std::vector<std::vector<int>> polygons;
    int frames = 0;
    /** frames * rest.size() points to reproduce, frame-major. */
    std::vector<glm::dvec3> targets;
    /** Bones whose motion is given (the skeleton): frames * bones rest-to-frame matrices, frame-major. */
    int bones = 0;
    std::vector<glm::dmat4> transforms;
    /** Starting weights, CSR over the points, influence ids below bones. */
    std::vector<uint32_t> offsets{0}, influences;
    std::vector<float> weights;
    /** Extra bones whose rigid motion is solved too, placed where the fixed bones fit worst. */
    int helpers = 0;
    int maxInfluences = 8;
    /** Weight passes; with helpers, rounds of alternating transform and weight solves. */
    int iterations = 3;
    /** Pull toward the neighbours' weights, Dem Bones' weightsSmooth. */
    double smoothness = 1e-4;
};

struct SsdrResult
{
    /** CSR, influence ids: fixed bones first, then helpers (bones + h). */
    std::vector<uint32_t> offsets{0}, influences;
    std::vector<float> weights;
    /** Each helper's rest pivot, and its rest-to-frame matrix per frame (frames * helpers, frame-major). */
    std::vector<glm::dvec3> helperPivots;
    std::vector<glm::dmat4> helperTransforms;
    /** Distances between linear skinning with the result and the targets: RMS over everything, the worst point's RMS. */
    double rmse = 0.0, maxError = 0.0;
    std::vector<double> pointError;
};

/** Throws std::invalid_argument on inconsistent sizes. Deterministic for the same input. */
LILA_SCENE_API SsdrResult ssdrFit(const SsdrProblem& problem);

/** RMS over frames of each point's distance from linear skinning (weights CSR, transforms frames * bones) to targets. */
LILA_SCENE_API std::vector<double> skinningError(const SsdrProblem& problem, const std::vector<uint32_t>& offsets, const std::vector<uint32_t>& influences,
                                                 const std::vector<float>& weights, const std::vector<glm::dmat4>& transforms, int bones);
} // namespace lila::deform
