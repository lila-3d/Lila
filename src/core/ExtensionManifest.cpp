#include "core/ExtensionManifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace lila::ext
{
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
bool validId(const std::string& id)
{
    if (id.empty() || !(std::isalpha((unsigned char)id[0]) || id[0] == '_'))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'; });
}

/** Inside the package: a relative path that does not climb out of it. */
bool insidePackage(const std::string& rel)
{
    fs::path p(rel);
    if (rel.empty() || p.is_absolute() || p.has_root_name())
        return false;
    for (const fs::path& part : p.lexically_normal())
        if (part == "..")
            return false;
    return true;
}
} // namespace

Manifest Manifest::parse(const json& j, const fs::path& root)
{
    std::vector<std::string> problems;
    Manifest m;
    m.root = root;
    if (!j.is_object())
        throw ManifestError("extension.json must hold an object");

    auto str = [&](const char* key, std::string& out, bool required) {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
        {
            if (required)
                problems.push_back(std::string("'") + key + "' is missing");
            return;
        }
        if (!it->is_string())
        {
            problems.push_back(std::string("'") + key + "' must be a string");
            return;
        }
        out = it->get<std::string>();
    };

    str("id", m.id, true);
    if (!m.id.empty() && !validId(m.id))
        problems.push_back("'id' must start with a letter and hold only letters, digits, '_', '-' and '.'");
    str("name", m.name, false);
    if (m.name.empty())
        m.name = m.id;
    str("version", m.version, true);
    str("vendor", m.vendor, false);
    str("description", m.description, false);
    str("python", m.python, false);
    str("resources", m.resources, false);
    if (m.resources.empty())
        m.resources = "resources";

    std::string sdk;
    str("sdk", sdk, true);
    if (!sdk.empty())
    {
        if (auto v = SdkVersion::parse(sdk))
            m.sdk = *v;
        else
            problems.push_back("'sdk' must be \"major.minor\", e.g. \"1.0\"");
    }

    if (auto it = j.find("entry"); it != j.end() && !it->is_null())
    {
        if (it->is_string())
            m.entry[platformName()] = it->get<std::string>();
        else if (it->is_object())
        {
            for (auto& [platform, lib] : it->items())
            {
                if (lib.is_string())
                    m.entry[platform] = lib.get<std::string>();
                else
                    problems.push_back("'entry." + platform + "' must be a string");
            }
        }
        else
            problems.push_back("'entry' must be a string or {platform: library}");
    }
    for (auto& [platform, lib] : m.entry)
        if (!insidePackage(lib))
            problems.push_back("'entry." + platform + "' must be a path inside the package");
    if (!m.python.empty() && !insidePackage(m.python))
        problems.push_back("'python' must be a path inside the package");
    if (auto it = j.find("usdPlugins"); it != j.end() && !it->is_null())
    {
        if (!it->is_array())
            problems.push_back("'usdPlugins' must be a list of folders");
        else
            for (const json& f : *it)
                if (!f.is_string() || !insidePackage(f.get<std::string>()))
                    problems.push_back("each of 'usdPlugins' must be a folder inside the package");
                else
                    m.usdPlugins.push_back(f.get<std::string>());
    }
    if (m.entry.empty() && m.python.empty() && m.usdPlugins.empty() && !j.contains("resources"))
        problems.push_back("give an 'entry' library, a 'python' module or both, or only 'resources' or 'usdPlugins' for a resource pack");

    if (auto it = j.find("dependencies"); it != j.end() && !it->is_null())
    {
        if (!it->is_array())
            problems.push_back("'dependencies' must be a list");
        else
            for (const json& d : *it)
            {
                Dependency dep;
                if (d.is_string())
                    dep.id = d.get<std::string>();
                else if (d.is_object() && d.contains("id") && d["id"].is_string())
                {
                    dep.id = d["id"].get<std::string>();
                    if (d.contains("version") && d["version"].is_string())
                        dep.minVersion = d["version"].get<std::string>();
                }
                else
                {
                    problems.push_back("each dependency must be an id or {\"id\": ..., \"version\": ...}");
                    continue;
                }
                if (dep.id == m.id)
                    problems.push_back("an extension can't depend on itself");
                else
                    m.dependencies.push_back(std::move(dep));
            }
    }

    if (auto it = j.find("enabledByDefault"); it != j.end() && !it->is_null())
    {
        if (it->is_boolean())
            m.enabledByDefault = it->get<bool>();
        else
            problems.push_back("'enabledByDefault' must be true or false");
    }
    if (auto it = j.find("license"); it != j.end())
        m.license = *it;

    if (!m.id.empty() && root.extension() == kPackageSuffix && root.stem().string() != m.id)
        problems.push_back("the package folder must be named " + m.id + kPackageSuffix + ", not " + root.filename().string());

    if (!problems.empty())
    {
        std::string text = (root.empty() ? std::string("extension.json") : (root / kManifestFile).string()) + ":";
        for (const std::string& p : problems)
            text += "\n  " + p;
        throw ManifestError(text);
    }
    return m;
}

Manifest Manifest::load(const fs::path& dir)
{
    fs::path file = dir / kManifestFile;
    std::ifstream in(file);
    if (!in)
        throw ManifestError("can't read " + file.string());
    json j = json::parse(in, nullptr, false, true);
    if (j.is_discarded())
        throw ManifestError(file.string() + " is not valid JSON");
    return parse(j, dir);
}

std::optional<fs::path> Manifest::library() const
{
    auto it = entry.find(platformName());
    if (it == entry.end())
        return std::nullopt;
    return (root / it->second).lexically_normal();
}

std::optional<fs::path> Manifest::pythonModule() const
{
    if (python.empty())
        return std::nullopt;
    return (root / python).lexically_normal();
}

json Manifest::toJson() const
{
    json deps = json::array();
    for (const Dependency& d : dependencies)
        deps.push_back(d.minVersion.empty() ? json(d.id) : json{{"id", d.id}, {"version", d.minVersion}});
    json j = {{"id", id},
              {"name", name},
              {"version", version},
              {"vendor", vendor},
              {"description", description},
              {"sdk", sdk.str()},
              {"entry", entry},
              {"resources", resources},
              {"dependencies", deps},
              {"enabledByDefault", enabledByDefault}};
    if (!python.empty())
        j["python"] = python;
    if (!usdPlugins.empty())
        j["usdPlugins"] = usdPlugins;
    return j;
}

const char* platformName()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::vector<std::string> dependencyOrder(const std::vector<std::string>& ids,
                                         const std::function<std::vector<std::string>(const std::string&)>& depsOf,
                                         std::vector<std::string>* cycleOut)
{
    std::set<std::string> known(ids.begin(), ids.end());
    std::set<std::string> done, visiting;
    std::vector<std::string> out;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (done.contains(id))
            return;
        if (visiting.contains(id))
        {
            if (cycleOut)
                cycleOut->push_back(id);
            return;
        }
        visiting.insert(id);
        for (const std::string& d : depsOf(id))
            if (known.contains(d))
                visit(d);
        visiting.erase(id);
        done.insert(id);
        out.push_back(id);
    };
    for (const std::string& id : ids)
        visit(id);
    return out;
}

int compareVersions(std::string_view a, std::string_view b)
{
    auto next = [](std::string_view& s) {
        long v = 0;
        size_t i = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i]))
            v = v * 10 + (s[i++] - '0');
        while (i < s.size() && s[i] != '.')
            ++i;
        s = i < s.size() ? s.substr(i + 1) : std::string_view();
        return v;
    };
    while (!a.empty() || !b.empty())
    {
        long x = next(a), y = next(b);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}
} // namespace lila::ext
