#include "io/Recovery.h"

#include "ops/Command.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace lila::io::recovery
{
using json = nlohmann::json;

namespace
{
struct Live
{
    std::mutex mutex;
    fs::path file;
    int64_t startedMs = 0;
    std::vector<SessionScene> scenes;
};

Live& live()
{
    static Live l;
    return l;
}

void writeLive(Live& l)
{
    json scenes = json::array();
    for (const SessionScene& s : l.scenes)
    {
        json folders = json::array();
        for (const fs::path& f : s.folders)
            folders.push_back(f.generic_string());
        scenes.push_back({{"path", s.scene.generic_string()}, {"folders", folders}});
    }
    json doc = {{"pid", currentPid()}, {"started", l.startedMs}, {"scenes", scenes}};
    std::error_code ec;
    fs::create_directories(l.file.parent_path(), ec);
    fs::path tmp = l.file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump(2);
    }
    fs::rename(tmp, l.file, ec);
}

void noteCurrent()
{
    noteScene(currentPath(), history::autosaveFolder(currentPath()));
}

void crashSave(char* out, size_t size)
{
    fs::path p = emergencySave();
    std::string s = p.string();
    if (size)
    {
        size_t n = std::min(size - 1, s.size());
        std::memcpy(out, s.data(), n);
        out[n] = 0;
    }
}

/** Every folder an autosave of scene could be in, recorded ones first. */
std::vector<fs::path> foldersFor(const SessionScene& s)
{
    std::vector<fs::path> out = s.folders;
    auto add = [&](const fs::path& f) {
        if (std::find(out.begin(), out.end(), f) == out.end())
            out.push_back(f);
    };
    add(history::folderFor(paths::userConfigDir() / "autosave", s.scene, {}));
    if (!s.scene.empty())
        if (auto root = Workspace::detect(s.scene))
            add(history::folderFor(*root / ".lila" / "autosave", s.scene, *root));
    return out;
}
} // namespace

fs::path sessionsDir()
{
    return paths::userConfigDir() / "sessions";
}

int64_t currentPid()
{
#ifdef _WIN32
    return int64_t(GetCurrentProcessId());
#else
    return int64_t(getpid());
#endif
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool processAlive(int64_t pid, int64_t startedMs)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return false;
    DWORD code = 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    if (alive && GetProcessTimes(h, &created, &exited, &kernel, &user))
    {
        ULARGE_INTEGER t{{created.dwLowDateTime, created.dwHighDateTime}};
        // FILETIME counts 100 ns ticks from 1601.
        int64_t createdMs = int64_t(t.QuadPart / 10000) - 11644473600000LL;
        if (createdMs > startedMs + 2000)
            alive = false;
    }
    CloseHandle(h);
    return alive;
#else
    (void)startedMs;
    return kill(pid_t(pid), 0) == 0;
#endif
}

std::vector<Session> staleSessions(const fs::path& dir, int64_t selfPid, const AliveFn& alive)
{
    std::vector<Session> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json")
            continue;
        Session s;
        s.file = e.path();
        try
        {
            std::ifstream in(e.path(), std::ios::binary);
            json doc = json::parse(in);
            s.pid = doc.value("pid", int64_t(0));
            s.startedMs = doc.value("started", int64_t(0));
            for (const json& sc : doc.value("scenes", json::array()))
            {
                SessionScene scene;
                scene.scene = fs::path(sc.value("path", std::string()));
                for (const json& f : sc.value("folders", json::array()))
                    scene.folders.push_back(fs::path(f.get<std::string>()));
                s.scenes.push_back(std::move(scene));
            }
        }
        catch (...)
        {
            // Unreadable: it was mid-write when the process died.
        }
        if (s.pid != selfPid && s.pid != 0 && alive(s.pid, s.startedMs))
            continue;
        out.push_back(std::move(s));
    }
    return out;
}

void beginSession(const fs::path& dir)
{
    Live& l = live();
    {
        std::scoped_lock lock(l.mutex);
        l.file = dir / (std::to_string(currentPid()) + ".json");
        l.startedMs = nowMs();
        l.scenes.clear();
        writeLive(l);
    }
    static bool hooked = false;
    if (!hooked)
    {
        hooked = true;
        onStateChanged(noteCurrent);
        Workspace::get().onChanged(noteCurrent);
    }
    noteCurrent();
}

