// Terrain files for engines: raw 16-bit heights laid out and scaled the way Unreal's landscape import wants them, and
// plain height and mask images for any other pipeline.

#include "Export.h"

#include <lila/ImageIO.h>
#include <lila/Math.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace lila;

namespace terrain
{
namespace
{
using json = nlohmann::json;
namespace fs = std::filesystem;

void writeRaw(const fs::path& path, const void* data, size_t bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), std::streamsize(bytes));
    if (!f)
        throw std::runtime_error("can't write " + path.string());
}

void writeGray(const fs::path& path, int w, int h, const std::vector<float>& values, int bits)
{
    std::vector<float> rgba(size_t(w) * size_t(h) * 4);
    for (size_t k = 0; k < values.size(); ++k)
    {
        const float v = values[k];
        rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = v;
        rgba[k * 4 + 3] = 1.0f;
    }
    image::WriteOptions o;
    o.bits = bits;
    image::write(path, {w, h, std::move(rgba)}, o);
}

/** A Lila grid in Unreal's heightmap layout: columns run along Unreal X (Lila +Z), rows along Unreal Y (Lila -X). */
template <class F> std::vector<float> unrealOrder(const Heightfield& h, F value)
{
    const int w = h.nz, rows = h.nx;
    std::vector<float> out(size_t(w) * size_t(rows));
    tbb::parallel_for(0, rows, [&](int j) {
        for (int i = 0; i < w; ++i)
            out[size_t(j) * size_t(w) + size_t(i)] = value(h.index(h.nx - 1 - j, i));
    });
    return out;
}
} // namespace

UnrealLayout chooseUnrealLayout(int vertsX, int vertsY)
{
    UnrealLayout best;
    long long bestWaste = std::numeric_limits<long long>::max();
    for (int quads : {7, 15, 31, 63, 127, 255})
        for (int sections : {1, 2})
        {
            const int perComponent = quads * sections;
            const int cx = std::max(1, (vertsX - 1 + perComponent - 1) / perComponent);
            const int cy = std::max(1, (vertsY - 1 + perComponent - 1) / perComponent);
            if (cx > 64 || cy > 64)
                continue;
            const long long waste = (long long)(cx * perComponent + 1 - vertsX) + (long long)(cy * perComponent + 1 - vertsY);
            // Ties go to fewer, larger components: fewer draw calls and less per-component overhead.
            if (waste < bestWaste || (waste == bestWaste && cx * cy < best.componentsX * best.componentsY))
            {
                bestWaste = waste;
                best = {quads, sections, cx, cy};
            }
        }
    return best;
}

std::string unrealLayerName(const std::string& mask)
{
    std::string out;
    for (char c : mask)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "L" + out;
    return out;
}

