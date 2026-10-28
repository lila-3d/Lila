#include "vcs/PerforceProvider.h"

#include "core/Profile.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>

namespace lila::vcs
{
namespace
{
std::string str(const json& r, const char* key)
{
    auto it = r.find(key);
    if (it == r.end())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number())
        return it->dump();
    return {};
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i]))
        ++i;
    return s.substr(i);
}

bool isDepotSyntax(const std::string& s)
{
    return s.size() > 2 && s[0] == '/' && s[1] == '/' && s.find('\\') == std::string::npos;
}

/** Comparable form of a local path; p4 hands back backslashes and its own casing. */
std::string pathKey(const std::string& p)
{
    if (isDepotSyntax(p))
        return lower(p);
    std::string s = fs::path(p).lexically_normal().generic_string();
#ifdef _WIN32
    s = lower(s);
#endif
    return s;
}

fs::path cwdFor(const std::vector<fs::path>& files)
{
    for (const fs::path& f : files)
    {
        if (isDepotSyntax(f.generic_string()))
            continue;
        fs::path dir = f.parent_path();
        std::error_code ec;
        while (!dir.empty() && !fs::is_directory(dir, ec))
        {
            if (dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        if (fs::is_directory(dir, ec))
            return dir;
    }
    return {};
}

std::string argList(const std::vector<fs::path>& files, bool escape = true)
{
    std::string s;
    for (const fs::path& f : files)
        s += (escape ? PerforceProvider::fileArg(f) : f.string()) + "\n";
    return s;
}

/** Values of key0, key1, ... in a tagged record. */
std::vector<std::string> indexed(const json& r, const std::string& key)
{
    std::vector<std::string> out;
    for (int i = 0;; ++i)
    {
        std::string v = str(r, (key + std::to_string(i)).c_str());
        if (v.empty())
            break;
        out.push_back(v);
    }
    return out;
}

bool isNumber(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}
} // namespace

PerforceSettings PerforceSettings::fromJson(const json& j)
{
    PerforceSettings s;
    if (!j.is_object())
        return s;
    auto get = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    s.exe = get("exe");
    s.port = get("port");
    s.user = get("user");
    s.client = get("client");
    s.charset = get("charset");
    if (j.contains("timeoutMs") && j["timeoutMs"].is_number_integer())
        s.timeoutMs = j["timeoutMs"].get<int>();
    return s;
}

json PerforceSettings::toJson() const
{
    return {{"exe", exe}, {"port", port}, {"user", user}, {"client", client}, {"charset", charset}, {"timeoutMs", timeoutMs}};
}

PerforceProvider::PerforceProvider(PerforceSettings settings, std::shared_ptr<ProcessRunner> runner)
    : mSettings(std::move(settings)), mRunner(std::move(runner))
{
}

std::string PerforceProvider::exe() const
{
    if (!mSettings.exe.empty())
        return mSettings.exe;
    if (fs::path p = findExecutable("p4"); !p.empty())
        return p.string();
#ifdef _WIN32
    std::error_code ec;
    if (fs::path p = "C:/Program Files/Perforce/p4.exe"; fs::is_regular_file(p, ec))
        return p.string();
#endif
    return "p4";
}

PerforceProvider::Output PerforceProvider::parse(const std::string& text)
{
    LILA_ZONE();

    Output o;
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        std::string_view line(text.data() + at, (end == std::string::npos ? text.size() : end) - at);
        at = end == std::string::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            o.raw.append(line);
            o.raw += '\n';
            continue;
        }
        if (j.contains("severity") && j.contains("data") && j.size() <= 4)
        {
            Message m;
            m.severity = std::stoi(str(j, "severity").empty() ? "0" : str(j, "severity"));
            m.generic = std::stoi(str(j, "generic").empty() ? "0" : str(j, "generic"));
            m.text = trim(str(j, "data"));
            o.messages.push_back(std::move(m));
            continue;
        }
        o.records.push_back(std::move(j));
    }
    return o;
}

