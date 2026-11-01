#include "brush/Presets.h"

#include "brush/Alphas.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "scene/Poses.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

namespace lila::brush::presets
{
namespace fs = std::filesystem;

namespace
{
constexpr const char* kFormat = "lilabrush";

bool isPreset(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == kExtension;
}

/** The presets in dir, then those in each subfolder (one level: the categories). */
std::vector<fs::path> presetsIn(const fs::path& dir)
{
    std::vector<fs::path> out, subdirs;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && isPreset(e.path()))
            out.push_back(e.path());
        else if (e.is_directory(ec))
            subdirs.push_back(e.path());
    std::ranges::sort(out);
    std::ranges::sort(subdirs);
    for (const fs::path& sub : subdirs)
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(sub, ec))
            if (e.is_regular_file(ec) && isPreset(e.path()))
                files.push_back(e.path());
        std::ranges::sort(files);
        out.insert(out.end(), files.begin(), files.end());
    }
    return out;
}

std::optional<std::string> readText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeText(const fs::path& file, std::string_view text, std::string* error)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out)
        out.write(text.data(), std::streamsize(text.size()));
    if (!out)
    {
        if (error)
            *error = "can't write " + paths::toUtf8(file);
        return false;
    }
    return true;
}

uint64_t fnv1a(std::string_view bytes)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

/** Writes an embedded alpha once per distinct image and returns the copy's path. */
std::string extractAlpha(const std::string& name, const std::string& bytes)
{
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(bytes)));
    std::string ext = paths::toUtf8(paths::fromUtf8(name).extension());
    const fs::path file = PrefStore::get().userDir() / "cache" / "brushAlphas" / (std::string(hex) + (ext.empty() ? ".png" : ext));
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) && !writeText(file, bytes, nullptr))
        return {};
    return paths::toUtf8(file);
}

std::string slashed(const fs::path& p)
{
    std::string s = paths::toUtf8(p);
    std::ranges::replace(s, '\\', '/');
    return s;
}

std::string idOf(const Folder& f, const fs::path& file)
{
    fs::path rel = file.lexically_relative(f.dir);
    rel.replace_extension();
    return f.source + "/" + slashed(rel);
}

std::string categoryOf(const Folder& f, const fs::path& file)
{
    const fs::path parent = file.parent_path().lexically_relative(f.dir);
    return parent == "." ? std::string() : slashed(parent);
}

std::string fileStem(std::string_view label)
{
    std::string out;
    for (unsigned char c : label)
        out += std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c >= 0x80 ? char(c) : '_';
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out.empty() ? std::string("Preset") : out;
}

fs::path freeName(const fs::path& dir, const std::string& stem, bool replace)
{
    fs::path target = dir / paths::fromUtf8(stem + std::string(kExtension));
    std::error_code ec;
    for (int n = 2; !replace && fs::exists(target, ec); ++n)
        target = dir / paths::fromUtf8(stem + " " + std::to_string(n) + std::string(kExtension));
    return target;
}

/** The id of a file sitting in one of the folders, empty when it is elsewhere. */
std::string idFor(const fs::path& file)
{
    std::error_code ec;
    const fs::path parent = file.parent_path();
    for (const Folder& f : folders())
        if (fs::exists(f.dir, ec) && (fs::equivalent(f.dir, parent, ec) || fs::equivalent(f.dir, parent.parent_path(), ec)))
            return idOf(f, file);
    return {};
}

std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}

json summary(const Preset& p)
{
    return {{"id", p.id},         {"label", p.label},       {"tool", p.tool},         {"brush", p.brush},
            {"icon", p.icon},     {"doc", p.doc},           {"source", p.source},     {"category", p.category},
            {"file", paths::toUtf8(p.file)}, {"alpha", p.settings.alphaPath}};
}

fs::path dirFor(const std::string& scope, const char* cmd)
{
    if (scope == "project")
    {
        fs::path d = projectDir();
        if (d.empty())
            throw CommandError(std::string(cmd) + ": no project is open");
        return d;
    }
    return userDir();
}
} // namespace

