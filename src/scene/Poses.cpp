#include "scene/Poses.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace lila::scene::poses
{
namespace
{
struct Owned
{
    Pose pose;
    std::string owner;
};

struct AliasSet
{
    std::string character;
    json aliases;
    std::string owner;
};

struct State
{
    std::vector<Pose> scene;
    std::vector<Owned> templates;
    std::vector<AliasSet> aliases;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    std::function<std::string(const std::vector<NodeId>&)> thumbnail;
    CharacterResolver resolver;
};

State& state()
{
    static State s;
    return s;
}

void notify()
{
    auto listeners = state().listeners;
    for (auto& [id, fn] : listeners)
        fn();
}

/** Swaps the scene tier whole, so undo and redo are the same operation. */
struct SwapAction final : UndoAction
{
    std::vector<Pose> other;
    explicit SwapAction(std::vector<Pose> before) : other(std::move(before)) {}
    void undo() override
    {
        std::swap(state().scene, other);
        notify();
    }
    void redo() override { undo(); }
    size_t bytes() const override
    {
        size_t n = sizeof(*this);
        for (const Pose& p : other)
            n += sizeof(Pose) + p.thumbnail.size() + p.nodes.size() * 256;
        return n;
    }
};

void replaceScene(std::vector<Pose> next)
{
    UndoStack::get().record(std::make_unique<SwapAction>(state().scene));
    state().scene = std::move(next);
    notify();
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** Safe as one path component on every platform. */
std::string fileComponent(const std::string& s)
{
    std::string out = s.empty() ? std::string("default") : s;
    for (char& c : out)
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || (unsigned char)c < 32)
            c = '_';
    return out;
}

fs::path poseFile(Tier t, const std::string& character, const std::string& name, bool create)
{
    fs::path dir = tierDir(t, create);
    if (dir.empty())
        return {};
    return dir / paths::fromUtf8(fileComponent(character)) / paths::fromUtf8(fileComponent(name) + ".json");
}

std::optional<Pose> readFile(const fs::path& file, const std::string& characterHint)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    try
    {
        json j = json::parse(in);
        if (!j.contains("name"))
            j["name"] = paths::toUtf8(file.stem());
        Pose p = Pose::fromJson(j);
        if (p.character.empty())
            p.character = characterHint;
        return p;
    }
    catch (const std::exception& e)
    {
        log::warn("Skipped pose file {}: {}", paths::toUtf8(file), e.what());
        return std::nullopt;
    }
}

void writeFile(const fs::path& file, const Pose& p)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
    out << p.toJson().dump(2);
}

void filePoses(Tier t, const std::string& character, std::vector<Entry>& out)
{
    fs::path dir = tierDir(t);
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return;
    std::vector<fs::path> groups;
    if (!character.empty())
        groups.push_back(dir / paths::fromUtf8(fileComponent(character)));
    else
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory())
                groups.push_back(e.path());
    for (const fs::path& group : groups)
    {
        if (!fs::is_directory(group, ec))
            continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(group, ec))
            if (e.is_regular_file() && e.path().extension() == ".json")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files)
            if (std::optional<Pose> p = readFile(f, paths::toUtf8(group.filename())))
                if (character.empty() || p->character == character)
                    out.push_back({std::move(*p), t, f});
    }
}

void tierPoses(Tier t, const std::string& character, std::vector<Entry>& out)
{
    switch (t)
    {
    case Tier::Scene:
        for (const Pose& p : state().scene)
            if (character.empty() || p.character == character)
                out.push_back({p, t, {}});
        break;
    case Tier::Workspace:
    case Tier::User:
        filePoses(t, character, out);
        break;
    case Tier::Template:
        for (const Owned& o : state().templates)
            if (character.empty() || o.pose.character == character)
                out.push_back({o.pose, t, {}});
        break;
    }
}

glm::dvec3 vec3Of(const json& j)
{
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("expected [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

json jsonOf(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dquat quatOfEuler(const glm::dvec3& degrees, RotateOrder order) { return glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(degrees, order)))); }

/** A pose's "rotate": [x, y, z, w], or Euler degrees in `order`. */
glm::dquat quatOf(const json& j, RotateOrder order)
{
    if (j.is_array() && j.size() == 4)
        return glm::normalize(glm::dquat(j[3].get<double>(), j[0].get<double>(), j[1].get<double>(), j[2].get<double>()));
    return quatOfEuler(vec3Of(j), order);
}

json jsonOf(const glm::dquat& q) { return json::array({q.x, q.y, q.z, q.w}); }

