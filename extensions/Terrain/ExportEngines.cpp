// Terrain for engines other than Unreal's landscape import: tiled files, Unity terrains, Godot Terrain3D regions, LOD'd
// mesh tiles, Unreal Mesh Terrain and Unreal region imports for huge landscapes.

#include "ExportEngines.h"

#include "Export.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/UnrealLink.h>

#include <miniz.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
namespace fs = std::filesystem;
using T = ArgType;

fs::path gScripts;

void writeBytes(const fs::path& path, const void* data, size_t bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), std::streamsize(bytes));
    if (!f)
        throw std::runtime_error("can't write " + path.string());
}

std::vector<char> readBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("can't read " + path.string());
    std::vector<char> out(size_t(f.tellg()));
    f.seekg(0);
    f.read(out.data(), std::streamsize(out.size()));
    return out;
}

void writeGrayImage(const fs::path& path, int w, int h, const std::vector<float>& values, int bits)
{
    std::vector<float> rgba(size_t(w) * size_t(h) * 4);
    for (size_t k = 0; k < values.size(); ++k)
    {
        rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = values[k];
        rgba[k * 4 + 3] = 1.0f;
    }
    image::WriteOptions o;
    o.bits = bits;
    image::write(path, {w, h, std::move(rgba)}, o);
}

uint16_t to16(double v01) { return uint16_t(std::lround(std::clamp(v01, 0.0, 1.0) * 65535.0)); }

/** The masks that become layers, in mask order, with their engine names. Named masks the terrain lacks are an error. */
std::vector<std::pair<std::string, std::string>> chosenLayers(const Heightfield& h, const EngineLayers& l)
{
    for (const std::string& m : l.masks)
        if (!m.empty() && !h.findMask(m))
            throw std::runtime_error("the terrain has no mask '" + m + "'");
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::string> taken;
    for (const auto& [mask, values] : h.masks)
    {
        if (values.size() != h.size())
            continue;
        if (!l.masks.empty() && std::find(l.masks.begin(), l.masks.end(), mask) == l.masks.end())
            continue;
        auto named = l.names.find(mask);
        const std::string layer = unrealLayerName(named != l.names.end() ? named->second : mask);
        if (!taken.insert(layer).second || (l.baseLayer && layer == "Base"))
            throw std::runtime_error("masks " + mask + " and another both map to layer " + layer);
        out.emplace_back(mask, layer);
    }
    return out;
}

/** Layer weights from raw mask values: clamped, scaled down when they sum past 1, the rest to Base when there is one. */
void layerWeights(const std::vector<double>& raw, bool base, std::vector<double>& out)
{
    out.assign(raw.size() + (base ? 1 : 0), 0.0);
    double sum = 0.0;
    for (size_t k = 0; k < raw.size(); ++k)
    {
        out[k + (base ? 1 : 0)] = std::clamp(raw[k], 0.0, 1.0);
        sum += out[k + (base ? 1 : 0)];
    }
    if (sum > 1.0)
    {
        for (double& w : out)
            w /= sum;
        sum = 1.0;
    }
    if (base)
        out[0] = 1.0 - sum;
    else if (sum > 0.0)
        for (double& w : out)
            w /= sum;
    else if (!out.empty())
        out[0] = 1.0;
}

/** Lila world (cm) to an engine's Y-up axes (cm), as rows: engine x, y (up) and z. */
struct EngineAxes
{
    glm::dvec3 x, y, z;
};

/** Unity: left-handed, Y up, Z forward, X right. */
EngineAxes unityAxes()
{
    const math::AxisBasis& b = math::axisBasis();
    return {b.right, b.up, b.forward};
}

/** Godot: right-handed, Y up, glTF's axes, which are Lila's own on a Y-up scene. */
EngineAxes godotAxes()
{
    const math::AxisBasis& b = math::axisBasis();
    return {-b.right, b.up, b.forward};
}

/** Samples a placed heightfield on an engine's ground grid, in engine cm. */
class EngineSampler
{
public:
    EngineSampler(const Heightfield& h, const glm::dmat4& world, const EngineAxes& e) : mH(h), mWorld(world), mInv(glm::inverse(world)), mE(e)
    {
        const glm::dvec2 ext = h.extent();
        mMin = glm::dvec2(std::numeric_limits<double>::max());
        mMax = -mMin;
        for (int c = 0; c < 4; ++c)
        {
            const glm::dvec3 w(world * glm::dvec4(h.origin.x + (c & 1 ? ext.x : 0.0), 0.0, h.origin.y + (c & 2 ? ext.y : 0.0), 1.0));
            const glm::dvec2 g = ground(w);
            mMin = glm::min(mMin, g);
            mMax = glm::max(mMax, g);
        }
        const glm::dvec3 colX(world[0]), colY(world[1]), colZ(world[2]);
        upright = glm::dot(glm::normalize(engine(colY)), glm::dvec3(0, 1, 0)) > 1.0 - 1e-9;
        auto axisAligned = [&](const glm::dvec3& v) {
            const glm::dvec3 n = glm::normalize(engine(v));
            return std::abs(n.x) > 1.0 - 1e-9 || std::abs(n.z) > 1.0 - 1e-9;
        };
        aligned = axisAligned(colX) && axisAligned(colZ);
        worldCell = h.cell * 0.5 * (glm::length(colX) + glm::length(colZ));
    }

    bool upright = false;
    bool aligned = false;
    double worldCell = 100.0;

    glm::dvec2 min() const { return mMin; }
    glm::dvec2 max() const { return mMax; }
    glm::dvec3 engine(const glm::dvec3& w) const { return {glm::dot(w, mE.x), glm::dot(w, mE.y), glm::dot(w, mE.z)}; }

    /** The heightfield's own XZ under an engine ground point. */
    glm::dvec2 local(double ex, double ez) const
    {
        const glm::dvec3 w = mE.x * ex + mE.z * ez;
        const glm::dvec4 q = mInv * glm::dvec4(w, 1.0);
        return {q.x, q.z};
    }
    /** Engine height (cm) of the ground at an engine ground point; edges carry on outside. */
    double height(double ex, double ez) const
    {
        const glm::dvec2 l = local(ex, ez);
        const double y = mH.heightAt(l.x, l.y);
        return glm::dot(glm::dvec3(mWorld * glm::dvec4(l.x, y, l.y, 1.0)), mE.y);
    }
    double mask(const std::string& name, double ex, double ez) const
    {
        const glm::dvec2 l = local(ex, ez);
        return mH.maskAt(name, l.x, l.y);
    }
    bool inside(double ex, double ez) const
    {
        const glm::dvec2 l = local(ex, ez);
        const double eps = mH.cell * 1e-6;
        const glm::dvec2 e = mH.extent();
        return l.x >= mH.origin.x - eps && l.y >= mH.origin.y - eps && l.x <= mH.origin.x + e.x + eps && l.y <= mH.origin.y + e.y + eps;
    }

private:
    glm::dvec2 ground(const glm::dvec3& w) const
    {
        const glm::dvec3 e = engine(w);
        return {e.x, e.z};
    }

    const Heightfield& mH;
    glm::dmat4 mWorld, mInv;
    EngineAxes mE;
    glm::dvec2 mMin, mMax;
};

json unrealRotation(const glm::dmat4& world)
{
    auto unreal = [](const glm::dvec3& v) {
        const math::Vector u = math::toUnreal(math::Vector(v.x, v.y, v.z));
        return glm::dvec3(u.x, u.y, u.z);
    };
    const glm::dvec3 colX(world[0]), colY(world[1]), colZ(world[2]);
    const glm::dvec3 ux = glm::normalize(unreal(colZ)), uy = glm::normalize(unreal(-colX)), uz = glm::normalize(unreal(colY));
    // FMatrix::Rotator's decomposition, as exportUnreal writes it.
    const double yaw = std::atan2(ux.y, ux.x), pitch = std::atan2(ux.z, std::sqrt(ux.x * ux.x + ux.y * ux.y));
    const glm::dvec3 sy(-std::sin(yaw), std::cos(yaw), 0.0);
    const double roll = std::atan2(glm::dot(uz, sy), glm::dot(uy, sy));
    constexpr double kDeg = 57.29577951308232;
    return json::array({pitch * kDeg, yaw * kDeg, roll * kDeg});
}

