#include "ops/OptionPresets.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace lila::ops::options::presets
{
namespace fs = std::filesystem;

namespace
{
constexpr std::string_view kFormat = "lilaOptions";

/** A file name from a preset name: letters, digits, space, dash and underscore kept, the rest dropped. */
std::string fileStem(std::string_view name)
{
    std::string out;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || static_cast<unsigned char>(c) >= 0x80)
            out += c;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? "preset" : out;
}

bool writeFile(const fs::path& file, const json& doc, std::string* error)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << doc.dump(2);
    if (out.good())
        return true;
    if (error)
        *error = "can't write " + paths::toUtf8(file);
    return false;
}
} // namespace

fs::path dir(std::string_view command, std::string_view source)
{
    const fs::path cmd = paths::fromUtf8(command);
    if (source == "user")
        return PrefStore::get().userDir() / "optionPresets" / cmd;
    if (source == "project")
        return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / "optionPresets" / cmd : fs::path();
    return {};
}

std::optional<Preset> load(const fs::path& file, std::string* error)
{
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const json j = in ? json::parse(text.str(), nullptr, false) : json();
    if (!j.is_object() || j.value("format", "") != kFormat || !j.contains("values") || !j["values"].is_object())
    {
        if (error)
            *error = paths::toUtf8(file.filename()) + (in ? ": not an option preset" : ": can't read it");
        return std::nullopt;
    }
    Preset p;
    p.name = j.value("name", paths::toUtf8(file.stem()));
    p.file = file;
    p.values = j["values"];
    return p;
}

std::vector<Preset> list(std::string_view command)
{
    std::vector<std::pair<fs::path, std::string>> folders;
    // Most specific studio folder last, so it overrides the broader ones.
    const std::vector<fs::path> studio = PrefStore::get().studioDirs();
    for (auto it = studio.rbegin(); it != studio.rend(); ++it)
        folders.push_back({*it / "optionPresets" / paths::fromUtf8(command), "studio"});
    folders.push_back({dir(command, "user"), "user"});
    if (fs::path p = dir(command, "project"); !p.empty())
        folders.push_back({p, "project"});

    std::vector<Preset> out;
    for (const auto& [folder, source] : folders)
    {
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(folder, ec))
            continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".json")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files)
            if (std::optional<Preset> p = load(f))
            {
                p->source = source;
                p->writable = source != "studio";
                std::erase_if(out, [&](const Preset& o) { return o.name == p->name; });
                out.push_back(std::move(*p));
            }
    }
    return out;
}

std::optional<Preset> find(std::string_view command, std::string_view name)
{
    for (Preset& p : list(command))
        if (p.name == name)
            return std::move(p);
    return std::nullopt;
}

std::optional<Preset> save(std::string_view command, std::string_view name, const json& values, std::string_view source, std::string* error)
{
    const fs::path folder = dir(command, source);
    if (folder.empty() || name.empty())
    {
        if (error)
            *error = name.empty() ? "a preset needs a name" : source == "project" ? "no project is open" : "presets save to user or project";
        return std::nullopt;
    }
    Preset p;
    p.name = std::string(name);
    p.source = std::string(source);
    p.writable = true;
    p.values = values.is_object() ? values : json::object();
    // A preset of this name saved before keeps its file, whatever it was called.
    std::error_code ec;
    p.file = folder / paths::fromUtf8(fileStem(name) + ".json");
    if (fs::is_directory(folder, ec))
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (std::optional<Preset> old = e.path().extension() == ".json" ? load(e.path()) : std::nullopt; old && old->name == name)
                p.file = e.path();
    const json doc = {{"format", kFormat}, {"version", 1}, {"command", command}, {"name", p.name}, {"values", p.values}};
    if (!writeFile(p.file, doc, error))
        return std::nullopt;
    return p;
}

bool remove(const Preset& p, std::string* error)
{
    std::error_code ec;
    if (p.writable && fs::remove(p.file, ec))
        return true;
    if (error)
        *error = p.writable ? "can't delete " + paths::toUtf8(p.file) : "studio presets are managed in the studio folder";
    return false;
}

std::optional<Preset> install(const fs::path& file, std::string* error)
{
    std::optional<Preset> p = load(file, error);
    if (!p)
        return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    const json j = json::parse(in, nullptr, false);
    const std::string command = j.value("command", "");
    if (command.empty())
    {
        if (error)
            *error = paths::toUtf8(file.filename()) + ": names no command";
        return std::nullopt;
    }
    return save(command, p->name, p->values, "user", error);
}
} // namespace lila::ops::options::presets
