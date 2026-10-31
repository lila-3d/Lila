#include "core/UsdPlugins.h"

#include "core/ExtensionContext.h"

namespace lila::usdPlugins
{
namespace fs = std::filesystem;

Registry<fs::path>& registry()
{
    static Registry<fs::path> r;
    return r;
}

bool add(ext::Context& ctx, const fs::path& folder)
{
    std::error_code ec;
    const fs::path abs = fs::absolute(folder, ec).lexically_normal();
    if (!fs::is_directory(abs, ec))
    {
        ctx.log(LogLevel::Error, "USD plugin folder " + abs.string() + " doesn't exist");
        return false;
    }
    const std::string id = abs.generic_string();
    if (registry().contains(id))
        return true;
    ctx.add(registry(), "usdPlugins", id, abs);
    return true;
}
} // namespace lila::usdPlugins
