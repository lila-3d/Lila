#include "core/Registry.h"

#include "core/Log.h"

#include <map>
#include <optional>

namespace lila::owners
{
namespace
{
struct Lists
{
    std::map<uint64_t, Sweep> registries;
    std::map<uint64_t, Sweep> cleanups;
    uint64_t next = 1;
};

Lists& lists()
{
    static Lists l;
    return l;
}

thread_local std::optional<std::string> tAttributed;
} // namespace

uint64_t track(Sweep sweep)
{
    Lists& l = lists();
    l.registries.emplace(l.next, std::move(sweep));
    return l.next++;
}

void untrack(uint64_t id)
{
    lists().registries.erase(id);
}

uint64_t addCleanup(Sweep cleanup)
{
    Lists& l = lists();
    l.cleanups.emplace(l.next, std::move(cleanup));
    return l.next++;
}

void removeCleanup(uint64_t id)
{
    lists().cleanups.erase(id);
}

size_t removeOwned(std::string_view owner)
{
    if (owner.empty())
        return 0;
    size_t n = 0;
    // By id, since a sweep can make or destroy registries (a panel's widget, say) while this runs.
    for (int pass = 0; pass < 2; ++pass)
    {
        std::map<uint64_t, Sweep>& from = pass == 0 ? lists().cleanups : lists().registries;
        std::vector<uint64_t> ids;
        for (auto& [id, fn] : from)
            ids.push_back(id);
        for (uint64_t id : ids)
        {
            auto it = from.find(id);
            if (it == from.end())
                continue;
            Sweep fn = it->second;
            try
            {
                n += fn(owner);
            }
            catch (const std::exception& e)
            {
                log::error("removing what {} registered: {}", owner, e.what());
            }
        }
    }
    return n;
}

const std::string* attributedOwner()
{
    return tAttributed && !tAttributed->empty() ? &*tAttributed : nullptr;
}

OwnerScope::OwnerScope(std::string owner) : mHadPrevious(tAttributed.has_value())
{
    if (mHadPrevious)
        mPrevious = std::move(*tAttributed);
    tAttributed = std::move(owner);
}

OwnerScope::~OwnerScope()
{
    if (mHadPrevious)
        tAttributed = std::move(mPrevious);
    else
        tAttributed.reset();
}
} // namespace lila::owners
