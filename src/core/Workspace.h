#pragma once

#include "core/Export.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace lila
{
/**
 * The folder you're working in. Any folder works; Lila only keeps its own state under <root>/.lila/, created the
 * first time something needs to be saved there. Having no workspace is a normal state.
 */
class LILA_FOUNDATION_API Workspace
{
public:
    static Workspace& get();

    /** Nearest ancestor of path (or path itself) containing .lila/. */
    static std::optional<std::filesystem::path> detect(const std::filesystem::path& path);

    void open(const std::filesystem::path& root);
    void close();

    bool isOpen() const { return !mRoot.empty(); }
    const std::filesystem::path& root() const { return mRoot; }

    /** <root>/.lila, created on demand. Empty when no workspace is open. */
    std::filesystem::path stateDir(bool create = true) const;

    using ChangeFn = std::function<void()>;
    void onChanged(ChangeFn fn) { mListeners.push_back(std::move(fn)); }

private:
    std::filesystem::path mRoot;
    std::vector<ChangeFn> mListeners;
};
} // namespace lila
