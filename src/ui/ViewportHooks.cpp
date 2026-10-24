#include "ui/ViewportHooks.h"

#include "core/Log.h"
#include "core/Registry.h"

#include <algorithm>
#include <map>

namespace lila::ui::viewportHooks
{
namespace
{
/** A hook and when it was first added, so hooks run in the order added and replacing one keeps its place. */
template <class Fn> struct Ordered
{
    Fn fn;
    uint64_t order = 0;
};

template <class Fn> class Hooks
{
public:
    explicit Hooks(const char* kind) : mKind(kind) {}

    void put(std::string id, Fn fn, std::string owner)
    {
        const Ordered<Fn>* had = mRegistry.find(id);
        uint64_t order = had ? had->order : mNext++;
        mRegistry.put(std::move(id), {std::move(fn), order}, std::move(owner));
    }

    ext::Context::Handle add(ext::Context& ctx, std::string id, Fn fn)
    {
        try
        {
            return ctx.add(mRegistry, mKind, std::move(id), Ordered<Fn>{std::move(fn), mNext++});
        }
        catch (const std::exception& e)
        {
            log::error("extension {}: viewport {}: {}", ctx.owner(), mKind, e.what());
            return 0;
        }
    }

    std::string anonymousId() { return std::string("lila.") + mKind + "." + std::to_string(mAnonymous++); }

    bool remove(std::string_view id) { return mRegistry.remove(id); }
    size_t removeOwned(std::string_view owner) { return mRegistry.removeOwned(owner); }

    std::vector<std::pair<uint64_t, std::string>> sorted() const
    {
        std::vector<std::pair<uint64_t, std::string>> out;
        for (const std::string& id : mRegistry.ids())
            out.emplace_back(mRegistry.find(id)->order, id);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<Fn> fns() const
    {
        std::vector<Fn> out;
        for (auto& [order, id] : sorted())
            out.push_back(mRegistry.find(id)->fn);
        return out;
    }

    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        for (auto& [order, id] : sorted())
            out.push_back(id);
        return out;
    }

private:
    const char* mKind;
    Registry<Ordered<Fn>> mRegistry;
    uint64_t mNext = 0;
    int mAnonymous = 0;
};

Hooks<Drawer>& drawerHooks()
{
    static Hooks<Drawer> h("drawer");
    return h;
}

Hooks<Picker>& pickerHooks()
{
    static Hooks<Picker> h("picker");
    return h;
}
} // namespace

void addDrawer(Drawer fn) { drawerHooks().put(drawerHooks().anonymousId(), std::move(fn), {}); }
void addPicker(Picker fn) { pickerHooks().put(pickerHooks().anonymousId(), std::move(fn), {}); }

void addDrawer(std::string id, Drawer fn, std::string owner) { drawerHooks().put(std::move(id), std::move(fn), std::move(owner)); }
void addPicker(std::string id, Picker fn, std::string owner) { pickerHooks().put(std::move(id), std::move(fn), std::move(owner)); }

ext::Context::Handle addDrawer(ext::Context& ctx, std::string id, Drawer fn) { return drawerHooks().add(ctx, std::move(id), std::move(fn)); }
ext::Context::Handle addPicker(ext::Context& ctx, std::string id, Picker fn) { return pickerHooks().add(ctx, std::move(id), std::move(fn)); }

bool removeDrawer(std::string_view id) { return drawerHooks().remove(id); }
bool removePicker(std::string_view id) { return pickerHooks().remove(id); }

size_t removeOwned(std::string_view owner) { return drawerHooks().removeOwned(owner) + pickerHooks().removeOwned(owner); }

std::vector<Drawer> drawers() { return drawerHooks().fns(); }
std::vector<Picker> pickers() { return pickerHooks().fns(); }
std::vector<std::string> drawerIds() { return drawerHooks().ids(); }
std::vector<std::string> pickerIds() { return pickerHooks().ids(); }
namespace
{
std::map<std::string, MaterialHook, std::less<>>& materialHooks()
{
    static std::map<std::string, MaterialHook, std::less<>> h;
    return h;
}
} // namespace

void setMaterialHook(std::string id, MaterialHook fn) { materialHooks()[std::move(id)] = std::move(fn); }

bool removeMaterialHook(std::string_view id)
{
    auto it = materialHooks().find(id);
    if (it == materialHooks().end())
        return false;
    materialHooks().erase(it);
    return true;
}

bool hookedMaterials(scene::NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& materials)
{
    for (const auto& [name, fn] : materialHooks())
        if (fn && fn(id, mesh, materials))
            return true;
    return false;
}
namespace
{
std::map<std::string, HideHook, std::less<>>& hideHooks()
{
    static std::map<std::string, HideHook, std::less<>> h;
    return h;
}
} // namespace

void setHideHook(std::string id, HideHook fn) { hideHooks()[std::move(id)] = std::move(fn); }

bool removeHideHook(std::string_view id)
{
    auto it = hideHooks().find(id);
    if (it == hideHooks().end())
        return false;
    hideHooks().erase(it);
    return true;
}

bool hiddenInView(scene::NodeId id)
{
    for (const auto& [name, fn] : hideHooks())
        if (fn && fn(id))
            return true;
    return false;
}
} // namespace lila::ui::viewportHooks
