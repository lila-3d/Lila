#include "link/Exporters.h"

#include <map>
#include <mutex>

namespace lila::link
{
namespace
{
std::mutex gMutex;

std::map<std::string, ExportFn, std::less<>>& registry()
{
    static std::map<std::string, ExportFn, std::less<>> r;
    return r;
}
} // namespace

void registerExporter(std::string format, ExportFn fn)
{
    std::scoped_lock lock(gMutex);
    if (fn)
        registry()[std::move(format)] = std::move(fn);
    else
        registry().erase(format);
}

void unregisterExporter(std::string_view format)
{
    std::scoped_lock lock(gMutex);
    if (auto it = registry().find(format); it != registry().end())
        registry().erase(it);
}

ExportFn findExporter(std::string_view format)
{
    std::scoped_lock lock(gMutex);
    auto it = registry().find(format);
    return it == registry().end() ? ExportFn{} : it->second;
}

std::vector<std::string> exporterFormats()
{
    std::scoped_lock lock(gMutex);
    std::vector<std::string> out;
    for (const auto& [format, fn] : registry())
        out.push_back(format);
    return out;
}
} // namespace lila::link
