#include "scene/Plug.h"

#include "scene/MeshData.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"

#include <tbb/parallel_for.h>

#include <map>
#include <stdexcept>

namespace lila::scene
{
namespace
{
Registry<data::FromJson>& dataKinds()
{
    return data::kinds();
}
} // namespace

Registry<data::FromJson>& data::kinds()
{
    static Registry<FromJson> r;
    return r;
}

void data::registerKind(const std::string& kind, FromJson fromJson)
{
    kinds().put(kind, std::move(fromJson));
}

bool data::registerKind(ext::Context& ctx, const std::string& kind, FromJson fromJson)
{
    if (kinds().contains(kind))
    {
        log::error("extension {}: data kind '{}' is already registered", ctx.owner(), kind);
        return false;
    }
    ctx.add(kinds(), "dataKind", kind, std::move(fromJson));
    return true;
}

json toJson(const PlugValue& v)
{
    return std::visit(
        [](const auto& x) -> json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, glm::dvec3>)
                return json::array({x.x, x.y, x.z});
            else if constexpr (std::is_same_v<T, MeshPtr>)
                return x ? meshToJson(*x) : json(nullptr);
            else if constexpr (std::is_same_v<T, DataPtr>)
                return x ? x->toJson() : json(nullptr);
            else if constexpr (std::is_same_v<T, glm::dmat4>)
            {
                json m = json::array();
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        m.push_back(x[c][r]);
                return m;
            }
            else
                return x;
        },
        v);
}

PlugValue fromJson(const AttrDef& def, const json& j)
{
    switch (def.type)
    {
    case PlugType::Bool:
        if (j.is_boolean())
            return j.get<bool>();
        if (j.is_number())
            return j.get<double>() != 0.0;
        break;
    case PlugType::Int:
        if (j.is_number())
            return j.get<int64_t>();
        if (j.is_boolean())
            return int64_t(j.get<bool>());
        break;
    case PlugType::Enum:
        if (j.is_number_integer())
            return j.get<int64_t>();
        if (j.is_string())
            for (size_t i = 0; i < def.enumNames.size(); ++i)
                if (def.enumNames[i] == j.get<std::string>())
                    return int64_t(i);
        break;
    case PlugType::Float:
        if (j.is_number())
            return j.get<double>();
        if (j.is_boolean())
            return j.get<bool>() ? 1.0 : 0.0;
        break;
    case PlugType::Vec3:
        if (j.is_array() && j.size() == 3)
            return glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
        break;
    case PlugType::Matrix:
        if (j.is_array() && j.size() == 16)
        {
            glm::dmat4 m;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    m[c][r] = j[c * 4 + r].get<double>();
            return m;
        }
        break;
    case PlugType::String:
        if (j.is_string())
            return j.get<std::string>();
        break;
    case PlugType::Mesh:
        if (j.is_null())
            return MeshPtr();
        if (j.is_object())
            return meshFromJson(j);
        break;
    case PlugType::Data:
        if (j.is_null())
            return DataPtr();
        if (j.is_object() && j.contains("kind") && j["kind"].is_string())
        {
            const data::FromJson* fromJson = dataKinds().find(j["kind"].get<std::string>());
            if (!fromJson)
                throw std::invalid_argument("unknown data kind '" + j["kind"].get<std::string>() + "'");
            return (*fromJson)(j);
        }
        // A bare value for a Data attr whose default names its kind: that kind's loader gets it as {"kind", "value"}.
        if (const DataPtr* d = std::get_if<DataPtr>(&def.defaultValue); d && *d)
            if (const data::FromJson* fromJson = dataKinds().find((*d)->kind()))
                return (*fromJson)(json{{"kind", (*d)->kind()}, {"value", j}});
        break;
    }
    throw std::invalid_argument(std::string("expected ") + typeName(def.type) + ", got " + j.dump());
}

std::vector<json> toJsonAll(const std::vector<const PlugValue*>& values, bool packed)
{
    std::vector<json> out(values.size());
    std::vector<size_t> heavy;
    for (size_t i = 0; i < values.size(); ++i)
        if (std::holds_alternative<MeshPtr>(*values[i]) || std::holds_alternative<DataPtr>(*values[i]))
            heavy.push_back(i);
        else
            out[i] = toJson(*values[i]);
    tbb::parallel_for(size_t(0), heavy.size(), [&](size_t k) {
        const PlugValue& v = *values[heavy[k]];
        const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
        out[heavy[k]] = packed && mesh && *mesh ? meshToJsonPacked(**mesh) : toJson(v);
    });
    return out;
}

