#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/**
 * Alembic (Ogawa) mesh caches: one Xform + PolyMesh per object, a sample per frame with P, face counts and indices
 * (topology may change between frames), facevarying N, uv (and uv1.. as extra params) and Cd. Faces are written
 * clockwise, the Alembic convention, so Unreal's default (Maya) conversion and other DCCs read them the right way out.
 */
namespace lila::seq::abc
{
namespace fs = std::filesystem;

struct WriteOptions
{
    double fps = 30.0;
    /** Time of the first sample, seconds. */
    double startTime = 0.0;
    bool normals = true;
    bool uvs = true;
    bool colors = true;
    bool triangulate = true;
};

/** Writes samples as frames arrive. Meshes are in file space already. */
class LILA_SCENE_API Writer
{
public:
    Writer();
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /** Throws std::runtime_error when the file cannot be created. */
    void open(const fs::path& path, const WriteOptions& opts);
    /** One sample per object; objects are matched by name, so they need not all exist on every frame. */
    void addFrame(const std::vector<std::string>& names, const std::vector<const geo::Mesh*>& meshes);
    /**
     * Rigid objects: one transform sample per object per frame over a shape written once, the first time a name appears
     * (shapes may be null after that). Matrices are in file space.
     */
    void addTransformFrame(const std::vector<std::string>& names, const std::vector<const geo::Mesh*>& shapes,
                           const std::vector<glm::dmat4>& transforms);
    void close();
    int frameCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

/** What a file holds, for tests and the import report. */
struct ObjectInfo
{
    std::string path; // full object path, like /Spike/SpikeShape
    size_t samples = 0;
    std::vector<size_t> points;   // per sample
    std::vector<size_t> faces;    // per sample
    std::vector<double> times;    // per sample, seconds
    bool normals = false;
    bool uvs = false;
    bool colors = false;
    /** First sample's positions, for round-trip checks. */
    std::vector<glm::vec3> firstPositions;
};

/** Every PolyMesh in the file. Empty with `error` set when it cannot be read. */
LILA_SCENE_API std::vector<ObjectInfo> read(const fs::path& path, std::string* error = nullptr);

struct MeshSample
{
    std::string path;
    geo::Mesh mesh;
};

/**
 * The PolyMeshes at a time in seconds (the nearest sample), through their parent transforms, faces turned back to
 * counter-clockwise, with uv0 and normals when the file has them. filter keeps objects whose path contains it (empty
 * keeps all). Empty with `error` set when the file cannot be read.
 */
LILA_SCENE_API std::vector<MeshSample> readMeshes(const fs::path& path, double time, const std::string& filter = {}, std::string* error = nullptr);

/** A PolyMesh as a point cache: its first sample as a mesh, then every sample's points, all through its transforms. */
struct MeshCache
{
    std::string path; // full object path
    std::string name; // its parent transform's name, or its own
    geo::Mesh rest;
    /** Seconds, one per sample. */
    std::vector<double> times;
    /** Per sample, laid out like rest's points; empty when the topology changes between samples. */
    std::vector<std::vector<glm::dvec3>> points;
    bool topologyChanges = false;
};

/** Every PolyMesh in the file as a cache. Empty with `error` set when the file cannot be read. */
LILA_SCENE_API std::vector<MeshCache> readCaches(const fs::path& path, std::string* error = nullptr);
} // namespace lila::seq::abc