/** Quaternion round trips leave noise far below anything a channel shows. */
bool nearlyEqual(const PlugValue& a, const PlugValue& b)
{
    constexpr double eps = 1e-9;
    if (const auto* va = std::get_if<glm::dvec3>(&a))
        return glm::all(glm::lessThanEqual(glm::abs(*va - std::get<glm::dvec3>(b)), glm::dvec3(eps)));
    if (const auto* da = std::get_if<double>(&a))
        return std::abs(*da - std::get<double>(b)) <= eps;
    return a == b;
}

bool shapeType(const NodeType& t) { return t.isA("mesh") || t.isA("camera") || t.isA("lattice") || t.isA("imagePlane"); }

bool poseable(const Node& n) { return n.type().isA("transform") && !shapeType(n.type()); }

/** The full path with every name's namespace stripped: "|Body|L|Arm". */
std::string barePath(const Graph& g, NodeId id)
{
    std::string out;
    for (NodeId cur = id; cur != kNoNode; cur = g.parent(cur))
        out = "|" + stripNamespace(g.node(cur)->name()) + out;
    return out;
}

/**
 * Pose node names to scene nodes: by name without namespace, preferring nameSpace among equals. A name with a '|' is
 * a barePath, which poses use for nodes whose name alone is shared.
 */
struct NameIndex
{
    std::unordered_map<std::string, std::vector<NodeId>> byName;
    std::unordered_map<std::string, std::vector<NodeId>> byPath;

    NameIndex(Graph& g, const std::vector<NodeId>& scope)
    {
        for (NodeId id : scope.empty() ? g.nodes() : scope)
            if (Node* n = g.node(id); n && poseable(*n))
            {
                byName[stripNamespace(n->name())].push_back(id);
                byPath[barePath(g, id)].push_back(id);
            }
    }

    NodeId find(Graph& g, const std::string& name, const std::string& nameSpace) const
    {
        const auto& map = name.find('|') == std::string::npos ? byName : byPath;
        auto it = map.find(name);
        if (it == map.end() || it->second.empty())
            return kNoNode;
        for (NodeId id : it->second)
            if (g.node(id)->nameSpace() == nameSpace)
                return id;
        return it->second.front();
    }
};

void mergeAliases(json& into, const json& from)
{
    if (!from.is_object())
        return;
    for (auto& [name, names] : from.items())
    {
        json& list = into[name];
        if (!list.is_array())
            list = json::array();
        if (names.is_string())
            list.push_back(names);
        else if (names.is_array())
            for (const json& n : names)
                if (n.is_string())
                    list.push_back(n);
    }
}

void reflect(json& vals, char axis)
{
    const int a = axis == 'y' ? 1 : axis == 'z' ? 2 : 0;
    if (vals.contains("translate") && vals["translate"].is_array() && vals["translate"].size() == 3)
        vals["translate"][a] = -vals["translate"][a].get<double>();
    if (vals.contains("rotate") && vals["rotate"].is_array())
    {
        // Rotations are axial vectors: under a reflection the two in-plane axes flip.
        json& r = vals["rotate"];
        for (int i = 0; i < 3; ++i)
            if (i != a)
                r[i] = -r[i].get<double>();
    }
}

const std::pair<const char*, const char*> kSidePairs[] = {
    {"_l", "_r"}, {"_L", "_R"}, {"_left", "_right"}, {"_Left", "_Right"}, {"Left", "Right"}, {"left", "right"}, {"L_", "R_"}, {"l_", "r_"},
};

Graph& graph() { return Graph::current(); }

std::vector<NodeId> namedNodes(const json& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const json& n : names)
    {
        Node* node = n.is_string() ? graph().find(n.get<std::string>()) : nullptr;
        if (!node)
            throw CommandError(std::string(cmd) + ": no node named '" + (n.is_string() ? n.get<std::string>() : n.dump()) + "'");
        out.push_back(node->id());
    }
    return out;
}

/** The nodes a command acts on: "nodes" or the selection. */
std::vector<NodeId> targetNodes(const Args& a, const char* cmd)
{
    if (a.has("nodes"))
        return namedNodes(a.flags["nodes"], cmd);
    return graph().selection();
}

std::vector<std::string> stringList(const Args& a, const char* flag)
{
    std::vector<std::string> out;
    if (!a.has(flag))
        return out;
    const json& j = a.flags[flag];
    if (j.is_string())
        out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& e : j)
            if (e.is_string())
                out.push_back(e.get<std::string>());
    return out;
}

std::optional<Tier> tierFlag(const Args& a, const char* cmd)
{
    if (!a.has("tier"))
        return std::nullopt;
    std::optional<Tier> t = tierFromName(a.get<std::string>("tier"));
    if (!t)
        throw CommandError(std::string(cmd) + ": tier is scene, workspace, user or template");
    return t;
}