std::string PerforceProvider::describeError(const std::string& text, const PerforceSettings& s)
{
    std::string t = trim(text);
    std::string l = lower(t);
    if (l.find("p4passwd") != std::string::npos || l.find("please login") != std::string::npos ||
        l.find("session has expired") != std::string::npos || l.find("password invalid") != std::string::npos)
    {
        std::string who = s.user.empty() ? std::string() : " as '" + s.user + "'";
        return "Perforce: not logged in" + who + ". Run 'p4 login' in a terminal, then try again.";
    }
    if ((l.find("client '") != std::string::npos && l.find("unknown") != std::string::npos) ||
        l.find("must create client") != std::string::npos)
    {
        std::smatch m;
        std::string client = s.client;
        if (std::regex_search(t, m, std::regex("client '([^']+)'", std::regex::icase)))
            client = m[1];
        return "Perforce: no such client workspace '" + client +
               "'. Set P4CLIENT (or \"perforce\": {\"client\": ...} in .lila/sourceControl.json) to an existing workspace.";
    }
    if (l.find("connect to server failed") != std::string::npos || l.find("tcp connect to") != std::string::npos)
    {
        std::string where = s.port.empty() ? std::string() : " '" + s.port + "'";
        return "Perforce: can't reach the server" + where + ". Check P4PORT. (" + t + ")";
    }
    return "Perforce: " + t;
}

std::string PerforceProvider::fileArg(const fs::path& file)
{
    std::string s = file.string();
    if (isDepotSyntax(file.generic_string()))
        return file.generic_string();
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '%': out += "%25"; break;
        case '@': out += "%40"; break;
        case '#': out += "%23"; break;
        case '*': out += "%2A"; break;
        default: out += c;
        }
    }
    return out;
}

PerforceProvider::Output PerforceProvider::run(const std::vector<std::string>& args, const fs::path& cwd,
                                               const std::string& input, bool tagged, int timeoutMs)
{
    LILA_ZONE();

    ProcessSpec spec;
    spec.argv.push_back(exe());
    if (tagged)
    {
        spec.argv.push_back("-ztag");
        spec.argv.push_back("-Mj");
    }
    spec.argv.push_back("-zprog=Lila");
    auto global = [&](const char* flag, const std::string& v) {
        if (!v.empty())
        {
            spec.argv.push_back(flag);
            spec.argv.push_back(v);
        }
    };
    global("-p", mSettings.port);
    global("-u", mSettings.user);
    global("-c", mSettings.client);
    global("-C", mSettings.charset);
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.cwd = cwd;
    spec.input = input;
    spec.timeoutMs = timeoutMs ? timeoutMs : mSettings.timeoutMs;

    ProcessResult r = mRunner->run(spec);
    if (!r.started)
        throw VcsError("Perforce: p4 is not installed or not on PATH (" + trim(r.err) + ")");
    if (r.timedOut)
        throw VcsError("Perforce: p4 " + (args.empty() ? std::string() : args.back()) + " timed out");

    Output o = parse(r.out);
    if (!tagged)
        o.raw = r.out;
    if (!trim(r.err).empty())
        o.messages.push_back({3, 0, trim(r.err)});

    // A failure with nothing to show for it fails the call; per-file failures among successes become warnings.
    const Message* failure = nullptr;
    for (const Message& m : o.messages)
        if (m.severity >= 3 && !failure)
            failure = &m;
    if (failure && o.records.empty())
        throw VcsError(describeError(failure->text, mSettings));
    if (r.exitCode != 0 && o.records.empty() && o.messages.empty() && (!tagged || o.raw.empty()))
        throw VcsError("Perforce: p4 exited with code " + std::to_string(r.exitCode));
    return o;
}

json PerforceProvider::info()
{
    LILA_ZONE();

    json j = {{"provider", name()}, {"settings", mSettings.toJson()}};
    try
    {
        Output o = run({"info"}, {}, {}, true, 15000);
        if (!o.records.empty())
            j["server"] = o.records.front();
    }
    catch (const VcsError& e)
    {
        j["error"] = e.what();
    }
    std::scoped_lock lock(mMutex);
    if (!mLastError.empty())
        j["lastError"] = mLastError;
    return j;
}

bool PerforceProvider::manages(const fs::path& dir)
{
    LILA_ZONE();

    try
    {
        Output o = run({"where", fileArg(dir / "...")}, dir, {}, true, 15000);
        for (const json& r : o.records)
            if (r.contains("path") && !r.contains("unmap"))
                return true;
    }
    catch (const VcsError& e)
    {
        std::scoped_lock lock(mMutex);
        mLastError = e.what();
    }
    return false;
}

