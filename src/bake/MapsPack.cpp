#include "bake/MapsInternal.h"

#include "bake/Backend.h"

#include <algorithm>

/** Channel packs: textures composed per RGBA channel from other maps or constants (Unreal's ORM). */
namespace lila::bake
{
std::vector<PackChannel> packChannels(const MapSettings& settings, const MapDef& def, std::vector<std::string>& warnings)
{
    const json spec = settings.options.contains("channels") ? settings.options["channels"] : def.pack;
    std::vector<PackChannel> out(4);
    out[3].constant = 1.0f;
    if (!spec.is_array())
    {
        warnings.push_back(settings.map + ": options.channels must be an array of four entries");
        return out;
    }
    if (spec.size() > 4)
        warnings.push_back(settings.map + ": only the first four channels are packed");
    for (size_t c = 0; c < std::min<size_t>(4, spec.size()); ++c)
    {
        const json& e = spec[c];
        PackChannel& ch = out[c];
        if (e.is_number())
        {
            ch.constant = e.get<float>();
            continue;
        }
        if (e.is_string())
        {
            ch.map = e.get<std::string>();
            continue;
        }
        if (!e.is_object())
        {
            warnings.push_back(settings.map + ": channel " + std::to_string(c) + " is not a number, map id or {map, channel}");
            continue;
        }
        if (e.contains("constant"))
        {
            ch.constant = e.value("constant", 0.0f);
            continue;
        }
        ch.map = e.value("map", std::string());
        if (const auto named = e.find("channel"); named != e.end() && named->is_string())
        {
            static constexpr std::pair<std::string_view, int> kNames[] = {{"r", 0}, {"g", 1}, {"b", 2}, {"a", 3}, {"luminance", kPackLuminance}, {"max", kPackMax}};
            const auto n = std::ranges::find(kNames, named->get<std::string>(), &std::pair<std::string_view, int>::first);
            if (n == std::end(kNames))
                warnings.push_back(settings.map + ": channel " + std::to_string(c) + ": source channel must be r, g, b, a, luminance or max");
            else
                ch.channel = n->second;
        }
        else
            ch.channel = std::clamp(e.value("channel", 0), 0, kPackMax);
        ch.invert = e.value("invert", false);
        if (e.contains("options") && e["options"].is_object())
        {
            ch.options = e["options"];
            ch.hasOptions = true;
        }
    }
    return out;
}

void composePack(TextureBuffer& pack, const std::vector<PackChannel>& channels, const std::vector<const TextureBuffer*>& sources, json& meta)
{
    const size_t n = pack.texelCount();
    pack.value.assign(n, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    pack.covered.assign(n, 0);
    json chMeta = json::array();
    for (size_t c = 0; c < 4 && c < channels.size(); ++c)
    {
        const PackChannel& ch = channels[c];
        const TextureBuffer* src = c < sources.size() ? sources[c] : nullptr;
        if (!src || src->value.size() != n)
        {
            for (size_t i = 0; i < n; ++i)
                pack.value[i][int(c)] = ch.constant;
            chMeta.push_back({{"constant", ch.constant}});
            continue;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const glm::vec4& t = src->value[i];
            const float v = ch.channel == kPackLuminance ? glm::dot(glm::vec3(t), glm::vec3(0.2126f, 0.7152f, 0.0722f))
                            : ch.channel == kPackMax     ? std::max({t.r, t.g, t.b})
                                                         : t[ch.channel];
            pack.value[i][int(c)] = ch.invert ? 1.0f - v : v;
            pack.covered[i] |= src->covered[i];
        }
        chMeta.push_back({{"map", ch.map}, {"channel", ch.channel}, {"invert", ch.invert}, {"meta", src->meta}});
    }
    meta["channels"] = chMeta;
}

void registerPackMaps()
{
    registerMap({.id = "pack",
                 .label = "Channel Pack",
                 .suffix = "packed",
                 .doc = "A texture composed per channel: options channels, four entries R G B A, each a constant number, a map id or "
                        "{map, channel (0-3, or r g b a luminance max), options, invert}. The maps are baked with it at their own settings and copied in "
                        "after their finalize and padding.",
                 .pack = json::array({json{{"map", "ao"}}, json{{"map", "roughness"}}, json{{"map", "metallic"}}, 1.0})});
    registerMap({.id = "orm",
                 .label = "Occlusion Roughness Metallic",
                 .suffix = "ORM",
                 .doc = "Unreal's packed ORM: R ambient occlusion, G roughness, B metallic (from the high's material), A 1. options "
                        "channels overrides the layout as for pack (MRO: metallic, roughness, ao).",
                 .pack = json::array({json{{"map", "ao"}}, json{{"map", "roughness"}}, json{{"map", "metallic"}}, 1.0})});
    registerMap({.id = "baseColorEmissive",
                 .label = "Base Colour + Emissive",
                 .suffix = "BCE",
                 .doc = "sRGB base colour in RGB with an emissive mask in A (the brightest emissive channel). options channels "
                        "overrides the layout as for pack.",
                 .linear = false,
                 .pack = json::array({json{{"map", "baseColor"}, {"channel", 0}}, json{{"map", "baseColor"}, {"channel", 1}},
                                      json{{"map", "baseColor"}, {"channel", 2}}, json{{"map", "emissive"}, {"channel", "max"}}})});
}
} // namespace lila::bake
