#pragma once

#include "UsdCommon.h"
#include "UsdMaterials.h"

#include <lila/Graph.h>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdSkel/cache.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lila::usd
{
struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Only the prim at this path and below; empty imports the whole stage. */
    std::string prim;
    /** Skeleton bindings become skinClusters; off imports skinned meshes in their bind shape, unskinned. */
    bool skins = true;
    /** Time samples become keys. */
    bool animation = true;
    /** Set the scene fps and playback range from the stage. */
    bool setPlayback = true;
    bool materials = true;
    /** Load payloads; off leaves them unloaded and reports them. */
    bool loadPayloads = true;
    /** Variant selections: set name to variant for every prim with that set... */
    std::map<std::string, std::string> variants;
    /** ...or per prim path. */
    std::map<std::string, std::map<std::string, std::string>> primVariants;
    /** Take to import from a Lila export's take variants; empty keeps the file's selection. */
    std::string take;
    /** Prim purposes to import; render-only files import fine with the default, proxies and guides are skipped. */
    std::set<std::string> purposes{"default", "render"};
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every node created, parents first
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins;
    std::vector<scene::NodeId> cameras;
    std::vector<scene::NodeId> curves;
    std::vector<std::string> materials;
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    std::string upAxis;
    double metersPerUnit = 0.01;
    double fps = 0.0;
    double start = 0.0, end = 0.0; // seconds
    std::string take;
    std::vector<std::string> takes;
    int keyedCurves = 0;
    int instances = 0;
};

/** Main thread. Opens the stage, resolves its composition and builds the composed result in the scene, undoably. */
ImportResult importUsd(scene::Graph& graph, const fs::path& path, const ImportOptions& opts);

/** What the import passes between its hierarchy, mesh and skeleton parts. */
struct Importer
{
    scene::Graph& g;
    const ImportOptions& opts;
    ImportResult& r;
    pxr::UsdStageRefPtr stage;
    Axes axes;
    double tcps = 24.0;
    /** Where rest values are read: the stage's start time, or the earliest sample when it has no range. */
    pxr::UsdTimeCode restTime = pxr::UsdTimeCode::EarliestTime();
    bool held = false; // the stage interpolates held, not linearly
    std::unique_ptr<MaterialReader> materials;
    /** Every time sample keyed, for a playback range when the stage declares none. */
    double firstKey = 1e300, lastKey = -1e300;

    /** Per node: Lila local = before * (file local in Lila axes) * after, turning cameras between file and Lila frames. */
    struct Frame
    {
        glm::dmat4 before{1.0}, after{1.0};
    };
    std::unordered_map<scene::NodeId, Frame> frames;

    /** Node made for each prim (by its path in the composed stage). */
    std::unordered_map<pxr::SdfPath, scene::NodeId, pxr::SdfPath::Hash> nodeOf;
    /** Meshes shared between instances of one prototype prim. */
    std::unordered_map<pxr::SdfPath, scene::MeshPtr, pxr::SdfPath::Hash> meshCache;
    /** Lila point per USD point of each mesh prim built, kInvalid for dropped ones. */
    std::unordered_map<pxr::SdfPath, std::vector<uint32_t>, pxr::SdfPath::Hash> pointMaps;

    struct Skel
    {
        scene::NodeId parent = scene::kNoNode; // node the root joints hang under
        std::vector<scene::NodeId> joints;     // per skeleton joint, in its order
        bool synthetic = false;                // added by Lila's exporter to carry blend shapes
    };
    std::unordered_map<pxr::SdfPath, Skel, pxr::SdfPath::Hash> skeletons;
    /**
     * Skinned or blend-shaped mesh prims and their nodes, bound after the hierarchy exists; and animated xformables,
     * keyed after that. By path: switching an LOD group's variants recomposes its prims.
     */
    std::vector<std::pair<pxr::SdfPath, scene::NodeId>> bound;
    std::vector<std::pair<pxr::SdfPath, scene::NodeId>> animated;
    /** Blend shape targets per mesh node, by the channel name the animation drives them with. */
    struct Shapes
    {
        pxr::SdfPath skeleton;
        std::vector<std::pair<std::string, scene::NodeId>> targets;
    };
    std::vector<Shapes> shapes;
    pxr::UsdSkelCache skelCache;

    /** Seconds of a time code. */
    double seconds(double timeCode) const { return timeCode / tcps; }

    // UsdImportSkel.cpp
    void buildSkeleton(const pxr::UsdPrim& prim, scene::NodeId node);
    void bindAll();
    void keySkeletons();
};

/**
 * Shared by the import parts: curves for the translate/rotate/scale channels that move, one key per sample (seconds),
 * linear or held between them. Joints, which key rotation curves, get one.
 */
void keyChannels(Importer& im, scene::NodeId node, const std::vector<double>& times, const std::vector<glm::dvec3>& t,
                 const std::vector<glm::dvec3>& r, const std::vector<glm::dvec3>& s);
/** A curve on one scalar plug from samples (seconds). */
void keyPlug(Importer& im, const scene::PlugRef& plug, const std::vector<double>& times, const std::vector<double>& values);
} // namespace lila::usd