fs::path userDir() { return PrefStore::get().userDir() / "brushes"; }

fs::path projectDir() { return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / "brushes" : fs::path(); }

std::vector<Folder> folders()
{
    std::vector<Folder> out = {{paths::resourcesDir() / "brushes", "lila", "Lila", false}};
    std::vector<std::string> exts;
    try
    {
        exts = extensions::ExtensionManager::get().loaded();
    }
    catch (const std::exception&)
    {
    }
    for (const std::string& id : exts)
        if (ext::Context* ctx = extensions::contextOf(id))
            out.push_back({ctx->resources() / "brushes", id, id, false});
    const std::vector<fs::path> studio = PrefStore::get().studioDirs();
    for (size_t i = 0; i < studio.size(); ++i)
        out.push_back({studio[i] / "brushes", i == 0 ? std::string("studio") : "studio" + std::to_string(i + 1), "Studio", false});
    out.push_back({userDir(), "user", "My Presets", true});
    if (fs::path p = projectDir(); !p.empty())
        out.push_back({p, "project", "Project", true});
    return out;
}

namespace
{
/** The file's document when it is a .lilabrush of any tool. */
std::optional<json> readPreset(const fs::path& file, std::string* error)
{
    std::optional<std::string> text = readText(file);
    json j = text ? json::parse(*text, nullptr, false) : json();
    if (j.is_object() && j.value("format", "") == kFormat)
        return j;
    if (error)
        *error = paths::toUtf8(file.filename()) + (text ? ": not a brush preset" : ": can't read it");
    return std::nullopt;
}
} // namespace

std::optional<Preset> load(const fs::path& file, std::string* error)
{
    auto fail = [&](std::string why) -> std::optional<Preset> {
        if (error)
            *error = paths::toUtf8(file.filename()) + ": " + why;
        return std::nullopt;
    };
    std::optional<json> doc = readPreset(file, error);
    if (!doc)
        return std::nullopt;
    const json& j = *doc;
    Preset p;
    p.file = file;
    p.tool = j.value("tool", "paint");
    if (p.tool != "sculpt")
        return fail("a " + p.tool + " preset");
    p.label = j.value("name", paths::toUtf8(file.stem()));
    p.icon = j.value("icon", "");
    p.doc = j.value("doc", "");
    p.brush = j.value("sculptBrush", "");
    if (p.brush.empty())
        return fail("names no sculptBrush");
    p.settings = BrushSettings::fromJson(j.value("brush", json::object()));
    p.settings.alphaPath.clear();
    const std::string ref = j.value("alphaRef", "");
    if (!ref.empty() && !alphas::resolve(ref).empty())
        p.settings.alphaPath = ref;
    else if (const json& img = j.value("alphaImage", json::object()); img.is_object() && img.contains("data"))
        p.settings.alphaPath = extractAlpha(img.value("name", "alpha.png"), scene::poses::base64Decode(img.value("data", "")));
    return p;
}

bool write(const Preset& p, const fs::path& file, std::string* error)
{
    json settings = p.settings.toJson();
    settings.erase("alpha");
    settings.erase("seed");
    json j = {{"format", kFormat}, {"version", 1}, {"tool", p.tool}, {"name", p.label}, {"sculptBrush", p.brush}, {"brush", settings}};
    if (!p.icon.empty())
        j["icon"] = p.icon;
    if (!p.doc.empty())
        j["doc"] = p.doc;
    if (!p.settings.alphaPath.empty())
    {
        if (p.settings.alphaPath.starts_with(alphas::kScheme))
            j["alphaRef"] = p.settings.alphaPath;
        const std::string image = alphas::resolve(p.settings.alphaPath);
        std::optional<std::string> bytes = image.empty() ? std::nullopt : readText(paths::fromUtf8(image));
        if (!bytes)
        {
            if (error)
                *error = "can't read the alpha " + p.settings.alphaPath;
            return false;
        }
        j["alphaImage"] = {{"name", paths::toUtf8(paths::fromUtf8(image).filename())}, {"data", scene::poses::base64Encode(*bytes)}};
    }
    return writeText(file, j.dump(2), error);
}