/** The character a command means: the flag, else the one the target nodes belong to. */
std::string characterFor(const Args& a, const std::vector<NodeId>& nodes, Character* found = nullptr)
{
    Character ch;
    if (!nodes.empty())
        ch = characterOf(graph(), nodes.front());
    if (found)
        *found = ch;
    return a.has("character") ? a.get<std::string>("character") : ch.id;
}

json summary(const Entry& e)
{
    return {{"name", e.pose.name},     {"character", e.pose.character}, {"template", e.pose.templateId},
            {"tier", tierName(e.tier)}, {"tags", e.pose.tags},           {"reserved", isReserved(e.pose.name)},
            {"thumbnail", !e.pose.thumbnail.empty()}, {"nodes", e.pose.nodes.size()}};
}

Entry requirePose(const std::string& character, const std::string& name, std::optional<Tier> tier, const char* cmd)
{
    std::optional<Entry> e = poses::find(character, name, tier);
    if (!e)
        throw CommandError(std::string(cmd) + ": no pose '" + name + "'" + (character.empty() ? "" : " for " + character) +
                           (tier ? std::string(" in the ") + tierName(*tier) + " tier" : ""));
    return *e;
}

std::string requireName(const Args& a, size_t i, const char* cmd)
{
    if (a.positional.size() <= i || !a.positional[i].is_string() || a.positional[i].get<std::string>().empty())
        throw CommandError(std::string(cmd) + ": give a pose name");
    return a.positional[i].get<std::string>();
}

ApplyReport applyAndReport(const Pose& pose, const ApplyOptions& opts)
{
    ApplyReport r = apply(graph(), pose, opts);
    if (!r.missing.empty())
        log::warn("Pose '{}': {} node(s) not found and skipped", pose.name, r.missing.size());
    return r;
}
} // namespace

const char* tierName(Tier t)
{
    switch (t)
    {
    case Tier::Scene: return "scene";
    case Tier::Workspace: return "workspace";
    case Tier::User: return "user";
    case Tier::Template: return "template";
    }
    return "scene";
}

std::optional<Tier> tierFromName(std::string_view name)
{
    for (Tier t : kTiers)
        if (name == tierName(t))
            return t;
    return std::nullopt;
}

json Pose::toJson() const
{
    json j = {{"name", name}, {"character", character}, {"nodes", nodes}};
    if (!templateId.empty())
        j["template"] = templateId;
    if (!tags.empty())
        j["tags"] = tags;
    if (!thumbnail.empty())
        j["thumbnail"] = base64Encode(thumbnail);
    return j;
}

Pose Pose::fromJson(const json& j)
{
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string())
        throw std::invalid_argument("a pose needs a name");
    Pose p;
    p.name = j["name"].get<std::string>();
    if (j.contains("character") && j["character"].is_string())
        p.character = j["character"].get<std::string>();
    if (j.contains("template") && j["template"].is_string())
        p.templateId = j["template"].get<std::string>();
    if (j.contains("tags") && j["tags"].is_array())
        for (const json& t : j["tags"])
            if (t.is_string())
                p.tags.push_back(t.get<std::string>());
    if (j.contains("thumbnail") && j["thumbnail"].is_string())
        p.thumbnail = base64Decode(j["thumbnail"].get<std::string>());
    if (j.contains("nodes") && j["nodes"].is_object())
        p.nodes = j["nodes"];
    return p;
}

json ApplyReport::toJson() const
{
    return {{"nodes", nodes}, {"channels", channels}, {"missing", missing}, {"locked", locked}};
}

bool isReserved(std::string_view name)
{
    std::string l = lower(std::string(name));
    return l == "bind" || l == "ref" || l == "test";
}

