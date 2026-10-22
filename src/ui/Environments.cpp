#include "ui/Environments.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "render/DisplayOptions.h"

#include <algorithm>
#include <system_error>

namespace lila::ui::environments
{
namespace
{
namespace fs = std::filesystem;

constexpr const char* kDefault = "studio_small_09_2k.hdr";

std::string utf8(const fs::path& p)
{
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

std::vector<fs::path> builtIn()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(paths::resourcesDir() / "hdri", ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".hdr")
            out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

std::string resolve(const std::string& pref)
{
    if (pref == kProcedural)
        return {};
    std::error_code ec;
    fs::path wanted = fs::path(reinterpret_cast<const char8_t*>(pref.c_str()));
    if (pref.empty())
        wanted = kDefault;
    if (wanted.is_relative())
    {
        fs::path shipped = paths::resourcesDir() / "hdri" / wanted;
        if (fs::exists(shipped, ec))
            return utf8(shipped);
        // No default HDRI installed: fall back to the procedural studio rather than a load error.
        if (pref.empty())
            return {};
    }
    return pref;
}

std::string hdriKey(const std::string& pref) { return pref.empty() ? kDefault : pref; }

namespace
{
std::string currentKey() { return hdriKey(PrefStore::get().value<std::string>("general", "/viewport/env/path", "")); }
} // namespace

float intensity()
{
    const PrefStore& p = PrefStore::get();
    return render::environmentIntensity(p.value("general", kIntensityByHdri), currentKey(), p.value<float>("general", kIntensity, 1.0f));
}

void setIntensity(std::optional<float> value)
{
    PrefStore& p = PrefStore::get();
    p.set("general", kIntensityByHdri, render::withEnvironmentIntensity(p.value("general", kIntensityByHdri), currentKey(), value));
}

bool hasOwnIntensity()
{
    json byHdri = PrefStore::get().value("general", kIntensityByHdri);
    return byHdri.is_object() && byHdri.contains(currentKey());
}
} // namespace lila::ui::environments
