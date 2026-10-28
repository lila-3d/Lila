#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace lila::vcs
{
using json = nlohmann::json;
namespace fs = std::filesystem;

/** A source control failure with a message fit for the user. */
struct VcsError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

enum class FileState : uint8_t
{
    Unknown,
    NotTracked,
    Synced,
    CheckedOut,      // opened for edit by me (git: modified)
    CheckedOutOther, // opened by someone else
    OutOfDate,       // a newer revision exists
    Locked,          // locked by someone else
    Added,
    Deleted,
};

LILA_SCENE_API const char* stateName(FileState s);

/** state is the one most worth showing; the flags carry the rest (a file can be mine and out of date). */
struct LILA_SCENE_API FileStatus
{
    std::string path;      // local path as given
    std::string depotPath; // p4 depot path, empty for git
    FileState state = FileState::Unknown;
    bool checkedOutByMe = false;
    bool outOfDate = false;
    bool lockedByMe = false;
    bool lockedByOther = false;
    bool exclusive = false; // p4 +l, git lockable
    std::vector<std::string> otherUsers; // "user@client" for p4, lock owner for git
    std::string action;     // p4 open action, git XY
    std::string changelist; // p4 change it is opened in
    std::string haveRev;
    std::string headRev;
    std::string fileType;

    json toJson() const;
};

struct LILA_SCENE_API OpResult
{
    std::vector<std::string> files;    // files the operation touched
    std::vector<std::string> warnings; // per-file problems that did not fail the whole call
    std::string change;                // submitted or created change, when there is one

    json toJson() const;
};

struct LILA_SCENE_API Changelist
{
    std::string id; // "default" or a number; git uses "default" for the index
    std::string description;
    std::string user;
    bool shelved = false;
    std::vector<std::string> files;

    json toJson() const;
};

struct LILA_SCENE_API Revision
{
    std::string revision; // p4 rev, git commit hash
    std::string change;
    std::string action;
    std::string user;
    int64_t time = 0; // unix seconds
    std::string description;

    json toJson() const;
};

/**
 * One source control system bound to its settings. Every call blocks and throws VcsError; SourceControl adds the
 * async layer. Implementations are safe to call from any thread.
 */
class LILA_SCENE_API Provider
{
public:
    virtual ~Provider() = default;

    virtual std::string name() const = 0;
    /** Connection and settings, for scProvider. */
    virtual json info() = 0;
    /** Whether files under root (a directory) belong to this provider. */
    virtual bool manages(const fs::path& dir) = 0;

    virtual std::vector<FileStatus> status(const std::vector<fs::path>& files) = 0;
    /** changelist empty = default. */
    virtual OpResult checkout(const std::vector<fs::path>& files, const std::string& changelist = {}) = 0;
    virtual OpResult add(const std::vector<fs::path>& files, const std::string& changelist = {}) = 0;
    virtual OpResult revert(const std::vector<fs::path>& files) = 0;
    virtual OpResult remove(const std::vector<fs::path>& files, const std::string& changelist = {}) = 0;
    /** Moves or renames a tracked file (or folder) so its history follows; to must not exist yet. */
    virtual OpResult move(const fs::path& from, const fs::path& to, const std::string& changelist = {}) = 0;
    /** files empty = the whole changelist (git: the index). Never pushes. */
    virtual OpResult submit(const std::string& changelist, const std::string& description,
                            const std::vector<fs::path>& files = {}) = 0;
    /** files empty = the whole workspace. */
    virtual OpResult sync(const std::vector<fs::path>& files, const fs::path& context) = 0;
    virtual std::vector<Changelist> changelists(const fs::path& context) = 0;
    virtual std::vector<Revision> history(const fs::path& file, int maxCount = 50) = 0;
    virtual OpResult lock(const std::vector<fs::path>& files) = 0;
    virtual OpResult unlock(const std::vector<fs::path>& files) = 0;

    /** Writes file as of revision (a Revision::revision from history()) to dest. */
    virtual void fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest);

    virtual std::string createChangelist(const std::string& description, const fs::path& context);
    virtual OpResult moveToChangelist(const std::vector<fs::path>& files, const std::string& changelist);
    virtual OpResult shelve(const std::string& changelist, const std::vector<fs::path>& files);
    virtual OpResult unshelve(const std::string& changelist);

    virtual bool supportsChangelists() const { return false; }
    virtual bool supportsShelving() const { return false; }
};
} // namespace lila::vcs