void warn(json& warnings, const std::string& what, const std::string& text)
{
    warnings.push_back(text);
    log::warn("{}: {}", what, text);
}

std::string fileName(const fs::path& p) { return p.filename().generic_string(); }
} // namespace

// ---------------------------------------------------------------------------------------------------- tiles

std::vector<TileRect> tileGrid(int nx, int nz, int tileSize, bool shared)
{
    std::vector<TileRect> out;
    if (nx < 1 || nz < 1)
        return out;
    const int size = std::max(shared ? 2 : 1, tileSize);
    const int step = shared ? size - 1 : size;
    auto starts = [&](int n) {
        std::vector<int> s;
        for (int x = 0;; x += step)
        {
            s.push_back(x);
            if (x + size >= n)
                break;
        }
        return s;
    };
    const std::vector<int> xs = starts(nx), zs = starts(nz);
    for (int j = 0; j < int(zs.size()); ++j)
        for (int i = 0; i < int(xs.size()); ++i)
            out.push_back({i, j, xs[size_t(i)], zs[size_t(j)], std::min(size, nx - xs[size_t(i)]), std::min(size, nz - zs[size_t(j)])});
    return out;
}

std::string tileSuffix(int ix, int iz) { return "_x" + std::to_string(ix) + "_y" + std::to_string(iz); }

Heightfield cropHeightfield(const Heightfield& h, const TileRect& r)
{
    Heightfield t = geo::makeHeightfield(r.nx, r.nz, h.cell, h.samplePos(r.x0, r.z0));
    for (int z = 0; z < r.nz; ++z)
        for (int x = 0; x < r.nx; ++x)
            t.heights[t.index(x, z)] = h.at(r.x0 + x, r.z0 + z);
    for (const auto& [name, values] : h.masks)
    {
        if (values.size() != h.size())
            continue;
        std::vector<float>& m = t.mask(name);
        for (int z = 0; z < r.nz; ++z)
            for (int x = 0; x < r.nx; ++x)
                m[t.index(x, z)] = values[h.index(r.x0 + x, r.z0 + z)];
    }
    return t;
}

// ---------------------------------------------------------------------------------------------------- files

json exportFilesTiled(const Heightfield& h, const fs::path& folder, const std::string& name, int tileSize, bool shared)
{
    LILA_ZONE();

    if (h.empty())
        throw std::runtime_error("the terrain is empty");
    if (tileSize < 2)
        throw std::runtime_error("tileSize is at least 2 samples");
    fs::create_directories(folder);
    const glm::dvec2 r = h.range();
    const double span = std::max(1e-6, r.y - r.x);
    const std::vector<TileRect> grid = tileGrid(h.nx, h.nz, tileSize, shared);
    std::vector<json> tiles(grid.size());
    tbb::parallel_for(size_t(0), grid.size(), [&](size_t k) {
        const TileRect& t = grid[k];
        const Heightfield c = cropHeightfield(h, t);
        const std::string stem = name + tileSuffix(t.ix, t.iz);
        const fs::path exr = folder / (stem + ".exr"), png = folder / (stem + ".png"), r16 = folder / (stem + ".r16");
        writeGrayImage(exr, c.nx, c.nz, c.heights, 32);
        std::vector<float> norm(c.size());
        std::vector<uint16_t> raw(c.size());
        for (size_t i = 0; i < c.size(); ++i)
        {
            // The same rounding as the untiled files, so a tile's samples match them exactly.
            norm[i] = float((c.heights[i] - r.x) / span);
            raw[i] = uint16_t(std::lround(std::clamp(norm[i], 0.0f, 1.0f) * 65535.0f));
        }
        writeGrayImage(png, c.nx, c.nz, norm, 16);
        writeBytes(r16, raw.data(), raw.size() * 2);
        json masks = json::object();
        for (const auto& [mask, values] : c.masks)
        {
            std::vector<float> m(values.size());
            for (size_t i = 0; i < m.size(); ++i)
                m[i] = std::clamp(values[i], 0.0f, 1.0f);
            const fs::path p = folder / (stem + "_" + mask + ".png");
            writeGrayImage(p, c.nx, c.nz, m, 16);
            masks[mask] = fileName(p);
        }
        const glm::dvec2 e = c.extent();
        json side = {{"tile", json::array({t.ix, t.iz})},
                     {"samples", json::array({t.x0, t.z0, t.nx, t.nz})},
                     {"resolution", json::array({c.nx, c.nz})},
                     {"cellSize", c.cell},
                     {"min", json::array({c.origin.x, c.origin.y})},
                     {"size", json::array({e.x, e.y})},
                     {"heightRange", json::array({r.x, r.y})},
                     {"tileHeightRange", json::array({c.range().x, c.range().y})},
                     {"exr", fileName(exr)},
                     {"png", fileName(png)},
                     {"r16", fileName(r16)},
                     {"masks", masks}};
        std::ofstream(folder / (stem + ".json")) << side.dump(2);
        side["json"] = stem + ".json";
        tiles[k] = std::move(side);
    });
    const glm::dvec2 e = h.extent();
    json out = {{"target", "files"},
                {"tiled", true},
                {"tileSize", tileSize},
                {"sharedEdges", shared},
                {"tileCount", json::array({grid.empty() ? 0 : grid.back().ix + 1, grid.empty() ? 0 : grid.back().iz + 1})},
                {"tiles", tiles},
                {"resolution", json::array({h.nx, h.nz})},
                {"cellSize", h.cell},
                {"min", json::array({h.origin.x, h.origin.y})},
                {"size", json::array({e.x, e.y})},
                {"heightRange", json::array({r.x, r.y})},
                {"layout",
                 "tiles are named _x<column>_y<row> from the -X, -Z corner; rows run along +X and advance along +Z; PNG and R16 map 0..1 to "
                 "heightRange (shared by every tile), EXR holds cm; samples is [x0, z0, nx, nz] in the whole grid"}};
    std::ofstream(folder / (name + ".json")) << out.dump(2);
    out["files"] = {{"settings", (folder / (name + ".json")).generic_string()}, {"folder", folder.generic_string()}};
    return out;
}