FileStatus PerforceProvider::statusFromFstat(const json& r)
{
    FileStatus s;
    s.path = str(r, "clientFile");
    s.depotPath = str(r, "depotFile");
    s.action = str(r, "action");
    s.changelist = str(r, "change");
    s.haveRev = str(r, "haveRev");
    s.headRev = str(r, "headRev");
    s.fileType = str(r, "type").empty() ? str(r, "headType") : str(r, "type");
    if (size_t plus = s.fileType.find('+'); plus != std::string::npos)
        s.exclusive = s.fileType.find('l', plus) != std::string::npos;
    s.lockedByMe = r.contains("ourLock");
    s.otherUsers = indexed(r, "otherOpen");
    s.lockedByOther = r.contains("otherLock") || !indexed(r, "otherLock").empty();
    s.checkedOutByMe = !s.action.empty();

    std::string headAction = str(r, "headAction");
    bool headDeleted = headAction == "delete" || headAction == "move/delete";
    s.outOfDate = !s.headRev.empty() && !headDeleted && s.haveRev != s.headRev;

    if (s.action == "add" || s.action == "move/add" || s.action == "branch")
        s.state = FileState::Added;
    else if (s.action == "delete" || s.action == "move/delete")
        s.state = FileState::Deleted;
    else if (!s.action.empty())
        s.state = FileState::CheckedOut;
    else if (s.lockedByOther || (s.exclusive && !s.otherUsers.empty()))
        s.state = FileState::Locked;
    else if (!s.otherUsers.empty())
        s.state = FileState::CheckedOutOther;
    else if (headDeleted && s.haveRev.empty())
        s.state = FileState::NotTracked;
    else if (s.outOfDate)
        s.state = FileState::OutOfDate;
    else
        s.state = FileState::Synced;
    return s;
}

std::vector<FileStatus> PerforceProvider::status(const std::vector<fs::path>& files)
{
    LILA_ZONE();

    if (files.empty())
        return {};
    Output o = run({"-x", "-", "fstat"}, cwdFor(files), argList(files));

    std::map<std::string, const json*> byPath;
    for (const json& r : o.records)
    {
        if (std::string c = str(r, "clientFile"); !c.empty())
            byPath[pathKey(c)] = &r;
        if (std::string d = str(r, "depotFile"); !d.empty())
            byPath[pathKey(d)] = &r;
    }

    std::vector<FileStatus> out;
    for (const fs::path& f : files)
    {
        FileStatus s;
        if (auto it = byPath.find(pathKey(f.string())); it != byPath.end())
            s = statusFromFstat(*it->second);
        else
            s.state = FileState::NotTracked;
        s.path = f.string();
        out.push_back(std::move(s));
    }
    return out;
}

OpResult PerforceProvider::fileOp(const std::string& cmd, const std::vector<std::string>& extra, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    OpResult res;
    if (files.empty())
        return res;
    std::vector<std::string> args{"-x", "-", cmd};
    args.insert(args.end(), extra.begin(), extra.end());
    // add -f takes names literally, so they must not be escaped.
    bool literal = cmd == "add";
    Output o = run(args, cwdFor(files), argList(files, !literal));
    for (const json& r : o.records)
    {
        std::string f = str(r, "clientFile");
        if (f.empty())
            f = str(r, "depotFile");
        if (!f.empty())
            res.files.push_back(f);
    }
    for (const Message& m : o.messages)
        if (m.severity >= 2)
            res.warnings.push_back(m.text);
    return res;
}

namespace
{
std::vector<std::string> changeArg(const std::string& changelist)
{
    if (changelist.empty() || changelist == "default")
        return {};
    return {"-c", changelist};
}
} // namespace

OpResult PerforceProvider::checkout(const std::vector<fs::path>& files, const std::string& changelist)
{
    return fileOp("edit", changeArg(changelist), files);
}

OpResult PerforceProvider::add(const std::vector<fs::path>& files, const std::string& changelist)
{
    std::vector<std::string> extra{"-f"};
    for (const std::string& a : changeArg(changelist))
        extra.push_back(a);
    return fileOp("add", extra, files);
}

OpResult PerforceProvider::revert(const std::vector<fs::path>& files)
{
    return fileOp("revert", {}, files);
}

OpResult PerforceProvider::remove(const std::vector<fs::path>& files, const std::string& changelist)
{
    return fileOp("delete", changeArg(changelist), files);
}

