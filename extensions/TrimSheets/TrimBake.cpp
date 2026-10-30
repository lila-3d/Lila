// One-click sheet bake: the generated high onto the flat low through the Baker, tiling strips wrapped so the textures
// tile, an exact ID map written from the layout, and per-trim rebakes composited into the existing textures.

#include "TrimBake.h"
#include "Generator.h"

#include <lila/Bake.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace trims
{
namespace
{
namespace bake = lila::bake;
namespace image = lila::image;

std::unique_ptr<bake::Backend> backendFor(const std::string& name)
{
    if (name == "cpu")
        return bake::createBackend("cpu");
    const std::vector<std::string> names = bake::backendNames();
    if (std::ranges::find(names, std::string("gpu")) != names.end())
        if (auto b = bake::createBackend("gpu"))
            return b;
    if (name == "gpu")
        throw std::runtime_error("no gpu bake backend in this session");
    return bake::createBackend("cpu");
}

int validSupersampling(int s)
{
    for (int v : {64, 16, 4})
        if (s >= v)
            return v;
    return 1;
}

bake::ProjectSettings projectSettings(const Sheet& sheet, const std::filesystem::path& folder, const Generated& g)
{
    bake::ProjectSettings ps = bake::ProjectSettings::defaults();
    ps.output.folder = folder.generic_string();
    ps.output.name = sheet.name + "_{map}";
    ps.output.format = "png";
    ps.output.width = sheet.width;
    ps.output.height = sheet.height;
    ps.output.padding = std::max(1, sheet.padding);
    ps.output.supersampling = validSupersampling(sheet.supersampling);
    ps.maps.clear();
    const double relief = std::max({g.above, g.below, 0.1});
    for (const std::string& m : sheet.maps)
    {
        bake::MapSettings ms;
        ms.map = m;
        // Occlusion reaches a few bevel heights: far enough for strips to shade each other's feet, not the whole sheet.
        if (m == "ao")
            ms.options = {{"rays", 64}, {"distance", relief * 4.0}};
        else if (m == "height")
            ms.options = {{"range", relief}};
        ps.maps.push_back(ms);
    }
    return ps;
}

bake::BakeInputs inputsFor(const Sheet& sheet, const Generated& g)
{
    bake::BakeInputs in;
    in.project = sheet.name;
    bake::GroupInputs grp;
    grp.name = sheet.name;
    grp.settings.lows = {sheet.name};
    // The low is a plane on the base of the relief: rays go up past the tallest detail and down past the deepest recess.
    grp.settings.frontal = g.above + 0.5;
    grp.settings.rear = g.below + 0.5;
    grp.frontal = float(grp.settings.frontal);
    grp.rear = float(grp.settings.rear);
    grp.lows.push_back(bake::lowFromMesh(g.low, glm::dmat4(1.0), sheet.name));
    bake::HighInput h;
    h.soup = bake::soupFromMesh(g.high, glm::dmat4(1.0), sheet.name + "_high");
    h.source = h.soup.name;
    h.sourceTriangles = h.soup.triangleCount();
    grp.highMin = h.soup.boundsMin;
    grp.highMax = h.soup.boundsMax;
    bake::HighEntry e;
    e.node = h.source;
    grp.settings.highs.push_back(e);
    grp.highs.push_back(std::move(h));
    in.highMin = grp.highMin;
    in.highMax = grp.highMax;
    in.groups.push_back(std::move(grp));
    return in;
}

/** Copies the trims' rectangles, grown by pad, from src into dst (same size). */
void composite(image::Image& dst, const image::Image& src, const Sheet& sheet, std::span<const size_t> only)
{
    for (size_t i : only)
    {
        if (i >= sheet.trims.size())
            continue;
        const Trim& t = sheet.trims[i];
        const glm::ivec2 lo = glm::max(t.min - glm::ivec2(sheet.padding), glm::ivec2(0));
        const glm::ivec2 hi = glm::min(t.max + glm::ivec2(sheet.padding), glm::ivec2(dst.width, dst.height));
        for (int y = lo.y; y < hi.y; ++y)
        {
            const size_t row = size_t(y) * size_t(dst.width) * 4;
            std::copy_n(src.rgba.begin() + std::ptrdiff_t(row + size_t(lo.x) * 4), size_t(hi.x - lo.x) * 4,
                        dst.rgba.begin() + std::ptrdiff_t(row + size_t(lo.x) * 4));
        }
    }
}
} // namespace

std::filesystem::path mapFile(const Sheet& sheet, const std::filesystem::path& sheetFile, std::string_view map)
{
    std::string suffix(map);
    if (const bake::MapDef* def = bake::findMap(map); def && !def->suffix.empty())
        suffix = def->suffix;
    return textureFolder(sheet, sheetFile) / (sheet.name + "_" + suffix + ".png");
}

double seamError(const image::Image& img, const Sheet& sheet)
{
    if (img.width < 2 || img.height < 1)
        return 0.0;
    double sum = 0.0;
    size_t n = 0;
    for (const Trim& t : sheet.trims)
    {
        if (t.kind != uv::TrimKind::StripU)
            continue;
        for (int y = std::max(0, t.min.y); y < std::min(img.height, t.max.y); ++y)
        {
            const float* a = &img.rgba[size_t(y) * size_t(img.width) * 4];
            const float* b = a + size_t(img.width - 1) * 4;
            for (int c = 0; c < 3; ++c)
                sum += std::abs(double(a[c]) - double(b[c]));
            n += 3;
        }
    }
    return n ? sum / double(n) : 0.0;
}

nlohmann::json bakeSheet(const Sheet& sheet, const std::filesystem::path& sheetFile, const SheetBakeOptions& opts)
{
    LILA_ZONE();

    const auto start = std::chrono::steady_clock::now();
    const std::filesystem::path folder = textureFolder(sheet, sheetFile);
    const bool partial = !opts.only.empty();
    const std::filesystem::path target = partial ? folder / ".rebake" : folder;
    std::filesystem::create_directories(target);

    // Neighbours stay in the high on a partial rebake so their occlusion on the rebaked trims stays the same.
    GenerateOptions go;
    go.tile = opts.tile;
    const Generated g = generate(sheet, go);
    if (g.high.numFaces() == 0)
        throw std::runtime_error("the sheet has no trims to bake");

    bake::BakeInputs inputs = inputsFor(sheet, g);
    const bake::ProjectSettings ps = projectSettings(sheet, target, g);
    bake::BakeOptions bo;
    bo.folder = target;
    std::unique_ptr<bake::Backend> backend = backendFor(opts.backend);
    try
    {
        backend->prepare(inputs);
    }
    catch (const std::exception& e)
    {
        if (opts.backend != "auto" || backend->name() == "cpu")
            throw;
        lila::log::warn("Trim sheet bake: {} backend failed ({}), baking on the cpu", backend->name(), e.what());
        backend = bake::createBackend("cpu");
        backend->prepare(inputs);
    }
    bake::BakeResult r = bake::runBake(inputs, ps, bo, *backend);
    r.backend = backend->name();
    nlohmann::json out = r.toJson();

    if (partial)
    {
        nlohmann::json files = nlohmann::json::array();
        for (const nlohmann::json& o : out.value("outputs", nlohmann::json::array()))
        {
            const std::filesystem::path fresh = o.value("file", std::string());
            const std::filesystem::path final = folder / fresh.filename();
            image::Image src = image::read(fresh);
            image::Image dst = std::filesystem::exists(final) ? image::read(final) : image::Image{};
            if (dst.width != src.width || dst.height != src.height)
                dst = src;
            else
                composite(dst, src, sheet, opts.only);
            image::write(final, dst, o.value("bits", 8));
            files.push_back(final.generic_string());
        }
        std::error_code ec;
        std::filesystem::remove_all(target, ec);
        out["composited"] = files;
    }

    const std::filesystem::path id = folder / (sheet.name + "_trimId.png");
    image::write(id, idMap(sheet), 8);
    out["idMap"] = id.generic_string();
    out["folder"] = folder.generic_string();
    out["trims"] = int64_t(partial ? opts.only.size() : sheet.trims.size());
    const std::filesystem::path normal = mapFile(sheet, sheetFile, "normal");
    if (std::filesystem::exists(normal))
        out["seamError"] = seamError(image::read(normal), sheet);
    out["totalSeconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return out;
}
} // namespace trims