json writeSplatPngs(const Heightfield& h, const fs::path& folder, const std::string& name, const EngineLayers& l)
{
    LILA_ZONE();

    const auto layers = chosenLayers(h, l);
    fs::create_directories(folder);
    json out = json::array();
    for (size_t first = 0; first < layers.size(); first += 4)
    {
        std::vector<float> rgba(h.size() * 4, 0.0f);
        json channels = json::array();
        for (size_t c = 0; c < 4 && first + c < layers.size(); ++c)
        {
            const std::vector<float>& m = *h.findMask(layers[first + c].first);
            for (size_t k = 0; k < h.size(); ++k)
                rgba[k * 4 + c] = std::clamp(m[k], 0.0f, 1.0f);
            channels.push_back({{"channel", std::string(1, "RGBA"[c])}, {"mask", layers[first + c].first}, {"layer", layers[first + c].second}});
        }
        const fs::path p = folder / (name + "_splat" + std::to_string(first / 4) + ".png");
        image::WriteOptions o;
        o.bits = 8;
        image::write(p, {h.nx, h.nz, std::move(rgba)}, o);
        out.push_back({{"file", p.generic_string()}, {"channels", channels}});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- Unity

bool validUnityResolution(int r) { return r >= 33 && r <= 4097 && ((r - 1) & (r - 2)) == 0; }

json exportUnity(const Heightfield& h, const glm::dmat4& world, const fs::path& folder, const std::string& name, const UnityOptions& o)
{
    LILA_ZONE();

    if (h.empty())
        throw std::runtime_error("the terrain is empty");
    if (!validUnityResolution(o.maxResolution))
        throw std::runtime_error("maxResolution must be 2^n + 1 from 33 to 4097");
    if (o.resolution && !validUnityResolution(o.resolution))
        throw std::runtime_error("resolution must be 2^n + 1 from 33 to 4097 (33, 65, 129, 257, 513, 1025, 2049, 4097)");
    const EngineSampler s(h, world, unityAxes());
    if (!s.upright)
        throw std::runtime_error("the terrain is tilted; a Unity terrain can only face up");
    json warnings = json::array();
    if (!s.aligned)
        warn(warnings, "terrainExport", "Unity terrains can't rotate: the tiles cover the rotated terrain's bounding box, its edges carried out to fill it");

    const glm::dvec2 size = s.max() - s.min();
    const int needX = int(std::ceil(size.x / s.worldCell - 1e-6)) + 1, needZ = int(std::ceil(size.y / s.worldCell - 1e-6)) + 1;
    int res = o.resolution;
    if (!res)
    {
        res = 33;
        while (res < std::max(needX, needZ) && res < o.maxResolution)
            res = (res - 1) * 2 + 1;
    }
    const int tilesX = std::max(1, (needX - 1 + res - 2) / (res - 1)), tilesZ = std::max(1, (needZ - 1 + res - 2) / (res - 1));
    const int tx = tilesX * (res - 1) + 1, tz = tilesZ * (res - 1) + 1;
    const double cellX = size.x / (tx - 1), cellZ = size.y / (tz - 1);

    std::vector<float> grid(size_t(tx) * size_t(tz));
    tbb::parallel_for(0, tz, [&](int j) {
        for (int i = 0; i < tx; ++i)
            grid[size_t(j) * size_t(tx) + size_t(i)] = float(s.height(s.min().x + i * cellX, s.min().y + j * cellZ));
    });
    const auto [lo, hi] = std::minmax_element(grid.begin(), grid.end());
    const double low = *lo, span = std::max(1.0, double(*hi) - low);

    const auto chosen = chosenLayers(h, o.layers);
    json layers = json::array();
    if (!chosen.empty() && o.layers.baseLayer)
        layers.push_back({{"name", "Base"}, {"mask", ""}});
    for (const auto& [mask, layer] : chosen)
        layers.push_back({{"name", layer}, {"mask", mask}});
    const int alpha = chosen.empty() ? 0 : res - 1;

    fs::create_directories(folder);
    std::vector<json> tiles(size_t(tilesX) * size_t(tilesZ));
    tbb::parallel_for(0, tilesX * tilesZ, [&](int k) {
        const int ti = k % tilesX, tj = k / tilesX;
        const std::string stem = name + tileSuffix(ti, tj);
        std::vector<uint16_t> raw(size_t(res) * size_t(res));
        for (int r = 0; r < res; ++r)
            for (int c = 0; c < res; ++c)
                raw[size_t(r) * size_t(res) + size_t(c)] =
                    to16((grid[size_t(tj * (res - 1) + r) * size_t(tx) + size_t(ti * (res - 1) + c)] - low) / span);
        const fs::path heightFile = folder / (stem + ".raw");
        writeBytes(heightFile, raw.data(), raw.size() * 2);
        const double x0 = s.min().x + ti * (res - 1) * cellX, z0 = s.min().y + tj * (res - 1) * cellZ;
        json tile = {{"x", ti}, {"y", tj}, {"heightmap", fileName(heightFile)}, {"position", json::array({x0 / 100.0, low / 100.0, z0 / 100.0})}};
        if (alpha > 0)
        {
            const size_t count = layers.size();
            std::vector<uint8_t> splat(size_t(alpha) * size_t(alpha) * count);
            std::vector<double> rawW(chosen.size()), w;
            const double ax = (res - 1) * cellX / (alpha - 1), az = (res - 1) * cellZ / (alpha - 1);
            for (int r = 0; r < alpha; ++r)
                for (int c = 0; c < alpha; ++c)
                {
                    for (size_t m = 0; m < chosen.size(); ++m)
                        rawW[m] = s.mask(chosen[m].first, x0 + c * ax, z0 + r * az);
                    layerWeights(rawW, o.layers.baseLayer, w);
                    for (size_t m = 0; m < count; ++m)
                        splat[(size_t(r) * size_t(alpha) + size_t(c)) * count + m] = uint8_t(std::lround(w[m] * 255.0));
                }
            const fs::path splatFile = folder / (stem + "_splat.raw");
            writeBytes(splatFile, splat.data(), splat.size());
            tile["splatmap"] = fileName(splatFile);
        }
        else
            tile["splatmap"] = "";
        tiles[size_t(k)] = std::move(tile);
    });

    json out = {{"target", "unity"},
                {"name", name},
                {"units", "m"},
                {"heightmapResolution", res},
                {"alphamapResolution", alpha},
                {"tileCount", json::array({tilesX, tilesZ})},
                {"tileSize", json::array({(res - 1) * cellX / 100.0, span / 100.0, (res - 1) * cellZ / 100.0})},
                {"heightRange", json::array({low, low + span})},
                {"cellSize", json::array({cellX, cellZ})},
                {"resampled", std::abs(cellX - s.worldCell) > 1e-6 * s.worldCell || std::abs(cellZ - s.worldCell) > 1e-6 * s.worldCell || !s.aligned},
                {"layers", layers},
                {"tiles", tiles},
                {"byteOrder", "little"},
                {"layout",
                 "heightmaps are heightmapResolution square, 16-bit little-endian, rows along Unity +Z from each tile's position, columns along "
                 "+X, 0..65535 over tileSize.y; neighbouring tiles repeat their shared edge; splatmaps are alphamapResolution square, 8-bit, "
                 "[row][column][layer] in the layers' order"},
                {"warnings", warnings}};
    const fs::path settings = folder / (name + ".json");
    std::ofstream(settings) << out.dump(2);
    json files = {{"settings", settings.generic_string()}};
    if (!o.script.empty())
    {
        if (!fs::exists(o.script))
            throw std::runtime_error("the Unity importer script is missing from the extension: " + o.script.generic_string());
        const fs::path script = folder / "LilaTerrainImporter.cs";
        fs::copy_file(o.script, script, fs::copy_options::overwrite_existing);
        files["importer"] = script.generic_string();
    }
    out["files"] = files;
    return out;
}

// ---------------------------------------------------------------------------------------------------- Godot

json exportGodot(const Heightfield& h, const glm::dmat4& world, const fs::path& folder, const std::string& name, const GodotOptions& o)
{
    LILA_ZONE();

    if (h.empty())
        throw std::runtime_error("the terrain is empty");
    if (o.regionSize && (o.regionSize < 64 || o.regionSize > 2048 || (o.regionSize & (o.regionSize - 1))))
        throw std::runtime_error("regionSize must be 64, 128, 256, 512, 1024 or 2048");
    const EngineSampler s(h, world, godotAxes());
    if (!s.upright)
        throw std::runtime_error("the terrain is tilted; Terrain3D can only face up");
    json warnings = json::array();
    if (!s.aligned)
        warn(warnings, "terrainExport", "Terrain3D can't rotate: the regions cover the rotated terrain's bounding box, outside it marked as holes");

    // Terrain3D's vertices sit at whole multiples of the vertex spacing, its regions on a 32 x 32 map around the origin.
    const double spacing = s.worldCell;
    if (spacing < 25.0 || spacing > 10000.0)
        throw std::runtime_error("Terrain3D's vertex spacing runs from 0.25 to 100 m; this terrain's cell is " + std::to_string(spacing / 100.0) + " m");
    const int k0x = int(std::ceil(s.min().x / spacing - 1e-6)), k1x = int(std::floor(s.max().x / spacing + 1e-6));
    const int k0z = int(std::ceil(s.min().y / spacing - 1e-6)), k1z = int(std::floor(s.max().y / spacing + 1e-6));
    auto regionOf = [](int k, int size) { return k >= 0 ? k / size : -((-k + size - 1) / size); };
    auto fits = [&](int size) {
        return regionOf(k0x, size) >= -16 && regionOf(k1x, size) <= 15 && regionOf(k0z, size) >= -16 && regionOf(k1z, size) <= 15;
    };
    int rs = o.regionSize;
    if (!rs)
        for (rs = 256; rs < 2048 && !fits(rs); rs *= 2)
            ;
    if (!fits(rs))
        throw std::runtime_error("the terrain reaches past Terrain3D's 32 x 32 region map at region size " + std::to_string(rs) +
                                 "; use a larger regionSize or move it nearer the origin");
    const bool onLattice = std::abs(s.min().x / spacing - std::round(s.min().x / spacing)) < 1e-6 &&
                           std::abs(s.min().y / spacing - std::round(s.min().y / spacing)) < 1e-6 && s.aligned;

    const auto chosen = chosenLayers(h, o.layers);
    json layers = json::array();
    if (!chosen.empty() && o.layers.baseLayer)
        layers.push_back({{"name", "Base"}, {"mask", ""}});
    for (const auto& [mask, layer] : chosen)
        layers.push_back({{"name", layer}, {"mask", mask}});
    if (layers.size() > 32)
        throw std::runtime_error("Terrain3D takes 32 texture layers; pick masks with layers=");

    fs::create_directories(folder);
    std::vector<std::pair<int, int>> regions;
    for (int rz = regionOf(k0z, rs); rz <= regionOf(k1z, rs); ++rz)
        for (int rx = regionOf(k0x, rs); rx <= regionOf(k1x, rs); ++rx)
            regions.emplace_back(rx, rz);
    std::vector<json> outRegions(regions.size());
    std::atomic<double> lowest{std::numeric_limits<double>::max()}, highest{-std::numeric_limits<double>::max()};
    tbb::parallel_for(size_t(0), regions.size(), [&](size_t k) {
        const auto [rx, rz] = regions[k];
        const std::string stem = name + tileSuffix(rx, rz);
        std::vector<float> heights(size_t(rs) * size_t(rs) * 4, 1.0f);
        std::vector<std::vector<float>> splats((layers.size() + 3) / 4, std::vector<float>(size_t(rs) * size_t(rs) * 4, 0.0f));
        std::vector<double> rawW(chosen.size()), w;
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        for (int pz = 0; pz < rs; ++pz)
            for (int px = 0; px < rs; ++px)
            {
                const double ex = double(rx * rs + px) * spacing, ez = double(rz * rs + pz) * spacing;
                const double y = s.height(ex, ez) / 100.0;
                const size_t p = size_t(pz) * size_t(rs) + size_t(px);
                heights[p * 4] = heights[p * 4 + 1] = heights[p * 4 + 2] = float(y);
                if (s.inside(ex, ez))
                {
                    lo = std::min(lo, y);
                    hi = std::max(hi, y);
                }
                if (layers.empty())
                    continue;
                for (size_t m = 0; m < chosen.size(); ++m)
                    rawW[m] = s.mask(chosen[m].first, ex, ez);
                layerWeights(rawW, o.layers.baseLayer, w);
                for (size_t m = 0; m < w.size(); ++m)
                    splats[m / 4][p * 4 + m % 4] = float(w[m]);
            }
        const fs::path exr = folder / (stem + ".exr");
        image::WriteOptions eo;
        eo.bits = 32;
        image::write(exr, {rs, rs, std::move(heights)}, eo);
        json splatFiles = json::array();
        for (size_t i = 0; i < splats.size(); ++i)
        {
            const fs::path png = folder / (stem + "_splat" + std::to_string(i) + ".png");
            image::WriteOptions po;
            po.bits = 8;
            image::write(png, {rs, rs, std::move(splats[i])}, po);
            splatFiles.push_back(fileName(png));
        }
        for (double cur = lowest.load(); lo < cur && !lowest.compare_exchange_weak(cur, lo);)
            ;
        for (double cur = highest.load(); hi > cur && !highest.compare_exchange_weak(cur, hi);)
            ;
        outRegions[k] = {{"x", rx}, {"y", rz}, {"heightmap", fileName(exr)}, {"splats", splatFiles},
                         {"position", json::array({double(rx * rs) * spacing / 100.0, 0.0, double(rz * rs) * spacing / 100.0})}};
    });

    json out = {{"target", "godot"},
                {"name", name},
                {"units", "m"},
                {"regionSize", rs},
                {"vertexSpacing", spacing / 100.0},
                {"regions", outRegions},
                {"footprint", json::array({s.min().x / 100.0, s.min().y / 100.0, s.max().x / 100.0, s.max().y / 100.0})},
                {"heightRange", json::array({lowest.load(), highest.load()})},
                {"layers", layers},
                {"resampled", !onLattice},
                {"layout",
                 "one EXR per Terrain3D region, regionSize square, heights in metres in red; pixel (x, y) is the vertex at (position.x + x * "
                 "vertexSpacing, position.z + y * vertexSpacing); splat PNGs carry the layers' weights four per image (RGBA) in the layers' "
                 "order; outside footprint is a hole"},
                {"warnings", warnings}};
    const fs::path settings = folder / (name + ".json");
    std::ofstream(settings) << out.dump(2);
    json files = {{"settings", settings.generic_string()}};
    if (!o.script.empty())
    {
        if (!fs::exists(o.script))
            throw std::runtime_error("the Terrain3D importer script is missing from the extension: " + o.script.generic_string());
        const std::vector<char> src = readBytes(o.script);
        std::string text(src.begin(), src.end());
        const std::string token = "%TERRAIN_JSON%";
        if (const size_t at = text.find(token); at != std::string::npos)
            text.replace(at, token.size(), fileName(settings));
        const fs::path script = folder / (name + "_terrain3d.gd");
        writeBytes(script, text.data(), text.size());
        files["importer"] = script.generic_string();
    }
    out["files"] = files;
    return out;
}

// ---------------------------------------------------------------------------------------------------- mesh tiles

namespace
{
/** The tile's samples as quads facing up, uv 0..1 over the whole terrain, masks as point attributes. */
geo::Mesh tileQuads(const Heightfield& h, const TileRect& r)
{
    std::vector<glm::dvec3> points(size_t(r.nx) * size_t(r.nz));
    for (int j = 0; j < r.nz; ++j)
        for (int i = 0; i < r.nx; ++i)
            points[size_t(j) * size_t(r.nx) + size_t(i)] = h.position(r.x0 + i, r.z0 + j);
    std::vector<geo::Index> sizes(size_t(r.nx - 1) * size_t(r.nz - 1), 4), idx;
    idx.reserve(sizes.size() * 4);
    auto id = [&](int i, int j) { return geo::Index(j * r.nx + i); };
    for (int j = 0; j + 1 < r.nz; ++j)
        for (int i = 0; i + 1 < r.nx; ++i)
            for (geo::Index p : {id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i + 1, j)})
                idx.push_back(p);
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, idx);
    for (const auto& [name, values] : h.masks)
    {
        if (values.size() != h.size())
            continue;
        std::span<float> dst = m.pointAttrs().getOrAdd<float>(name);
        for (int j = 0; j < r.nz; ++j)
            for (int i = 0; i < r.nx; ++i)
                dst[size_t(j) * size_t(r.nx) + size_t(i)] = values[h.index(r.x0 + i, r.z0 + j)];
    }
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    const double ux = 1.0 / std::max(1, h.nx - 1), uz = 1.0 / std::max(1, h.nz - 1);
    for (geo::Index c = 0; c < geo::Index(uv.size()); ++c)
    {
        const geo::Index p = m.cornerPoint(c);
        uv[c] = {float((r.x0 + int(p) % r.nx) * ux), float((r.z0 + int(p) / r.nx) * uz)};
    }
    return m;
}

/** Largest height difference, cm, between the mesh seen from above and the tile's samples. */
double heightError(const Heightfield& h, const TileRect& r, const geo::Mesh& m)
{
    const Heightfield back = geo::meshToHeightfield(m, r.nx, r.nz, h.cell, h.samplePos(r.x0, r.z0), std::numeric_limits<float>::quiet_NaN());
    double worst = 0.0;
    for (int z = 0; z < r.nz; ++z)
        for (int x = 0; x < r.nx; ++x)
            if (const float y = back.at(x, z); !std::isnan(y))
                worst = std::max(worst, std::abs(double(y) - h.at(r.x0 + x, r.z0 + z)));
    return worst;
}

/** A strip hung depth straight down from every open edge, the edge's uvs and point attributes carried down with it. */
void addSkirt(geo::Mesh& m, double depth)
{
    struct Edge
    {
        geo::Index face, c;
    };
    std::vector<Edge> edges;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e) && m.edgeCorner(e) != geo::kInvalid)
            edges.push_back({m.cornerFace(m.edgeCorner(e)), m.edgeCorner(e)});
    std::map<geo::Index, geo::Index> below;
    auto lowered = [&](geo::Index p) {
        auto [it, fresh] = below.try_emplace(p, geo::kInvalid);
        if (fresh)
        {
            it->second = m.addPoint(m.position(p) - glm::dvec3(0.0, depth, 0.0));
            m.pointAttrs().copy(it->second, p);
        }
        return it->second;
    };
    for (const Edge& e : edges)
    {
        const geo::Index cp = e.c, cq = m.cornerNext(e.c);
        const geo::Index p = m.cornerPoint(cp), q = m.cornerPoint(cq);
        const geo::Index pts[] = {q, p, lowered(p), lowered(q)};
        const geo::Index src[] = {cq, cp, cp, cq};
        m.addFace(pts, e.face, src);
    }
}
} // namespace

