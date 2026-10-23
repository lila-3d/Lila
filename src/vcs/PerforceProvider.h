#pragma once

#include "core/Export.h"

#include "vcs/Process.h"
#include "vcs/Provider.h"

#include <memory>
#include <mutex>

namespace lila::vcs
{
/** Empty fields fall through to p4's own lookup: P4CONFIG files, P4PORT/P4USER/P4CLIENT, then `p4 set`. */
struct LILA_SCENE_API PerforceSettings
{
    std::string exe; // empty = p4 on PATH, then the default install location
    std::string port;
    std::string user;
    std::string client;
    std::string charset;
    int timeoutMs = 0;

    /** The "perforce" object of sourceControl.json. */
    static PerforceSettings fromJson(const json& j);
    json toJson() const;
    bool operator==(const PerforceSettings&) const = default;
};

class LILA_SCENE_API PerforceProvider final : public Provider
{
public:
    struct Message
    {
        int severity = 0; // 1 info, 2 warning, 3 failed, 4 fatal
        int generic = 0;
        std::string text;
    };
    struct Output
    {
        std::vector<json> records;
        std::vector<Message> messages;
        std::string raw; // untagged stdout
    };

    PerforceProvider(PerforceSettings settings, std::shared_ptr<ProcessRunner> runner);

    std::string name() const override { return "perforce"; }
    json info() override;
    bool manages(const fs::path& dir) override;

    std::vector<FileStatus> status(const std::vector<fs::path>& files) override;
    OpResult checkout(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult add(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult revert(const std::vector<fs::path>& files) override;
    OpResult remove(const std::vector<fs::path>& files, const std::string& changelist = {}) override;
    OpResult submit(const std::string& changelist, const std::string& description,
                    const std::vector<fs::path>& files = {}) override;
    OpResult sync(const std::vector<fs::path>& files, const fs::path& context) override;
    std::vector<Changelist> changelists(const fs::path& context) override;
    std::vector<Revision> history(const fs::path& file, int maxCount = 50) override;
    OpResult lock(const std::vector<fs::path>& files) override;
    OpResult unlock(const std::vector<fs::path>& files) override;

    void fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest) override;
    std::string createChangelist(const std::string& description, const fs::path& context) override;
    OpResult moveToChangelist(const std::vector<fs::path>& files, const std::string& changelist) override;
    OpResult shelve(const std::string& changelist, const std::vector<fs::path>& files) override;
    OpResult unshelve(const std::string& changelist) override;

    bool supportsChangelists() const override { return true; }
    bool supportsShelving() const override { return true; }

    const PerforceSettings& settings() const { return mSettings; }
    /** Why the last manages() call failed, if it did. */
    std::string lastError()
    {
        std::scoped_lock lock(mMutex);
        return mLastError;
    }

    /** Runs p4 with -ztag -Mj (unless tagged is false) and the configured global options. Throws on errors. */
    Output run(const std::vector<std::string>& args, const fs::path& cwd, const std::string& input = {}, bool tagged = true,
               int timeoutMs = 0);

    /** Splits -Mj output into data records and messages. */
    static Output parse(const std::string& stdoutText);
    /** Turns p4's error text into something that says what to do about it. */
    static std::string describeError(const std::string& text, const PerforceSettings& settings);
    /** p4 file argument: escapes @ # % * in local paths, leaves depot syntax alone. */
    static std::string fileArg(const fs::path& file);
    static FileStatus statusFromFstat(const json& record);
    static std::vector<Revision> parseFilelog(const json& record);

private:
    std::string exe() const;
    OpResult fileOp(const std::string& cmd, const std::vector<std::string>& extra, const std::vector<fs::path>& files);
    std::string clientName(const fs::path& cwd);

    PerforceSettings mSettings;
    std::shared_ptr<ProcessRunner> mRunner;
    std::mutex mMutex;
    std::string mLastError;
};
} // namespace lila::vcs
