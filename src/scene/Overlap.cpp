#include "scene/Overlap.h"

namespace lila::overlap
{
Registry<Provider>& providers()
{
    static Registry<Provider> r;
    return r;
}

std::unique_ptr<Session> open(const geo::Heightfield& ground, std::string* label)
{
    for (const std::string& id : providers().ids())
        if (const Provider* p = providers().find(id); p && p->open)
            if (std::unique_ptr<Session> s = p->open(ground))
            {
                if (label)
                    *label = p->label;
                return s;
            }
    return nullptr;
}
} // namespace lila::overlap