std::string stripNamespace(std::string_view name)
{
    size_t colon = name.rfind(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

std::string mirrorNodeName(const std::string& name)
{
    for (auto [left, right] : kSidePairs)
    {
        if (std::string m = rig::mirrorName(name, left, right); m != name)
            return m;
        if (std::string m = rig::mirrorName(name, right, left); m != name)
            return m;
    }
    return name;
}

void setCharacterResolver(CharacterResolver fn)
{
    state().resolver = std::move(fn);
}

Character characterOf(Graph& g, NodeId node)
{
    if (!g.node(node))
        return {};
    if (state().resolver)
        if (std::optional<Character> c = state().resolver(g, node))
            return *c;
    Character c;
    auto isJoint = [&](NodeId id) { return id != kNoNode && rig::isJoint(*g.node(id)); };
    NodeId root = node;
    if (isJoint(node))
        while (isJoint(g.parent(root)))
            root = g.parent(root);
    else
        while (g.parent(root) != kNoNode)
            root = g.parent(root);
    c.root = root;
    NodeId named = root;
    if (!isJoint(root))
        for (NodeId d : g.descendants(root))
            if (isJoint(d) && !isJoint(g.parent(d)))
            {
                named = d;
                break;
            }
    c.id = stripNamespace(g.node(named)->name());
    c.nameSpace = g.node(root)->nameSpace();
    if (poseable(*g.node(root)))
        c.nodes.push_back(root);
    for (NodeId d : g.descendants(root))
        if (poseable(*g.node(d)))
            c.nodes.push_back(d);
    return c;
}

bool channelWanted(const std::vector<std::string>& channels, const std::string& attr)
{
    if (channels.empty())
        return true;
    for (const std::string& c : channels)
        if (c == attr || (c == "t" && attr == "translate") || (c == "r" && attr == "rotate") || (c == "s" && attr == "scale"))
            return true;
    return false;
}

Pose capture(Graph& g, const std::vector<NodeId>& nodes, const std::vector<std::string>& channels)
{
    LILA_ZONE();

    Pose p;
    std::unordered_map<std::string, int> named;
    for (NodeId id : nodes)
        if (Node* n = g.node(id); n && poseable(*n))
            ++named[stripNamespace(n->name())];
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n || !poseable(*n))
            continue;
        json vals = json::object();
        const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
        const auto& attrs = n->type().attrs;
        for (int a = 0; a < int(attrs.size()); ++a)
        {
            const AttrDef& def = attrs[size_t(a)];
            // Poses hold values in whatever space is active, so the space itself stays out of them.
            if (!(def.flags & AttrKeyable) || !(def.flags & AttrInput) || a == Transform::kVisibility || a == Transform::kSpace ||
                !channelWanted(channels, def.name))
                continue;
            PlugValue v = g.value({id, a, -1});
            if (a == Transform::kRotate)
                vals[def.name] = jsonOf(quatOfEuler(std::get<glm::dvec3>(v), order));
            else if (def.type == PlugType::Vec3)
                vals[def.name] = jsonOf(std::get<glm::dvec3>(v));
            else if (def.type == PlugType::Float)
                vals[def.name] = std::get<double>(v);
            else if (def.type == PlugType::Int || def.type == PlugType::Enum)
                vals[def.name] = std::get<int64_t>(v);
            else if (def.type == PlugType::Bool)
                vals[def.name] = std::get<bool>(v);
        }
        if (!vals.empty())
            p.nodes[named[stripNamespace(n->name())] > 1 ? barePath(g, id) : stripNamespace(n->name())] = std::move(vals);
    }
    return p;
}

ApplyReport apply(Graph& g, const Pose& source, const ApplyOptions& opts)
{
    LILA_ZONE();

    const Pose& pose = opts.mirror ? mirrored(g, source, opts.scope, opts.mirrorAxis) : source;
    ApplyReport report;
    NameIndex index(g, opts.scope);
    json aliases = json::object();
    for (const AliasSet& s : state().aliases)
        if (s.character == pose.character)
            mergeAliases(aliases, s.aliases);
    mergeAliases(aliases, opts.aliases);
    const double w = std::clamp(opts.weight, 0.0, 1.0);

    for (auto& [name, vals] : pose.nodes.items())
    {
        NodeId id = index.find(g, name, opts.nameSpace);
        if (id == kNoNode && aliases.contains(name))
            for (const json& alt : aliases[name])
                if ((id = index.find(g, alt.get<std::string>(), opts.nameSpace)) != kNoNode)
                    break;
        if (id == kNoNode)
        {
            report.missing.push_back(name);
            continue;
        }
        Node* n = g.node(id);
        const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
        int set = 0;
        for (auto& [attr, value] : vals.items())
        {
            if (!channelWanted(opts.channels, attr))
                continue;
            const int a = n->type().attrIndex(attr);
            if (a < 0)
                continue;
            const AttrDef& def = n->type().attrs[size_t(a)];
            const PlugRef plug{id, a, -1};
            PlugValue current = g.value(plug);
            PlugValue next = current;
            try
            {
                if (a == Transform::kRotate)
                {
                    const glm::dvec3 euler = std::get<glm::dvec3>(current);
                    const glm::dquat q = glm::slerp(quatOfEuler(euler, order), quatOf(value, order), w);
                    next = xformops::closestEuler(glm::mat3_cast(q), order, euler);
                }
                else if (def.type == PlugType::Vec3)
                    next = glm::mix(std::get<glm::dvec3>(current), vec3Of(value), w);
                else if (def.type == PlugType::Float)
                    next = std::get<double>(current) + (value.get<double>() - std::get<double>(current)) * w;
                else if (def.type == PlugType::Int || def.type == PlugType::Enum)
                    next = w >= 0.5 ? value.get<int64_t>() : std::get<int64_t>(current);
                else if (def.type == PlugType::Bool)
                    next = w >= 0.5 ? value.get<bool>() : std::get<bool>(current);
                else
                    continue;
            }
            catch (const std::exception&)
            {
                continue; // a malformed value: leave the channel alone
            }
            if (g.anyLocked(plug))
                next = g.keepLocked(plug, next);
            if (nearlyEqual(next, current))
                continue;
            try
            {
                g.setValue(plug, next);
                ++set;
            }
            catch (const std::invalid_argument&)
            {
                report.locked.push_back(g.plugName(plug));
            }
        }
        report.channels += set;
        report.nodes += set > 0;
    }
    return report;
}

