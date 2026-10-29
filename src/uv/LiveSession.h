#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/Plug.h"
#include "uv/LiveSolver.h"
#include "uv/SymmetryUv.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * Live uv solving of scene meshes for the UI: one threaded LiveSolver per mesh, a preview written to the scene with undo
 * off while it works, and one undo step per transaction (a drag, a brush stroke, a Relax) once it settles. Main thread.
 */
namespace lila::uv::live
{
/** Solver settings the UI shares across sessions (balance slider, boundary smoothing...). */
LILA_SCENE_API LiveOptions& options();
/** Pushes options() to every session's islands. */
LILA_SCENE_API void optionsChanged();

class LILA_SCENE_API Session
{
public:
    Session(scene::NodeId node, std::string uvSet);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    scene::NodeId node() const { return mNode; }
    const std::string& uvSet() const { return mSet; }
    /** The node still exists. */
    bool alive() const;
    /** The scene's mesh is the one this session loaded or wrote, so its islands match it. */
    bool current() const;
    /** Rebuilds the islands from the scene's mesh (committing an open transaction first). */
    void reload();

    LiveSolver& solver() { return *mSolver; }
    /** Mesh the islands were loaded from, uvs as of the last load or commit. */
    const scene::MeshPtr& base() const { return mBase; }
    size_t islandCount() const { return mTopology.size(); }
    /** Charts as loaded: corners, triangles, 3D positions (uvs as of the load). */
    const Chart& topology(size_t island) const { return mTopology[island]; }
    /** Island and vertex of a mesh corner; island -1 when the corner is not in any. */
    std::pair<int, Index> wedge(Index corner) const;
    int islandOfFace(Index face) const;
    /** Islands holding any of the faces (all islands when faces is empty). */
    std::vector<int> islandsOfFaces(std::span<const Index> faces) const;
    /** Epoch of the solver's islands that matches this topology. */
    uint64_t epoch() const { return mEpoch; }

    /** Opens a transaction (if none is open) labelled for undo, and unpauses the solver. */
    void begin(std::string label);
    bool open() const { return mOpen; }
    /** While stroking, the transaction stays open even when the solver goes idle. */
    void setStroking(bool on);
    bool stroking() const { return mStroking; }
    /** Writes the preview and commits a settled transaction. True when a new solver frame arrived. */
    bool tick();
    /** Ends the transaction as one undo step (command uvLiveCommit). */
    void commit();
    /** Waits for the solver to settle (at most ms), then commits. */
    void finish(int ms = 10000);

    /** Wakes the islands (all when empty) on the worker, optionally editing each first. */
    void wake(std::vector<int> islands, std::function<void(LiveIsland&)> fn = {});

private:
    geo::Mesh meshFromFrame(const LiveSolver::Frame& f) const;
    /** With the node's symmetry on, the uvs and pins the frame changed on one side follow on the other, mirrored. True if any did. */
    bool mirrorFrame(geo::Mesh& m) const;
    void writePreview(const LiveSolver::Frame& f);

    scene::NodeId mNode;
    std::string mSet;
    std::unique_ptr<LiveSolver> mSolver;
    scene::MeshPtr mBase;
    scene::MeshPtr mStamp; // outMesh the scene shows when nothing else changed it
    std::vector<Chart> mTopology;
    std::vector<std::pair<int, Index>> mCornerWedge;
    std::vector<int> mFaceIsland;
    mutable std::optional<UvMirror> mMirror; // of mBase, for mMirrorAxis
    mutable scene::MeshPtr mMirrorBase;
    mutable int mMirrorAxis = -1;
    uint64_t mEpoch = 0;
    uint64_t mSeen = 0;

    bool mOpen = false;
    bool mStroking = false;
    bool mPreviewed = false;
    /** Where drag previews are written: the stored base mesh, or none when a generator feeds it. */
    std::optional<scene::PlugRef> mPreviewPlug;
    std::string mLabel;
    scene::MeshPtr mBeforeIn;
    std::chrono::steady_clock::time_point mReleased;
    std::chrono::steady_clock::time_point mLastPreview;
};

/** The session of a mesh node for the current uv set, made on demand (nullptr when create is false and none exists). */
LILA_SCENE_API Session* get(scene::NodeId node, bool create = true);
/** The session if it exists and still matches the scene. */
LILA_SCENE_API Session* find(scene::NodeId node);
/** Ticks every session; true when any of them got a new frame. */
LILA_SCENE_API bool tickAll();
LILA_SCENE_API bool anyOpen();
LILA_SCENE_API void commitAll();
/** Drops every session (new scene). */
LILA_SCENE_API void clear();

/** Hands a finished mesh to uvLiveCommit, which sets it as the node's inMesh. Returns the token to pass. */
LILA_SCENE_API uint64_t stage(scene::NodeId node, scene::MeshPtr mesh);
/** For uvLiveCommit: the staged mesh and its node, removed from the stage; null when the token is unknown. */
LILA_SCENE_API scene::MeshPtr takeStaged(uint64_t token, scene::NodeId& node);

/** Commits open transactions before any other command that edits the scene (or undo/redo) runs. Installed once. */
LILA_SCENE_API void installAutoCommit();
} // namespace lila::uv::live
