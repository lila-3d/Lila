#include "UsdCommon.h"

#include <lila/UsdPlugins.h>

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/token.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <mutex>
#include <set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
namespace
{
const TfToken kLila("lila");
}

Axes makeAxes(bool fileZUp, double metersPerUnit)
{
    Axes a;
    a.zUp = fileZUp;
    a.cmPerUnit = metersPerUnit > 0.0 ? metersPerUnit * 100.0 : 1.0;
    a.basis = fbx::profileBasis(fileZUp ? fbx::AxisProfile::Ue5 : fbx::AxisProfile::Ue6, math::upAxis());
    a.map = fbx::axisMap(a.basis);
    a.toFile = glm::scale(glm::dmat4(1.0), glm::dvec3(1.0 / a.cmPerUnit)) * glm::dmat4(a.basis);
    a.toLila = glm::inverse(a.toFile);
    return a;
}

glm::dmat4 toGlm(const GfMatrix4d& m)
{
    // Gf is row-vector, glm column-vector: the same sixteen numbers in the same order.
    glm::dmat4 out(1.0);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = m[i][j];
    return out;
}

GfMatrix4d toGf(const glm::dmat4& m)
{
    GfMatrix4d out(1.0);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = m[i][j];
    return out;
}

std::string identifier(std::string_view raw, const char* fallback)
{
    std::string s;
    for (char c : raw)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        s.push_back(ok ? c : '_');
    }
    if (s.empty())
        return fallback;
    if (s[0] >= '0' && s[0] <= '9')
        s.insert(s.begin(), '_');
    return s;
}

fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p)
{
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

VtDictionary lilaData(const UsdObject& obj)
{
    VtValue v = obj.GetCustomDataByKey(kLila);
    return v.IsHolding<VtDictionary>() ? v.UncheckedGet<VtDictionary>() : VtDictionary();
}

void setLilaData(const UsdObject& obj, const std::string& key, const VtValue& value)
{
    obj.SetCustomDataByKey(TfToken("lila:" + key), value);
}

bool lilaFlag(const UsdObject& obj, const std::string& key)
{
    const VtDictionary d = lilaData(obj);
    auto it = d.find(key);
    return it != d.end() && it->second.IsHolding<bool>() && it->second.UncheckedGet<bool>();
}

Trs decompose(const glm::dmat4& m, scene::RotateOrder order, const glm::dvec3& ref, const glm::dmat3& pre)
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, q, out.t, skew, persp))
        return out;
    out.sheared = glm::length(skew) > 1e-6;
    out.r = scene::eulerNear(glm::transpose(pre) * glm::dmat3(glm::mat3_cast(q)), order, ref);
    for (int i = 0; i < 3; ++i)
        if (std::abs(out.r[i]) < 1e-9)
            out.r[i] = 0.0;
    return out;
}

bool nearMatrix(const glm::dmat4& a, const glm::dmat4& b, double eps)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0, std::abs(b[c][r])))
                return false;
    return true;
}

struct Diagnostics::Impl final : TfDiagnosticMgr::Delegate
{
    TfErrorMark mark;
    std::mutex lock;
    std::vector<std::string> warnings;
    std::set<std::string> seen;

    void add(const std::string& s)
    {
        std::lock_guard<std::mutex> g(lock);
        if (seen.insert(s).second && warnings.size() < 50)
            warnings.push_back(s);
    }
    void IssueError(const TfError& e) override { add(e.GetCommentary()); }
    void IssueFatalError(const TfCallContext&, const std::string& msg) override { add(msg); }
    void IssueStatus(const TfStatus&) override {}
    void IssueWarning(const TfWarning& w) override { add(w.GetCommentary()); }
};

Diagnostics::Diagnostics() : mImpl(std::make_unique<Impl>()) { TfDiagnosticMgr::GetInstance().AddDelegate(mImpl.get()); }

