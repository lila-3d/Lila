#include "ui/Gizmo.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Manipulator.h"

#include <map>

namespace lila::ui::gizmos
{
namespace
{
std::string gOverride;
std::map<int, std::function<void()>> gListeners;
int gNextListener = 1;

void notify()
{
    auto copy = gListeners;
    for (auto& [id, fn] : copy)
        fn();
}
} // namespace

Registry<Provider>& registry()
{
    static Registry<Provider>* r = [] {
        auto* reg = new Registry<Provider>();
        reg->add(kDefault, {.id = kDefault, .label = "Move / Rotate / Scale", .create = [] { return std::make_unique<Manipulator>(); }});
        // Every change can retire an instance a viewport holds, so viewports hear about each one.
        reg->addListener([](RegistryChange, const std::string&, const std::string&) { notify(); });
        return reg;
    }();
    return *r;
}

const Provider& add(Provider provider)
{
    std::string id = provider.id;
    return registry().add(std::move(id), std::move(provider));
}

const Provider* add(ext::Context& ctx, Provider provider)
{
    std::string id = provider.id;
    try
    {
        ctx.add(registry(), "gizmo", id, std::move(provider));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: gizmo: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

const Provider* find(std::string_view id) { return registry().find(id); }

bool setActive(std::string_view id)
{
    if (!id.empty() && !registry().contains(id))
        return false;
    if (gOverride == id)
        return true;
    gOverride = std::string(id);
    notify();
    return true;
}

const std::string& activeOverride() { return gOverride; }

std::string activeId()
{
    if (!gOverride.empty() && registry().contains(gOverride))
        return gOverride;
    std::string pref = PrefStore::get().value<std::string>("general", kPref, kDefault);
    if (registry().contains(pref))
        return pref;
    return registry().contains(kDefault) ? std::string(kDefault) : std::string();
}

std::unique_ptr<Gizmo> create(std::string_view id)
{
    const Provider* p = registry().find(id);
    return p && p->create ? p->create() : nullptr;
}

int addListener(std::function<void()> fn)
{
    gListeners[gNextListener] = std::move(fn);
    return gNextListener++;
}

void removeListener(int id) { gListeners.erase(id); }
} // namespace lila::ui::gizmos