std::vector<MeshTile> buildMeshTiles(const Heightfield& h, const MeshTileOptions& o)
{
    LILA_ZONE();

    if (h.empty())
        throw std::runtime_error("the terrain is empty");
    const std::vector<TileRect> grid = tileGrid(h.nx, h.nz, std::max(2, o.tileSize), true);
    std::vector<MeshTile> out(grid.size());
    tbb::parallel_for(size_t(0), grid.size(), [&](size_t k) {
        MeshTile& t = out[k];
        t.rect = grid[k];
        geo::Mesh lod0 = tileQuads(h, t.rect);
        const double tris0 = double(lod0.numFaces()) * 2.0;
        t.lods.push_back(lod0);
        t.errors.push_back(0.0);
        for (int level = 1; level < std::max(1, o.lods); ++level)
        {
            geo::ReduceOptions ro;
            ro.keepBorders = true;
            ro.targetTriangles = geo::Index(std::max(2.0, std::round(tris0 * std::pow(std::clamp(o.lodRatio, 0.001, 1.0), level))));
            t.lods.push_back(geo::reduceMesh(lod0, ro).mesh);
            t.errors.push_back(heightError(h, t.rect, t.lods.back()));
        }
        if (o.collision)
        {
            geo::ReduceOptions ro;
            ro.keepBorders = true;
            ro.targetTriangles = geo::Index(std::max(2.0, std::round(tris0 * std::clamp(o.collisionRatio, 0.001, 1.0))));
            t.collision = geo::reduceMesh(lod0, ro).mesh;
        }
        if (o.skirt > 0.0)
            for (geo::Mesh& m : t.lods)
                addSkirt(m, o.skirt);
    });
    return out;
}