OpResult PerforceProvider::move(const fs::path& from, const fs::path& to, const std::string& changelist)
{
    LILA_ZONE();

    // p4 move only moves files opened for edit; a folder moves as from/... to to/...
    const bool folder = fs::is_directory(from);
    const fs::path src = folder ? from / "..." : from;
    const fs::path dst = folder ? to / "..." : to;
    const fs::path cwd = cwdFor({from});
    std::vector<std::string> edit{"edit"};
    for (const std::string& a : changeArg(changelist))
        edit.push_back(a);
    edit.push_back(folder ? src.string() : fileArg(src));
    run(edit, cwd);
    std::vector<std::string> args{"move"};
    for (const std::string& a : changeArg(changelist))
        args.push_back(a);
    args.push_back(folder ? src.string() : fileArg(src));
    args.push_back(folder ? dst.string() : fileArg(dst));
    Output o = run(args, cwd);
    OpResult res;
    for (const json& r : o.records)
        if (std::string f = str(r, "clientFile"); !f.empty())
            res.files.push_back(f);
    for (const Message& m : o.messages)
        if (m.severity >= 2)
            res.warnings.push_back(m.text);
    return res;
}

OpResult PerforceProvider::lock(const std::vector<fs::path>& files)
{
    return fileOp("lock", {}, files);
}

OpResult PerforceProvider::unlock(const std::vector<fs::path>& files)
{
    return fileOp("unlock", {}, files);
}

OpResult PerforceProvider::moveToChangelist(const std::vector<fs::path>& files, const std::string& changelist)
{
    return fileOp("reopen", {"-c", changelist.empty() ? std::string("default") : changelist}, files);
}

OpResult PerforceProvider::submit(const std::string& changelist, const std::string& description, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    fs::path cwd = cwdFor(files);
    bool numbered = isNumber(changelist);
    std::vector<std::string> args{"submit"};
    if (numbered)
    {
        if (!description.empty())
        {
            Output spec = run({"--field", "Description=" + description, "change", "-o", changelist}, cwd, {}, false);
            run({"change", "-i"}, cwd, spec.raw);
        }
        args.insert(args.end(), {"-c", changelist});
    }
    else
    {
        if (trim(description).empty())
            throw VcsError("Perforce: submitting the default changelist needs a description");
        args.insert(args.end(), {"-d", description});
        for (const fs::path& f : files)
            args.push_back(fileArg(f));
    }

    Output o = run(args, cwd);
    OpResult res;
    for (const json& r : o.records)
    {
        if (std::string c = str(r, "submittedChange"); !c.empty())
            res.change = c;
        else if (std::string d = str(r, "depotFile"); !d.empty())
            res.files.push_back(d);
    }
    for (const Message& m : o.messages)
        if (m.severity >= 2)
            res.warnings.push_back(m.text);
    if (res.change.empty())
        throw VcsError(o.messages.empty() ? "Perforce: submit failed" : describeError(o.messages.back().text, mSettings));
    return res;
}

OpResult PerforceProvider::sync(const std::vector<fs::path>& files, const fs::path& context)
{
    LILA_ZONE();

    Output o = files.empty() ? run({"-q", "sync"}, context) : run({"-q", "-x", "-", "sync"}, cwdFor(files), argList(files));
    OpResult res;
    for (const json& r : o.records)
        if (std::string f = str(r, "clientFile"); !f.empty())
            res.files.push_back(f);
    for (const Message& m : o.messages)
        if (m.severity >= 2)
            res.warnings.push_back(m.text);
    return res;
}

std::string PerforceProvider::clientName(const fs::path& cwd)
{
    if (!mSettings.client.empty())
        return mSettings.client;
    Output o = run({"info"}, cwd);
    std::string c = o.records.empty() ? std::string() : str(o.records.front(), "clientName");
    if (c.empty() || c == "*unknown*")
        throw VcsError(describeError("Client '" + c + "' unknown", mSettings));
    return c;
}

std::vector<Changelist> PerforceProvider::changelists(const fs::path& context)
{
    LILA_ZONE();

    std::string client = clientName(context);
    std::vector<Changelist> out;
    out.push_back({"default", "", "", false, {}});
    Output changes = run({"changes", "-l", "-s", "pending", "-c", client}, context);
    for (const json& r : changes.records)
    {
        Changelist c;
        c.id = str(r, "change");
        c.description = trim(str(r, "desc"));
        c.user = str(r, "user");
        c.shelved = r.contains("shelved");
        out.push_back(std::move(c));
    }

    Output opened = run({"opened", "-C", client}, context);
    for (const json& r : opened.records)
    {
        std::string change = str(r, "change");
        auto it = std::find_if(out.begin(), out.end(), [&](const Changelist& c) { return c.id == change; });
        if (it == out.end())
            it = out.insert(out.end(), Changelist{change, "", str(r, "user"), false, {}});
        it->files.push_back(str(r, "depotFile"));
    }
    return out;
}