json exportUnreal(const Heightfield& src, const glm::dmat4& world, const fs::path& folder, const std::string& name, const UnrealOptions& o)
{
    LILA_ZONE();

    if (src.empty())
        throw std::runtime_error("the terrain is empty");
    // Unreal's columns follow Lila Z and its rows Lila X, so the layout is chosen in that order.
    const UnrealLayout layout = chooseUnrealLayout(src.nz, src.nx);
    const int vertsX = layout.vertsX(), vertsY = layout.vertsY();
    Heightfield h;
    if (o.pad)
        h = geo::resampled(src, vertsY, vertsX, src.cell, src.origin);
    else
    {
        // Same footprint, new spacing: cell from the longer fit, the other side follows its sample count.
        const glm::dvec2 e = src.extent();
        const double cell = std::max(e.x / double(vertsY - 1), e.y / double(vertsX - 1));
        h = geo::resampled(src, vertsY, vertsX, cell, src.origin + e * 0.5 - glm::dvec2(vertsY - 1, vertsX - 1) * cell * 0.5);
    }

    const glm::dvec2 r = h.range();
    const double mid = (r.x + r.y) * 0.5;
    const double half = std::max(1.0, (r.y - r.x) * 0.5 * 1.01);
    // Unreal height in cm is (value - 32768) * scaleZ / 128.
    const double scaleZ = std::max(o.minScaleZ, half * 128.0 / 32767.0);
    std::vector<uint16_t> raw(h.size());
    const std::vector<float> ordered = unrealOrder(h, [&](size_t k) { return h.heights[k]; });
    size_t clipped = 0;
    for (size_t k = 0; k < ordered.size(); ++k)
    {
        const double v = std::round(32768.0 + (ordered[k] - mid) * 128.0 / scaleZ);
        if (v < 0.0 || v > 65535.0)
            ++clipped;
        raw[k] = uint16_t(std::clamp(v, 0.0, 65535.0));
    }

    fs::create_directories(folder);
    json files = json::object();
    const fs::path heightFile = folder / (name + ".r16");
    writeRaw(heightFile, raw.data(), raw.size() * sizeof(uint16_t));
    files["height"] = heightFile.generic_string();
    {
        std::vector<float> preview(raw.size());
        for (size_t k = 0; k < raw.size(); ++k)
            preview[k] = float(raw[k]) / 65535.0f;
        const fs::path png = folder / (name + ".png");
        writeGray(png, vertsX, vertsY, preview, 16);
        files["heightPng"] = png.generic_string();
    }
    json layers = json::array();
    std::set<std::string> layerTaken;
    for (const auto& [mask, values] : h.masks)
    {
        if (values.size() != h.size())
            continue;
        if (!o.masks.empty() && std::find(o.masks.begin(), o.masks.end(), mask) == o.masks.end())
            continue;
        auto named = o.layerNames.find(mask);
        const std::string layer = unrealLayerName(named != o.layerNames.end() ? named->second : mask);
        if (!layerTaken.insert(layer).second)
            throw std::runtime_error("masks " + mask + " and another both map to paint layer " + layer);
        const std::vector<float> m = unrealOrder(h, [&](size_t k) { return std::clamp(values[k], 0.0f, 1.0f); });
        std::vector<uint8_t> bytes(m.size());
        for (size_t k = 0; k < m.size(); ++k)
            bytes[k] = uint8_t(std::lround(m[k] * 255.0f));
        const fs::path raw8 = folder / (name + "_" + layer + ".raw");
        writeRaw(raw8, bytes.data(), bytes.size());
        layers.push_back({{"mask", mask}, {"layer", layer}, {"file", raw8.generic_string()}});
    }

    // The landscape's corner is its first sample: local (max X, min Z). Its X runs along local +Z, Y along local -X and Z
    // up local Y, each carried into the world and on into Unreal.
    const glm::dvec2 e = h.extent();
    const glm::dvec3 corner = glm::dvec3(world * glm::dvec4(h.origin.x + e.x, mid, h.origin.y, 1.0));
    const glm::dvec3 colX(world[0]), colY(world[1]), colZ(world[2]);
    auto unreal = [](const glm::dvec3& v) {
        const math::Vector u = math::toUnreal(math::Vector(v.x, v.y, v.z));
        return glm::dvec3(u.x, u.y, u.z);
    };
    const glm::dvec3 ux = glm::normalize(unreal(colZ)), uy = glm::normalize(unreal(-colX)), uz = glm::normalize(unreal(colY));
    // FMatrix::Rotator's decomposition, so Unreal rebuilds the same axes.
    const double yaw = std::atan2(ux.y, ux.x), pitch = std::atan2(ux.z, std::sqrt(ux.x * ux.x + ux.y * ux.y));
    const glm::dvec3 sy(-std::sin(yaw), std::cos(yaw), 0.0);
    const double roll = std::atan2(glm::dot(uz, sy), glm::dot(uy, sy));
    constexpr double kDeg = 57.29577951308232;
    const glm::dvec3 loc = unreal(corner);
    json out = {{"target", "unreal"},
                {"files", files},
                {"layers", layers},
                {"resolution", json::array({vertsX, vertsY})},
                {"quadsPerSection", layout.quadsPerSection},
                {"sectionsPerComponent", layout.sectionsPerComponent},
                {"components", json::array({layout.componentsX, layout.componentsY})},
                {"location", json::array({loc.x, loc.y, loc.z})},
                {"rotation", json::array({pitch * kDeg, yaw * kDeg, roll * kDeg})},
                {"scale", json::array({h.cell * glm::length(colZ), h.cell * glm::length(colX), scaleZ * glm::length(colY)})},
                {"heightRange", json::array({r.x, r.y})},
                {"clipped", int64_t(clipped)},
                {"resampled", src.nx != h.nx || src.nz != h.nz},
                {"cellSize", h.cell}};
    std::ofstream(folder / (name + ".json")) << out.dump(2);
    out["files"]["settings"] = (folder / (name + ".json")).generic_string();
    return out;
}

