// Seeing terrains at a glance: terrainMap writes a shaded, tinted top-down map of a terrain, and terrainWedge builds
// variants of a spec (seed, age, preset, any key) in parallel lilabatch processes and lays their maps out as a
// labelled contact sheet with each variant's stats.

#include "Spec.h"
#include "Stamps.h"
#include "Wedge.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace lila;

namespace terrain
{
namespace wedge_
{
using json = nlohmann::json;
using T = ArgType;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------- 5 x 7 font

struct Glyph
{
    char c;
    uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}}, {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}}, {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}}, {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}, {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}}, {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
};

/** An RGBA float image the map and sheet are drawn into. */
struct Canvas
{
    int w = 0, h = 0;
    std::vector<float> px;

    Canvas(int width, int height, glm::vec3 fill) : w(width), h(height), px(size_t(width) * size_t(height) * 4, 1.0f)
    {
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
            for (int c = 0; c < 3; ++c)
                px[i * 4 + size_t(c)] = fill[c];
    }
    void set(int x, int y, glm::vec3 c)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        float* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = 1.0f;
    }
    /** Text in the 5 x 7 font, scale pixels per dot; returns the width drawn. Letters are drawn as capitals. */
    int text(int x, int y, const std::string& s, glm::vec3 c, int scale)
    {
        int at = x;
        for (char ch : s)
        {
            const char up = char(std::toupper(static_cast<unsigned char>(ch)));
            for (const Glyph& g : kGlyphs)
                if (g.c == up)
                    for (int r = 0; r < 7; ++r)
                        for (int b = 0; b < 5; ++b)
                            if (g.rows[r] & (0x10 >> b))
                                for (int sy = 0; sy < scale; ++sy)
                                    for (int sx = 0; sx < scale; ++sx)
                                        set(at + b * scale + sx, y + r * scale + sy, c);
            at += 6 * scale;
        }
        return at - x;
    }
};

glm::vec3 hypsometric(double t)
{
    static const std::pair<double, glm::vec3> stops[] = {{0.0, {0.27f, 0.45f, 0.27f}}, {0.3, {0.55f, 0.62f, 0.36f}}, {0.55, {0.62f, 0.52f, 0.38f}},
                                                         {0.78, {0.55f, 0.52f, 0.50f}}, {1.0, {0.96f, 0.96f, 0.97f}}};
    t = std::clamp(t, 0.0, 1.0);
    for (size_t i = 1; i < std::size(stops); ++i)
        if (t <= stops[i].first)
        {
            const double f = (t - stops[i - 1].first) / (stops[i].first - stops[i - 1].first);
            return glm::mix(stops[i - 1].second, stops[i].second, float(f));
        }
    return stops[std::size(stops) - 1].second;
}

double maskOr(const Heightfield& h, const char* name, double x, double z)
{
    return h.findMask(name) ? h.maskAt(name, x, z) : 0.0;
}

/** The terrain seen from above, -Z at the top: height tint, light from the north-west, water and roads over it. */
Canvas mapOf(const Heightfield& h, int width, double shade, bool water, bool roads)
{
    LILA_ZONE();

    const glm::dvec2 e = h.extent();
    const int height = std::max(1, int(std::lround(width * e.y / std::max(e.x, 1e-9))));
    Canvas c(width, height, {0, 0, 0});
    const glm::dvec2 r = h.range();
    const double span = std::max(r.y - r.x, 1e-6);
    const glm::dvec3 sun = glm::normalize(glm::dvec3(-1.0, 1.4, -1.0));
    tbb::parallel_for(0, height, [&](int py) {
        for (int px = 0; px < width; ++px)
        {
            const double x = h.origin.x + e.x * (width > 1 ? double(px) / (width - 1) : 0.5);
            const double z = h.origin.y + e.y * (height > 1 ? double(py) / (height - 1) : 0.5);
            glm::vec3 col = hypsometric((h.heightAt(x, z) - r.x) / span);
            const double lambert = std::max(0.0, glm::dot(h.normalAt(x, z), sun));
            col *= float(1.0 - shade + shade * std::min(1.3, lambert * 1.25));
            if (water)
            {
                const double w = std::max({maskOr(h, "water", x, z), maskOr(h, "sea", x, z), maskOr(h, "riverBed", x, z), maskOr(h, "lakeBed", x, z)});
                col = glm::mix(col, glm::vec3(0.18f, 0.36f, 0.62f), float(std::clamp(w * 1.5, 0.0, 1.0)));
            }
            if (roads)
                col = glm::mix(col, glm::vec3(0.22f, 0.21f, 0.2f), float(std::clamp(maskOr(h, "roadSurface", x, z) * 1.5, 0.0, 1.0)));
            c.set(px, py, glm::clamp(col, glm::vec3(0.0f), glm::vec3(1.0f)));
        }
    });
    return c;
}

