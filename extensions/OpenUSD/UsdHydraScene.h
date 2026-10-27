#pragma once

#include "UsdStage.h"

#include <lila/Graph.h>

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * The scene as Hydra scene indices. Lila's own nodes (meshes, cameras, lights) are prims of a scene index kept up to
 * date by diffing the graph on each render: a moved node dirties its transform, a new mesh snapshot its primvars (or its
 * topology when that changed), so a drag or a deformation never rebuilds the rest. Lila's materials are written as
 * UsdPreviewSurface networks into a stage in memory and read through UsdImaging. Live USD stages go in through
 * UsdImaging too, each prim Lila has a node for blanked out of the stage (the node draws it, with its edits) and placed
 * by that node, so the stage's other prims follow it.
 */
namespace lila::usd::hydra
{
/** Where Lila's nodes and materials sit in the merged scene. */
inline const pxr::SdfPath& nodesRoot()
{
    static const pxr::SdfPath p("/Lila");
    return p;
}
inline const pxr::SdfPath& materialsRoot()
{
    static const pxr::SdfPath p("/LilaMaterials");
    return p;
}
/** The prefix a live stage's prims get. */
pxr::SdfPath stageRoot(scene::NodeId stageNode);

/** Lila's meshes, cameras and lights as Hydra prims, flat under nodesRoot() with world transforms. */
class LilaSceneIndex final : public pxr::HdSceneIndexBase
{
public:
    static pxr::TfRefPtr<LilaSceneIndex> New() { return pxr::TfCreateRefPtr(new LilaSceneIndex()); }

    pxr::HdSceneIndexPrim GetPrim(const pxr::SdfPath& path) const override;
    pxr::SdfPathVector GetChildPrimPaths(const pxr::SdfPath& path) const override;

    /** Main thread, g evaluated. bind(slot) is the material path a material slot binds to. */
    void sync(scene::Graph& g, const std::function<pxr::SdfPath(const std::string& slot)>& bind);
    /** The prims added, re-added, changed and removed by the last sync, for tests. */
    struct Counts
    {
        size_t added = 0, dirtied = 0, removed = 0;
    };
    Counts lastSync() const { return mCounts; }
    size_t lightCount() const { return mLights; }
    /** The material slots the meshes use. */
    const std::set<std::string>& usedSlots() const { return mSlots; }

private:
    LilaSceneIndex() = default;

    struct MeshData;
    struct Node
    {
        pxr::SdfPath path;
        pxr::TfToken type;
        std::shared_ptr<const MeshData> mesh;
        glm::dmat4 world{1.0};
        bool visible = true;
        size_t settings = 0;
        std::vector<pxr::SdfPath> bindings;
        pxr::HdContainerDataSourceHandle own; // camera or light parameters
    };
    struct Prim
    {
        pxr::TfToken type;
        pxr::HdContainerDataSourceHandle ds;
    };

    void put(const pxr::SdfPath& path, Prim prim);
    void drop(const pxr::SdfPath& path);
    pxr::HdContainerDataSourceHandle nodeSource(const Node& n) const;
    std::shared_ptr<const MeshData> meshData(const scene::MeshPtr& mesh);

    std::map<pxr::SdfPath, Prim> mPrims;
    std::map<pxr::SdfPath, std::set<pxr::SdfPath>> mChildren;
    std::unordered_map<scene::NodeId, Node> mNodes;
    std::map<const void*, std::pair<scene::MeshPtr, std::shared_ptr<const MeshData>>> mMeshes;
    std::set<std::string> mSlots;
    size_t mLights = 0;
    Counts mCounts;
};

/**
 * Lila's materials as UsdPreviewSurface networks in a stage in memory (written by MaterialWriter, as USD export writes
 * them), rewritten when their material node changes. stage() goes into Hydra through UsdImaging at materialsRoot().
 */
class MaterialLibrary
{
public:
    MaterialLibrary();
    pxr::UsdStageRefPtr stage() const { return mStage; }
    /** The material prim of a slot, under materialsRoot(). */
    pxr::SdfPath bind(const std::string& slot) const;
    /** Writes the slots not written yet, and those whose material changed since. */
    void sync(scene::Graph& g, const std::set<std::string>& used);
    /** A material node changed (Value, Created, Deleted, Renamed), or anything did (Reset, Cleared). */
    void changed(scene::Graph& g, scene::Change c, scene::NodeId id);

private:
    pxr::UsdStageRefPtr mStage;
    std::set<std::string> mWritten;
    std::set<std::string> mStale;
    bool mAllStale = false;
};

/**
 * Before UsdImaging flattens a live stage: prims Lila's nodes stand for are placed and shown as those nodes are, and
 * blanked (left as transforms) when Lila's scene index draws the node; PointInstancers Lila realised as nodes are pruned.
 */
class StageOverrides final : public pxr::HdSingleInputFilteringSceneIndexBase
{
public:
    static pxr::TfRefPtr<StageOverrides> New(const pxr::HdSceneIndexBaseRefPtr& input)
    {
        return pxr::TfCreateRefPtr(new StageOverrides(input));
    }

    struct Override
    {
        glm::dmat4 matrix{1.0}; // stage-space world matrix
        bool visible = true;
        bool blank = false;
        bool operator==(const Override&) const = default;
    };
    void update(std::map<pxr::SdfPath, Override> owned, std::set<pxr::SdfPath> pruned);

    pxr::HdSceneIndexPrim GetPrim(const pxr::SdfPath& path) const override;
    pxr::SdfPathVector GetChildPrimPaths(const pxr::SdfPath& path) const override;

protected:
    void _PrimsAdded(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    explicit StageOverrides(const pxr::HdSceneIndexBaseRefPtr& input) : HdSingleInputFilteringSceneIndexBase(input) {}
    bool insidePruned(const pxr::SdfPath& path) const;

    std::map<pxr::SdfPath, Override> mOwned;
    std::set<pxr::SdfPath> mPruned;
};

/** After flattening: carries a live stage's world transforms into Lila's world (the stage node's place, axes and units). */
class StageToWorld final : public pxr::HdSingleInputFilteringSceneIndexBase
{
public:
    static pxr::TfRefPtr<StageToWorld> New(const pxr::HdSceneIndexBaseRefPtr& input) { return pxr::TfCreateRefPtr(new StageToWorld(input)); }

    void setToWorld(const glm::dmat4& m);

    pxr::HdSceneIndexPrim GetPrim(const pxr::SdfPath& path) const override;
    pxr::SdfPathVector GetChildPrimPaths(const pxr::SdfPath& path) const override;

protected:
    void _PrimsAdded(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::AddedPrimEntries& entries) override;
    void _PrimsRemoved(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::RemovedPrimEntries& entries) override;
    void _PrimsDirtied(const pxr::HdSceneIndexBase& sender, const pxr::HdSceneIndexObserver::DirtiedPrimEntries& entries) override;

private:
    explicit StageToWorld(const pxr::HdSceneIndexBaseRefPtr& input) : HdSingleInputFilteringSceneIndexBase(input) {}

    glm::dmat4 mToWorld{1.0};
};

/** True when Lila's scene index draws a node of this type, so a stage prim it stands for is blanked. */
bool drawnByLila(const scene::Node& n);
} // namespace lila::usd::hydra
