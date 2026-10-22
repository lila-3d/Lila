#include "core/Paths.h"

#include "core/Workspace.h"

#include <cstdlib>
#include <cwctype>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace lila::paths
{
namespace
{
fs::path envPath(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
}

/** Installed layout first, then the source tree for dev builds. */
fs::path findDataDir(const char* name)
{
    fs::path installed = executableDir() / name;
    if (fs::exists(installed))
        return installed;
    return fs::path(LILA_SOURCE_DIR) / name;
}

bool isUrl(std::string_view s)
{
    size_t colon = s.find("://");
    return colon != std::string_view::npos && colon > 1;
}

bool sameElement(const fs::path& a, const fs::path& b)
{
#ifdef _WIN32
    const std::wstring& x = a.native();
    const std::wstring& y = b.native();
    if (x.size() != y.size())
        return false;
    for (size_t i = 0; i < x.size(); ++i)
    {
        wchar_t cx = x[i] == L'\\' ? L'/' : wchar_t(std::towlower(x[i]));
        wchar_t cy = y[i] == L'\\' ? L'/' : wchar_t(std::towlower(y[i]));
        if (cx != cy)
            return false;
    }
    return true;
#else
    return a == b;
#endif
}

std::vector<fs::path> elements(const fs::path& p)
{
    std::vector<fs::path> out;
    for (const fs::path& e : p.lexically_normal().relative_path())
        if (!e.empty() && e != ".")
            out.push_back(e);
    return out;
}

fs::path& sceneDirStore()
{
    static fs::path dir;
    return dir;
}
} // namespace

fs::path userConfigDir()
{
    static const fs::path dir = [] {
        fs::path base;
#ifdef _WIN32
        base = envPath("APPDATA");
#elif defined(__APPLE__)
        base = envPath("HOME") / "Library" / "Application Support";
#else
        base = envPath("XDG_CONFIG_HOME");
        if (base.empty())
            base = envPath("HOME") / ".config";
#endif
        fs::path d = base / "Lila";
        if (fs::path over = envPath("LILA_CONFIG_DIR"); !over.empty())
            d = over;
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path executableDir()
{
    static const fs::path dir = [] {
#ifdef _WIN32
        wchar_t buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
        return fs::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
        char buf[4096];
        uint32_t size = sizeof(buf);
        _NSGetExecutablePath(buf, &size);
        return fs::canonical(buf).parent_path();
#else
        return fs::canonical("/proc/self/exe").parent_path();
#endif
    }();
    return dir;
}

fs::path resourcesDir()
{
    static const fs::path dir = findDataDir("resources");
    return dir;
}

fs::path pythonDir()
{
    static const fs::path dir = findDataDir("python");
    return dir;
}
fs::path fromUtf8(std::string_view utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string toUtf8(const fs::path& p)
{
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

const fs::path& currentSceneDir()
{
    return sceneDirStore();
}

void setCurrentSceneDir(const fs::path& dir)
{
    sceneDirStore() = dir;
}

fs::path relativeTo(const fs::path& path, const fs::path& base)
{
    if (!path.is_absolute() || !base.is_absolute() || !sameElement(path.root_name(), base.root_name()))
        return {};
    std::vector<fs::path> p = elements(path), b = elements(base);
    size_t common = 0;
    while (common < p.size() && common < b.size() && sameElement(p[common], b[common]))
        ++common;
    fs::path rel;
    for (size_t i = common; i < b.size(); ++i)
        rel /= "..";
    for (size_t i = common; i < p.size(); ++i)
        rel /= p[i];
    return rel.empty() ? fs::path(".") : rel;
}

std::string storedPath(const fs::path& path, const fs::path& sceneDir)
{
    std::string text = toUtf8(path);
    if (text.empty() || isUrl(text))
        return text;
    fs::path abs = path.is_absolute() ? path : resolvePath(text);
    // A workspace on another drive can't hold the path, but the scene folder still can.
    if (Workspace::get().isOpen())
        if (fs::path rel = relativeTo(abs, Workspace::get().root()); !rel.empty())
            return toUtf8(rel);
    if (fs::path rel = relativeTo(abs, sceneDir); !rel.empty())
        return toUtf8(rel);
    return toUtf8(abs.lexically_normal());
}

fs::path resolvePath(std::string_view stored, const fs::path& sceneDir)
{
    fs::path p = fromUtf8(stored);
    if (stored.empty() || isUrl(stored) || p.is_absolute())
        return p;
    std::vector<fs::path> bases;
    if (Workspace::get().isOpen())
        bases.push_back(Workspace::get().root());
    if (!sceneDir.empty())
        bases.push_back(sceneDir);
    std::error_code ec;
    for (const fs::path& b : bases)
        if (fs::exists(b / p, ec))
            return (b / p).lexically_normal();
    for (const fs::path& b : bases)
        if (fs::is_directory((b / p).parent_path(), ec))
            return (b / p).lexically_normal();
    return bases.empty() ? p : (bases.front() / p).lexically_normal();
}
} // namespace lila::paths