json landscapeParams(const json& settings, const LandscapeSend& s)
{
    json layers = json::array();
    for (const json& l : settings.value("layers", json::array()))
        layers.push_back({{"name", l["layer"]}, {"mask", l["mask"]}, {"weightmap", l["file"]}});
    json p = {{"id", s.id},
              {"label", s.label},
              {"heightmap", settings["files"]["height"]},
              {"resolution", settings["resolution"]},
              {"quadsPerSection", settings["quadsPerSection"]},
              {"sectionsPerComponent", settings["sectionsPerComponent"]},
              {"location", settings["location"]},
              {"rotation", settings["rotation"]},
              {"scale", settings["scale"]},
              {"layers", layers},
              {"makeMaterial", s.material.empty() && s.makeMaterial},
              {"gridSize", s.gridSize}};
    if (!s.material.empty())
        p["material"] = s.material;
    return p;
}

json exportFiles(const Heightfield& h, const fs::path& folder, const std::string& name)
{
    LILA_ZONE();

    if (h.empty())
        throw std::runtime_error("the terrain is empty");
    fs::create_directories(folder);
    const glm::dvec2 r = h.range();
    const double span = std::max(1e-6, r.y - r.x);
    json files = json::object();

    const fs::path exr = folder / (name + ".exr");
    writeGray(exr, h.nx, h.nz, h.heights, 32);
    files["heightExr"] = exr.generic_string();

    std::vector<float> norm(h.size());
    std::vector<uint16_t> raw(h.size());
    for (size_t k = 0; k < h.size(); ++k)
    {
        norm[k] = float((h.heights[k] - r.x) / span);
        raw[k] = uint16_t(std::lround(std::clamp(norm[k], 0.0f, 1.0f) * 65535.0f));
    }
    const fs::path png = folder / (name + ".png");
    writeGray(png, h.nx, h.nz, norm, 16);
    files["heightPng"] = png.generic_string();
    const fs::path r16 = folder / (name + ".r16");
    writeRaw(r16, raw.data(), raw.size() * sizeof(uint16_t));
    files["heightR16"] = r16.generic_string();

    json masks = json::object();
    for (const auto& [mask, values] : h.masks)
        if (values.size() == h.size())
        {
            std::vector<float> m(values.size());
            for (size_t k = 0; k < m.size(); ++k)
                m[k] = std::clamp(values[k], 0.0f, 1.0f);
            const fs::path p = folder / (name + "_" + mask + ".png");
            writeGray(p, h.nx, h.nz, m, 16);
            masks[mask] = p.generic_string();
        }
    files["masks"] = masks;
    const glm::dvec2 e = h.extent();
    json out = {{"target", "files"},
                {"files", files},
                {"resolution", json::array({h.nx, h.nz})},
                {"cellSize", h.cell},
                {"min", json::array({h.origin.x, h.origin.y})},
                {"size", json::array({e.x, e.y})},
                {"heightRange", json::array({r.x, r.y})},
                {"layout", "rows run along +X and advance along +Z; the first row is the -Z edge; PNG and R16 map 0..1 to heightRange, EXR holds cm"}};
    std::ofstream(folder / (name + ".json")) << out.dump(2);
    out["files"]["settings"] = (folder / (name + ".json")).generic_string();
    return out;
}
} // namespace terrain
