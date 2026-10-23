#include "link/Exporters.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Registry.h"

#include <map>
#include <mutex>

namespace lila::link
{
namespace
{
// The link's worker thread looks exporters up, so every registry access holds this.
std::mutex gMutex;

Registry<ExportFn>& registry()
{
    static Registry<ExportFn> r;
    return r;
}

std::map<std::string, TakeExportFn, std::less<>>& takeExporters()
{
    static std::map<std::string, TakeExportFn, std::less<>> m;
    return m;
}
} // namespace

void registerExporter(std::string format, ExportFn fn)
{
    std::scoped_lock lock(gMutex);
    if (fn)
        registry().put(std::move(format), std::move(fn));
    else
        registry().remove(format);
}

bool registerExporter(ext::Context& ctx, std::string format, ExportFn fn)
{
    std::string owner = ctx.owner();
    {
        std::scoped_lock lock(gMutex);
        if (!fn || registry().contains(format))
        {
            log::error("extension {}: link exporter '{}' is {}", owner, format, fn ? "already registered" : "empty");
            return false;
        }
        registry().add(format, std::move(fn), owner);
    }
    ctx.track("linkExporter:" + format, [format, owner] {
        std::scoped_lock lock(gMutex);
        if (const std::string* o = registry().ownerOf(format); o && *o == owner)
            registry().remove(format);
    });
    return true;
}

void unregisterExporter(std::string_view format)
{
    std::scoped_lock lock(gMutex);
    registry().remove(format);
}

ExportFn findExporter(std::string_view format)
{
    std::scoped_lock lock(gMutex);
    const ExportFn* fn = registry().find(format);
    return fn ? *fn : ExportFn{};
}

std::vector<std::string> exporterFormats()
{
    std::scoped_lock lock(gMutex);
    return registry().ids();
}

void registerTakeExporter(std::string format, TakeExportFn fn)
{
    std::scoped_lock lock(gMutex);
    if (fn)
        takeExporters()[std::move(format)] = std::move(fn);
    else
        takeExporters().erase(format);
}

TakeExportFn findTakeExporter(std::string_view format)
{
    std::scoped_lock lock(gMutex);
    auto it = takeExporters().find(format);
    return it == takeExporters().end() ? TakeExportFn{} : it->second;
}

std::string exporterOwner(std::string_view format)
{
    std::scoped_lock lock(gMutex);
    const std::string* owner = registry().ownerOf(format);
    return owner ? *owner : std::string();
}
} // namespace lila::link
