#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * The scene change stream: scene edits as JSON deltas, one message per command or undo step, for in-process
 * subscribers and for other processes over a local socket (see startServer). Main thread only unless noted.
 *
 * Messages are objects with a "type":
 *   group     {seq, command, source, ok, undo?, redo?, changes: [...]}: one user action's changes
 *   resync    {seq, reason, path?}: the scene was replaced or changed wholesale; ask for a snapshot
 *   event     {seq, event, path}: sceneSaved, sceneImported, sceneExported
 *   snapshot  {seq, scene, time, selection, nodes, connections}: the whole scene with every message up to seq applied
 *   live      {seq, source, changes}: interim values while a drag or other interactive edit is still open (Filter::live)
 *   frame     {seq, time, changes}: world and evaluated changes that time or another evaluation caused, outside any group
 * A change has a "kind": created, deleted, renamed, reparented, value, connection, selection, time, world or evaluated.
 */
namespace lila::stream
{
using json = nlohmann::json;

/** What a subscriber hears. An empty list matches everything. */
struct LILA_SCENE_API Filter
{
    /** Node type names; subtypes match too. */
    std::vector<std::string> types;
    /** Attribute long names, for value and connection changes and the attrs of created and snapshot nodes. */
    std::vector<std::string> attrs;
    /** Full paths ("|rig|arm"): the node and everything under it. */
    std::vector<std::string> under;
    /** Whole meshes and large data values instead of summaries (counts and a hash). */
    bool full = false;
    /** Live messages while an interactive edit's undo chunk is open, at most about 30 a second per plug. */
    bool live = false;
    /** A world change (the evaluated world matrix) for each transform that moved, and every transform under it. */
    bool world = false;
    /**
     * An evaluated change for each output plug (and each input a connection drives) whose evaluated value differs from
     * the last one this filter was sent. worldMatrix is left to world.
     */
    bool evaluated = false;

    /** From {"types", "attrs", "under", "full", "live", "world", "evaluated"}; throws std::invalid_argument on a malformed one. */
    static Filter fromJson(const json& j);
    json toJson() const;
};

/** Called on the main thread with each message the filter lets through. */
using Handler = std::function<void(const json& message)>;

/** 0 is never a valid id. Recording starts with the first subscriber or the server. */
LILA_SCENE_API uint64_t subscribe(Handler fn, Filter filter = {}, std::string owner = {});
/** For extensions: owned by ctx and removed when it unloads. */
LILA_SCENE_API uint64_t subscribe(ext::Context& ctx, Handler fn, Filter filter = {});
LILA_SCENE_API bool unsubscribe(uint64_t id);
LILA_SCENE_API size_t unsubscribeOwned(std::string_view owner);

/**
 * The whole scene as a snapshot message, after sending any changes still gathering. Taken while a command or an
 * interactive edit is still open, the changes it made so far become a group of their own, numbered with the
 * snapshot's seq, and the rest follow in the next group.
 */
LILA_SCENE_API json snapshot(const Filter& filter = {});
/** Sends the changes gathered outside any command or open interactive edit now rather than when the main queue next drains. */
LILA_SCENE_API void flush();
/** The seq of the newest message sent; 0 before any. Any thread. */
LILA_SCENE_API uint64_t sequence();
/** True while changes are being recorded (a subscriber or the server exists). */
LILA_SCENE_API bool recording();

/**
 * Serves the stream as newline-delimited JSON over TCP. localOnly binds 127.0.0.1; otherwise every interface. Port 0
 * picks a free one. Returns false, with err set, when the port is taken. Clients send one JSON request per line:
 *   {"op": "auth", "token": "..."}
 *   {"op": "subscribe", "types": [...], "attrs": [...], "under": [...], "full": false, "live": false, "world": false,
 *    "evaluated": false}
 *   {"op": "snapshot"}
 *   {"op": "ping"}
 * Bound to every interface, or with requireToken, a client must first send tokenFromPrefs() in an auth or subscribe
 * request; anything else, or a wrong token, closes the connection.
 */
LILA_SCENE_API bool startServer(int port, bool localOnly = true, std::string* err = nullptr, bool requireToken = false);
LILA_SCENE_API void stopServer();
/** Any thread. */
LILA_SCENE_API bool serverRunning();
LILA_SCENE_API int serverPort();
LILA_SCENE_API bool serverLocalOnly();
LILA_SCENE_API bool serverRequiresToken();
/** general.json /changeStream/token, created on first use. Kept apart from the MCP token, which can run code. */
LILA_SCENE_API std::string tokenFromPrefs();
LILA_SCENE_API size_t clientCount();

constexpr int kDefaultPort = 8742;

/** The changeStream command. */
LILA_SCENE_API void registerCommands();
/** Starts the server when general.json /changeStream/enabled is on, and follows that pref while Lila runs. */
LILA_SCENE_API void startFromPrefs();
/**
 * lilabatch's opt-in, in place of the prefs: a port from --change-stream (cliPort >= 0), else from the
 * LILA_CHANGE_STREAM environment variable. Localhost only. Does nothing, returning true, when neither is given.
 */
LILA_SCENE_API bool startForBatch(int cliPort = -1, std::string* err = nullptr);
} // namespace lila::stream
