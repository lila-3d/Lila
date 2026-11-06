// SPDX-License-Identifier: Apache-2.0
#include "groomlab/scalp_maps.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"

namespace groomlab {

ScalpMaps ScalpMaps::fromJson(const std::string& text) {
    ScalpMaps maps;
    if (text.empty()) return maps;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("scalpMaps: bad JSON: ") + e.what());
    }
    if (doc.is_null()) return maps;
    if (!doc.is_object()) throw std::runtime_error("scalpMaps: expected an object of maps");
    for (const auto& [name, entry] : doc.items()) {
        if (!entry.is_object()) throw std::runtime_error("scalpMaps: map '" + name + "' is not an object");
        ScalpMapEntry m;
        m.kind = entry.value("kind", "vertices");
        if (m.kind != "vertices" && m.kind != "texture") {
            throw std::runtime_error("scalpMaps: map '" + name + "' has unknown kind '" + m.kind + "'");
        }
        m.channels = entry.value("channels", 1);
        if (m.channels != 1 && m.channels != 4) {
            throw std::runtime_error("scalpMaps: map '" + name + "' has " + std::to_string(m.channels) + " channels (1 or 4)");
        }
        if (m.kind == "vertices") {
            const auto values = entry.find("values");
            if (values != entry.end() && values->is_array()) {
                m.values.reserve(values->size());
                for (const auto& v : *values) m.values.push_back(v.is_number() ? v.get<float>() : 0.0f);
            }
        } else {
            m.texture = entry.value("texture", "");
        }
        maps.maps[name] = std::move(m);
    }
    return maps;
}

std::string ScalpMaps::toJson() const {
    if (maps.empty()) return "";
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [name, m] : maps) {
        nlohmann::json entry;
        entry["kind"] = m.kind;
        if (m.channels != 1) entry["channels"] = m.channels;  // absent: gray
        if (m.kind == "vertices") {
            nlohmann::json values = nlohmann::json::array();
            for (const float v : m.values) values.push_back(std::round(static_cast<double>(v) * 1000.0) / 1000.0);
            entry["values"] = std::move(values);
        } else {
            entry["texture"] = m.texture;
        }
        doc[name] = std::move(entry);
    }
    return doc.dump();
}

std::uint64_t ScalpMaps::hash() const {
    std::uint64_t h = hash64(0x5CA1BAD5);
    for (const auto& [name, m] : maps) {
        h = hashText(h, name);
        h = hashText(h, m.kind);
        h = hashText(h, m.texture);
        if (m.channels != 1) h = hashCombine(h, static_cast<std::uint64_t>(m.channels));
        for (const float v : m.values) h = hashFloat(h, v);
    }
    return h;
}

MapCatalog ScalpMaps::catalog() const {
    MapCatalog out;
    for (const auto& [name, m] : maps) out[name] = {m.kind, m.texture, m.channels == 4 ? 4 : 1};
    return out;
}

MeshPtr withVertexMaps(const MeshPtr& scalp, const ScalpMaps& maps, const std::string& previewName,
                       const std::vector<float>* previewValues, std::vector<std::string>* errors, int previewChannels) {
    if (!scalp) return scalp;
    bool any = previewValues != nullptr;
    for (const auto& [name, m] : maps.maps) any = any || m.kind == "vertices";
    if (!any) return scalp;
    auto mapped = std::make_shared<ScalpMesh>(*scalp);
    auto put = [&](const std::string& name, const std::vector<float>& given, int channels) {
        const std::size_t per = channels == 4 ? 4 : 1;
        // Painted before other scalps were joined after the groom's own (scalp_pieces.h): the
        // vertices of the scalps it was painted on, the rest 0 (alpha 1).
        std::vector<float> padded;
        const std::size_t total = scalp->pointCount() * per;
        if (given.size() < total && !scalp->pieces().empty()) {
            std::size_t covered = 0;
            for (const ScalpPiece& p : scalp->pieces()) {
                covered += p.pointCount * per;
                if (covered != given.size()) continue;
                padded = given;
                padded.resize(total, 0.0f);
                if (per == 4) {
                    for (std::size_t i = given.size() + 3; i < total; i += 4) padded[i] = 1.0f;
                }
                break;
            }
        }
        const std::vector<float>& values = padded.empty() ? given : padded;
        if (values.size() != total) {
            if (errors) {
                errors->push_back("map '" + name + "' was painted on a scalp with " + std::to_string(values.size() / per) +
                                  " vertices; this one has " + std::to_string(scalp->pointCount()) +
                                  " (convert it to a texture or guide layer on the old scalp, or fill it anew)");
            }
            return;
        }
        mapped->setCornerColors(name, cornerColorsFromVertexMap(values, *scalp, static_cast<int>(per)));
    };
    for (const auto& [name, m] : maps.maps) {
        if (m.kind != "vertices" || (previewValues && name == previewName)) continue;
        put(name, m.values, m.channels);
    }
    if (previewValues) put(previewName, *previewValues, previewChannels);
    return mapped;
}