std::vector<Preset> library(std::string_view tool)
{
    std::vector<Preset> out;
    for (const Folder& f : folders())
    {
        const size_t first = out.size();
        for (const fs::path& file : presetsIn(f.dir))
        {
            std::string error;
            std::optional<json> doc = readPreset(file, &error);
            if (!doc)
            {
                log::warn("Brush presets: {}", error);
                continue;
            }
            // Texture paint presets are listed by tex/BrushPresets.h.
            if (doc->value("tool", "paint") != "sculpt" || (!tool.empty() && tool != "sculpt"))
                continue;
            std::optional<Preset> p = load(file, &error);
            if (!p)
            {
                log::warn("Brush presets: {}", error);
                continue;
            }
            p->id = idOf(f, file);
            p->source = f.source;
            p->sourceLabel = f.label;
            p->category = categoryOf(f, file);
            out.push_back(std::move(*p));
        }
        std::stable_sort(out.begin() + ptrdiff_t(first), out.end(), [](const Preset& a, const Preset& b) {
            return std::tie(a.category, a.label) < std::tie(b.category, b.label);
        });
    }
    return out;
}

std::optional<Preset> find(std::string_view id)
{
    const size_t slash = id.find('/');
    if (slash == std::string_view::npos || id.find("..") != std::string_view::npos)
        return std::nullopt;
    const std::string_view source = id.substr(0, slash);
    for (const Folder& f : folders())
        if (f.source == source)
        {
            const fs::path file = f.dir / paths::fromUtf8(std::string(id.substr(slash + 1)) + std::string(kExtension));
            std::error_code ec;
            if (!fs::is_regular_file(file, ec))
                return std::nullopt;
            std::optional<Preset> p = load(file);
            if (p)
            {
                p->id = std::string(id);
                p->source = f.source;
                p->sourceLabel = f.label;
                p->category = categoryOf(f, file);
            }
            return p;
        }
    return std::nullopt;
}

std::string save(const Preset& p, const fs::path& dir, bool replace, std::string* error)
{
    const fs::path file = freeName(dir, fileStem(p.label), replace);
    if (!write(p, file, error))
        return {};
    notifyChanged();
    return idFor(file);
}

std::string install(const fs::path& file, const fs::path& dir, std::string* error)
{
    if (!readPreset(file, error))
        return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path target = freeName(dir, paths::toUtf8(file.stem()), false);
    if (!fs::copy_file(file, target, ec))
    {
        if (error)
            *error = ec.message();
        return {};
    }
    notifyChanged();
    return idFor(target);
}

bool remove(std::string_view id, std::string* error)
{
    std::optional<Preset> p = find(id);
    auto fail = [&](std::string why) {
        if (error)
            *error = why;
        return false;
    };
    if (!p)
        return fail("no preset " + std::string(id));
    if (p->source != "user" && p->source != "project")
        return fail(std::string(id) + " belongs to " + p->sourceLabel + "; only your own and the project's presets delete");
    std::error_code ec;
    if (!fs::remove(p->file, ec))
        return fail(ec ? ec.message() : "can't delete " + paths::toUtf8(p->file));
    notifyChanged();
    return true;
}

