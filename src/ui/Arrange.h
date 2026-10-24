#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace lila::ui
{
/** Items anchored after or before another item go next to it; the rest keep registration order. */
template <class T, class Key, class Anchor>
std::vector<T*> arrange(std::vector<T*> all, Key key, Anchor anchor)
{
    std::vector<T*> out, pending;
    for (T* t : all)
        (anchor(t).first.empty() && anchor(t).second.empty() ? out : pending).push_back(t);
    // Anchors may point at other anchored entries, so place what can be placed until nothing moves.
    bool moved = true;
    while (moved && !pending.empty())
    {
        moved = false;
        for (auto it = pending.begin(); it != pending.end();)
        {
            auto [after, before] = anchor(*it);
            const std::string& target = after.empty() ? before : after;
            auto at = std::find_if(out.begin(), out.end(), [&](T* t) { return key(t) == target; });
            if (at == out.end())
            {
                ++it;
                continue;
            }
            out.insert(after.empty() ? at : at + 1, *it);
            it = pending.erase(it);
            moved = true;
        }
    }
    out.insert(out.end(), pending.begin(), pending.end());
    return out;
}
} // namespace lila::ui