std::vector<float> vertexMapFromColorSet(const ScalpMesh& scalp, const std::vector<ScalpMesh::Color>& corners, int* channels) {
    bool color = false;
    for (const ScalpMesh::Color& c : corners) {
        if (c[0] != c[1] || c[1] != c[2] || c[3] != 1.0f) {
            color = true;
            break;
        }
    }
    const std::size_t per = color ? 4 : 1;
    if (channels) *channels = static_cast<int>(per);
    std::vector<float> values(scalp.pointCount() * per, 0.0f);
    const auto& triangles = scalp.triangles();
    for (std::size_t t = 0; t < triangles.size() && t * 3 + 2 < corners.size(); ++t) {
        for (std::size_t k = 0; k < 3; ++k) {
            const auto v = static_cast<std::size_t>(triangles[t][k]);
            const ScalpMesh::Color& c = corners[t * 3 + k];
            if (per == 4) {
                for (std::size_t i = 0; i < 4; ++i) values[v * 4 + i] = c[i];
            } else {
                values[v] = c[0];
            }
        }
    }
    return values;
}

ScalpMaps scalpMapsFromGroomData(const GroomData& data) {
    ScalpMaps maps;
    const ScalpMesh* scalp = data.scalp.get();
    for (const auto& [name, info] : data.maps) {
        if (info.kind == "vertices") {
            const std::vector<ScalpMesh::Color>* corners = scalp ? scalp->cornerColors(name) : nullptr;
            if (!corners || corners->size() != scalp->triangleCount() * 3) continue;
            ScalpMapEntry entry;
            entry.kind = "vertices";
            entry.values = vertexMapFromColorSet(*scalp, *corners, &entry.channels);
            if (info.channels == kColorMap && entry.channels == kGrayMap) {  // a color map painted gray so far
                entry.values = convertMapChannels(entry.values, kGrayMap, kColorMap);
                entry.channels = kColorMap;
            }
            maps.maps[name] = std::move(entry);
        } else if (info.kind == "texture") {
            ScalpMapEntry entry;
            entry.kind = "texture";
            entry.texture = info.texture;
            entry.channels = info.channels == kColorMap ? kColorMap : kGrayMap;
            maps.maps[name] = std::move(entry);
        }
    }
    return maps;
}

// ---- Map names ---------------------------------------------------------------------------------