Pose mirrored(Graph& g, const Pose& pose, const std::vector<NodeId>& scope, char axis)
{
    Pose out = pose;
    out.nodes = json::object();
    NameIndex index(g, scope);
    auto jointNamed = [&](const std::string& name) -> std::optional<bool> {
        NodeId id = index.find(g, name, {});
        if (id == kNoNode)
            return std::nullopt;
        return rig::isJoint(*g.node(id));
    };
    for (auto& [name, vals] : pose.nodes.items())
    {
        const std::string other = mirrorNodeName(name);
        json next = vals;
        const bool joint = jointNamed(other).value_or(jointNamed(name).value_or(true));
        if (other != name && joint)
        {
            // Mirrored joints have mirrored axes: the same rotation mirrors the motion, and offsets point the other way.
            if (next.contains("translate") && next["translate"].is_array())
                for (json& c : next["translate"])
                    c = -c.get<double>();
        }
        else
            reflect(next, axis);
        out.nodes[other] = std::move(next);
    }
    return out;
}

fs::path tierDir(Tier t, bool create)
{
    std::error_code ec;
    fs::path dir;
    if (t == Tier::Workspace)
    {
        if (!Workspace::get().isOpen())
            return {};
        dir = Workspace::get().stateDir(create);
        if (dir.empty())
            return {};
        dir /= "poses";
    }
    else if (t == Tier::User)
        dir = paths::userConfigDir() / "poses";
    else
        return {};
    if (create)
        fs::create_directories(dir, ec);
    return dir;
}

std::vector<Entry> list(const std::string& character, std::optional<Tier> tier, bool all)
{
    std::vector<Entry> raw;
    for (Tier t : kTiers)
        if (!tier || *tier == t)
            tierPoses(t, character, raw);
    std::vector<Entry> out;
    for (Entry& e : raw)
    {
        if (!all && std::any_of(out.begin(), out.end(), [&](const Entry& o) { return o.pose.character == e.pose.character && o.pose.name == e.pose.name; }))
            continue;
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.pose.character != b.pose.character)
            return a.pose.character < b.pose.character;
        if (isReserved(a.pose.name) != isReserved(b.pose.name))
            return isReserved(a.pose.name);
        return lower(a.pose.name) < lower(b.pose.name);
    });
    return out;
}

std::optional<Entry> find(const std::string& character, const std::string& name, std::optional<Tier> tier)
{
    for (Tier t : kTiers)
    {
        if (tier && *tier != t)
            continue;
        std::vector<Entry> found;
        tierPoses(t, character, found);
        for (Entry& e : found)
            if (e.pose.name == name)
                return std::move(e);
    }
    return std::nullopt;
}

std::vector<std::string> characters()
{
    std::vector<std::string> out;
    for (const Entry& e : list({}, std::nullopt, true))
        if (std::find(out.begin(), out.end(), e.pose.character) == out.end())
            out.push_back(e.pose.character);
    std::sort(out.begin(), out.end());
    return out;
}

void save(const Pose& pose, Tier tier)
{
    switch (tier)
    {
    case Tier::Scene:
    {
        std::vector<Pose> next = state().scene;
        auto it = std::find_if(next.begin(), next.end(), [&](const Pose& p) { return p.character == pose.character && p.name == pose.name; });
        if (it != next.end())
            *it = pose;
        else
            next.push_back(pose);
        replaceScene(std::move(next));
        return;
    }
    case Tier::Workspace:
    case Tier::User:
    {
        fs::path file = poseFile(tier, pose.character, pose.name, true);
        if (file.empty())
            throw std::runtime_error("no workspace is open");
        writeFile(file, pose);
        notify();
        return;
    }
    case Tier::Template: throw std::runtime_error("template poses are registered by extensions");
    }
}

