#include "scene/AnimInterpolator.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"

namespace lila::scene::anim
{
Registry<Interpolator>& interpolators()
{
    static Registry<Interpolator> r;
    return r;
}

const Interpolator& registerInterpolator(Interpolator ip)
{
    if (!ip.evaluate)
        throw std::invalid_argument("interpolator '" + ip.name + "' has no evaluate");
    std::string name = ip.name;
    return interpolators().add(std::move(name), std::move(ip));
}

const Interpolator* registerInterpolator(ext::Context& ctx, Interpolator ip)
{
    std::string name = ip.name;
    try
    {
        if (!ip.evaluate)
            throw std::invalid_argument("interpolator '" + name + "' has no evaluate");
        ctx.add(interpolators(), "interpolator", name, std::move(ip));
        return interpolators().find(name);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

const Interpolator* findInterpolator(std::string_view name)
{
    return name.empty() ? nullptr : interpolators().find(name);
}
} // namespace lila::scene::anim
