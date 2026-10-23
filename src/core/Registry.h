#pragma once

#include "core/Export.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lila
{
enum class RegistryChange : uint8_t
{
    Added,
    Removed,
};

/** What every Registry shares, so an extension's registrations can be found and removed wherever they went. */
namespace owners
{
using Sweep = std::function<size_t(std::string_view owner)>;

/** Every live Registry is listed here (Registry does it itself); untrack with the id track returned. */
LILA_FOUNDATION_API uint64_t track(Sweep sweep);
LILA_FOUNDATION_API void untrack(uint64_t id);

/**
 * Runs before the registries are swept, for systems whose entries must go in a particular order or with side effects
 * (a mode is left before it goes). Returns an id for removeCleanup.
 */
LILA_FOUNDATION_API uint64_t addCleanup(Sweep cleanup);
LILA_FOUNDATION_API void removeCleanup(uint64_t id);

/** Runs the cleanups, then removes owner's entries from every registry. Returns the count removed. */
LILA_FOUNDATION_API size_t removeOwned(std::string_view owner);

/** The owner registrations on this thread are attributed to while an OwnerScope is open; null for none. */
LILA_FOUNDATION_API const std::string* attributedOwner();

/**
 * While open, everything registered on this thread is owned by owner whatever owner the call names: Lila opens one
 * around an extension's init, so what it registers is removed with it even through calls that take no context. An
 * empty owner suspends the attribution, for Lila's own entries made lazily during someone's init.
 */
class LILA_FOUNDATION_API OwnerScope
{
public:
    explicit OwnerScope(std::string owner);
    ~OwnerScope();
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

private:
    std::string mPrevious;
    bool mHadPrevious;
};
} // namespace owners

/**
 * Items registered by id, each tagged with its owner: an extension id, or empty for Lila itself. Owners are removed
 * as a whole when an extension unloads (newest first), so every registry unloads the same way. Main thread only.
 *
 * The registry lives in whichever library owns the system it serves; this template holds no state of its own.
 */
template <class T> class Registry
{
public:
    using Listener = std::function<void(RegistryChange, const std::string& id, const std::string& owner)>;

    Registry() : mSweep(owners::track([this](std::string_view owner) { return removeOwned(owner); })) {}
    ~Registry() { owners::untrack(mSweep); }
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /** Throws std::invalid_argument when id is taken. */
    T& add(std::string id, T item, std::string owner = {})
    {
        if (mEntries.contains(id))
            throw std::invalid_argument("'" + id + "' is already registered");
        return put(std::move(id), std::move(item), std::move(owner));
    }

    /** Adds, or replaces what id held. */
    T& put(std::string id, T item, std::string owner = {})
    {
        if (const std::string* attributed = owners::attributedOwner())
            owner = *attributed;
        if (mEntries.contains(id))
            take(id);
        Entry& e = mEntries[id];
        e.owner = std::move(owner);
        e.item = std::make_unique<T>(std::move(item));
        e.order = mNextOrder++;
        T& out = *e.item;
        notify(RegistryChange::Added, id, e.owner);
        return out;
    }

    /** Removes id and hands back its item, for callers that must keep it alive past removal. Null when absent. */
    std::unique_ptr<T> take(std::string_view id)
    {
        auto it = mEntries.find(id);
        if (it == mEntries.end())
            return nullptr;
        std::unique_ptr<T> item = std::move(it->second.item);
        std::string key = it->first;
        std::string owner = std::move(it->second.owner);
        mEntries.erase(it);
        notify(RegistryChange::Removed, key, owner);
        return item;
    }

    bool remove(std::string_view id) { return take(id) != nullptr; }

    /** Removes everything owner registered, newest first. taken, when given, receives the items. Returns the count. */
    size_t removeOwned(std::string_view owner, std::vector<std::unique_ptr<T>>* taken = nullptr)
    {
        std::vector<std::string> ids = owned(owner);
        for (auto id = ids.rbegin(); id != ids.rend(); ++id)
            if (std::unique_ptr<T> item = take(*id); item && taken)
                taken->push_back(std::move(item));
        return ids.size();
    }

    T* find(std::string_view id) const
    {
        auto it = mEntries.find(id);
        return it == mEntries.end() ? nullptr : it->second.item.get();
    }

    bool contains(std::string_view id) const { return mEntries.find(id) != mEntries.end(); }

    /** Null when id is not registered. */
    const std::string* ownerOf(std::string_view id) const
    {
        auto it = mEntries.find(id);
        return it == mEntries.end() ? nullptr : &it->second.owner;
    }

    /** By id. */
    std::vector<T*> all() const
    {
        std::vector<T*> out;
        out.reserve(mEntries.size());
        for (auto& [id, e] : mEntries)
            out.push_back(e.item.get());
        return out;
    }

    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        out.reserve(mEntries.size());
        for (auto& [id, e] : mEntries)
            out.push_back(id);
        return out;
    }

    /** Every id, oldest first: the order menus, panels and pages appear in. */
    std::vector<std::string> idsInOrder() const
    {
        std::vector<std::pair<uint64_t, std::string>> found;
        for (auto& [id, e] : mEntries)
            found.emplace_back(e.order, id);
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        out.reserve(found.size());
        for (auto& [order, id] : found)
            out.push_back(std::move(id));
        return out;
    }

    /** Every item, oldest first. */
    std::vector<T*> allInOrder() const
    {
        std::vector<T*> out;
        for (const std::string& id : idsInOrder())
            out.push_back(find(id));
        return out;
    }

    /** The ids owner registered, oldest first. */
    std::vector<std::string> owned(std::string_view owner) const
    {
        std::vector<std::pair<uint64_t, std::string>> found;
        for (auto& [id, e] : mEntries)
            if (e.owner == owner)
                found.emplace_back(e.order, id);
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        out.reserve(found.size());
        for (auto& [order, id] : found)
            out.push_back(std::move(id));
        return out;
    }

    size_t size() const { return mEntries.size(); }
    bool empty() const { return mEntries.empty(); }

    /** Called after every add and removal. Returns an id for removeListener. */
    int addListener(Listener fn)
    {
        mListeners.emplace(mNextListener, std::move(fn));
        return mNextListener++;
    }
    void removeListener(int id) { mListeners.erase(id); }

private:
    struct Entry
    {
        std::string owner;
        std::unique_ptr<T> item;
        uint64_t order = 0;
    };

    void notify(RegistryChange change, const std::string& id, const std::string& owner)
    {
        if (mListeners.empty())
            return;
        // A listener may add or remove listeners.
        std::vector<Listener> fns;
        for (auto& [lid, fn] : mListeners)
            fns.push_back(fn);
        for (const Listener& fn : fns)
            fn(change, id, owner);
    }

    std::map<std::string, Entry, std::less<>> mEntries;
    std::map<int, Listener> mListeners;
    int mNextListener = 1;
    uint64_t mNextOrder = 0;
    uint64_t mSweep = 0;
};
} // namespace lila
