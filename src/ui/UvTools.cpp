#include "ui/UvTools.h"

#include <map>

namespace lila::ui::uvTools
{
namespace
{
UvTool*& current()
{
    static UvTool* t = nullptr;
    return t;
}

std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}
} // namespace

void set(UvTool* tool)
{
    current() = tool;
    requestRedraw();
}

UvTool* active() { return current(); }

void requestRedraw()
{
    const auto copy = listeners();
    for (const auto& [id, fn] : copy)
        fn();
}

int addRedrawListener(std::function<void()> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeRedrawListener(int id) { listeners().erase(id); }
} // namespace lila::ui::uvTools