bool remove(const std::string& character, const std::string& name, Tier tier)
{
    switch (tier)
    {
    case Tier::Scene:
    {
        std::vector<Pose> next = state().scene;
        if (std::erase_if(next, [&](const Pose& p) { return p.character == character && p.name == name; }) == 0)
            return false;
        replaceScene(std::move(next));
        return true;
    }
    case Tier::Workspace:
    case Tier::User:
    {
        std::optional<Entry> e = poses::find(character, name, tier);
        std::error_code ec;
        if (!e || e->file.empty() || !fs::remove(e->file, ec))
            return false;
        notify();
        return true;
    }
    case Tier::Template: return false;
    }
    return false;
}

void rename(const std::string& character, const std::string& from, const std::string& to, Tier tier)
{
    std::optional<Entry> e = poses::find(character, from, tier);
    if (!e)
        throw std::runtime_error("no pose '" + from + "'");
    if (poses::find(character, to, tier))
        throw std::runtime_error("a pose '" + to + "' exists already");
    Pose p = e->pose;
    p.name = to;
    if (tier == Tier::Scene)
    {
        std::vector<Pose> next = state().scene;
        for (Pose& o : next)
            if (o.character == character && o.name == from)
                o = p;
        replaceScene(std::move(next));
        return;
    }
    if (tier == Tier::Template)
        throw std::runtime_error("template poses are read only");
    writeFile(poseFile(tier, character, to, true), p);
    std::error_code ec;
    fs::remove(e->file, ec);
    notify();
}

void registerTemplatePose(const Pose& pose, const std::string& owner)
{
    auto& t = state().templates;
    std::erase_if(t, [&](const Owned& o) { return o.pose.character == pose.character && o.pose.name == pose.name; });
    t.push_back({pose, owner});
    notify();
}

void registerAliases(const std::string& character, const json& aliases, const std::string& owner)
{
    state().aliases.push_back({character, aliases, owner});
}

void unregisterOwned(const std::string& owner)
{
    std::erase_if(state().aliases, [&](const AliasSet& s) { return s.owner == owner; });
    if (std::erase_if(state().templates, [&](const Owned& o) { return o.owner == owner; }) > 0)
        notify();
}

json sceneToJson()
{
    json out = json::array();
    for (const Pose& p : state().scene)
        out.push_back(p.toJson());
    return out;
}

void sceneFromJson(const json& j)
{
    std::vector<Pose> next;
    if (j.is_array())
        for (const json& e : j)
            try
            {
                next.push_back(Pose::fromJson(e));
            }
            catch (const std::exception& ex)
            {
                log::warn("Skipped a scene pose: {}", ex.what());
            }
    state().scene = std::move(next);
    notify();
}

void clearScene()
{
    if (state().scene.empty())
        return;
    state().scene.clear();
    notify();
}

int addListener(std::function<void()> fn)
{
    State& s = state();
    s.listeners.emplace(s.nextListener, std::move(fn));
    return s.nextListener++;
}

void removeListener(int id)
{
    state().listeners.erase(id);
}

void setThumbnailProvider(std::function<std::string(const std::vector<NodeId>&)> fn)
{
    state().thumbnail = std::move(fn);
}

std::string base64Encode(std::string_view bytes)
{
    static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
        if (i + 1 < bytes.size())
            v |= uint32_t(uint8_t(bytes[i + 1])) << 8;
        if (i + 2 < bytes.size())
            v |= uint32_t(uint8_t(bytes[i + 2]));
        out += kChars[(v >> 18) & 63];
        out += kChars[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kChars[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kChars[v & 63] : '=';
    }
    return out;
}

std::string base64Decode(std::string_view text)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text)
    {
        int v = value(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out += char((acc >> bits) & 0xff);
        }
    }
    return out;
}