void noteScene(const fs::path& scene, const fs::path& folder)
{
    Live& l = live();
    std::scoped_lock lock(l.mutex);
    if (l.file.empty())
        return;
    auto it = std::find_if(l.scenes.begin(), l.scenes.end(), [&](const SessionScene& s) { return s.scene == scene; });
    if (it == l.scenes.end())
    {
        l.scenes.push_back({scene, {}});
        it = l.scenes.end() - 1;
    }
    else if (std::find(it->folders.begin(), it->folders.end(), folder) != it->folders.end())
        return;
    it->folders.push_back(folder);
    writeLive(l);
}

void endSession()
{
    Live& l = live();
    std::scoped_lock lock(l.mutex);
    if (l.file.empty())
        return;
    std::error_code ec;
    fs::remove(l.file, ec);
    l.file.clear();
}

std::vector<Candidate> findRecoverable(const std::vector<Session>& sessions)
{
    std::map<std::string, Candidate> best;
    std::error_code ec;
    for (const Session& session : sessions)
        for (const SessionScene& sc : session.scenes)
        {
            std::string stem = history::stemFor(sc.scene);
            bool saved = !sc.scene.empty() && fs::is_regular_file(sc.scene, ec);
            auto savedAt = saved ? fs::last_write_time(sc.scene, ec) : fs::file_time_type::min();
            for (const fs::path& folder : foldersFor(sc))
                for (auto [tag, kind] : {std::pair{"autosave", history::Kind::Autosave}, std::pair{"emergency", history::Kind::Emergency}})
                    for (const fs::path& f : history::entries(folder, stem, tag))
                    {
                        auto at = fs::last_write_time(f, ec);
                        if (ec || at <= savedAt)
                            continue;
                        int64_t t = history::unixTime(f);
                        // Older autosaves belong to earlier sessions that already had their chance.
                        if (session.startedMs && t * 1000 < session.startedMs - 2000)
                            continue;
                        Candidate c{sc.scene, f, kind, t, saved ? history::unixTime(sc.scene) : 0};
                        auto [it, added] = best.try_emplace(sc.scene.generic_string(), c);
                        if (!added && (c.fileTime > it->second.fileTime ||
                                       (c.fileTime == it->second.fileTime && kind == history::Kind::Emergency)))
                            it->second = c;
                    }
        }
    std::vector<Candidate> out;
    for (auto& [_, c] : best)
        out.push_back(c);
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.fileTime > b.fileTime; });
    return out;
}

void forget(const std::vector<Session>& sessions)
{
    std::error_code ec;
    for (const Session& s : sessions)
        fs::remove(s.file, ec);
}

void installCrashSave()
{
    crash::setEmergencySave(crashSave);
}