namespace {

bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Every reader of `from` in a graph's nodes (and its compounds' definitions) reads `to`.
bool retargetNodes(nlohmann::json& graph, const std::string& from, const std::string& to, int depth) {
    bool changed = false;
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return false;
    for (nlohmann::json& node : *nodes) {
        if (!node.is_object()) continue;
        if (!node.contains("params") || !node["params"].is_object()) {
            if (node.value("type", std::string()) != "Map" || from != "mask") continue;
            node["params"] = nlohmann::json::object();  // a Map node reading the default "mask"
        }
        nlohmann::json& params = node["params"];
        if (node.value("type", std::string()) == "Map" && params.value("name", std::string("mask")) == from) {
            params["name"] = to;
            changed = true;
        }
        // HairColor's map and Interpolate's regionMap name a map.
        for (const auto& [type, param] : {std::pair<const char*, const char*>{"HairColor", "map"}, {"Interpolate", "regionMap"}}) {
            if (node.value("type", std::string()) == type && params.contains(param) && params[param].is_string() &&
                params[param].get<std::string>() == from) {
                params[param] = to;
                changed = true;
            }
        }
        for (auto& [key, value] : params.items()) {
            if (!value.is_object()) continue;
            if (key == "definition" && node.value("type", std::string()) == "Compound" && depth < 16 && value.contains("graph") &&
                value["graph"].is_object()) {
                changed = retargetNodes(value["graph"], from, to, depth + 1) || changed;
                continue;
            }
            for (const char* source : {"guide", "colorSet"}) {
                if (value.contains(source) && value[source].is_string() && value[source].get<std::string>() == from) {
                    value[source] = to;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

}  // namespace

bool validMapName(const std::string& name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(), nameChar);
}

std::string mapNameError(const std::string& name) {
    return validMapName(name) ? std::string() : std::string("map names are letters, digits and _ (not starting with a digit)");
}

std::string sanitizeMapName(const std::string& name) {
    std::string out;
    for (const char c : name) out += nameChar(c) ? c : '_';
    if (out.empty()) return "map";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

std::string retargetMapReaders(const std::string& graphJson, const std::string& from, const std::string& to, bool* changed) {
    if (changed) *changed = false;
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded() || !graph.is_object() || from == to) return graphJson;
    if (!retargetNodes(graph, from, to, 0)) return graphJson;
    if (changed) *changed = true;
    return graph.dump();
}

std::string renamedMapTexture(const std::string& stored, const std::string& from, const std::string& to) {
    if (stored.empty() || from.empty()) return {};
    std::string path = stored;
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::size_t slash = path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = base.rfind('.');
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : base.substr(dot);
    // <map>_2: a numbered name taken when <map> was.
    std::string numbered = stem;
    const std::size_t underscore = stem.rfind('_');
    if (underscore != std::string::npos && underscore + 1 < stem.size() &&
        std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(underscore) + 1, stem.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        numbered = stem.substr(0, underscore);
    }
    for (const std::string& root : {stem, numbered}) {
        const bool own = root == from;
        const bool prefixed = root.size() > from.size() + 1 && root.compare(root.size() - from.size(), from.size(), from) == 0 &&
                              root[root.size() - from.size() - 1] == '_';  // an older <groom>_<map>
        if (own || prefixed) return folder + root.substr(0, root.size() - from.size()) + to + ext;
    }
    return {};
}

std::map<std::string, std::string> sanitizeMapNames(GroomData& data) {
    // Every map name the groom has: the catalog's and the guide layers'.
    std::set<std::string> names;
    for (const auto& [name, info] : data.maps) names.insert(name);
    std::unique_ptr<GuideHierarchy> hierarchy;
    if (!data.hierarchyJson.empty()) {
        try {
            hierarchy = std::make_unique<GuideHierarchy>(GuideHierarchy::fromJson(data.hierarchyJson));
            for (const std::string& layer : hierarchy->attributeNames()) names.insert(layer);
        } catch (const std::exception&) {
            hierarchy.reset();  // the session reports a bad hierarchy; its layers are left alone
        }
    }
    std::map<std::string, std::string> renames;
    std::set<std::string> taken(names.begin(), names.end());
    for (const std::string& name : names) {
        if (validMapName(name)) continue;
        const std::string base = sanitizeMapName(name);
        std::string fresh = base;
        for (int k = 2; taken.count(fresh); ++k) fresh = base + "_" + std::to_string(k);
        taken.insert(fresh);
        renames[name] = fresh;
    }
    if (renames.empty()) return renames;
    std::shared_ptr<ScalpMesh> scalp;
    bool layers = false;
    for (const auto& [from, to] : renames) {
        const auto entry = data.maps.find(from);
        if (entry != data.maps.end()) {
            MapInfo info = entry->second;
            data.maps.erase(entry);
            data.maps[to] = info;
        }
        if (data.scalp && data.scalp->hasColorSet(from)) {  // a vertex map's color set
            if (!scalp) scalp = std::make_shared<ScalpMesh>(*data.scalp);
            scalp->setCornerColors(to, *scalp->cornerColors(from));
            scalp->removeColorSet(from);
        }
        if (hierarchy) {
            const std::vector<std::string> own = hierarchy->attributeNames();
            if (std::find(own.begin(), own.end(), from) != own.end()) {
                hierarchy->renameAttribute(from, to);
                layers = true;
            }
        }
        data.graphJson = retargetMapReaders(data.graphJson, from, to);
    }
    if (scalp) data.scalp = scalp;
    if (layers) data.hierarchyJson = hierarchy->toJson();
    return renames;
}

}  // namespace groomlab
