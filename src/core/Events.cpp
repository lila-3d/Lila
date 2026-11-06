#include "core/Events.h"

#include "core/Log.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace lila::events
{
namespace
{
struct Subscription
{
    Event event;
    std::string owner;
    std::shared_ptr<Handler> handler;
};

struct Bus
{
    std::mutex mutex;
    std::map<SubscriptionId, Subscription> subs; // id order is subscription order
    SubscriptionId next = 1;
    std::array<std::atomic<uint32_t>, size_t(Event::Count)> counts{};
};

Bus& bus()
{
    static Bus b;
    return b;
}

constexpr const char* kNames[] = {
    "sceneNew",
    "sceneOpened",
    "sceneBeforeSave",
    "sceneSaved",
    "sceneImported",
    "sceneExported",
    "sceneStateChanged",
    "selectionChanged",
    "timeChanged",
    "playbackStarted",
    "playbackStopped",
    "undo",
    "redo",
    "undoStackChanged",
    "nodeCreated",
    "nodeDeleted",
    "nodeRenamed",
    "viewportToolChanged",
    "modeChanged",
    "layoutChanged",
    "extensionLoaded",
    "extensionUnloaded",
    "extensionChanged",
    "appIdle",
    "shutdown",
};
static_assert(std::size(kNames) == size_t(Event::Count));
} // namespace

SubscriptionId subscribe(Event event, Handler handler, std::string owner)
{
    if (event >= Event::Count || !handler)
        return 0;
    Bus& b = bus();
    std::lock_guard lock(b.mutex);
    SubscriptionId id = b.next++;
    b.subs.emplace(id, Subscription{event, std::move(owner), std::make_shared<Handler>(std::move(handler))});
    b.counts[size_t(event)].fetch_add(1, std::memory_order_relaxed);
    return id;
}

bool unsubscribe(SubscriptionId id)
{
    Bus& b = bus();
    std::lock_guard lock(b.mutex);
    auto it = b.subs.find(id);
    if (it == b.subs.end())
        return false;
    b.counts[size_t(it->second.event)].fetch_sub(1, std::memory_order_relaxed);
    b.subs.erase(it);
    return true;
}

size_t unsubscribeOwned(std::string_view owner)
{
    Bus& b = bus();
    std::lock_guard lock(b.mutex);
    size_t n = 0;
    for (auto it = b.subs.begin(); it != b.subs.end();)
    {
        if (it->second.owner == owner)
        {
            b.counts[size_t(it->second.event)].fetch_sub(1, std::memory_order_relaxed);
            it = b.subs.erase(it);
            ++n;
        }
        else
            ++it;
    }
    return n;
}

void send(const EventData& data)
{
    if (data.type >= Event::Count || !hasSubscribers(data.type))
        return;
    Bus& b = bus();
    std::vector<std::pair<SubscriptionId, std::shared_ptr<Handler>>> targets;
    {
        std::lock_guard lock(b.mutex);
        for (auto& [id, s] : b.subs)
            if (s.event == data.type)
                targets.emplace_back(id, s.handler);
    }
    for (auto& [id, handler] : targets)
    {
        {
            // Unsubscribed by an earlier handler of this same event.
            std::lock_guard lock(b.mutex);
            if (!b.subs.contains(id))
                continue;
        }
        try
        {
            (*handler)(data);
        }
        catch (const std::exception& e)
        {
            log::error("{} handler failed: {}", name(data.type), e.what());
        }
    }
}

bool hasSubscribers(Event event)
{
    return event < Event::Count && bus().counts[size_t(event)].load(std::memory_order_relaxed) > 0;
}

size_t subscriberCount(Event event)
{
    return event < Event::Count ? bus().counts[size_t(event)].load(std::memory_order_relaxed) : 0;
}

size_t subscriptionCount(std::string_view owner)
{
    Bus& b = bus();
    std::lock_guard lock(b.mutex);
    size_t n = 0;
    for (auto& [id, s] : b.subs)
        n += s.owner == owner;
    return n;
}

const char* name(Event event)
{
    return event < Event::Count ? kNames[size_t(event)] : "";
}

std::optional<Event> fromName(std::string_view n)
{
    for (size_t i = 0; i < std::size(kNames); ++i)
        if (n == kNames[i])
            return Event(i);
    return std::nullopt;
}
} // namespace lila::events
