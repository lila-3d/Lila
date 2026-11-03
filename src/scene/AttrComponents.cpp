#include "scene/AttrComponents.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace lila::scene::attrComponents
{
namespace
{
constexpr const char* kExt = ".lilacomponent";
constexpr const char* kFormat = "lila.component";
constexpr const char* kExportAttr = "lilaComponentExport";
constexpr const char* kSchemasAttr = "usdApiSchemas";

constexpr std::pair<UnrealMode, const char*> kModes[] = {
    {UnrealMode::None, "none"},           {UnrealMode::ActorTags, "actorTags"}, {UnrealMode::AssetUserData, "assetUserData"},
    {UnrealMode::Component, "component"}, {UnrealMode::DataAsset, "dataAsset"}, {UnrealMode::Custom, "custom"},
};

std::vector<Component> savedIn(const fs::path& dir, const std::string& source)
{
    std::vector<Component> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return out;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == kExt)
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        json j = json::parse(ss.str(), nullptr, false);
        try
        {
            if (!j.is_object() || j.value("format", "") != kFormat)
                throw std::invalid_argument("not a component file");
            Component c = Component::fromJson(j);
            c.source = source;
            out.push_back(std::move(c));
        }
        catch (const std::exception& e)
        {
            log::warn("component {}: {}", paths::toUtf8(f), e.what());
        }
    }
    return out;
}

/** A file name for the component: its name with characters files can't hold replaced. */
std::string fileStem(std::string_view name)
{
    std::string s(name);
    for (char& c : s)
        if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    return s;
}

int stringArrayAttr(const Node& n, const char* name)
{
    const int i = n.type().attrIndex(name);
    if (i < 0)
        return -1;
    const AttrDef& d = n.type().attrs[size_t(i)];
    return d.type == PlugType::String && (d.flags & AttrArray) ? i : -1;
}

/** One value as unrealPayload sends it; null for meshes and data, which are not component values. */
json payloadValue(const AttrDef& d, const PlugValue& v)
{
    if (const int64_t* e = std::get_if<int64_t>(&v); e && d.type == PlugType::Enum)
        return *e >= 0 && *e < int64_t(d.enumNames.size()) ? json(d.enumNames[size_t(*e)]) : json(*e);
    return std::visit(
        [](const auto& x) -> json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, glm::dvec3>)
                return json::array({x.x, x.y, x.z});
            else if constexpr (std::is_same_v<T, glm::dmat4>)
            {
                json m = json::array();
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        m.push_back(x[c][r]);
                return m;
            }
            else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, std::string>)
                return json(x);
            else
                return json();
        },
        v);
}
} // namespace

const char* unrealModeName(UnrealMode m)
{
    for (auto [mode, name] : kModes)
        if (mode == m)
            return name;
    return "none";
}

UnrealMode unrealModeNamed(std::string_view name)
{
    for (auto [mode, n] : kModes)
        if (name == n)
            return mode;
    throw std::invalid_argument("unknown Unreal export '" + std::string(name) +
                                "': none, actorTags, assetUserData, component, dataAsset or custom");
}

json Component::toJson() const
{
    json j = {{"format", kFormat}, {"version", 1}, {"name", name}};
    if (!doc.empty())
        j["doc"] = doc;
    json list = json::array();
    for (const AttrDef& d : attrs)
    {
        json a = attrDefToJson(d);
        a.erase("category");
        list.push_back(std::move(a));
    }
    j["attrs"] = std::move(list);
    if (!usdSchema.empty())
        j["usdSchema"] = usdSchema;
    j["unreal"] = {{"mode", unrealModeName(unreal)}};
    if (!unrealClass.empty())
        j["unreal"]["class"] = unrealClass;
    if (!unrealTags.empty())
        j["unreal"]["tags"] = unrealTags;
    return j;
}

Component Component::fromJson(const json& j)
{
    Component c;
    c.name = j.at("name").get<std::string>();
    if (c.name.empty())
        throw std::invalid_argument("a component needs a name");
    c.doc = j.value("doc", "");
    for (const json& a : j.value("attrs", json::array()))
    {
        AttrDef d = attrDefFromJson(a);
        d.category.clear();
        c.attrs.push_back(std::move(d));
    }
    c.usdSchema = j.value("usdSchema", "");
    if (j.contains("unreal") && j["unreal"].is_object())
    {
        c.unreal = unrealModeNamed(j["unreal"].value("mode", "assetUserData"));
        c.unrealClass = j["unreal"].value("class", "");
        c.unrealTags = j["unreal"].value("tags", "");
    }
    return c;
}