// ---------------------------------------------------------------------------------------------------- Unreal Mesh Terrain

void writeGray16Png(const fs::path& path, int width, int height, const std::vector<uint16_t>& values)
{
    if (width <= 0 || height <= 0 || values.size() != size_t(width) * size_t(height))
        throw std::runtime_error("writeGray16Png: bad size");
    const size_t stride = size_t(width) * 2 + 1;
    std::vector<unsigned char> rows(stride * size_t(height));
    for (int y = 0; y < height; ++y)
    {
        unsigned char* row = rows.data() + size_t(y) * stride;
        row[0] = 0;
        for (int x = 0; x < width; ++x)
        {
            const uint16_t v = values[size_t(y) * size_t(width) + size_t(x)];
            row[1 + x * 2] = uint8_t(v >> 8);
            row[2 + x * 2] = uint8_t(v & 0xff);
        }
    }
    mz_ulong packedSize = mz_compressBound(mz_ulong(rows.size()));
    std::vector<unsigned char> packed(packedSize);
    if (mz_compress2(packed.data(), &packedSize, rows.data(), mz_ulong(rows.size()), 6) != MZ_OK)
        throw std::runtime_error("writeGray16Png: deflate failed");
    packed.resize(packedSize);

    std::string png("\x89PNG\r\n\x1a\n", 8);
    auto be32 = [](std::string& s, uint32_t v) {
        for (int k = 3; k >= 0; --k)
            s += char((v >> (k * 8)) & 0xff);
    };
    auto chunk = [&](const char* type, const unsigned char* data, size_t n) {
        be32(png, uint32_t(n));
        std::string body(type, 4);
        body.append(reinterpret_cast<const char*>(data), n);
        png += body;
        be32(png, uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(body.data()), body.size())));
    };
    std::string ihdr;
    be32(ihdr, uint32_t(width));
    be32(ihdr, uint32_t(height));
    ihdr += char(16); // bit depth
    ihdr += char(0);  // greyscale
    ihdr += std::string(3, '\0');
    chunk("IHDR", reinterpret_cast<const unsigned char*>(ihdr.data()), ihdr.size());
    chunk("IDAT", packed.data(), packed.size());
    chunk("IEND", nullptr, 0);
    writeBytes(path, png.data(), png.size());
}

json exportUnrealMeshTerrain(const Heightfield& h, const glm::dmat4& world, const fs::path& folder, const std::string& name, const MeshTerrainOptions& o)
{
    LILA_ZONE();

    if (h.empty())
        throw std::runtime_error("the terrain is empty");
    // Unreal X runs along Lila +Z and Unreal Y along Lila -X, as for landscapes.
    const int quadsX = h.nz - 1, quadsY = h.nx - 1;
    auto vertex = [&](int k, int l) { return double(h.at(h.nx - 1 - l, k)); };
    const glm::dvec2 r = h.range();
    const double span = std::max(1e-3, r.y - r.x);

    // FHeightmapImporter reads pixels as cells (centre at (i + 0.5) / W) and samples vertex k at k / M across. With
    // W = 2M, vertex k (0 < k) lands midway between pixels 2k - 1 and 2k, both holding its height; pixel 0 is set so
    // the importer's extrapolation at vertex 0 gives its height too.
    auto pixelsOf = [](int quads) {
        std::vector<std::array<std::pair<int, double>, 2>> map(size_t(quads) * 2);
        map[0] = {{{0, 2.0 / 3.0}, {std::min(1, quads), 1.0 / 3.0}}};
        for (int i = 1; i < quads * 2; ++i)
            map[size_t(i)] = {{{(i + 1) / 2, 1.0}, {0, 0.0}}};
        return map;
    };
    const auto mx = pixelsOf(quadsX), my = pixelsOf(quadsY);
    const int w = quadsX * 2, ht = quadsY * 2;
    std::vector<uint16_t> pixels(size_t(w) * size_t(ht));
    tbb::parallel_for(0, ht, [&](int j) {
        for (int i = 0; i < w; ++i)
        {
            double v = 0.0;
            for (const auto& [l, wy] : my[size_t(j)])
                if (wy != 0.0)
                    for (const auto& [k, wx] : mx[size_t(i)])
                        if (wx != 0.0)
                            v += wx * wy * vertex(k, l);
            pixels[size_t(j) * size_t(w) + size_t(i)] = to16((v - r.x) / span);
        }
    });

    int sectionQuads = std::max(8, o.sectionQuads);
    auto sectionsOf = [&](int quads) { return std::max(1, (quads + sectionQuads - 1) / sectionQuads); };
    while (sectionsOf(quadsX) * sectionsOf(quadsY) > 4096)
        sectionQuads *= 2;

    fs::create_directories(folder);
    const fs::path png = folder / (name + "_meshterrain.png");
    writeGray16Png(png, w, ht, pixels);

    const glm::dvec3 colX(world[0]), colY(world[1]), colZ(world[2]);
    const glm::dvec2 e = h.extent();
    const glm::dvec3 centre(world * glm::dvec4(h.origin.x + e.x * 0.5, r.x, h.origin.y + e.y * 0.5, 1.0));
    const math::Vector loc = math::toUnreal(math::Vector(centre.x, centre.y, centre.z));
    json out = {{"target", "unrealMeshTerrain"},
                {"meshResolution", json::array({quadsX, quadsY})},
                {"sections", json::array({sectionsOf(quadsX), sectionsOf(quadsY)})},
                {"sectionQuads", sectionQuads},
                {"meshSize", json::array({e.y * glm::length(colZ), e.x * glm::length(colX), span * glm::length(colY)})},
                {"location", json::array({loc.x, loc.y, loc.z})},
                {"rotation", unrealRotation(world)},
                {"heightRange", json::array({r.x, r.y})},
                {"pngSize", json::array({w, ht})},
                {"cellSize", h.cell},
                {"layout",
                 "16-bit greyscale PNG, pngSize = 2 x meshResolution; FHeightmapImporter builds a mesh centred on the origin from Z 0 to "
                 "meshSize.z, X along Lila +Z and Y along Lila -X; location is that centre's place (the terrain's middle at its lowest "
                 "height) and rotation its turn, in Unreal axes"}};
    out["files"] = {{"heightmap", png.generic_string()}, {"settings", (folder / (name + "_meshterrain.json")).generic_string()}};
    std::ofstream(folder / (name + "_meshterrain.json")) << out.dump(2);
    return out;
}