namespace
{
fs::path sidecarOf(const fs::path& file)
{
    fs::path p = file;
    p += ".json";
    return p;
}

std::optional<Recovered> readKept(const fs::path& sidecar, json* doc = nullptr)
{
    try
    {
        std::ifstream in(sidecar, std::ios::binary);
        json j = json::parse(in);
        Recovered r;
        r.file = fs::path(sidecar).replace_extension();
        r.scene = fs::path(std::u8string(reinterpret_cast<const char8_t*>(j.value("scene", std::string()).c_str())));
        r.kind = j.value("kind", std::string()) == "emergency" ? history::Kind::Emergency : history::Kind::Autosave;
        r.time = j.value("time", int64_t(0));
        if (doc)
            *doc = std::move(j);
        std::error_code ec;
        if (!fs::is_regular_file(r.file, ec))
            return std::nullopt;
        return r;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::string utf8(const fs::path& p)
{
    std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

void removeKept(const fs::path& file)
{
    std::error_code ec;
    fs::remove(file, ec);
    fs::remove(sidecarOf(file), ec);
}
} // namespace

fs::path recoveredDir()
{
    return paths::userConfigDir() / "recovered";
}

std::vector<Recovered> keep(const std::vector<Candidate>& candidates, const fs::path& dir)
{
    std::vector<Recovered> out;
    if (candidates.empty())
        return out;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<std::pair<std::string, int64_t>> kept;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        json doc;
        if (e.path().extension() == ".json" && readKept(e.path(), &doc))
            kept.emplace_back(doc.value("source", std::string()), doc.value("time", int64_t(0)));
    }
    for (const Candidate& c : candidates)
    {
        std::string source = utf8(c.file);
        if (std::find(kept.begin(), kept.end(), std::pair{source, c.fileTime}) != kept.end())
            continue;
        const char* tag = c.kind == history::Kind::Emergency ? "emergency" : "autosave";
        std::string stamp = history::timestamp(std::chrono::system_clock::time_point(std::chrono::seconds(c.fileTime)));
        fs::path target = history::uniqueName(dir, history::stemFor(c.scene), stamp, tag);
        if (!fs::copy_file(c.file, target, ec) || ec)
        {
            log::warn("could not keep recovered scene {}: {}", c.file.string(), ec.message());
            continue;
        }
        json doc = {{"scene", utf8(c.scene)}, {"source", source}, {"kind", tag}, {"time", c.fileTime}};
        std::ofstream(sidecarOf(target), std::ios::binary) << doc.dump(2);
        kept.emplace_back(source, c.fileTime);
        out.push_back({target, c.scene, c.kind, c.fileTime});
    }
    return out;
}

std::vector<Recovered> recovered(const fs::path& dir)
{
    std::vector<Recovered> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".json")
            if (auto r = readKept(e.path()))
                out.push_back(std::move(*r));
    std::sort(out.begin(), out.end(), [](const Recovered& a, const Recovered& b) {
        return a.time != b.time ? a.time > b.time : a.file.filename() > b.file.filename();
    });
    return out;
}

int prune(int days, const fs::path& dir, int64_t nowSeconds)
{
    if (days <= 0)
        return 0;
    if (nowSeconds == 0)
        nowSeconds = nowMs() / 1000;
    int removed = 0;
    for (const Recovered& r : recovered(dir))
        if (r.time < nowSeconds - int64_t(days) * 86400)
        {
            removeKept(r.file);
            ++removed;
        }
    return removed;
}

int clear(const fs::path& dir)
{
    int removed = 0;
    for (const Recovered& r : recovered(dir))
    {
        removeKept(r.file);
        ++removed;
    }
    return removed;
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "recoveredScenes",
        .doc = "Scenes recovered after a crash and kept for File > Recovered Scenes, newest first: {path, scene, kind, time}. "
               "scene is where it belongs (null when untitled), time when it was written (unix seconds).",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Recovered& r : recovered())
                out.push_back({{"path", utf8(r.file)},
                               {"scene", r.scene.empty() ? json(nullptr) : json(utf8(r.scene))},
                               {"kind", history::kindName(r.kind)},
                               {"time", r.time}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "openRecoveredScene",
        .doc = "Open a recovered scene (a path from recoveredScenes) as a new untitled scene with unsaved changes; its "
               "references resolve against the scene it came from. Saving asks for a file, so the original is never "
               "overwritten by accident. Not undoable.",
        .positionalDoc = "path",
        .flags = {{"force", "f", T::Bool, false, "Discard unsaved changes in the current scene."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("openRecoveredScene: give one path");
            if (io::modified() && !a.get<bool>("force"))
                throw CommandError("openRecoveredScene: the scene has unsaved changes; save it or pass force=True");
            const std::string& arg = a.positional[0].get_ref<const std::string&>();
            fs::path file(std::u8string(arg.begin(), arg.end()));
            std::optional<Recovered> r = readKept(sidecarOf(file));
            if (!r)
                throw CommandError("openRecoveredScene: not a recovered scene: " + arg);
            try
            {
                loadVersion(r->file, r->scene, false, "untitled (recovered)");
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return r->scene.empty() ? json(nullptr) : json(utf8(r->scene));
        },
    });

    ops::registerCommand({
        .name = "clearRecoveredScenes",
        .doc = "Delete every recovered scene kept for File > Recovered Scenes. Returns how many were deleted.",
        .undoable = false,
        .run = [](const Args&) -> json { return clear(); },
    });
}
} // namespace lila::io::recovery