Registry<Source>& sources()
{
    static Registry<Source> r;
    static bool once = [] {
        r.add("lila.saved", [] {
            std::vector<Component> out = savedIn(folder(false), "user");
            for (const fs::path& s : PrefStore::get().studioDirs())
                for (Component& c : savedIn(s / "components", "studio"))
                    out.push_back(std::move(c));
            return out;
        }, "lila");
        return true;
    }();
    (void)once;
    return r;
}

bool addSource(ext::Context& ctx, const std::string& id, Source source)
{
    if (sources().contains(id))
    {
        log::error("extension {}: component source '{}' is already registered", ctx.owner(), id);
        return false;
    }
    ctx.add(sources(), "componentSource", id, std::move(source));
    return true;
}

std::vector<Component> all()
{
    std::vector<Component> out;
    std::set<std::string> seen;
    std::vector<std::string> ids = sources().idsInOrder();
    // Saved ones last, so a user's file never hides a schema or an extension's component of the same name.
    std::stable_partition(ids.begin(), ids.end(), [](const std::string& id) { return id != "lila.saved"; });
    for (const std::string& id : ids)
    {
        const Source* s = sources().find(id);
        if (!s || !*s)
            continue;
        std::vector<Component> list;
        try
        {
            list = (*s)();
        }
        catch (const std::exception& e)
        {
            log::warn("component source {}: {}", id, e.what());
        }
        for (Component& c : list)
            if (seen.insert(c.name).second)
                out.push_back(std::move(c));
    }
    return out;
}

std::optional<Component> find(std::string_view name)
{
    for (Component& c : all())
        if (c.name == name)
            return std::move(c);
    return std::nullopt;
}

fs::path folder(bool studio)
{
    if (!studio)
        return PrefStore::get().userDir() / "components";
    std::vector<fs::path> dirs = PrefStore::get().studioDirs();
    return dirs.empty() ? fs::path() : dirs.front() / "components";
}

fs::path save(const Component& c, bool studio)
{
    fs::path dir = folder(studio);
    if (dir.empty())
        throw std::runtime_error("no studio folder is set: set LILA_STUDIO_PREFS or studio.json's prefs list");
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / paths::fromUtf8(fileStem(c.name) + kExt);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
    out << c.toJson().dump(2) << '\n';
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
    return file;
}

bool removeSaved(std::string_view name, bool studio)
{
    fs::path dir = folder(studio);
    std::error_code ec;
    return !dir.empty() && fs::remove(dir / paths::fromUtf8(fileStem(name) + kExt), ec);
}

json exportSettings(const Node& n)
{
    const int i = n.type().attrIndex(kExportAttr);
    if (i < 0 || n.type().attrs[size_t(i)].type != PlugType::String)
        return json::object();
    json j = json::parse(n.as<std::string>(i), nullptr, false);
    return j.is_object() ? j : json::object();
}

void setExportSettings(Graph& g, NodeId node, const std::string& component, UnrealMode mode, const std::string& unrealClass, const std::string& tags)
{
    Node* n = g.node(node);
    if (!n)
        throw std::invalid_argument("no such node");
    if (!tags.empty() && tags != "keyValue" && tags != "value")
        throw std::invalid_argument("tags is keyValue or value, not '" + tags + "'");
    json j = exportSettings(*n);
    j[component] = {{"mode", unrealModeName(mode)}};
    if (!unrealClass.empty())
        j[component]["class"] = unrealClass;
    if (!tags.empty() && tags != "keyValue")
        j[component]["tags"] = tags;
    int i = n->type().attrIndex(kExportAttr);
    if (i < 0)
        i = g.addAttr(node, AttrDef{.name = kExportAttr, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrHidden,
                                    .doc = "Per component: how it exports to Unreal."});
    g.setValue({node, i}, j.dump());
}

Component fromNode(const Node& n, const std::string& category)
{
    Component c;
    c.name = category;
    c.source = "user";
    for (const AttrDef& d : n.type().attrs)
        if ((d.flags & AttrUser) && d.category == category)
        {
            AttrDef a = d;
            a.category.clear();
            a.flags &= ~AttrUser;
            c.attrs.push_back(std::move(a));
        }
    if (const int list = stringArrayAttr(n, kSchemasAttr); list >= 0)
        for (const auto& [index, v] : n.elements(list))
            if (std::get<std::string>(v) == category)
                c.usdSchema = category;
    json settings = exportSettings(n);
    if (settings.contains(category) && settings[category].is_object())
    {
        c.unreal = unrealModeNamed(settings[category].value("mode", "assetUserData"));
        c.unrealClass = settings[category].value("class", "");
        c.unrealTags = settings[category].value("tags", "");
    }
    return c;
}