std::vector<FromJsonResult> fromJsonAll(const std::vector<std::pair<const AttrDef*, const json*>>& items)
{
    std::vector<FromJsonResult> out(items.size());
    auto convert = [&](size_t i) {
        try
        {
            out[i].value = fromJson(*items[i].first, *items[i].second);
        }
        catch (const std::exception& e)
        {
            out[i].error = e.what();
        }
    };
    std::vector<size_t> meshes;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].first->type == PlugType::Mesh)
            meshes.push_back(i);
        else
            convert(i);
    tbb::parallel_for(size_t(0), meshes.size(), [&](size_t k) { convert(meshes[k]); });
    return out;
}

PlugValue defaultFor(PlugType type)
{
    switch (type)
    {
    case PlugType::Bool: return false;
    case PlugType::Int:
    case PlugType::Enum: return int64_t(0);
    case PlugType::Float: return 0.0;
    case PlugType::Vec3: return glm::dvec3(0.0);
    case PlugType::Matrix: return glm::dmat4(1.0);
    case PlugType::String: return std::string();
    case PlugType::Mesh: return MeshPtr();
    case PlugType::Data: return DataPtr();
    }
    return 0.0;
}

PlugType typeNamed(std::string_view name)
{
    for (PlugType t : {PlugType::Bool, PlugType::Int, PlugType::Float, PlugType::Vec3, PlugType::Matrix, PlugType::String, PlugType::Enum,
                       PlugType::Mesh, PlugType::Data})
        if (name == typeName(t))
            return t;
    throw std::invalid_argument("unknown attribute type '" + std::string(name) + "'");
}

json attrDefToJson(const AttrDef& d)
{
    json j = {{"name", d.name}, {"type", typeName(d.type)}};
    if (!d.shortName.empty())
        j["short"] = d.shortName;
    if (d.flags & AttrArray)
        j["array"] = true;
    if (d.flags & AttrKeyable)
        j["keyable"] = true;
    if (d.flags & AttrHidden)
        j["hidden"] = true;
    if (d.flags & AttrColor)
        j["color"] = true;
    if (d.defaultValue != defaultFor(d.type))
        j["default"] = toJson(d.defaultValue);
    if (!d.enumNames.empty())
        j["enum"] = d.enumNames;
    if (d.type == PlugType::Vec3)
        j["children"] = d.childNames;
    for (auto [key, v] : {std::pair{"min", d.minValue}, {"max", d.maxValue}, {"softMin", d.softMin}, {"softMax", d.softMax}})
        if (v)
            j[key] = *v;
    if (!d.niceName.empty())
        j["niceName"] = d.niceName;
    if (!d.doc.empty())
        j["doc"] = d.doc;
    return j;
}

AttrDef attrDefFromJson(const json& j)
{
    AttrDef d;
    d.name = j.at("name").get<std::string>();
    d.shortName = j.value("short", "");
    d.type = typeNamed(j.at("type").get<std::string>());
    d.defaultValue = defaultFor(d.type);
    d.flags = AttrInput | AttrStorable | AttrUser;
    if (j.value("array", false))
        d.flags |= AttrArray;
    if (j.value("keyable", false))
        d.flags |= AttrKeyable;
    if (j.value("hidden", false))
        d.flags |= AttrHidden;
    if (j.value("color", false))
        d.flags |= AttrColor;
    d.enumNames = j.value("enum", std::vector<std::string>{});
    if (j.contains("children"))
        d.childNames = j["children"].get<std::array<std::string, 3>>();
    if (j.contains("default"))
        d.defaultValue = fromJson(d, j["default"]);
    for (auto [key, v] : {std::pair{"min", &d.minValue}, {"max", &d.maxValue}, {"softMin", &d.softMin}, {"softMax", &d.softMax}})
        if (j.contains(key))
            *v = j[key].get<double>();
    d.niceName = j.value("niceName", "");
    d.doc = j.value("doc", "");
    return d;
}

const char* typeName(PlugType type)
{
    switch (type)
    {
    case PlugType::Bool: return "bool";
    case PlugType::Int: return "int";
    case PlugType::Float: return "float";
    case PlugType::Vec3: return "vec3";
    case PlugType::Matrix: return "matrix";
    case PlugType::String: return "string";
    case PlugType::Enum: return "enum";
    case PlugType::Mesh: return "mesh";
    case PlugType::Data: return "data";
    }
    return "?";
}
} // namespace lila::scene
