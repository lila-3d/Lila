#include "io/Examples.h"

#include "core/Paths.h"
#include "io/SceneFile.h"
#include "ops/Command.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace lila::io::examples
{
using json = nlohmann::json;

namespace
{
std::string trim(std::string s)
{
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

struct Readme
{
    std::string title;
    std::string paragraph;
};

Readme readReadme(const fs::path& file)
{
    Readme r;
    std::ifstream in(file);
    std::string line, para;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.starts_with("# ") && r.title.empty())
            r.title = trim(line.substr(2));
        else if (line.starts_with("#") || (line.empty() && !para.empty()))
        {
            if (!para.empty())
                break;
        }
        else if (!line.empty())
            para += (para.empty() ? "" : " ") + line;
    }
    para.erase(std::remove(para.begin(), para.end(), '`'), para.end());
    r.paragraph = para;
    return r;
}

/** "Destruction_Tools" in folder "Destruction" reads "Tools". */
std::string variantName(std::string stem, const std::string& folder)
{
    std::replace(stem.begin(), stem.end(), '_', ' ');
    if (stem.size() > folder.size() && stem.compare(0, folder.size(), folder) == 0 && stem[folder.size()] == ' ')
        stem = stem.substr(folder.size() + 1);
    return stem;
}

std::string utf8(const fs::path& p)
{
    return paths::toUtf8(p);
}
} // namespace

fs::path examplesDir()
{
    return paths::resourcesDir() / "examples";
}

std::vector<Example> list(const fs::path& dir)
{
    std::vector<Example> out;
    std::error_code ec;
    std::vector<fs::path> folders;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec))
            folders.push_back(e.path());
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders)
    {
        std::vector<fs::path> scenes;
        for (const fs::directory_entry& e : fs::directory_iterator(folder, ec))
            if (e.is_regular_file(ec) && (e.path().extension() == ".lila" || e.path().extension() == ".lilaa"))
                scenes.push_back(e.path());
        std::sort(scenes.begin(), scenes.end());
        const Readme readme = readReadme(folder / "README.md");
        const std::string title = readme.title.empty() ? utf8(folder.filename()) : readme.title;
        for (const fs::path& scene : scenes)
        {
            Example x;
            x.name = scenes.size() == 1 ? title : title + ": " + variantName(utf8(scene.stem()), utf8(folder.filename()));
            x.description = readme.paragraph;
            x.scene = scene;
            for (const fs::path& thumb : {fs::path(scene).replace_extension(".png"), folder / "thumb.png"})
                if (fs::is_regular_file(thumb, ec))
                {
                    x.thumbnail = thumb;
                    break;
                }
            out.push_back(std::move(x));
        }
    }
    return out;
}

void open(const Example& example)
{
    loadVersion(example.scene, example.scene, false, example.name);
    markClean();
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "examples",
        .doc = "The example scenes shipped with Lila: {name, description, path}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Example& x : list())
                out.push_back({{"name", x.name}, {"description", x.description}, {"path", utf8(x.scene)}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "openExample",
        .doc = "Open an example scene by name (from examples) as an untitled scene. Saving asks for a file, so the shipped "
               "example is never overwritten. Not undoable.",
        .positionalDoc = "name",
        .flags = {{"force", "f", T::Bool, false, "Discard unsaved changes in the current scene."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("openExample: give one name");
            if (io::modified() && !a.get<bool>("force"))
                throw CommandError("openExample: the scene has unsaved changes; save it or pass force=True");
            const std::string& name = a.positional[0].get_ref<const std::string&>();
            const std::vector<Example> all = list();
            auto it = std::find_if(all.begin(), all.end(), [&](const Example& x) { return x.name == name; });
            if (it == all.end())
                throw CommandError("openExample: no example named " + name);
            try
            {
                open(*it);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return utf8(it->scene);
        },
    });
}
} // namespace lila::io::examples
