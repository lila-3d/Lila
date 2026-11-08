#include "scene/SpecSteps.h"

#include <algorithm>

namespace lila::specs
{
Registry<Step>& steps()
{
    static Registry<Step> r;
    return r;
}

std::vector<const Step*> stepsFor(std::string_view spec)
{
    std::vector<const Step*> out;
    for (const std::string& id : steps().ids())
        if (const Step* s = steps().find(id); s && s->spec == spec)
            out.push_back(s);
    std::sort(out.begin(), out.end(), [](const Step* a, const Step* b) { return a->key < b->key; });
    return out;
}
} // namespace lila::specs