std::string PerforceProvider::createChangelist(const std::string& description, const fs::path& context)
{
    LILA_ZONE();

    std::string desc = trim(description);
    if (desc.empty())
        throw VcsError("Perforce: a new changelist needs a description");
    std::string spec = "Change:\tnew\n\nDescription:\n";
    size_t at = 0;
    while (at <= desc.size())
    {
        size_t end = desc.find('\n', at);
        spec += "\t" + desc.substr(at, end == std::string::npos ? std::string::npos : end - at) + "\n";
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    Output o = run({"change", "-i"}, context, spec);

    static const std::regex created("Change (\\d+) created");
    std::smatch m;
    for (const Message& msg : o.messages)
        if (std::regex_search(msg.text, m, created))
            return m[1];
    for (const json& r : o.records)
    {
        if (std::string c = str(r, "change"); isNumber(c))
            return c;
        for (auto& [k, v] : r.items())
        {
            std::string s = v.is_string() ? v.get<std::string>() : std::string();
            if (std::regex_search(s, m, created))
                return m[1];
        }
    }
    if (std::regex_search(o.raw, m, created))
        return m[1];
    throw VcsError("Perforce: could not read the new changelist number");
}

OpResult PerforceProvider::shelve(const std::string& changelist, const std::vector<fs::path>& files)
{
    LILA_ZONE();

    if (!isNumber(changelist))
        throw VcsError("Perforce: shelving needs a numbered changelist; create one and move the files into it first");
    std::vector<std::string> args{"shelve", "-f", "-c", changelist};
    for (const fs::path& f : files)
        args.push_back(fileArg(f));
    Output o = run(args, cwdFor(files));
    OpResult res;
    res.change = changelist;
    for (const json& r : o.records)
        if (std::string d = str(r, "depotFile"); !d.empty())
            res.files.push_back(d);
    for (const Message& m : o.messages)
        if (m.severity >= 2)
            res.warnings.push_back(m.text);
    return res;
}

OpResult PerforceProvider::unshelve(const std::string& changelist)
{
    LILA_ZONE();

    if (!isNumber(changelist))
        throw VcsError("Perforce: unshelve needs a numbered changelist");
    Output o = run({"unshelve", "-s", changelist, "-c", changelist}, {});
    OpResult res;
    res.change = changelist;
    for (const json& r : o.records)
        if (std::string d = str(r, "depotFile"); !d.empty())
            res.files.push_back(d);
    for (const Message& m : o.messages)
        if (m.severity >= 2)
            res.warnings.push_back(m.text);
    return res;
}

std::vector<Revision> PerforceProvider::parseFilelog(const json& r)
{
    std::vector<Revision> out;
    std::vector<std::string> revs = indexed(r, "rev");
    for (size_t i = 0; i < revs.size(); ++i)
    {
        std::string n = std::to_string(i);
        Revision v;
        v.revision = revs[i];
        v.change = str(r, ("change" + n).c_str());
        v.action = str(r, ("action" + n).c_str());
        v.user = str(r, ("user" + n).c_str());
        v.description = trim(str(r, ("desc" + n).c_str()));
        std::string t = str(r, ("time" + n).c_str());
        v.time = isNumber(t) ? std::stoll(t) : 0;
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<Revision> PerforceProvider::history(const fs::path& file, int maxCount)
{
    LILA_ZONE();

    Output o = run({"filelog", "-l", "-m", std::to_string(std::max(1, maxCount)), fileArg(file)}, cwdFor({file}));
    std::vector<Revision> out;
    for (const json& r : o.records)
    {
        std::vector<Revision> revs = parseFilelog(r);
        out.insert(out.end(), revs.begin(), revs.end());
    }
    return out;
}

void PerforceProvider::fetchRevision(const fs::path& file, const std::string& revision, const fs::path& dest)
{
    LILA_ZONE();

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    run({"print", "-q", "-o", dest.string(), fileArg(file) + "#" + revision}, cwdFor({file}));
    if (!fs::exists(dest, ec))
        throw VcsError("Perforce: could not fetch " + file.filename().string() + "#" + revision);
    fs::permissions(dest, fs::perms::owner_write, fs::perm_options::add, ec);
}
} // namespace lila::vcs
