#pragma once

#include "scene/Plug.h"
#include "stream/ChangeStream.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lila::scene
{
struct NodeType;
}

namespace lila::stream::detail
{
using Attrs = std::vector<std::pair<std::string, scene::PlugValue>>;

struct NodeInfo
{
    std::string path; // full path
    const scene::NodeType* type = nullptr;
};

struct Entry
{
    enum class Kind : uint8_t
    {
        Created,
        Deleted,
        Renamed,
        Reparented,
        Value,
        Connection,
        Selection,
        Time,
        World,
        Evaluated,
    };
    Kind kind = Kind::Value;
    scene::NodeId id = scene::kNoNode;
    int attr = -1;
    int component = -1; // connection: one component of a Vec3
    int only = -1; // an index into Message::keys: only filters with that key see it; -1 for every filter
    NodeInfo node;
    std::string oldPath;  // renamed, reparented
    std::string attrName; // value, connection
    std::string text;     // reparented: the new parent; connection: the source plug, empty once disconnected
    scene::PlugValue value; // value, evaluated; world: the world matrix
    bool locked = false;
    bool live = true; // false when the node went away before the group was sent
    Attrs attrs;      // created
    std::vector<std::string> paths; // selection
    double time = 0.0;
};

/**
 * One outgoing message, built on the main thread and read-only after: values stay as plug values (meshes and data are
 * shared), so turning them into JSON can happen on the server's thread. Mesh summaries are worked out on the main
 * thread as the message goes out, so a mesh edited in place afterwards can't change what it says.
 */
struct Message
{
    enum class Type : uint8_t
    {
        Group,
        Resync,
        Event,
        Snapshot,
        Live,
        Frame,
    };
    Type type = Type::Group;
    uint64_t seq = 0;

    // Group, and the command behind a resync
    std::string command;
    std::string source;
    bool ok = true;
    std::string undo;
    std::string redo;
    std::vector<Entry> entries;
    /** Filter keys (Filter::toJson dumped) that entries with `only` set are meant for. */
    std::vector<std::string> keys;
    /** Mesh summaries by the mesh's address, for every mesh the message holds. */
    std::unordered_map<const void*, json> summaries;

    // Resync and event
    std::string reason;
    std::string event;
    std::string path;

    // Snapshot
    struct Node
    {
        scene::NodeId id = scene::kNoNode;
        NodeInfo info;
        std::string parent;
        Attrs attrs;
        std::optional<glm::dmat4> world; // transforms, when a subscriber wants world matrices
        Attrs outputs;                   // evaluated outputs, when a subscriber wants them
    };
    std::vector<Node> nodes;
    std::vector<std::pair<std::string, std::string>> connections; // source plug, destination plug
    std::vector<std::string> selection;
    double time = 0.0; // snapshot, frame
};
using MessagePtr = std::shared_ptr<const Message>;

/** The message as one subscriber with this filter sees it; null when the filter leaves nothing of it. Any thread. */
json toJson(const Message& m, const Filter& f);
/** One line of the wire protocol: compact JSON and a newline. */
std::string line(const json& j);

/**
 * Builds a snapshot of the current scene (main thread), after sending what is still gathering; see stream::snapshot
 * for one taken while a group is open. world adds the transforms' world matrices, evaluated their evaluated outputs.
 */
MessagePtr buildSnapshot(bool world = false, bool evaluated = false);
void setServerActive(bool on);
/** The filters of the server's clients, so the recorder knows what to work out for them. Any thread. */
void setServerFilters(std::vector<Filter> filters);
/** Has the recorder start diffing evaluated values for filters newly given to setServerFilters. Main thread. */
void syncServerFilters();

/** Hands a message to the server's thread; target 0 sends it to every client, else to that one. */
void serverSend(MessagePtr m, uint64_t target = 0);
} // namespace lila::stream::detail