std::vector<std::string> apply(Graph& g, NodeId node, const Component& c)
{
    Node* n = g.node(node);
    if (!n)
        throw std::invalid_argument("no such node");
    std::vector<std::string> added;
    for (const AttrDef& d : c.attrs)
    {
        if (n->type().attrIndex(d.name) >= 0)
            continue;
        AttrDef a = d;
        a.category = c.name;
        added.push_back(g.plugName({node, g.addAttr(node, std::move(a))}));
    }
    if (!c.usdSchema.empty())
    {
        int list = stringArrayAttr(*n, kSchemasAttr);
        if (list < 0 && n->type().attrIndex(kSchemasAttr) < 0)
            list = g.addAttr(node, AttrDef{.name = kSchemasAttr, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrArray,
                                           .doc = "The API schemas applied to this object's prim that Lila keeps as attributes; export applies them again.",
                                           .niceName = "USD API Schemas"});
        if (list >= 0)
        {
            std::vector<PlugValue> names;
            bool has = false;
            for (const auto& [index, v] : n->elements(list))
            {
                names.push_back(v);
                has |= std::get<std::string>(v) == c.usdSchema;
            }
            if (!has)
            {
                names.push_back(c.usdSchema);
                g.setElements({node, list, -1}, names);
            }
        }
    }
    if (c.unreal != UnrealMode::AssetUserData || !c.unrealClass.empty() || !c.unrealTags.empty())
        setExportSettings(g, node, c.name, c.unreal, c.unrealClass, c.unrealTags);
    return added;
}

json unrealPayload(Graph& g, NodeId node)
{
    Node* n = g.node(node);
    if (!n)
        throw std::invalid_argument("no such node");
    const json settings = exportSettings(*n);
    json out = json::array();
    std::vector<std::string> order;
    std::map<std::string, json> attrsOf;
    const std::vector<AttrDef>& defs = n->type().attrs;
    for (size_t i = 0; i < defs.size(); ++i)
    {
        const AttrDef& d = defs[i];
        if (!(d.flags & AttrUser) || (d.flags & AttrHidden) || d.name == kSchemasAttr || d.name == kExportAttr)
            continue;
        json value;
        if (d.flags & AttrArray)
        {
            value = json::array();
            for (const auto& [index, v] : n->elements(int(i)))
                if (json e = payloadValue(d, v); !e.is_null())
                    value.push_back(std::move(e));
        }
        else
            value = payloadValue(d, g.value({node, int(i), -1}));
        if (value.is_null())
            continue;
        if (!attrsOf.contains(d.category))
            order.push_back(d.category);
        attrsOf[d.category][d.name] = std::move(value);
    }
    for (const std::string& name : order)
    {
        UnrealMode mode = UnrealMode::AssetUserData;
        std::string cls, tags;
        if (settings.contains(name) && settings[name].is_object())
        {
            try
            {
                mode = unrealModeNamed(settings[name].value("mode", "assetUserData"));
            }
            catch (const std::invalid_argument& e)
            {
                log::warn("{}: component {}: {}", n->name(), name, e.what());
            }
            cls = settings[name].value("class", "");
            tags = settings[name].value("tags", "");
        }
        if (mode == UnrealMode::None)
            continue;
        json c = {{"name", name}, {"mode", unrealModeName(mode)}, {"attrs", std::move(attrsOf[name])}};
        if (!cls.empty())
            c["class"] = cls;
        if (mode == UnrealMode::ActorTags && !tags.empty())
            c["tags"] = tags;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::pair<std::string, json>> exportProperties(Graph& g, NodeId node)
{
    std::vector<std::pair<std::string, json>> out;
    const Node* n = g.node(node);
    if (!n)
        return out;
    const std::vector<AttrDef>& defs = n->type().attrs;
    for (size_t i = 0; i < defs.size(); ++i)
    {
        const AttrDef& d = defs[i];
        if (!(d.flags & AttrUser) || (d.flags & AttrHidden) || d.name == kSchemasAttr || d.name == kExportAttr)
            continue;
        json value;
        if (d.flags & AttrArray)
        {
            value = json::array();
            for (const auto& [index, v] : n->elements(int(i)))
                if (json e = payloadValue(d, v); !e.is_null())
                    value.push_back(std::move(e));
        }
        else
            value = payloadValue(d, g.value({node, int(i), -1}));
        if (!value.is_null())
            out.emplace_back(d.category.empty() ? d.name : d.category + "." + d.name, std::move(value));
    }
    return out;
}

Registry<std::string>& unrealExporters()
{
    static Registry<std::string> r;
    return r;
}

bool addUnrealExporter(ext::Context& ctx, const std::string& name, std::string doc)
{
    if (unrealExporters().contains(name))
    {
        log::error("extension {}: Unreal component exporter '{}' is already registered", ctx.owner(), name);
        return false;
    }
    ctx.add(unrealExporters(), "unrealComponentExporter", name, std::move(doc));
    return true;
}
} // namespace lila::scene::attrComponents