Diagnostics::~Diagnostics()
{
    TfDiagnosticMgr::GetInstance().RemoveDelegate(mImpl.get());
    mImpl->mark.Clear();
}

std::vector<std::string> Diagnostics::errors()
{
    std::vector<std::string> out;
    for (auto it = mImpl->mark.GetBegin(); it != mImpl->mark.GetEnd(); ++it)
        out.push_back(it->GetCommentary());
    mImpl->mark.Clear();
    return out;
}

std::vector<std::string> Diagnostics::warnings()
{
    std::lock_guard<std::mutex> g(mImpl->lock);
    return mImpl->warnings;
}

namespace
{
/** Plug loads plugin libraries with a plain LoadLibrary, whose dependency search skips the library's own folder: a
 * plugin needing a library beside it that nothing has loaded yet (MaterialX, Hydra) finds it through PATH. */
void addToPath([[maybe_unused]] const fs::path& folder)
{
#if defined(_WIN32)
    const std::wstring dir = folder.wstring();
    std::wstring path(GetEnvironmentVariableW(L"PATH", nullptr, 0), L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size())));
    if ((L";" + path + L";").find(L";" + dir + L";") == std::wstring::npos)
        SetEnvironmentVariableW(L"PATH", (path.empty() ? dir : path + L";" + dir).c_str());
#endif
}

/** A PXR_PLUGINPATH_NAME entry's plugInfo files: its own, and each subfolder's own or resources/ one. */
void plugInfoFiles(const fs::path& root, std::vector<std::string>& out)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return;
    if (fs::exists(root / "plugInfo.json", ec))
        out.push_back(utf8(root / "plugInfo.json"));
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec))
        for (const fs::path& f : {e.path() / "plugInfo.json", e.path() / "resources" / "plugInfo.json"})
            if (fs::exists(f, ec))
                out.push_back(utf8(f));
}

void registerExtensionPlugins(const fs::path& folder)
{
    addToPath(folder);
    std::vector<std::string> files;
    plugInfoFiles(folder, files);
    if (!files.empty())
        PlugRegistry::GetInstance().RegisterPlugins(files);
}
} // namespace

bool registerPlugins(const fs::path& packageRoot, std::string* error)
{
    addToPath(packageRoot);
    PlugRegistry& reg = PlugRegistry::GetInstance();
    std::vector<std::string> paths;
    for (const fs::path& folder : {packageRoot / "usd", packageRoot / "plugin" / "usd"})
    {
        std::error_code ec;
        if (fs::exists(folder / "plugInfo.json", ec))
            paths.push_back(utf8(folder / "plugInfo.json"));
    }
    // Plug reads only <entry>/plugInfo.json from PXR_PLUGINPATH_NAME; the viewport's Renderer menu also lists each
    // subfolder's own and resources/ plugInfo.json there, so register those too.
    if (const char* env = std::getenv("PXR_PLUGINPATH_NAME"))
    {
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::stringstream list(env);
        for (std::string entry; std::getline(list, entry, sep);)
            if (!entry.empty())
                plugInfoFiles(fs::path(entry), paths);
    }
    if (!paths.empty())
        reg.RegisterPlugins(paths);
    // Plug can't forget a plugin, so a folder stays registered after its extension unloads.
    for (const fs::path* folder : usdPlugins::registry().allInOrder())
        registerExtensionPlugins(*folder);
    usdPlugins::registry().addListener([](RegistryChange change, const std::string& id, const std::string&) {
        if (change == RegistryChange::Added)
            if (const fs::path* folder = usdPlugins::registry().find(id))
                registerExtensionPlugins(*folder);
    });
    if (!reg.GetPluginWithName("usdGeom") || !reg.GetPluginWithName("usdShade") || !reg.GetPluginWithName("usdSkel"))
    {
        if (error)
            *error = "OpenUSD's plugInfo.json files are missing from " + utf8(packageRoot / "usd");
        return false;
    }
    return true;
}
} // namespace lila::usd
