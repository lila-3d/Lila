#include "ui/RenderPasses.h"

#include "core/Log.h"

#include <algorithm>
#include <tuple>

namespace lila::ui::renderPasses
{
Registry<Pass>& registry()
{
    static Registry<Pass> r;
    return r;
}

const Pass& add(Pass pass)
{
    std::string id = pass.id;
    return registry().add(std::move(id), std::move(pass));
}

const Pass* add(ext::Context& ctx, Pass pass)
{
    std::string id = pass.id;
    try
    {
        ctx.add(registry(), "renderPass", id, std::move(pass));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: render pass: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

void run(Stage stage, Context& ctx)
{
    if (registry().empty())
        return;
    std::vector<Pass> passes;
    for (const Pass* p : registry().all())
        if (p->stage == stage && p->run)
            passes.push_back(*p);
    std::sort(passes.begin(), passes.end(), [](const Pass& a, const Pass& b) { return std::tie(a.order, a.id) < std::tie(b.order, b.id); });
    for (const Pass& p : passes)
    {
        try
        {
            p.run(ctx);
        }
        catch (const std::exception& e)
        {
            log::error("render pass {}: {}", p.id, e.what());
        }
    }
}
} // namespace lila::ui::renderPasses
