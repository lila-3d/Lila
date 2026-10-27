// Placeholder Lights: the light provider Lila falls back on. Creates and draws lights while no light extension that
// renders them is enabled, and stands aside as soon as one is.

#include "Glyphs.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Docs.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Menus.h>

#include <array>
#include <string>
#include <vector>

using namespace lila;

namespace
{
constexpr const char* kProviderId = "placeholder";

struct State
{
    ext::Context* ctx = nullptr;
    /** The menu and actions, live with the draw override while no other provider renders lights. */
    std::vector<ext::Context::Handle> ui;
};

State gState;

bool rendererInstalled()
{
    for (const scene::lights::Provider* p : scene::lights::providers().all())
        if (!p->placeholder)
            return true;
    return false;
}

void addUi(ext::Context& ctx)
{
    struct Entry
    {
        const char* type;
        const char* label;
        const char* doc;
    };
    static constexpr std::array<Entry, 6> kEntries{{
        {"point", "Point Light", "A light shining every way from one point."},
        {"spot", "Spot Light", "A cone of light down its -Z, with an inner and outer angle."},
        {"directional", "Directional Light", "Parallel light down its -Z, like the sun. Only its rotation matters."},
        {"area", "Area Light", "A rectangle of light facing its -Z."},
        {"disc", "Disc Light", "A disc of light facing its -Z."},
        {"dome", "Dome Light", "Light from all around the scene, from an environment image when it has one."},
    }};

    std::vector<ext::Context::Handle>& handles = gState.ui;
    ui::drawOverrides::add(ctx, {.id = "lights.placeholder", .nodeType = "light", .draw = lights_ext::draw, .pick = lights_ext::pick});
    handles.push_back(ui::menus::addItem(ctx, {.id = "lights.menu", .menu = "Create", .kind = ui::menus::Kind::Submenu, .label = "Light"}));
    for (const Entry& e : kEntries)
    {
        const std::string type = e.type;
        const std::string id = "lights.create." + type;
        handles.push_back(ui::actions::add(ctx, {.id = id,
                                            .label = e.label,
                                            .category = "Create",
                                            .run =
                                                [type] {
                                                    try
                                                    {
                                                        ops::execute("light", {{"type", type}}, CallSource::UI);
                                                    }
                                                    catch (const std::exception& ex)
                                                    {
                                                        log::write(LogLevel::Warning, ex.what());
                                                    }
                                                },
                                            .doc = e.doc,
                                            .python = "light(type='" + type + "')"}));
        handles.push_back(ui::menus::addItem(ctx, {.id = "lights.menu." + type, .menu = "Create/Light", .action = id}));
    }
    handles.push_back(ui::menus::addItem(ctx, {.id = "lights.menu.docsSeparator", .menu = "Create/Light", .kind = ui::menus::Kind::Separator}));
    handles.push_back(ui::menus::addItem(ctx, {.id = "lights.menu.docs", .menu = "Create/Light", .action = ui::docs::pageAction("lights")}));
}

void removeUi(ext::Context& ctx)
{
    for (auto it = gState.ui.rbegin(); it != gState.ui.rend(); ++it)
        ctx.remove(*it);
    gState.ui.clear();
    ui::drawOverrides::remove("lights.placeholder");
}

/** Shows the placeholder UI exactly while nothing else renders lights. */
void follow()
{
    if (!gState.ctx)
        return;
    const bool wanted = !rendererInstalled();
    const bool shown = ui::drawOverrides::registry().contains("lights.placeholder");
    if (wanted && !shown)
        addUi(*gState.ctx);
    else if (!wanted && shown)
        removeUi(*gState.ctx);
}
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    gState.ctx = &ctx;
    scene::lights::registerProvider(ctx, {.id = kProviderId, .name = "Placeholder Lights", .placeholder = true});
    const int listener = scene::lights::providers().addListener([](RegistryChange, const std::string&, const std::string&) { follow(); });
    ctx.track("lightProviderListener", [listener] { scene::lights::providers().removeListener(listener); });
    follow();
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(ext::Context&)
{
    gState.ctx = nullptr;
    gState.ui.clear();
}