json splitUnrealRegions(json settings, int regionQuads)
{
    LILA_ZONE();

    if (regionQuads < 1)
        throw std::runtime_error("regions need at least one quad");
    const int vx = settings["resolution"][0].get<int>(), vy = settings["resolution"][1].get<int>();
    const fs::path heightFile = settings["files"]["height"].get<std::string>();
    const fs::path folder = heightFile.parent_path();
    const std::string stem = heightFile.stem().string();
    const std::vector<char> heights = readBytes(heightFile);
    if (heights.size() != size_t(vx) * size_t(vy) * 2)
        throw std::runtime_error(heightFile.generic_string() + " isn't " + std::to_string(vx) + " x " + std::to_string(vy) + " 16-bit");
    std::vector<std::pair<std::string, std::vector<char>>> layers;
    for (const json& l : settings.value("layers", json::array()))
        layers.emplace_back(l["layer"].get<std::string>(), readBytes(l["file"].get<std::string>()));

    const int rx = std::max(1, (vx - 1 + regionQuads - 1) / regionQuads), ry = std::max(1, (vy - 1 + regionQuads - 1) / regionQuads);
    std::vector<json> regions(size_t(rx) * size_t(ry));
    tbb::parallel_for(0, rx * ry, [&](int k) {
        const int i = k % rx, j = k / rx;
        const int x0 = i * regionQuads, y0 = j * regionQuads, x1 = std::min(x0 + regionQuads, vx - 1), y1 = std::min(y0 + regionQuads, vy - 1);
        const int w = x1 - x0 + 1, hgt = y1 - y0 + 1;
        auto cut = [&](const std::vector<char>& src, int bytes, const fs::path& to) {
            std::vector<char> tile(size_t(w) * size_t(hgt) * size_t(bytes));
            for (int y = 0; y < hgt; ++y)
                std::memcpy(tile.data() + size_t(y) * size_t(w) * bytes, src.data() + (size_t(y0 + y) * size_t(vx) + size_t(x0)) * bytes, size_t(w) * bytes);
            writeBytes(to, tile.data(), tile.size());
        };
        const fs::path hf = folder / (stem + tileSuffix(i, j) + ".r16");
        cut(heights, 2, hf);
        json ls = json::array();
        for (const auto& [layer, bytes] : layers)
        {
            const fs::path lf = folder / (stem + "_" + layer + tileSuffix(i, j) + ".raw");
            cut(bytes, 1, lf);
            ls.push_back({{"name", layer}, {"weightmap", lf.generic_string()}});
        }
        regions[size_t(k)] = {{"x", i}, {"y", j}, {"rect", json::array({x0, y0, x1, y1})}, {"heightmap", hf.generic_string()}, {"layers", ls}};
    });
    settings["regions"] = regions;
    settings["regionQuads"] = regionQuads;
    settings["regionCount"] = json::array({rx, ry});
    return settings;
}

// ---------------------------------------------------------------------------------------------------- commands

