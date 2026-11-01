#include "ui/PaintColor.h"

#include "ui/SculptTool.h"

#include <map>

namespace lila::ui::paintColor
{
namespace
{
std::map<int, std::function<void(const glm::vec3&)>>& listeners()
{
    static std::map<int, std::function<void(const glm::vec3&)>> m;
    return m;
}
} // namespace

void pick(const glm::vec3& linear)
{
    sculptSettings().color = linear;
    sculptSettingsChanged();
    auto copy = listeners();
    for (auto& [id, fn] : copy)
        if (fn)
            fn(linear);
}

int addListener(std::function<void(const glm::vec3& linear)> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeListener(int id)
{
    listeners().erase(id);
}
} // namespace lila::ui::paintColor
