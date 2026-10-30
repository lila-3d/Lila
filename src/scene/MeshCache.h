#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/** A point cache: every point's object-space position at each sample time. */
struct LILA_SCENE_API MeshCacheSamples final : Data
{
    /** Seconds, ascending. */
    std::vector<double> times;
    uint32_t pointCount = 0;
    /** times.size() * pointCount xyz triples, sample after sample. */
    std::vector<float> points;

    const char* kind() const override { return "meshCacheSamples"; }
    size_t bytes() const override { return times.size() * sizeof(double) + points.size() * sizeof(float); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);

    size_t sampleCount() const { return pointCount ? std::min(times.size(), points.size() / (size_t(pointCount) * 3)) : 0; }
    /** Sample i's xyz triples. */
    std::span<const float> sample(size_t i) const { return {points.data() + i * size_t(pointCount) * 3, size_t(pointCount) * 3}; }
    /** Appends a sample at seconds; its points must number pointCount (the first sets it). */
    void add(double seconds, std::span<const glm::dvec3> pts);
};

/**
 * The meshCache deformer plays back per-frame point positions (a USD or Alembic point cache, or a recorded stack) on a
 * mesh of the same point count: at each scene time it sets every point to the cached position, interpolated between
 * samples, so anything above it in the stack deforms the cached motion. The samples are stored in the scene file.
 * Cache time is (scene time - timeOffset) * speed; outside the samples it holds the first or last one, or loops.
 */
namespace meshCache
{
inline constexpr const char* kType = "meshCache";

/** The first meshCache in a mesh's stack, or kNoNode. */
LILA_SCENE_API NodeId find(Graph& g, NodeId mesh);
/** A meshCache node's samples, or null. */
LILA_SCENE_API std::shared_ptr<const MeshCacheSamples> samples(Graph& g, NodeId cache);
/** Puts a meshCache holding samples on top of the mesh's stack and returns it. Undoable inside a command. */
LILA_SCENE_API NodeId create(Graph& g, NodeId mesh, std::shared_ptr<const MeshCacheSamples> samples, const std::string& name = "MeshCache");
/** Replaces a meshCache node's samples. Undoable. */
LILA_SCENE_API void setSamples(Graph& g, NodeId cache, std::shared_ptr<const MeshCacheSamples> samples);

/** The points at cache time t (seconds): linear between samples or the sample at or before t; held or looped outside. */
LILA_SCENE_API void pointsAt(const MeshCacheSamples& s, double t, bool interpolate, bool loop, std::vector<glm::dvec3>& out);

LILA_SCENE_API void registerTypes();
/** bakeMeshCache and meshCacheInfo. */
LILA_SCENE_API void registerCommands();
} // namespace meshCache
} // namespace lila::scene