namespace
{
EngineLayers layersFromArgs(const Args& a)
{
    EngineLayers l;
    l.baseLayer = a.get<bool>("baseLayer", true);
    if (!a.has("layers"))
        return l;
    const json v = a.flags["layers"];
    if (v.is_object())
        for (const auto& [mask, layer] : v.items())
        {
            l.masks.push_back(mask);
            l.names[mask] = layer.get<std::string>();
        }
    else if (v.is_array())
        for (const json& m : v)
            l.masks.push_back(m.get<std::string>());
    else
        throw std::runtime_error("layers is a list of masks or {mask: layer name}");
    if (l.masks.empty())
        l.masks.push_back("");
    return l;
}

MeshTileOptions meshOptions(const Args& a)
{
    MeshTileOptions o;
    if (const int64_t t = a.get<int64_t>("tileSize", 0); t > 0)
        o.tileSize = int(t);
    o.lods = int(std::clamp<int64_t>(a.get<int64_t>("lods", 3), 1, 8));
    o.lodRatio = a.get<double>("lodRatio", 0.25);
    o.skirt = a.get<double>("skirt", 0.0);
    o.collision = a.get<bool>("collision", true);
    o.collisionRatio = a.get<double>("collisionRatio", 0.1);
    return o;
}

/**
 * Scene nodes made only to hand meshes to the exporters: their creation and removal are dropped from the undo
 * history, so the export leaves neither the nodes nor a step behind.
 */
class ScratchEdits
{
public:
    ScratchEdits()
    {
        UndoStack& u = UndoStack::get();
        mOwn = !u.chunkOpen();
        if (mOwn)
            u.openChunk("Terrain Export");
        mMark = u.openEdits();
    }
    ~ScratchEdits()
    {
        UndoStack& u = UndoStack::get();
        if (mOwn)
            u.cancelChunk();
        else
            u.rollbackTo(mMark);
    }

private:
    bool mOwn = false;
    size_t mMark = 0;
};

struct TileNodes
{
    scene::NodeId root = scene::kNoNode;
    /** Per tile: the LOD group (or the single mesh) and the collision mesh. */
    std::vector<std::vector<scene::NodeId>> tiles;
    json described = json::array();
};

scene::NodeId makeNode(scene::Graph& g, const char* type, const std::string& name, scene::NodeId parent)
{
    const scene::NodeType* t = scene::types::find(type);
    if (!t)
        throw CommandError(std::string("terrain tiles: no node type ") + type);
    return g.createNode(*t, name, parent)->id();
}

scene::NodeId makeMesh(scene::Graph& g, const std::string& name, scene::NodeId parent, geo::Mesh m)
{
    const scene::NodeId id = makeNode(g, "mesh", name, parent);
    g.setValue({id, scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return id;
}

/** The tiles as scene meshes in world space under one group: a LOD group per tile when it has levels, and its collision mesh. */
TileNodes makeTileNodes(scene::Graph& g, std::vector<MeshTile>& tiles, const glm::dmat4& world, const std::string& name, const std::string& collisionSuffix)
{
    TileNodes out;
    out.root = makeNode(g, "transform", name, scene::kNoNode);
    for (MeshTile& t : tiles)
    {
        const std::string stem = name + tileSuffix(t.rect.ix, t.rect.iz);
        std::vector<scene::NodeId> nodes;
        json levels = json::array(), triangles = json::array();
        for (geo::Mesh& m : t.lods)
            gn::transformMesh(m, world);
        if (t.lods.size() > 1)
        {
            const scene::NodeId group = makeNode(g, "lodGroup", stem, out.root);
            nodes.push_back(group);
            for (size_t l = 0; l < t.lods.size(); ++l)
            {
                triangles.push_back(int64_t(geo::triangulate(t.lods[l]).faces.size()));
                const scene::NodeId lod = makeMesh(g, stem + "_LOD" + std::to_string(l), group, std::move(t.lods[l]));
                levels.push_back(g.fullPath(lod));
            }
        }
        else
        {
            triangles.push_back(int64_t(geo::triangulate(t.lods[0]).faces.size()));
            const scene::NodeId mesh = makeMesh(g, stem, out.root, std::move(t.lods[0]));
            nodes.push_back(mesh);
            levels.push_back(g.fullPath(mesh));
        }
        json tile = {{"x", t.rect.ix}, {"y", t.rect.iz}, {"node", g.fullPath(nodes[0])}, {"levels", levels}, {"triangles", triangles}, {"errors", t.errors}};
        if (!t.collision.empty())
        {
            gn::transformMesh(t.collision, world);
            tile["collisionTriangles"] = int64_t(geo::triangulate(t.collision).faces.size());
            const scene::NodeId col = makeMesh(g, stem + collisionSuffix, out.root, std::move(t.collision));
            nodes.push_back(col);
            tile["collision"] = g.fullPath(col);
        }
        out.tiles.push_back(nodes);
        out.described.push_back(std::move(tile));
    }
    return out;
}

json tileStats(const json& tiles)
{
    json triangles = json::array(), errors = json::array();
    int64_t collision = 0;
    for (const json& t : tiles)
    {
        for (size_t l = 0; l < t["triangles"].size(); ++l)
        {
            if (triangles.size() <= l)
            {
                triangles.push_back(int64_t(0));
                errors.push_back(0.0);
            }
            triangles[l] = triangles[l].get<int64_t>() + t["triangles"][l].get<int64_t>();
            errors[l] = std::max(errors[l].get<double>(), t["errors"][l].get<double>());
        }
        collision += t.value("collisionTriangles", int64_t(0));
    }
    return {{"tiles", tiles.size()}, {"trianglesPerLod", triangles}, {"maxErrorPerLod", errors}, {"collisionTriangles", collision}};
}

std::vector<std::string> formatsOf(const std::string& list)
{
    std::vector<std::string> out;
    std::stringstream ss(list);
    for (std::string f; std::getline(ss, f, ',');)
    {
        f.erase(std::remove_if(f.begin(), f.end(), [](unsigned char c) { return std::isspace(c); }), f.end());
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (f.empty())
            continue;
        if (f != "fbx" && f != "gltf" && f != "glb" && f != "usd" && f != "usda" && f != "usdc" && f != "usdz")
            throw std::runtime_error("format " + f + " isn't fbx, gltf, glb, usd, usda, usdc or usdz");
        out.push_back(f);
    }
    if (out.empty())
        throw std::runtime_error("give a format: fbx, gltf, glb or usd");
    return out;
}

json exportMeshTarget(const Args& a, const Heightfield& local, const glm::dmat4& world, const fs::path& folder, const std::string& name,
                      const std::string& defaultFormat, const std::string& collisionSuffix)
{
    LILA_ZONE();

    const std::vector<std::string> formats = formatsOf(a.get<std::string>("format", defaultFormat));
    for (const std::string& f : formats)
    {
        const char* cmd = f == "fbx" ? "exportFbx" : (f == "gltf" || f == "glb") ? "exportGltf" : "exportUsd";
        if (!ops::find(cmd))
            throw std::runtime_error(std::string(cmd) + " isn't available (the extension that writes ." + f + " is off)");
    }
    const MeshTileOptions mo = meshOptions(a);
    std::vector<MeshTile> tiles = buildMeshTiles(local, mo);
    const bool perTile = a.get<bool>("perTile", true);
    fs::create_directories(folder);

    scene::Graph& g = scene::Graph::current();
    json files = json::array(), described;
    {
        ScratchEdits scratch;
        TileNodes nodes = makeTileNodes(g, tiles, world, name, collisionSuffix);
        described = nodes.described;
        auto exportTo = [&](const std::string& format, const fs::path& path, const std::vector<scene::NodeId>& ids) {
            json args = json::array({path.generic_string()});
            for (scene::NodeId id : ids)
                args.push_back(g.fullPath(id));
            if (format == "fbx")
                ops::execute("exportFbx", {{"args", args}, {"kind", "static"}});
            else if (format == "gltf" || format == "glb")
                ops::execute("exportGltf", {{"args", args}, {"kind", "static"}});
            else
                ops::execute("exportUsd", {{"args", args}});
            files.push_back(path.generic_string());
        };
        try
        {
            for (const std::string& f : formats)
            {
                const std::string ext = f == "usd" ? "usd" : f;
                if (perTile)
                    for (size_t k = 0; k < tiles.size(); ++k)
                        exportTo(f, folder / (name + tileSuffix(tiles[k].rect.ix, tiles[k].rect.iz) + "." + ext), nodes.tiles[k]);
                else
                    exportTo(f, folder / (name + "." + ext), {nodes.root});
            }
        }
        catch (...)
        {
            g.deleteNode(nodes.root);
            throw;
        }
        g.deleteNode(nodes.root);
    }
    for (json& t : described)
    {
        t.erase("node");
        t.erase("levels");
        t.erase("collision");
    }
    json out = tileStats(described);
    out["target"] = "mesh";
    out["formats"] = formats;
    out["tileSize"] = mo.tileSize;
    out["lods"] = mo.lods;
    out["skirt"] = mo.skirt;
    out["collisionSuffix"] = mo.collision ? json(collisionSuffix) : json(nullptr);
    out["perTile"] = perTile;
    out["tileList"] = described;
    out["files"] = files;
    out["layout"] = "tiles are named _x<column>_y<row> from the terrain's -X, -Z corner and share their edge vertices at every LOD; points are in "
                    "world space (each file's units as its exporter writes them)";
    return out;
}

/** The link's client after checking it is connected and its LilaLink has the feature. */
std::shared_ptr<link::RpcClient> linkWith(const std::string& feature, const std::string& cmd, const std::string& hint)
{
    try
    {
        link::Link::get().ensureConnected();
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": not connected to Unreal: " + e.what());
    }
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    const json features = link::Link::get().status().value("features", json::array());
    if (!client || !features.is_array() || std::find(features.begin(), features.end(), json(feature)) == features.end())
        throw CommandError(cmd + ": " + hint);
    return client;
}
} // namespace

std::vector<FlagDef> engineExportFlags(std::vector<FlagDef> flags)
{
    for (FlagDef& f : flags)
        if (f.name == "target")
        {
            f.doc = "unreal, files, unity, godot or mesh.";
            for (const char* t : {"unity", "godot", "mesh"})
                if (std::find(f.choices.begin(), f.choices.end(), t) == f.choices.end())
                    f.choices.push_back(t);
        }
        else if (f.name == "layers")
            f.doc = "unreal, unity, godot, files splats: masks to send as paint layers, a list, or {mask: layer name}; default every mask.";
    const std::vector<FlagDef> more = {
        {"tileSize", "ts", T::Int, int64_t(0), "files: samples per tile side, 0 writes one set; mesh: samples per tile (257).", {}, {}, 0.0, 65536.0},
        {"sharedEdges", "se", T::Bool, false, "files: neighbouring tiles repeat their common edge; off, they abut as Unreal's tiled import reads them."},
        {"splats", "sp", T::Bool, false, "files: also write the masks packed four to an RGBA PNG (<name>_splat<k>.png)."},
        {"resolution", "res", T::Int, int64_t(0), "unity: heightmap resolution per tile, 2^n + 1 from 33 to 4097; 0 keeps the terrain's detail.", {}, {}, 0.0, 4097.0},
        {"maxResolution", "mres", T::Int, int64_t(4097), "unity: largest tile resolution picked when resolution is 0; bigger terrains become more tiles.", {}, {}, 33.0, 4097.0},
        {"baseLayer", "bl", T::Bool, true, "unity, godot: a Base layer first takes the weight the masks leave below 1."},
        {"regionSize", "rgs", T::Int, int64_t(0), "godot: Terrain3D region size, 64 to 2048; 0 picks from 256 up to fit its region map.", {}, {}, 0.0, 2048.0},
        {"mode", "md", T::String, "terrain3d", "godot: terrain3d (regions for the Terrain3D addon) or mesh (glTF tiles).", {"terrain3d", "mesh"}},
        {"format", "fmt", T::String, nullptr, "mesh: fbx, gltf, glb, usd, usda, usdc or usdz, comma separated for several; default fbx (glb for godot)."},
        {"lods", "lod", T::Int, int64_t(3), "mesh: levels per tile, LOD0 at full resolution.", {}, {}, 1.0, 8.0},
        {"lodRatio", "lr", T::Float, 0.25, "mesh: triangles each level keeps of the one before.", {}, {}, 0.001, 1.0},
        {"skirt", "sk", T::Float, 0.0, "mesh: depth of a skirt hung from every tile edge, cm; 0 for none.", {}, {}, 0.0},
        {"collision", "col", T::Bool, true, "mesh: write a reduced collision mesh per tile."},
        {"collisionRatio", "cr", T::Float, 0.1, "mesh: triangles the collision mesh keeps of LOD0.", {}, {}, 0.001, 1.0},
        {"perTile", "pt", T::Bool, true, "mesh: one file per tile; off writes every tile into one file."},
    };
    flags.insert(flags.end(), more.begin(), more.end());
    return flags;
}

std::vector<FlagDef> engineSendFlags(std::vector<FlagDef> flags)
{
    const std::vector<FlagDef> more = {
        {"kind", "k", T::String, "landscape", "landscape, or meshTerrain for UE 5.8's experimental Mesh Terrain (needs the LilaLinkMeshTerrain plugin).",
         {"landscape", "meshTerrain"}},
        {"regions", "rg", T::String, "auto",
         "landscape: write the heights region by region (bounded memory for huge maps); auto does above 8193 x 8193 vertices.",
         {"auto", "on", "off"}},
        {"regionComponents", "rc", T::Int, int64_t(0), "landscape regions: components per region side; 0 uses gridSize, else 16.", {}, {}, 0.0, 256.0},
        {"sectionQuads", "sq", T::Int, int64_t(256), "meshTerrain: quads per Mesh Partition section side (at most 4096 sections).", {}, {}, 8.0, 8192.0},
        {"saveAndUnload", "su", T::Bool, false,
         "meshTerrain in a World Partition level: save and unload sections as they are made (huge maps); placement is then left at the origin."},
    };
    flags.insert(flags.end(), more.begin(), more.end());
    return flags;
}

json exportEngineTarget(const Args& a, const Heightfield& local, const glm::dmat4& world, const fs::path& folder, const std::string& name)
{
    const std::string target = a.get<std::string>("target", "unreal");
    if (target == "unity")
    {
        UnityOptions o;
        o.resolution = int(a.get<int64_t>("resolution", 0));
        o.maxResolution = int(a.get<int64_t>("maxResolution", 4097));
        o.layers = layersFromArgs(a);
        o.script = gScripts.empty() ? fs::path() : gScripts / "LilaTerrainImporter.cs";
        json out = exportUnity(local, world, folder, name, o);
        if (gScripts.empty())
            warn(out["warnings"], "terrainExport", "the Unity importer script wasn't copied: the Terrain extension's resources weren't found");
        return out;
    }
    if (target == "godot")
    {
        if (a.get<std::string>("mode", "terrain3d") == "mesh")
            return exportMeshTarget(a, local, world, folder, name, "glb", "_colonly");
        GodotOptions o;
        o.regionSize = int(a.get<int64_t>("regionSize", 0));
        o.layers = layersFromArgs(a);
        o.script = gScripts.empty() ? fs::path() : gScripts / "lila_terrain3d_import.gd";
        json out = exportGodot(local, world, folder, name, o);
        if (gScripts.empty())
            warn(out["warnings"], "terrainExport", "the Terrain3D importer script wasn't copied: the Terrain extension's resources weren't found");
        return out;
    }
    if (target == "mesh")
        return exportMeshTarget(a, local, world, folder, name, "fbx", "_collision");
    if (target == "files" && (a.get<int64_t>("tileSize", 0) > 0 || a.get<bool>("splats", false)))
    {
        // Files go out in the terrain's own space with its world matrix, as the untiled files do when it is rotated.
        gn::Geometry geom;
        geom.heightfield = std::make_shared<Heightfield>(local);
        const gn::Geometry w = gn::transformed(geom, world);
        const bool inWorld = w.heightfield && math::upAxis() != math::UpAxis::Z;
        const Heightfield& h = inWorld ? *w.heightfield : local;
        json out;
        if (a.get<int64_t>("tileSize", 0) > 0)
            out = exportFilesTiled(h, folder, name, int(a.get<int64_t>("tileSize", 0)), a.get<bool>("sharedEdges", false));
        else
        {
            out = {{"target", "files"}, {"tiled", false}, {"files", {{"folder", folder.generic_string()}}}};
        }
        if (a.get<bool>("splats", false))
            out["splats"] = writeSplatPngs(h, folder, name, layersFromArgs(a));
        if (!inWorld)
        {
            json m = json::array();
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    m.push_back(world[c][r]);
            out["matrix"] = m;
        }
        return out;
    }
    return nullptr;
}

json sendEngineVariant(const Args& a, const Heightfield& local, const glm::dmat4& world, const std::string& id, const std::string& name, const fs::path& cache)
{
    LILA_ZONE();

    const std::string kind = a.get<std::string>("kind", "landscape");
    const int timeoutMs = int(std::clamp(a.get<double>("timeout", 300.0), 1.0, 3600.0) * 1000.0);
    if (kind == "meshTerrain")
    {
        std::shared_ptr<link::RpcClient> client = linkWith("meshTerrain", "terrainSendToUnreal",
                                                           "Mesh Terrain needs the LilaLinkMeshTerrain plugin: copy Lila's ue/LilaLinkMeshTerrain next to "
                                                           "LilaLink and enable it (it brings UE's experimental Mesh Partition plugin)");
        MeshTerrainOptions mo;
        mo.sectionQuads = int(a.get<int64_t>("sectionQuads", 256));
        json settings;
        try
        {
            settings = exportUnrealMeshTerrain(local, world, cache / "terrain", name, mo);
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string("terrainSendToUnreal: ") + e.what());
        }
        json params = {{"id", id},
                       {"label", name},
                       {"heightmap", settings["files"]["heightmap"]},
                       {"meshResolution", settings["meshResolution"]},
                       {"sections", settings["sections"]},
                       {"meshSize", settings["meshSize"]},
                       {"location", settings["location"]},
                       {"rotation", settings["rotation"]},
                       {"saveAndUnload", a.get<bool>("saveAndUnload", false)}};
        if (a.has("material") && !a.get<std::string>("material", "").empty())
            params["material"] = a.get<std::string>("material", "");
        link::RpcResult r = client->call("setMeshTerrain", params, timeoutMs);
        if (!r.ok)
            throw CommandError("terrainSendToUnreal: " + r.error);
        json out = r.result;
        out["kind"] = "meshTerrain";
        out["settings"] = settings;
        return out;
    }