int addListener(std::function<void()> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeListener(int id) { listeners().erase(id); }

void notifyChanged()
{
    auto copy = listeners();
    for (auto& [id, fn] : copy)
        fn();
}

void registerCommands()
{
    using T = ArgType;
    const std::vector<std::string> scopes = {"user", "project"};

    ops::registerCommand({
        .name = "brushPresets",
        .doc = "The brush presets in Lila's, the extensions', the studio's, your and the project's brushes folders: "
               "[{id, label, tool, brush, icon, doc, source, category, file, alpha}].",
        .flags = {{"tool", "", T::String, "", "Only presets for this tool (sculpt)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            for (const Preset& p : library(a.get<std::string>("tool")))
                out.push_back(summary(p));
            return out;
        },
    });

    ops::registerCommand({
        .name = "brushPresetSave",
        .doc = "Save a brush with its stroke settings as a preset: a .lilabrush file in your brushes folder, or the "
               "project's. The alpha image is stored inside the file. Returns the preset's summary.",
        .positionalDoc = "label",
        .flags = {{"brush", "", T::String, "draw", "The brush it drives: a built-in's name or a registered brush's."},
                  {"tool", "", T::String, "sculpt", "The tool it is for."},
                  {"settings", "", T::Any, json::object(), "Stroke settings (radius, strength, falloff, spacing, ...); missing keys take the defaults."},
                  {"alpha", "", T::String, "", "Alpha image: a file or a library alpha (alpha://...)."},
                  {"icon", "", T::String, "", "An icon name; empty uses the brush's."},
                  {"doc", "", T::String, "", "Tooltip text."},
                  {"scope", "", T::String, "user", "user or project.", scopes},
                  {"replace", "", T::Bool, false, "Overwrite a preset of the same name instead of numbering the new one."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
                throw CommandError("brushPresetSave: give the preset a label");
            Preset p;
            p.label = a.positional[0].get<std::string>();
            p.brush = a.get<std::string>("brush", "draw");
            p.tool = a.get<std::string>("tool", "sculpt");
            p.icon = a.get<std::string>("icon");
            p.doc = a.get<std::string>("doc");
            p.settings = BrushSettings::fromJson(a.flags.value("settings", json::object()));
            p.settings.alphaPath = a.get<std::string>("alpha");
            std::string error;
            const std::string id = save(p, dirFor(a.get<std::string>("scope", "user"), "brushPresetSave"), a.get<bool>("replace"), &error);
            if (id.empty())
                throw CommandError("brushPresetSave: " + error);
            return summary(*find(id));
        },
    });

    ops::registerCommand({
        .name = "brushPresetInstall",
        .doc = "Copy .lilabrush files into your brushes folder, or the project's, numbering a name that is taken. Returns their ids.",
        .positionalDoc = "files",
        .flags = {{"scope", "", T::String, "user", "user or project.", scopes}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("brushPresetInstall: give the .lilabrush files");
            const fs::path dir = dirFor(a.get<std::string>("scope", "user"), "brushPresetInstall");
            json ids = json::array();
            for (const json& f : a.positional)
            {
                std::string error;
                const std::string id = f.is_string() ? install(paths::fromUtf8(f.get<std::string>()), dir, &error) : std::string();
                if (id.empty())
                    throw CommandError("brushPresetInstall: " + (error.empty() ? std::string("files are paths") : error));
                ids.push_back(id);
            }
            return ids;
        },
    });

    ops::registerCommand({
        .name = "brushPresetExport",
        .doc = "Write a preset to a .lilabrush file to share, its alpha image inside.",
        .positionalDoc = "id",
        .flags = {{"file", "", T::String, "", "The file to write."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string id = a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>();
            std::optional<Preset> p = find(id);
            if (!p)
                throw CommandError("brushPresetExport: no preset '" + id + "'");
            fs::path file = paths::fromUtf8(a.get<std::string>("file"));
            if (file.empty())
                throw CommandError("brushPresetExport: give a file");
            if (!isPreset(file))
                file += std::string(kExtension);
            std::string error;
            if (!write(*p, file, &error))
                throw CommandError("brushPresetExport: " + error);
            return paths::toUtf8(file);
        },
    });

    ops::registerCommand({
        .name = "brushPresetDelete",
        .doc = "Delete one of your or the project's brush presets.",
        .positionalDoc = "id",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string id = a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>();
            std::string error;
            if (!remove(id, &error))
                throw CommandError("brushPresetDelete: " + error);
            return true;
        },
    });

    if (!io::translators::find("lilabrush"))
        io::translators::add({.id = "lilabrush",
                              .label = "Lila Brush Preset",
                              .doc = "A brush preset: installs into your brushes folder.",
                              .extensions = {std::string(kExtension)},
                              .import = {.command = "brushPresetInstall"}});
}
} // namespace lila::brush::presets