void writeCanvas(const fs::path& file, const Canvas& c, const std::string& cmd)
{
    try
    {
        if (file.has_parent_path())
            fs::create_directories(file.parent_path());
        image::write(file, image::Image{c.w, c.h, c.px}, 8);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": writing " + file.generic_string() + " failed: " + e.what());
    }
}

// ---------------------------------------------------------------------------------------------------- workers

const char* kWorkerScript = R"(# Builds one terrainWedge variant in its own lilabatch process.
import json, sys
from lila import ops

spec_file, image, out_file, tile = sys.argv[-4:]
with open(spec_file) as f:
    spec = json.load(f)
res = {}
try:
    r = ops.terrainSpec(spec=spec, replace=True)
    res = {'ok': r['ok'], 'revision': r['revision'], 'stats': r['stats'], 'check': r.get('check'), 'failed': r.get('failed'),
           'steps': [{'step': s['step'], 'ok': s['ok'], 'ms': s['ms'], 'error': s.get('error')} for s in r['steps']]}
    ops.terrainMap(r['terrain'], file=image, resolution=int(tile))
    res['image'] = image
except Exception as e:
    res = {'ok': False, 'error': str(e)}
with open(out_file, 'w') as f:
    json.dump(res, f)
)";

fs::path lilabatchPath()
{
#ifdef _WIN32
    return paths::executableDir() / "lilabatch.exe";
#else
    return paths::executableDir() / "lilabatch";
#endif
}

