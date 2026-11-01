#include "brush/Alphas.h"

#include "core/ExtensionContext.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"

#include <algorithm>
#include <cctype>

namespace lila::brush::alphas
{
namespace fs = std::filesystem;

namespace
{
bool isImage(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".png" || ext == ".exr" || ext == ".tga" || ext == ".tif" || ext == ".tiff" || ext == ".psd";
}

/** The images in dir, then those in each subfolder (one level: the categories), each run by name. */
std::vector<fs::path> imagesIn(const fs::path& dir)
{
    std::vector<fs::path> out, subdirs;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && isImage(e.path()))
            out.push_back(e.path());
        else if (e.is_directory(ec))
            subdirs.push_back(e.path());
    std::ranges::sort(out);
    std::ranges::sort(subdirs);
    for (const fs::path& sub : subdirs)
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(sub, ec))
            if (e.is_regular_file(ec) && isImage(e.path()))
                files.push_back(e.path());
        std::ranges::sort(files);
        out.insert(out.end(), files.begin(), files.end());
    }
    return out;
}

std::string refOf(const Folder& f, const fs::path& file)
{
    return std::string(kScheme) + f.source + "/" + paths::toUtf8(file.lexically_relative(f.dir));
}

std::string categoryOf(const Folder& f, const fs::path& file)
{
    const fs::path parent = file.parent_path().lexically_relative(f.dir);
    return parent == "." ? std::string() : paths::toUtf8(parent);
}
} // namespace

fs::path userDir() { return PrefStore::get().userDir() / "alphas"; }

fs::path projectDir() { return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / "alphas" : fs::path(); }

std::vector<Folder> folders()
{
    std::vector<Folder> out = {{paths::resourcesDir() / "alphas", "lila", "Lila"}};
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
            out.push_back({ctx->resources() / "alphas", id, id});
    out.push_back({userDir(), "user", "My Alphas"});
    if (fs::path p = projectDir(); !p.empty())
        out.push_back({p, "project", "Project"});
    return out;
}

std::vector<Entry> library()
{
    std::vector<Entry> out;
    for (const Folder& f : folders())
        for (const fs::path& file : imagesIn(f.dir))
            out.push_back({refOf(f, file), paths::toUtf8(file.stem()), file, f.source, f.label, categoryOf(f, file)});
    return out;
}

std::string resolve(std::string_view alphaPath)
{
    if (!alphaPath.starts_with(kScheme))
        return std::string(alphaPath);
    const std::string_view rest = alphaPath.substr(kScheme.size());
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos)
        return {};
    const std::string_view source = rest.substr(0, slash), name = rest.substr(slash + 1);
    for (const Folder& f : folders())
        if (f.source == source)
        {
            const fs::path file = f.dir / paths::fromUtf8(name);
            std::error_code ec;
            return fs::is_regular_file(file, ec) ? paths::toUtf8(file) : std::string();
        }
    return {};
}

std::string displayName(std::string_view alphaPath)
{
    if (alphaPath.empty())
        return {};
    const std::string_view name = alphaPath.starts_with(kScheme) ? alphaPath.substr(alphaPath.rfind('/') + 1) : alphaPath;
    return paths::toUtf8(paths::fromUtf8(name).stem());
}

std::string add(const fs::path& image, const fs::path& dir, std::string* error)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path target = dir / image.filename();
    for (int n = 2; fs::exists(target, ec) && !fs::equivalent(target, image, ec); ++n)
        target = dir / (paths::toUtf8(image.stem()) + "_" + std::to_string(n) + paths::toUtf8(image.extension()));
    if (!fs::exists(target, ec) && !fs::copy_file(image, target, ec))
    {
        if (error)
            *error = ec.message();
        return {};
    }
    for (const Folder& f : folders())
        if (fs::equivalent(f.dir, dir, ec))
            return refOf(f, target);
    return paths::toUtf8(target);
}
} // namespace lila::brush::alphas
