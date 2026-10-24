#include "bake/BakeWrite.h"

#include "bake/Backend.h"
#include "bake/Maps.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <span>
#include <stdexcept>

namespace lila::bake
{
namespace fs = std::filesystem;

std::string formatExt(std::string_view format)
{
    if (format == "exr" || format == "tga" || format == "tif" || format == "psd")
        return std::string(format);
    if (format == "tiff")
        return "tif";
    return "png";
}

int formatBits(std::string_view ext, int bits)
{
    if (ext == "exr")
        return bits >= 32 ? 32 : 16;
    if (ext == "tif")
        return bits >= 32 ? 32 : bits >= 16 ? 16 : 8;
    if (ext == "png" || ext == "psd")
        return bits >= 16 ? 16 : 8;
    return 8;
}

std::string layeredName(std::string_view tmpl, NameTokens tokens, bool forceUdim)
{
    std::string t(tmpl);
    auto isSep = [](char c) { return c == '_' || c == '-' || c == '.' || c == ' '; };
    for (size_t at; (at = t.find("{map}")) != std::string::npos;)
    {
        size_t from = at, to = at + 5;
        if (from > 0 && isSep(t[from - 1]))
            --from;
        else if (to < t.size() && isSep(t[to]))
            ++to;
        t.erase(from, to - from);
    }
    tokens.map.clear();
    tokens.ext = "psd";
    return expandName(t.empty() ? std::string("{mesh}") : t, tokens, forceUdim);
}

json textureInfo(const TextureBuffer& tex)
{
    json j = {{"map", tex.def ? tex.def->id : std::string()},
              {"suffix", tex.settings.suffix.empty() && tex.def ? tex.def->suffix : tex.settings.suffix},
              {"colorSpace", tex.def && !tex.def->linear ? "srgb" : "linear"},
              {"bits", tex.bits},
              {"format", tex.format},
              {"udim", tex.udim},
              {"width", tex.width},
              {"height", tex.height},
              {"groups", tex.groups},
              {"meshes", tex.meshes},
              {"materials", tex.materials},
              {"meta", tex.meta}};
    if (tex.def && tex.def->id == "normal")
        j["normalFormat"] = tex.settings.options.value("greenUp", false) ? "opengl" : "directx";
    return j;
}

TextureWriter::TextureWriter(const OutputSettings& output, std::string inputHash) : mOutput(output), mHash(std::move(inputHash)) {}

image::WriteOptions TextureWriter::optionsFor(const TextureBuffer& tex) const
{
    // Dither would split an exact colour across neighbouring 8-bit steps.
    const bool exact = tex.def && tex.def->resolve == Resolve::Exact;
    return {.bits = tex.bits, .dither = mOutput.dither && !exact, .compress = mOutput.compression != "none", .level = mOutput.compressionLevel};
}

void TextureWriter::add(const TextureBuffer& tex, image::Image img, const fs::path& layered)
{
    LILA_ZONE();

    json info = textureInfo(tex);
    if (!mHash.empty())
        info["inputHash"] = mHash;
    if (layered.empty())
    {
        image::write(tex.file, img, optionsFor(tex));
        if (mOutput.sidecar)
            image::writeSidecar(tex.file, info);
        return;
    }
    std::scoped_lock lock(mMutex);
    auto [it, fresh] = mLayered.try_emplace(layered);
    Pending& p = it->second;
    if (fresh)
    {
        mLayeredOrder.push_back(layered);
        const image::WriteOptions o = optionsFor(tex);
        p.bits = o.bits;
        p.dither = o.dither;
    }
    else
    {
        // One document has one depth: the deepest any layer asked for.
        p.bits = std::max(p.bits, tex.bits);
        p.dither = p.dither && optionsFor(tex).dither;
    }
    if (!p.layers.empty() && (p.layers.front().second.width != img.width || p.layers.front().second.height != img.height))
        throw std::runtime_error("layers of " + layered.string() + " differ in size");
    info["layer"] = info["suffix"];
    p.info.push_back(std::move(info));
    p.layers.emplace_back(p.info.back()["suffix"].get<std::string>(), std::move(img));
}

void TextureWriter::finish()
{
    LILA_ZONE();

    for (const fs::path& file : mLayeredOrder)
    {
        Pending& p = mLayered[file];
        std::vector<image::Layer> layers;
        for (const auto& [name, img] : p.layers)
            layers.push_back({name, img.rgba});
        const image::Image& first = p.layers.front().second;
        image::writePsd(file, first.width, first.height, layers, formatBits("psd", p.bits), p.dither);
        if (mOutput.sidecar)
            image::writeSidecar(file, {{"layers", p.info}, {"inputHash", mHash}});
    }
    mLayered.clear();
    mLayeredOrder.clear();
}
} // namespace lila::bake