void registerCommands()
{
    using T = ArgType;
    const std::vector<std::string> tierChoices = {"scene", "workspace", "user", "template"};

    ops::registerCommand({
        .name = "savePose",
        .doc = "Store the current pose of the selected nodes (and everything under them) in the pose library, keyed by "
               "node name without namespace, rotations as quaternions. The character is the selection's root joint (or "
               "the given one). bind, ref and test are each character's reserved poses. Returns the pose's summary.",
        .positionalDoc = "name",
        .flags = {
            {"tier", "ti", T::String, "scene", "scene (saved in the scene file), workspace (.lila/poses) or user.", {"scene", "workspace", "user"}},
            {"character", "ch", T::String, nullptr, "The library group; defaults to the selection's character."},
            {"nodes", "n", T::List, nullptr, "Nodes to save instead of the selection."},
            {"hierarchy", "hi", T::Bool, true, "Include everything under the nodes."},
            {"channels", "c", T::List, nullptr, "Only these channels: translate, rotate, scale or attr names."},
            {"tags", "tg", T::List, nullptr, "Search tags."},
            {"thumbnail", "th", T::Bool, true, "Capture a thumbnail from the viewport (GUI only)."},
        },
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, 0, "savePose");
            std::vector<NodeId> seeds = targetNodes(a, "savePose");
            if (seeds.empty())
                throw CommandError("savePose: select the nodes to save");
            Graph& g = graph();
            std::vector<NodeId> nodes;
            for (NodeId id : seeds)
            {
                if (std::find(nodes.begin(), nodes.end(), id) == nodes.end())
                    nodes.push_back(id);
                if (a.get<bool>("hierarchy", true))
                    for (NodeId d : g.descendants(id))
                        if (std::find(nodes.begin(), nodes.end(), d) == nodes.end())
                            nodes.push_back(d);
            }
            Character ch;
            Pose p = capture(g, nodes, stringList(a, "channels"));
            if (p.nodes.empty())
                throw CommandError("savePose: nothing poseable selected");
            p.name = name;
            p.character = characterFor(a, seeds, &ch);
            p.templateId = ch.templateId;
            p.tags = stringList(a, "tags");
            if (a.get<bool>("thumbnail", true) && state().thumbnail)
                p.thumbnail = state().thumbnail(seeds);
            Tier tier = tierFlag(a, "savePose").value_or(Tier::Scene);
            try
            {
                save(p, tier);
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("savePose: ") + e.what());
            }
            return summary({p, tier, {}});
        },
    });

    ops::registerCommand({
        .name = "applyPose",
        .doc = "Apply a library pose to the selected character by node name: namespaces are ignored, aliases tried, "
               "weight blends from the current pose (rotations slerp), mirror applies it to the other side. Names the "
               "rig doesn't have are skipped and reported. Returns {nodes, channels, missing, locked}.",
        .positionalDoc = "name",
        .flags = {
            {"character", "ch", T::String, nullptr, "The library group; defaults to the selection's character."},
            {"tier", "ti", T::String, nullptr, "Only look in this tier.", tierChoices},
            {"nodes", "n", T::List, nullptr, "Nodes picking the character instead of the selection."},
            {"weight", "w", T::Float, 1.0, "0 keeps the current pose, 1 applies the stored one.", {}, {}, 0.0, 1.0},
            {"mirror", "m", T::Bool, false, "Apply to the opposite side (arm_l values onto arm_r)."},
            {"mirrorAxis", "ma", T::String, "x", "Axis the mirror plane faces.", {"x", "y", "z"}},
            {"channels", "c", T::List, nullptr, "Only these channels: translate, rotate, scale or attr names."},
            {"aliases", "al", T::Any, nullptr, "Extra names per pose node: {poseName: [name, ...]}."},
            {"namespace", "ns", T::String, nullptr, "Preferred namespace; defaults to the character's."},
            {"onlySelected", "os", T::Bool, false, "Only the selected nodes, not the whole character."},
        },
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, 0, "applyPose");
            std::vector<NodeId> seeds = targetNodes(a, "applyPose");
            Character ch;
            std::string character = characterFor(a, seeds, &ch);
            Entry e = requirePose(character, name, tierFlag(a, "applyPose"), "applyPose");
            ApplyOptions o;
            o.weight = a.get<double>("weight", 1.0);
            o.mirror = a.get<bool>("mirror");
            o.mirrorAxis = a.get<std::string>("mirrorAxis", "x").c_str()[0];
            o.channels = stringList(a, "channels");
            if (a.has("aliases"))
                o.aliases = a.flags["aliases"];
            o.scope = a.get<bool>("onlySelected") ? seeds : ch.nodes;
            o.nameSpace = a.has("namespace") ? a.get<std::string>("namespace") : ch.nameSpace;
            return applyAndReport(e.pose, o).toJson();
        },
    });

    ops::registerCommand({
        .name = "mirrorPose",
        .doc = "With a pose name, store its mirror image as a new pose (newName, default <name>_mirror). Without one, "
               "mirror the selected character's current pose onto itself: left and right swap, centre nodes reflect.",
        .positionalDoc = "name",
        .flags = {
            {"newName", "nn", T::String, nullptr, "Name of the mirrored copy."},
            {"character", "ch", T::String, nullptr, "The library group; defaults to the selection's character."},
            {"tier", "ti", T::String, nullptr, "Where the source is looked up and the copy saved (template copies go to scene).", tierChoices},
            {"nodes", "n", T::List, nullptr, "Nodes picking the character instead of the selection."},
            {"mirrorAxis", "ma", T::String, "x", "Axis the mirror plane faces.", {"x", "y", "z"}},
        },
        .run = [](const Args& a) -> json {
            std::vector<NodeId> seeds = targetNodes(a, "mirrorPose");
            Character ch;
            std::string character = characterFor(a, seeds, &ch);
            const char axis = a.get<std::string>("mirrorAxis", "x").c_str()[0];
            Graph& g = graph();
            if (a.positional.empty())
            {
                if (seeds.empty())
                    throw CommandError("mirrorPose: select a character or give a pose name");
                Pose live = capture(g, ch.nodes);
                live.name = "mirror";
                live.character = character;
                ApplyOptions o;
                o.mirror = true;
                o.mirrorAxis = axis;
                o.scope = ch.nodes;
                o.nameSpace = ch.nameSpace;
                return applyAndReport(live, o).toJson();
            }
            std::string name = requireName(a, 0, "mirrorPose");
            Entry e = requirePose(character, name, tierFlag(a, "mirrorPose"), "mirrorPose");
            Pose m = mirrored(g, e.pose, ch.nodes, axis);
            m.name = a.has("newName") ? a.get<std::string>("newName") : name + "_mirror";
            if (m.name.empty())
                throw CommandError("mirrorPose: the new name is empty");
            Tier tier = e.tier == Tier::Template ? Tier::Scene : e.tier;
            try
            {
                save(m, tier);
            }
            catch (const std::runtime_error& ex)
            {
                throw CommandError(std::string("mirrorPose: ") + ex.what());
            }
            return summary({m, tier, {}});
        },
    });

    ops::registerCommand({
        .name = "listPoses",
        .doc = "List library poses for the selected character (or the given one, or every character with neither): "
               "[{name, character, template, tier, tags, reserved, thumbnail, nodes}], one per name with the first tier "
               "winning (scene, workspace, user, template) unless all is set.",
        .flags = {
            {"character", "ch", T::String, nullptr, "Only this library group; \"*\" for every character."},
            {"tier", "ti", T::String, nullptr, "Only this tier.", tierChoices},
            {"all", "a", T::Bool, false, "Include poses shadowed by a higher tier."},
            {"search", "s", T::String, nullptr, "Only names or tags containing this text (any case)."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string character = characterFor(a, graph().selection());
            if (character == "*")
                character.clear();
            std::string search = lower(a.get<std::string>("search"));
            json out = json::array();
            for (const Entry& e : list(character, tierFlag(a, "listPoses"), a.get<bool>("all")))
            {
                bool hit = search.empty() || lower(e.pose.name).find(search) != std::string::npos ||
                           std::any_of(e.pose.tags.begin(), e.pose.tags.end(), [&](const std::string& t) { return lower(t).find(search) != std::string::npos; });
                if (hit)
                    out.push_back(summary(e));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "deletePose",
        .doc = "Delete a library pose from its tier (the first tier holding it unless tier is given). Reserved poses "
               "(bind, ref, test) need force. Scene poses come back with undo; file poses don't.",
        .positionalDoc = "name",
        .flags = {
            {"character", "ch", T::String, nullptr, "The library group; defaults to the selection's character."},
            {"tier", "ti", T::String, nullptr, "The tier to delete from.", {"scene", "workspace", "user"}},
            {"force", "f", T::Bool, false, "Allow deleting a reserved pose."},
        },
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, 0, "deletePose");
            std::string character = characterFor(a, graph().selection());
            Entry e = requirePose(character, name, tierFlag(a, "deletePose"), "deletePose");
            if (isReserved(name) && !a.get<bool>("force"))
                throw CommandError("deletePose: '" + name + "' is a reserved pose; pass force to delete it");
            if (e.tier == Tier::Template)
                throw CommandError("deletePose: template poses are read only");
            if (!remove(e.pose.character, name, e.tier))
                throw CommandError("deletePose: couldn't delete '" + name + "'");
            return summary(e);
        },
    });

    ops::registerCommand({
        .name = "renamePose",
        .doc = "Rename a library pose within its tier. Reserved names (bind, ref, test) can't be renamed to or from.",
        .positionalDoc = "name newName",
        .flags = {
            {"character", "ch", T::String, nullptr, "The library group; defaults to the selection's character."},
            {"tier", "ti", T::String, nullptr, "The tier holding it; defaults to the first that does.", {"scene", "workspace", "user"}},
        },
        .run = [](const Args& a) -> json {
            std::string from = requireName(a, 0, "renamePose");
            std::string to = requireName(a, 1, "renamePose");
            if (isReserved(from) || isReserved(to))
                throw CommandError("renamePose: bind, ref and test are reserved names");
            std::string character = characterFor(a, graph().selection());
            Entry e = requirePose(character, from, tierFlag(a, "renamePose"), "renamePose");
            try
            {
                rename(e.pose.character, from, to, e.tier);
            }
            catch (const std::runtime_error& ex)
            {
                throw CommandError(std::string("renamePose: ") + ex.what());
            }
            e.pose.name = to;
            return summary(e);
        },
    });
}
} // namespace lila::scene::poses
