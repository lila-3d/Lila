#pragma once

#include "core/Export.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Lila's event bus (Maya's MMessage callbacks). Subscriptions are tagged with an owner (an extension id, or empty for
 * Lila) and removed with it when an extension unloads. Events are sent on the main thread; a handler that throws is
 * logged and skipped.
 */
namespace lila::events
{
enum class Event : uint16_t
{
    SceneNew,        // after File > New
    SceneOpened,     // after a scene file loaded; path
    SceneBeforeSave, // path
    SceneSaved,      // path
    SceneImported,   // path
    SceneExported,   // path
    SceneStateChanged, // the current path or the modified flag changed
    SelectionChanged,
    TimeChanged,     // time, in seconds
    PlaybackStarted,
    PlaybackStopped,
    Undo,            // name is the step's label
    Redo,            // name is the step's label
    UndoStackChanged, // a step pushed, undone, redone or the stack cleared
    NodeCreated,     // node
    NodeDeleted,     // node
    NodeRenamed,     // node, name
    ViewportToolChanged,
    ModeChanged,     // name
    LayoutChanged,   // name
    ExtensionLoaded, // name is the extension id
    ExtensionUnloaded,
    ExtensionChanged, // a watched package's files changed and settled; name, path (the package), paths
    AppIdle,         // the UI event loop is about to wait; only sent while someone listens
    Shutdown,        // before extensions unload and services stop
    Count,
};

struct EventData
{
    Event type = Event::Count;
    std::filesystem::path path;
    std::string name;
    uint64_t node = 0; // scene::NodeId
    double time = 0.0;
    std::vector<std::filesystem::path> paths;
};

using Handler = std::function<void(const EventData&)>;
using SubscriptionId = uint64_t;

/** 0 is never a valid id. */
LILA_FOUNDATION_API SubscriptionId subscribe(Event event, Handler handler, std::string owner = {});
/** False when id is not subscribed. Safe from inside a handler. */
LILA_FOUNDATION_API bool unsubscribe(SubscriptionId id);
/** Every subscription owner made. Returns the count. */
LILA_FOUNDATION_API size_t unsubscribeOwned(std::string_view owner);

LILA_FOUNDATION_API void send(const EventData& data);
/** Cheap: lets hot paths skip building an event nobody hears. */
LILA_FOUNDATION_API bool hasSubscribers(Event event);
LILA_FOUNDATION_API size_t subscriberCount(Event event);
LILA_FOUNDATION_API size_t subscriptionCount(std::string_view owner);

/** "sceneSaved" and so on: the names Python and docs use. */
LILA_FOUNDATION_API const char* name(Event event);
LILA_FOUNDATION_API std::optional<Event> fromName(std::string_view name);
} // namespace lila::events
