#include "vcs/GitProvider.h"

#include "core/Profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace lila::vcs
{
namespace
{
std::string trim(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i]))
        ++i;
    return s.substr(i);
}

/** Paths compare case-insensitively where the file system does. */
std::string key(std::string s)
{
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}

std::vector<std::string> splitNul(const std::string& s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size())
    {
        size_t end = s.find('\0', at);
        if (end == std::string::npos)
            end = s.size();
        out.push_back(s.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

fs::path canonicalPath(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? fs::absolute(p).lexically_normal() : c;
}

std::string exeOf(const GitSettings& s)
{
    if (!s.exe.empty())
        return s.exe;
    fs::path p = findExecutable("git");
    return p.empty() ? std::string("git") : p.string();
}

ProcessSpec gitSpec(const GitSettings& s, const fs::path& dir, const std::vector<std::string>& args)
{
    ProcessSpec spec;
    spec.argv = {exeOf(s), "-c", "core.quotepath=off", "-C", dir.string()};
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.env = {{"GIT_TERMINAL_PROMPT", "0"}, {"GCM_INTERACTIVE", "never"}, {"GIT_LITERAL_PATHSPECS", "1"}};
    return spec;
}
} // namespace

GitSettings GitSettings::fromJson(const json& j)
{
    GitSettings s;
    if (j.is_object() && j.contains("exe") && j["exe"].is_string())
        s.exe = j["exe"].get<std::string>();
    return s;
}

json GitSettings::toJson() const
{
    return {{"exe", exe}};
}

GitProvider::GitProvider(fs::path root, GitSettings settings, std::shared_ptr<ProcessRunner> runner)
    : mRoot(canonicalPath(root)), mSettings(std::move(settings)), mRunner(std::move(runner))
{
    mRootKey = key(mRoot.generic_string());
}

std::optional<fs::path> GitProvider::findRoot(const fs::path& dir, const GitSettings& settings, ProcessRunner& runner)
{
    LILA_ZONE();

    fs::path d = dir;
    std::error_code ec;
    while (!d.empty() && !fs::is_directory(d, ec) && d != d.parent_path())
        d = d.parent_path();
    if (!fs::is_directory(d, ec))
        return std::nullopt;
    ProcessSpec spec = gitSpec(settings, d, {"rev-parse", "--show-toplevel"});
    spec.timeoutMs = 15000;
    ProcessResult r = runner.run(spec);
    if (!r.started || r.exitCode != 0)
        return std::nullopt;
    std::string top = trim(r.out);
    if (top.empty())
        return std::nullopt;
    return fs::path(top);
}

std::string GitProvider::run(const std::vector<std::string>& args, int* exitCode)
{
    LILA_ZONE();

    ProcessResult r = mRunner->run(gitSpec(mSettings, mRoot, args));
    if (!r.started)
        throw VcsError("git: not installed or not on PATH (" + trim(r.err) + ")");
    if (exitCode)
    {
        *exitCode = r.exitCode;
        return r.out;
    }
    if (r.exitCode != 0)
    {
        std::string msg = trim(r.err.empty() ? r.out : r.err);
        throw VcsError("git " + (args.empty() ? std::string() : args.front()) + ": " +
                       (msg.empty() ? "exited with code " + std::to_string(r.exitCode) : msg));
    }
    return r.out;
}

std::string GitProvider::rel(const fs::path& file) const
{
    std::string f = canonicalPath(file).generic_string();
    std::string k = key(f);
    if (k == mRootKey)
        return ".";
    if (k.size() > mRootKey.size() && k.compare(0, mRootKey.size(), mRootKey) == 0 && k[mRootKey.size()] == '/')
        return f.substr(mRootKey.size() + 1);
    if (mRootKey.ends_with('/') && k.starts_with(mRootKey))
        return f.substr(mRootKey.size());
    throw VcsError("git: " + file.string() + " is outside the repository at " + mRoot.string());
}

fs::path GitProvider::abs(const std::string& r) const
{
    return (mRoot / fs::path(r)).lexically_normal();
}

std::vector<std::string> GitProvider::rels(const std::vector<fs::path>& files) const
{
    std::vector<std::string> out;
    for (const fs::path& f : files)
        out.push_back(rel(f));
    return out;
}

json GitProvider::info()
{
    LILA_ZONE();

    json j = {{"provider", name()}, {"root", mRoot.string()}, {"settings", mSettings.toJson()}};
    int code = 0;
    std::string branch = trim(run({"rev-parse", "--abbrev-ref", "HEAD"}, &code));
    if (code == 0)
        j["branch"] = branch;
    std::string upstream = trim(run({"rev-parse", "--abbrev-ref", "@{u}"}, &code));
    if (code == 0)
        j["upstream"] = upstream;
    return j;
}

bool GitProvider::manages(const fs::path& dir)
{
    try
    {
        rel(dir);
        return true;
    }
    catch (const VcsError&)
    {
        return false;
    }
}

std::map<std::string, GitProvider::Attrs> GitProvider::attrs(const std::vector<std::string>& paths)
{
    LILA_ZONE();

    std::map<std::string, Attrs> out;
    if (paths.empty())
        return out;
    std::vector<std::string> args{"check-attr", "-z", "filter", "lockable", "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    int code = 0;
    std::vector<std::string> parts = splitNul(run(args, &code));
    if (code != 0)
        return out;
    for (size_t i = 0; i + 2 < parts.size(); i += 3)
    {
        Attrs& a = out[key(parts[i])];
        const std::string& attr = parts[i + 1];
        const std::string& value = parts[i + 2];
        if (attr == "filter" && value == "lfs")
            a.lfs = true;
        if (attr == "lockable" && value != "unspecified" && value != "unset")
            a.lockable = true;
    }
    return out;
}

std::vector<FileStatus> GitProvider::status(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    std::vector<FileStatus> out;
    if (files.empty())
        return out;
    std::vector<std::string> paths = rels(files);

    auto withPaths = [&](std::vector<std::string> args) {
        args.push_back("--");
        args.insert(args.end(), paths.begin(), paths.end());
        return args;
    };

    std::set<std::string> tracked;
    for (const std::string& p : splitNul(run(withPaths({"ls-files", "-z"}))))
        tracked.insert(key(p));

    struct Entry
    {
        char kind;
        std::string xy;
    };
    std::map<std::string, Entry> entries;
    std::vector<std::string> st = splitNul(run(withPaths({"status", "--porcelain=v2", "-z", "--untracked-files=all"})));
    for (size_t i = 0; i < st.size(); ++i)
    {
        const std::string& e = st[i];
        if (e.size() < 2)
            continue;
        char kind = e[0];
        // Fields before the path: ordinary 8, renamed 9 (plus the original path as the next entry), unmerged 10.
        int fields = kind == '1' ? 8 : kind == '2' ? 9 : kind == 'u' ? 10 : 1;
        size_t at = 0;
        for (int f = 0; f < fields && at != std::string::npos; ++f)
            at = e.find(' ', at + (f ? 1 : 0));
        if (at == std::string::npos)
            continue;
        std::string path = e.substr(at + 1);
        entries[key(path)] = {kind, kind == '?' || kind == '!' ? std::string() : e.substr(2, 2)};
        if (kind == '2')
            ++i;
    }

    std::set<std::string> behind;
    int code = 0;
    run({"rev-parse", "--verify", "-q", "@{u}"}, &code);
    if (code == 0)
    {
        std::string diff = run(withPaths({"diff", "--name-only", "-z", "HEAD...@{u}"}), &code);
        if (code == 0)
            for (const std::string& p : splitNul(diff))
                behind.insert(key(p));
    }

    std::map<std::string, Attrs> fileAttrs = attrs(paths);
    std::set<std::string> myLocks;
    bool anyLfs = std::any_of(fileAttrs.begin(), fileAttrs.end(), [](auto& kv) { return kv.second.lfs || kv.second.lockable; });
    if (anyLfs)
    {
        // --local reads the lock cache, so status never waits on the LFS server.
        json locks = json::parse(run({"lfs", "locks", "--local", "--json"}, &code), nullptr, false);
        if (code == 0 && locks.is_array())
            for (const json& l : locks)
                if (l.contains("path") && l["path"].is_string())
                    myLocks.insert(key(l["path"].get<std::string>()));
    }

    for (size_t i = 0; i < files.size(); ++i)
    {
        FileStatus s;
        s.path = files[i].string();
        std::string k = key(paths[i]);
        auto a = fileAttrs.find(k);
        s.exclusive = a != fileAttrs.end() && a->second.lockable;
        s.lockedByMe = myLocks.contains(k);
        s.outOfDate = behind.contains(k);

        auto it = entries.find(k);
        if (it != entries.end() && (it->second.kind == '?' || it->second.kind == '!'))
        {
            s.state = FileState::NotTracked;
        }
        else if (it != entries.end())
        {
            const std::string& xy = it->second.xy;
            s.action = xy;
            s.checkedOutByMe = true;
            if (xy.size() == 2 && xy[0] == 'A')
                s.state = FileState::Added;
            else if (xy.find('D') != std::string::npos)
                s.state = FileState::Deleted;
            else
                s.state = FileState::CheckedOut;
        }
        else if (tracked.contains(k))
        {
            s.state = s.outOfDate ? FileState::OutOfDate : FileState::Synced;
            s.checkedOutByMe = s.lockedByMe;
            if (s.lockedByMe)
                s.state = FileState::CheckedOut;
        }
        else
        {
            s.state = FileState::NotTracked;
        }
        out.push_back(std::move(s));
    }
    return out;
}

OpResult GitProvider::lfsEach(const char* verb, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    OpResult res;
    for (const fs::path& f : files)
    {
        int code = 0;
        ProcessResult r = mRunner->run(gitSpec(mSettings, mRoot, {"lfs", verb, rel(f)}));
        code = r.started ? r.exitCode : -1;
        if (code == 0)
            res.files.push_back(f.string());
        else
            res.warnings.push_back(f.string() + ": " + trim(r.err.empty() ? r.out : r.err));
    }
    return res;
}

OpResult GitProvider::checkout(const std::vector<fs::path>& files, const std::string&)
{
    LILA_ZONE();

    std::vector<std::string> paths = rels(files);
    std::map<std::string, Attrs> fileAttrs = attrs(paths);
    std::vector<fs::path> toLock;
    OpResult res;
    for (size_t i = 0; i < files.size(); ++i)
    {
        auto it = fileAttrs.find(key(paths[i]));
        if (it != fileAttrs.end() && (it->second.lfs || it->second.lockable))
            toLock.push_back(files[i]);
        else
            res.files.push_back(files[i].string());
    }
    OpResult locked = lfsEach("lock", toLock);
    res.files.insert(res.files.end(), locked.files.begin(), locked.files.end());
    res.warnings = std::move(locked.warnings);
    return res;
}

OpResult GitProvider::add(const std::vector<fs::path>& files, const std::string&)
{
    LILA_ZONE();

    OpResult res;
    if (files.empty())
        return res;
    std::vector<std::string> args{"add", "--"};
    std::vector<std::string> paths = rels(files);
    args.insert(args.end(), paths.begin(), paths.end());
    run(args);
    for (const fs::path& f : files)
        res.files.push_back(f.string());
    return res;
}

OpResult GitProvider::revert(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    OpResult res;
    std::vector<std::string> added, modified;
    std::vector<fs::path> locked;
    for (const FileStatus& s : status(files))
    {
        std::string r = rel(s.path);
        if (s.state == FileState::Added)
            added.push_back(r);
        else if (s.state == FileState::NotTracked)
            res.warnings.push_back(s.path + ": not tracked");
        else
            modified.push_back(r);
        if (s.lockedByMe)
            locked.push_back(s.path);
        if (s.state != FileState::NotTracked)
            res.files.push_back(s.path);
    }
    if (!added.empty())
    {
        std::vector<std::string> args{"rm", "--cached", "-q", "--"};
        args.insert(args.end(), added.begin(), added.end());
        run(args);
    }
    if (!modified.empty())
    {
        std::vector<std::string> args{"restore", "--source=HEAD", "--staged", "--worktree", "--"};
        args.insert(args.end(), modified.begin(), modified.end());
        run(args);
    }
    OpResult unlocked = lfsEach("unlock", locked);
    res.warnings.insert(res.warnings.end(), unlocked.warnings.begin(), unlocked.warnings.end());
    return res;
}

OpResult GitProvider::remove(const std::vector<fs::path>& files, const std::string&)
{
    LILA_ZONE();

    OpResult res;
    if (files.empty())
        return res;
    std::vector<std::string> args{"rm", "-q", "--"};
    std::vector<std::string> paths = rels(files);
    args.insert(args.end(), paths.begin(), paths.end());
    run(args);
    for (const fs::path& f : files)
        res.files.push_back(f.string());
    return res;
}

OpResult GitProvider::move(const fs::path& from, const fs::path& to, const std::string&)
{
    LILA_ZONE();

    std::vector<std::string> paths = rels({from, to});
    run({"mv", "--", paths[0], paths[1]});
    OpResult res;
    res.files = {from.string(), to.string()};
    return res;
}

OpResult GitProvider::submit(const std::string&, const std::string& description, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    if (trim(description).empty())
        throw VcsError("git: a commit needs a description");
    std::vector<std::string> args{"commit", "-q", "-m", description};
    if (!files.empty())
    {
        args.push_back("--");
        std::vector<std::string> paths = rels(files);
        args.insert(args.end(), paths.begin(), paths.end());
    }
    run(args);

    OpResult res;
    res.change = trim(run({"rev-parse", "HEAD"}));
    for (const std::string& p : splitNul(run({"diff-tree", "--no-commit-id", "--name-only", "-r", "-z", "--root", "HEAD"})))
        if (!p.empty())
            res.files.push_back(abs(p).string());
    return res;
}

OpResult GitProvider::sync(const std::vector<fs::path>& files, const fs::path&)
{
    LILA_ZONE();

    if (!files.empty())
        throw VcsError("git: sync updates the whole repository; call it without files");
    run({"pull", "--ff-only", "-q"});
    OpResult res;
    res.change = trim(run({"rev-parse", "HEAD"}));
    return res;
}

std::vector<Changelist> GitProvider::changelists(const fs::path&)
{
    LILA_ZONE();

    Changelist staged{"default", "Staged changes", "", false, {}};
    for (const std::string& p : splitNul(run({"diff", "--cached", "--name-only", "-z"})))
        if (!p.empty())
            staged.files.push_back(abs(p).string());
    return {staged};
}

std::vector<Revision> GitProvider::history(const fs::path& file, int maxCount)
{
    LILA_ZONE();

    std::string log = run({"log", "--follow", "-n", std::to_string(std::max(1, maxCount)),
                           "--format=%H%x1f%an%x1f%at%x1f%B%x1e", "--", rel(file)});
    std::vector<Revision> out;
    size_t at = 0;
    while (at < log.size())
    {
        size_t end = log.find('\x1e', at);
        if (end == std::string::npos)
            break;
        std::string rec = trim(log.substr(at, end - at));
        at = end + 1;
        std::vector<std::string> f;
        size_t s = 0;
        for (int i = 0; i < 3; ++i)
        {
            size_t e = rec.find('\x1f', s);
            if (e == std::string::npos)
                break;
            f.push_back(rec.substr(s, e - s));
            s = e + 1;
        }
        if (f.size() < 3)
            continue;
        Revision r;
        r.revision = f[0];
        r.change = f[0].substr(0, 10);
        r.user = f[1];
        r.time = f[2].empty() ? 0 : std::stoll(f[2]);
        r.description = trim(rec.substr(s));
        out.push_back(std::move(r));
    }
    return out;
}

OpResult GitProvider::lock(const std::vector<fs::path>& files)
{
    return lfsEach("lock", files);
}

OpResult GitProvider::unlock(const std::vector<fs::path>& files)
{
    return lfsEach("unlock", files);
}

void GitProvider::fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest)
{
    LILA_ZONE();

    std::string blob = run({"cat-file", "blob", revision + ":" + rel(file)});
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(blob.data(), std::streamsize(blob.size())))
        throw VcsError("git: can't write " + dest.string());
}
} // namespace lila::vcs