/** One lilabatch process, its output in a log, ended when it is destroyed. */
class Worker
{
public:
    Worker(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log, std::string* error)
    {
#ifdef _WIN32
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        for (const std::string& a : args)
            cmd += L" \"" + fs::path(a).wstring() + L"\"";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
        si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                       nullptr, nullptr, &si, &pi);
        if (out != INVALID_HANDLE_VALUE)
            CloseHandle(out);
        if (!ok)
        {
            *error = "cannot start " + exe.generic_string() + " (error " + std::to_string(GetLastError()) + ")";
            return;
        }
        mJob = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(mJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(mJob, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        mProcess = pi.hProcess;
#else
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        std::vector<std::string> all = {exe.string()};
        all.insert(all.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (std::string& a : all)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        if (posix_spawn(&mPid, exe.c_str(), &fa, nullptr, argv.data(), environ) != 0)
        {
            *error = "cannot start " + exe.generic_string();
            mPid = 0;
        }
        posix_spawn_file_actions_destroy(&fa);
#endif
    }
    ~Worker() { kill(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool started() const
    {
#ifdef _WIN32
        return mProcess != nullptr;
#else
        return mPid != 0;
#endif
    }
    /** True once it has ended. */
    bool done()
    {
#ifdef _WIN32
        return !mProcess || WaitForSingleObject(mProcess, 0) == WAIT_OBJECT_0;
#else
        if (mPid == 0 || mExited)
            return true;
        int status = 0;
        mExited = waitpid(mPid, &status, WNOHANG) == mPid;
        return mExited;
#endif
    }
    void kill()
    {
#ifdef _WIN32
        if (mJob)
            CloseHandle(mJob);
        if (mProcess)
            CloseHandle(mProcess);
        mJob = mProcess = nullptr;
#else
        if (mPid != 0 && !mExited)
        {
            ::kill(mPid, SIGKILL);
            int status = 0;
            waitpid(mPid, &status, 0);
        }
        mPid = 0;
#endif
    }

private:
#ifdef _WIN32
    HANDLE mProcess = nullptr;
    HANDLE mJob = nullptr;
#else
    pid_t mPid = 0;
    bool mExited = false;
#endif
};

json readJson(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return nullptr;
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    return j.is_discarded() ? json() : j;
}

void writeText(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary);
    out << text;
}

/** Sets a dotted key ("erosion.age") in a spec. */
void setPath(json& spec, const std::string& key, const json& value)
{
    json* at = &spec;
    size_t from = 0;
    for (size_t dot = key.find('.'); dot != std::string::npos; dot = key.find('.', from))
    {
        const std::string part = key.substr(from, dot - from);
        if (!at->contains(part) || !(*at)[part].is_object())
            (*at)[part] = json::object();
        at = &(*at)[part];
        from = dot + 1;
    }
    (*at)[key.substr(from)] = value;
}

std::string shortValue(const json& v)
{
    if (v.is_number_float())
    {
        std::ostringstream ss;
        ss << std::round(v.get<double>() * 1000.0) / 1000.0;
        return ss.str();
    }
    return v.is_string() ? v.get<std::string>() : v.dump();
}

/** The variants' settings: each vary key cycled over count, or every combination with grid. */
std::vector<json> variantSettings(const json& vary, int count, bool grid, int baseSeed)
{
    std::vector<json> out;
    if (vary.is_null() || vary.empty())
    {
        for (int k = 0; k < count; ++k)
            out.push_back({{"seed", baseSeed + k}});
        return out;
    }
    if (!vary.is_object())
        throw CommandError("terrainWedge: vary is {key: [values]}, keys dotted for nested ones ('erosion.age')");
    for (const auto& [k, v] : vary.items())
        if (!v.is_array() || v.empty())
            throw CommandError("terrainWedge: vary." + k + " is a list of values");
    if (grid)
    {
        out.push_back(json::object());
        for (const auto& [k, values] : vary.items())
        {
            std::vector<json> next;
            for (const json& base : out)
                for (const json& v : values)
                {
                    json one = base;
                    one[k] = v;
                    next.push_back(one);
                }
            out = std::move(next);
        }
        return out;
    }
    for (int i = 0; i < count; ++i)
    {
        json one = json::object();
        for (const auto& [k, values] : vary.items())
            one[k] = values[size_t(i) % values.size()];
        out.push_back(one);
    }
    return out;
}

std::string labelOf(int index, const json& settings)
{
    std::string s = std::to_string(index);
    for (const auto& [k, v] : settings.items())
    {
        const size_t dot = k.rfind('.');
        s += " " + (dot == std::string::npos ? k : k.substr(dot + 1)) + "=" + shortValue(v);
    }
    return s;
}

json runWedge(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainWedge";
    json base = a.has("preset") ? terrainSpecPreset(a.get<std::string>("preset", "")) : json::object();
    if (a.has("spec"))
        base = mergeSpec(base, a.flags["spec"]);
    if (!a.has("spec") && !a.has("preset"))
    {
        const std::vector<std::string> objects = a.objects();
        if (objects.size() != 1)
            throw CommandError(cmd + ": give spec=, preset= or one terrain to wedge from");
        base = readTerrainSpec(objects[0])["spec"];
        if (base.contains("graph"))
            throw CommandError(cmd + ": " + objects[0] + " was not built from a spec (or changed since); wedge a spec instead");
    }
    if (a.has("overrides"))
        base = mergeSpec(base, a.flags["overrides"]);
    if (a.has("resolution"))
        base["resolution"] = a.get<int64_t>("resolution", 257);
    base.erase("export");

    const int count = int(std::clamp<int64_t>(a.get<int64_t>("count", 4), 1, 64));
    std::vector<json> settings = variantSettings(a.has("vary") ? a.flags["vary"] : json(), count, a.get<bool>("grid", false), base.value("seed", 0));
    if (settings.size() > 64)
        throw CommandError(cmd + ": " + std::to_string(settings.size()) + " variants; 64 at most");
    const std::string stem = base.value("name", std::string("Terrain"));
    fs::path folder = a.has("folder") ? fs::path(a.get<std::string>("folder", "")) : fs::temp_directory_path() / "lila_terrain_wedge" / stem;
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec)
        throw CommandError(cmd + ": cannot make " + folder.generic_string() + ": " + ec.message());
    const int tile = int(std::clamp<int64_t>(a.get<int64_t>("tile", 256), 64, 2048));
    const double timeout = std::max(10.0, a.get<double>("timeout", 900.0));

    std::vector<json> specs;
    for (size_t k = 0; k < settings.size(); ++k)
    {
        json s = base;
        for (const auto& [key, v] : settings[k].items())
            setPath(s, key, v);
        s["name"] = stem + "_wedge" + std::to_string(k);
        specs.push_back(std::move(s));
    }

    std::vector<json> results(specs.size());
    std::vector<fs::path> images(specs.size());
    for (size_t k = 0; k < specs.size(); ++k)
        images[k] = folder / ("variant" + std::to_string(k) + ".png");

    // Parallel lilabatch processes unless asked not to, or unless there is no lilabatch to run.
    std::string mode = "parallel", why;
    const fs::path exe = lilabatchPath();
    if (!a.get<bool>("parallel", true))
    {
        mode = "inProcess";
        why = "parallel=False";
    }
    else if (!fs::exists(exe, ec))
    {
        mode = "inProcess";
        why = "no lilabatch next to " + paths::executableDir().generic_string() + ", so the variants were built here one after another";
        log::warn("{}: {}", cmd, why);
    }
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const int workers = mode == "parallel" ? int(std::clamp<int64_t>(a.get<int64_t>("workers", int64_t(std::min<unsigned>(8, hw / 2))), 1, 32)) : 1;

    const auto t0 = std::chrono::steady_clock::now();
    if (mode == "parallel")
    {
        const fs::path script = folder / "wedge_worker.py";
        writeText(script, kWorkerScript);
        std::vector<std::unique_ptr<Worker>> running(specs.size());
        std::vector<double> started(specs.size(), 0.0);
        size_t next = 0, finished = 0;
        auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        std::vector<uint8_t> done(specs.size(), 0);
        while (finished < specs.size())
        {
            int active = 0;
            for (size_t k = 0; k < specs.size(); ++k)
                if (running[k] && !done[k])
                    ++active;
            while (active < workers && next < specs.size())
            {
                const size_t k = next++;
                const fs::path specFile = folder / ("variant" + std::to_string(k) + ".json");
                const fs::path out = folder / ("variant" + std::to_string(k) + "_result.json");
                fs::remove(out, ec);
                writeText(specFile, specs[k].dump(2));
                std::string err;
                running[k] = std::make_unique<Worker>(exe, std::vector<std::string>{"-b", script.string(), specFile.string(), images[k].string(), out.string(), std::to_string(tile)},
                                                      folder / ("variant" + std::to_string(k) + ".log"), &err);
                started[k] = now();
                if (!running[k]->started())
                {
                    results[k] = {{"ok", false}, {"error", err}};
                    done[k] = 1;
                    ++finished;
                    continue;
                }
                ++active;
            }
            for (size_t k = 0; k < specs.size(); ++k)
            {
                if (!running[k] || done[k])
                    continue;
                const bool ended = running[k]->done();
                if (!ended && now() - started[k] < timeout)
                    continue;
                running[k]->kill();
                done[k] = 1;
                ++finished;
                json r = readJson(folder / ("variant" + std::to_string(k) + "_result.json"));
                if (!ended)
                    r = {{"ok", false}, {"error", "timed out after " + std::to_string(int(timeout)) + " s"}};
                else if (r.is_null())
                    r = {{"ok", false}, {"error", "the worker ended without a result; see " + (folder / ("variant" + std::to_string(k) + ".log")).generic_string()}};
                r["seconds"] = now() - started[k];
                results[k] = r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    else
    {
        scene::Graph& g = scene::Graph::current();
        for (size_t k = 0; k < specs.size(); ++k)
        {
            const auto v0 = std::chrono::steady_clock::now();
            json r;
            try
            {
                const json built = buildTerrainSpec(specs[k], true, true, false);
                r = {{"ok", built["ok"]}, {"revision", built["revision"]}, {"stats", built["stats"]}, {"check", built.value("check", json())},
                     {"failed", built.value("failed", json())}};
                const TerrainRef t = findTerrainRef(built["terrain"].get<std::string>(), cmd);
                writeCanvas(images[k], mapOf(*t.local, tile, 0.8, true, true), cmd);
                r["image"] = images[k].string();
            }
            catch (const std::exception& e)
            {
                r = {{"ok", false}, {"error", e.what()}};
            }
            std::vector<std::string> gone;
            const std::string name = specs[k]["name"];
            for (const std::string& n : {name, name + "_water", name + "_GroundCover"})
                if (scene::Node* node = g.find(n))
                    gone.push_back(node->path());
            if (!gone.empty())
                ops::execute("delete", {{"args", gone}});
            r["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - v0).count();
            results[k] = r;
        }
    }

    // The sheet: tiles in a grid, a label strip under each.
    const int columns = int(std::ceil(std::sqrt(double(specs.size()))));
    const int rows = int((specs.size() + size_t(columns) - 1) / size_t(columns));
    const int scale = tile >= 384 ? 3 : 2, strip = 9 * scale + 6, gap = 6;
    Canvas sheet(columns * (tile + gap) + gap, rows * (tile + strip + gap) + gap, {0.12f, 0.12f, 0.13f});
    json variants = json::array();
    for (size_t k = 0; k < specs.size(); ++k)
    {
        const int cx = gap + int(k % size_t(columns)) * (tile + gap), cy = gap + int(k / size_t(columns)) * (tile + strip + gap);
        std::string err;
        const image::Image img = fs::exists(images[k], ec) ? image::read(images[k], &err) : image::Image{};
        if (img.width > 0)
        {
            const double s = double(tile) / double(std::max(img.width, img.height));
            for (int y = 0; y < tile; ++y)
                for (int x = 0; x < tile; ++x)
                {
                    const int sx = int(x / s), sy = int(y / s);
                    if (sx >= img.width || sy >= img.height)
                        continue;
                    const float* p = &img.rgba[(size_t(sy) * size_t(img.width) + size_t(sx)) * 4];
                    sheet.set(cx + x, cy + y, {p[0], p[1], p[2]});
                }
        }
        else
            sheet.text(cx + 8, cy + tile / 2, "FAILED", {0.9f, 0.4f, 0.4f}, scale);
        const json& r = results[k];
        const bool ok = r.value("ok", false);
        std::string label = labelOf(int(k), settings[k]);
        if (!ok)
            label += " !";
        sheet.text(cx + 2, cy + tile + 4, label, ok ? glm::vec3(0.9f) : glm::vec3(0.95f, 0.55f, 0.45f), scale);
        json v = r;
        v["index"] = int64_t(k);
        v["vary"] = settings[k];
        v["image"] = img.width > 0 ? json(images[k].generic_string()) : json();
        variants.push_back(v);
    }
    const fs::path sheetFile = a.has("file") ? fs::path(a.get<std::string>("file", "")) : folder / (stem + "_wedge.png");
    writeCanvas(sheetFile, sheet, cmd);
    json out = {{"sheet", sheetFile.generic_string()}, {"columns", columns}, {"variants", variants}, {"mode", mode}, {"workers", workers},
                {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()}};
    if (!why.empty())
        out["note"] = why;
    const fs::path stats = folder / (stem + "_wedge.json");
    writeText(stats, out.dump(2));
    out["json"] = stats.generic_string();
    out["base"] = base;
    return out;
}
} // namespace wedge_

using namespace wedge_;

void registerWedge(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainMap",
        .doc = "Write a top-down map of a terrain as a PNG, -Z (north) at the top: tinted by height (green low to white high), lit "
               "from the north-west, water blue and roads dark. For a look at the shape without a viewport. Returns the file, "
               "its size in pixels and the revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"file", "f", T::String, nullptr, "PNG to write; default <temp>/lila_terrain_map/<terrain>.png."},
                  {"resolution", "r", T::Int, int64_t(512), "Width in pixels.", {}, {}, 16.0, 8192.0},
                  {"shade", "sh", T::Float, 0.8, "0 to 1: how much hill shading.", {}, {}, 0.0, 1.0},
                  {"water", "wa", T::Bool, true, "Draw water from the water, sea, riverBed and lakeBed masks."},
                  {"roads", "ro", T::Bool, true, "Draw roads from the roadSurface mask."},
                  {"label", "lb", T::String, "", "Text in the top-left corner."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> objects = a.objects();
            if (objects.size() != 1)
                throw CommandError("terrainMap: give one terrain");
            const TerrainRef t = findTerrainRef(objects[0], "terrainMap");
            Canvas c = mapOf(*t.local, int(a.get<int64_t>("resolution", 512)), a.get<double>("shade", 0.8), a.get<bool>("water", true), a.get<bool>("roads", true));
            if (const std::string label = a.get<std::string>("label", ""); !label.empty())
                c.text(6, 6, label, {1.0f, 1.0f, 1.0f}, c.w >= 768 ? 3 : 2);
            const fs::path file = a.has("file") ? fs::path(a.get<std::string>("file", "")) : fs::temp_directory_path() / "lila_terrain_map" / (t.name + ".png");
            writeCanvas(file, c, "terrainMap");
            return {{"file", file.generic_string()}, {"size", json::array({c.w, c.h})}, {"revision", revision(*t.local)}};
        },
        .examples = {"terrainMap('Terrain', file='maps/terrain.png', resolution=1024)['file']"},
    });

    ctx.registerCommand({
        .name = "terrainWedge",
        .doc = "Try variants of a terrain spec side by side: each variant is the spec with the vary settings changed, built in "
               "its own lilabatch process (several at once, headless), mapped from above (terrainMap) and laid out in a labelled "
               "contact sheet PNG. vary is {key: [values]} with dotted keys for nested settings ('erosion.age', "
               "'erosion.preset', 'source.ridged'); each variant takes the next value of every key (count variants), or "
               "grid=True builds every combination. Without vary, count seeds from the spec's.\n\n"
               "Returns the sheet, mode (parallel, or inProcess with a note saying why when lilabatch can't be run or "
               "parallel=False), and per variant its settings, ok, revision, stats, check, map image, time and any error; the "
               "same goes to <folder>/<name>_wedge.json. Nothing is added to the scene. Build the one you like with "
               "terrainSpec(spec=..., overrides=variant['vary']).",
        .positionalDoc = "[terrain built from a spec, to wedge its spec]",
        .flags = {{"spec", "s", T::Any, nullptr, "The spec to vary."},
                  {"preset", "p", T::String, nullptr, "Start from a terrainSpec preset."},
                  {"overrides", "o", T::Any, nullptr, "Merged over the spec before varying."},
                  {"vary", "v", T::Any, nullptr, "{key: [values]}: {'seed': [1, 2, 3], 'erosion.age': [0.3, 0.6]}."},
                  {"count", "c", T::Int, int64_t(4), "Variants when not grid.", {}, {}, 1.0, 64.0},
                  {"grid", "g", T::Bool, false, "Every combination of the vary values."},
                  {"resolution", "r", T::Int, nullptr, "Build every variant at this resolution (257 is quick).", {}, {}, 17.0, 8193.0},
                  {"parallel", "pl", T::Bool, true, "Build in lilabatch processes; False builds here one after another."},
                  {"workers", "w", T::Int, nullptr, "Processes at once; default half the cores, at most 8.", {}, {}, 1.0, 32.0},
                  {"tile", "t", T::Int, int64_t(256), "Map size per variant in the sheet, pixels.", {}, {}, 64.0, 2048.0},
                  {"timeout", "to", T::Float, 900.0, "Seconds a variant may take before its process is ended.", {}, {}, 10.0},
                  {"folder", "f", T::String, nullptr, "Where the maps, logs and json go; default <temp>/lila_terrain_wedge/<name>."},
                  {"file", "fi", T::String, nullptr, "The sheet's path; default <folder>/<name>_wedge.png."}},
        .undoable = false,
        .run = [](const Args& a) { return runWedge(a); },
        .examples = {"w = terrainWedge(preset='alpineValley', vary={'seed': [1, 2, 3, 4]}, resolution=257)\nw['sheet'], [v['stats']['heightRange'] for v in w['variants']]",
                     "terrainWedge(spec=s, vary={'erosion.age': [0.2, 0.5, 0.8], 'erosion.preset': ['natural', 'alpine']}, grid=True)"},
    });
}
} // namespace terrain
