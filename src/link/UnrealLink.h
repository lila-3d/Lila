#pragma once

#include "core/Export.h"

#include "link/RpcClient.h"
#include "scene/Node.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lila::link
{
namespace fs = std::filesystem;

/** First port LilaLink tries in the editor; it walks up from here when that one is taken. */
constexpr uint16_t kDefaultPort = 8741;
constexpr int kPortSearchCount = 8;

/** A running editor with LilaLink, found through its port file or by probing. */
struct Instance
{
    uint16_t port = 0;
    int64_t pid = 0;
    std::string project;
    fs::path projectDir;
    std::string engineVersion;
    std::string source; // "portFile" or "probe"
};

LILA_SCENE_API json toJson(const Instance& i);

/** <projectDir>/Saved/LilaLink/port.json for each dir (a .uproject path works too). Missing files are skipped. */
LILA_SCENE_API std::vector<Instance> readPortFiles(const std::vector<fs::path>& projectDirs);

/** Project dirs listed under "projects" in the "unreal" pref file. */
LILA_SCENE_API std::vector<fs::path> configuredProjects();

/** Port files of configured projects, then any default port that accepts a connection and wasn't already found. */
LILA_SCENE_API std::vector<Instance> discover(bool probe = true, const std::string& host = "127.0.0.1");

/** Workspace mappings from the "unreal" pref file: {node, contentPath, kind, skeleton, ...import options}. */
LILA_SCENE_API json mappings();
/** Adds or replaces the mapping for m["node"]. */
LILA_SCENE_API void setMapping(const json& m);
LILA_SCENE_API bool removeMapping(const std::string& node);

/**
 * importAsset params for a file and a mapping (or the flags of an unrealImport call shaped the same way). An array
 * `materials` (from materialParams, even empty) goes along as params.materials: it tells the plugin to match the slots
 * to Unreal materials by name and never make one for the "Default" slot. Without it Interchange handles the materials.
 */
LILA_SCENE_API json importParams(const fs::path& file, const json& mapping, const json& materials = json());

/**
 * The materials the meshes under `roots` use, in slot order, for the plugin to find or create by name:
 * [{name, baseColor, metallic, ..., baseColorMap (absolute), ...}]. Faces on the default slot are left out; a slot
 * with no material node gives just {name}.
 */
LILA_SCENE_API json materialParams(scene::Graph& g, const std::vector<scene::NodeId>& roots);

/**
 * Timeouts for importAsset: the "unreal" prefs "/importTimeoutMs" (overall, default 10 min) and "/importIdleTimeoutMs"
 * (without any reply or progress from the editor, default 60 s).
 */
LILA_SCENE_API CallOptions importCallOptions();

/** Where auto-sync writes exports: the "/cacheDir" pref, else <workspace>/.lila/cache/unreal, else the user dir. */
LILA_SCENE_API fs::path cacheDir();

/** The connection to one Unreal editor plus a worker thread for slow jobs (export + import). */
class LILA_SCENE_API Link
{
public:
    static Link& get();

    struct ConnectOptions
    {
        std::string host = "127.0.0.1";
        std::optional<uint16_t> port;
        std::string project; // name or directory; empty = first instance found
        int timeoutMs = 2000;
    };

    /** Connects and says hello. Returns the hello result; throws std::runtime_error with a readable message. */
    json connect(const ConnectOptions& opts);
    /** Connects with default options unless already connected. Throws like connect(). */
    void ensureConnected();
    void disconnect();
    bool connected() const;

    /** hello result plus port, or {"connected": false}. */
    json status() const;

    /** The live client; null when not connected. Shared so worker jobs keep it alive across a reconnect. */
    std::shared_ptr<RpcClient> client() const;

    /** Runs fn on the worker thread, in order. */
    void post(std::function<void()> fn);
    /** Blocks until everything posted so far has run. */
    void waitIdle();
    /** Stops the worker and disconnects. Call before the app exits. */
    void shutdown();

    Link() = default;
    ~Link();

private:
    void workerLoop();

    mutable std::mutex mMutex;
    std::shared_ptr<RpcClient> mClient;
    json mHello;

    std::mutex mQueueMutex;
    std::condition_variable mQueueCv;
    std::deque<std::function<void()>> mQueue;
    bool mBusy = false;
    bool mQuit = false;
    std::thread mWorker;
};

/**
 * Auto-sync: for each mapping whose node exists, export it with the "fbx" exporter to cacheDir()/<node>.fbx on the
 * worker and send importAsset. A mapped object set exports its member objects together into one file, through the
 * "fbx:set:<kind>" exporter when one is registered. Results are logged. Main thread. Off when the "unreal" pref "/autoSync" is false.
 */
LILA_SCENE_API void onSceneSaved();

/** What onSceneSaved does, minus the pref check when `force`. Returns how many mappings were queued. */
LILA_SCENE_API int syncMappings(bool force);

/** Registers unrealConnect, unrealStatus, unrealImport, unrealMap, unrealUnmap, unrealMappings, unrealSync. */
LILA_SCENE_API void registerCommands();
} // namespace lila::link
