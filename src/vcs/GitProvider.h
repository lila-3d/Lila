#pragma once

#include "core/Export.h"

#include "vcs/Process.h"
#include "vcs/Provider.h"

#include <map>
#include <memory>
#include <optional>

namespace lila::vcs
{
struct LILA_SCENE_API GitSettings
{
    std::string exe; // empty = git on PATH

    /** The "git" object of sourceControl.json. */
    static GitSettings fromJson(const json& j);
    json toJson() const;
    bool operator==(const GitSettings&) const = default;
};

/**
 * One work tree. Submit is a local commit and never pushes. Checkout takes a `git lfs lock` on LFS-tracked or
 * lockable files and is a no-op otherwise, since git files are always writable.
 */
class LILA_SCENE_API GitProvider final : public Provider
{
public:
    GitProvider(fs::path root, GitSettings settings, std::shared_ptr<ProcessRunner> runner);

    /** Top of the work tree containing dir, if any. */
    static std::optional<fs::path> findRoot(const fs::path& dir, const GitSettings& settings, ProcessRunner& runner);

    std::string name() const override { return "git"; }
    json info() override;
    bool manages(const fs::path& dir) override;

    std::vector<FileStatus> status(const std::vector<fs::path>& files) override;
    OpResult checkout(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult add(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult revert(const std::vector<fs::path>& files) override;
    OpResult remove(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult move(const fs::path& from, const fs::path& to, const std::string& changelist = {}) override;
    OpResult submit(const std::string& changelist, const std::string& description,
                    const std::vector<fs::path>& files = {}) override;
    OpResult sync(const std::vector<fs::path>& files, const fs::path& context) override;
    std::vector<Changelist> changelists(const fs::path& context) override;
    std::vector<Revision> history(const fs::path& file, int maxCount = 50) override;
    void fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest) override;
    OpResult lock(const std::vector<fs::path>& files) override;
    OpResult unlock(const std::vector<fs::path>& files) override;

    const fs::path& root() const { return mRoot; }

    /** Runs git -C <root>. Throws VcsError on a non-zero exit unless exitCode is given. */
    std::string run(const std::vector<std::string>& args, int* exitCode = nullptr);

private:
    struct Attrs
    {
        bool lfs = false;
        bool lockable = false;
    };

    std::string rel(const fs::path& file) const;
    fs::path abs(const std::string& rel) const;
    std::map<std::string, Attrs> attrs(const std::vector<std::string>& rels);
    std::vector<std::string> rels(const std::vector<fs::path>& files) const;
    OpResult lfsEach(const char* verb, const std::vector<fs::path>& files);

    fs::path mRoot;
    std::string mRootKey;
    GitSettings mSettings;
    std::shared_ptr<ProcessRunner> mRunner;
};
} // namespace lila::vcs
