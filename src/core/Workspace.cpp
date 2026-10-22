#include "core/Workspace.h"

#include "core/Log.h"
#include "core/PrefStore.h"

#include <algorithm>

namespace lila
{
namespace fs = std::filesystem;

Workspace& Workspace::get()
{
    static Workspace ws;
    return ws;
}

std::optional<fs::path> Workspace::detect(const fs::path& path)
{
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    if (!fs::is_directory(p, ec))
        p = p.parent_path();
    for (; !p.empty(); p = p.parent_path())
    {
        if (fs::is_directory(p / ".lila", ec))
            return p;
        if (p == p.root_path())
            break;
    }
    return std::nullopt;
}

void Workspace::open(const fs::path& root)
{
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(root, ec);
    if (canon == mRoot)
        return;
    mRoot = canon;
    // The workspace layer reads .lila/ if it exists; writing creates it.
    PrefStore::get().setWorkspaceDir(mRoot / ".lila");

    json recent = PrefStore::get().value("general", "/recentFolders", json::array());
    if (!recent.is_array())
        recent = json::array();
    std::string s = mRoot.generic_string();
    recent.erase(std::remove(recent.begin(), recent.end(), json(s)), recent.end());
    recent.insert(recent.begin(), s);
    if (recent.size() > 15)
        recent.erase(recent.begin() + 15, recent.end());
    PrefStore::get().set("general", "/recentFolders", recent);

    log::info("workspace: {}", s);
    for (auto& fn : mListeners)
        fn();
}

void Workspace::close()
{
    if (mRoot.empty())
        return;
    mRoot.clear();
    PrefStore::get().setWorkspaceDir({});
    for (auto& fn : mListeners)
        fn();
}

fs::path Workspace::stateDir(bool create) const
{
    if (mRoot.empty())
        return {};
    fs::path dir = mRoot / ".lila";
    if (create)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
    }
    return dir;
}
} // namespace lila