    // The layout exportUnreal will choose, to decide on regions before writing anything.
    const UnrealLayout layout = chooseUnrealLayout(local.nz, local.nx);
    const std::string regions = a.get<std::string>("regions", "auto");
    const bool huge = double(layout.vertsX()) * double(layout.vertsY()) > 8193.0 * 8193.0;
    if (regions == "off" || (regions == "auto" && !huge))
        return nullptr;
    std::shared_ptr<link::RpcClient> client =
        linkWith("landscapeRegions", "terrainSendToUnreal", "the LilaLink plugin in Unreal predates region imports; rebuild it from Lila's ue/LilaLink");
    UnrealOptions uo;
    uo.pad = a.get<bool>("pad", false);
    const EngineLayers el = layersFromArgs(a);
    uo.masks = el.masks;
    uo.layerNames = el.names;
    LandscapeSend send;
    send.id = id;
    send.label = name;
    send.material = a.get<std::string>("material", "");
    send.makeMaterial = a.get<bool>("makeMaterial", true);
    send.gridSize = int(a.get<int64_t>("gridSize", 0));
    int components = int(a.get<int64_t>("regionComponents", 0));
    if (components <= 0)
        components = send.gridSize > 0 ? send.gridSize : 16;
    json settings, params;
    try
    {
        settings = exportUnreal(local, world, cache / "terrain", name, uo);
        settings = splitUnrealRegions(settings, components * settings["quadsPerSection"].get<int>() * settings["sectionsPerComponent"].get<int>());
        params = landscapeParams(settings, send);
        params["regions"] = settings["regions"];
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("terrainSendToUnreal: ") + e.what());
    }
    params.erase("heightmap");
    link::RpcResult r = client->call("setLandscapeRegions", params, timeoutMs);
    if (!r.ok)
        throw CommandError("terrainSendToUnreal: " + r.error);
    json out = r.result;
    out["kind"] = "landscape";
    out["regionCount"] = settings["regionCount"];
    out["regionReason"] = regions == "on" ? "asked" : "the landscape is over 8193 x 8193 vertices";
    settings.erase("regions");
    out["settings"] = settings;
    return out;
}

void setEngineScripts(const fs::path& folder)
{
    gScripts = folder;
    if (!gScripts.empty() && !fs::exists(gScripts))
    {
        log::warn("Terrain: no importer scripts at {}; Unity and Godot exports will go without them", gScripts.generic_string());
        gScripts.clear();
    }
}

json makeTerrainTiles(const Args& a, const Heightfield& local, const glm::dmat4& world, const std::string& name)
{
    std::vector<MeshTile> tiles;
    try
    {
        tiles = buildMeshTiles(local, meshOptions(a));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("terrainTiles: ") + e.what());
    }
    scene::Graph& g = scene::Graph::current();
    TileNodes nodes = makeTileNodes(g, tiles, world, name, "_collision");
    json out = tileStats(nodes.described);
    out["group"] = g.fullPath(nodes.root);
    out["tileList"] = nodes.described;
    return out;
}
} // namespace terrain
