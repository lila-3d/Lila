#include "gn/Clipboard.h"

#include "core/Profile.h"
#include "gn/NodeLib.h"
#include "gn/Zones.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace lila::gn
{
namespace
{
/** Why a value doesn't fit a socket, or empty when it does. */
std::string valueProblem(const SocketDecl& s, const json& v)
{
    bool ok = true;
    switch (s.type)
    {
    case SocketType::Geometry:
    case SocketType::Bundle:
    case SocketType::Closure: ok = false; break;
    case SocketType::Matrix: ok = v.is_array() && v.size() == 16 && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); }); break;
    case SocketType::String: ok = v.is_string(); break;
    case SocketType::Vector:
    case SocketType::Rotation: ok = (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) || v.is_number(); break;
    case SocketType::Color:
        ok = (v.is_array() && (v.size() == 3 || v.size() == 4) && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); })) ||
             v.is_number() || (v.is_string() && v.get<std::string>().starts_with("#"));
        break;
    default: ok = v.is_number() || v.is_boolean(); break;
    }
    return ok ? std::string() : std::format("{} takes a {}, got {}", s.id, socketTypeName(s.type), v.dump());
}

/** The JSON object in the text: from the first '{' to the last '}', so prose and code fences around it don't matter. */
json extractJson(std::string_view text)
{
    size_t a = text.find('{'), b = text.rfind('}');
    if (a == std::string_view::npos || b == std::string_view::npos || b < a)
        throw std::invalid_argument("can't paste: no node data in the text");
    try
    {
        return json::parse(text.substr(a, b - a + 1));
    }
    catch (const json::parse_error& e)
    {
        throw std::invalid_argument(std::string("can't paste: the node data isn't valid JSON (") + e.what() + ")");
    }
}

bool isVec2(const json& j) { return j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number(); }

bool hasCycle(const std::vector<GLink>& links)
{
    std::map<std::string, std::vector<std::string>> next;
    for (const GLink& l : links)
        next[l.fromNode].push_back(l.toNode);
    std::map<std::string, int> state; // 1 visiting, 2 done
    std::function<bool(const std::string&)> visit = [&](const std::string& n) {
        int& s = state[n];
        if (s == 1)
            return true;
        if (s == 2)
            return false;
        s = 1;
        for (const std::string& m : next[n])
            if (visit(m))
                return true;
        state[n] = 2;
        return false;
    };
    for (auto& [n, list] : next)
        if (visit(n))
            return true;
    return false;
}
} // namespace

std::string copyNodesText(const Group& g, const std::vector<std::string>& names)
{
    LILA_ZONE();

    std::vector<const GNode*> picked;
    std::set<std::string> set;
    for (const std::string& name : withZonePartners(g, names))
    {
        const GNode* n = g.node(name);
        if (!n)
            throw std::invalid_argument("no node '" + name + "' in " + g.name);
        if (n->type == "GroupOutput" || !set.insert(name).second)
            continue;
        picked.push_back(n);
    }
    glm::dvec2 centre(0.0);
    for (const GNode* n : picked)
        centre += n->position;
    if (!picked.empty())
        centre /= double(picked.size());

    // Ordered, so the header reads first for whoever finds this pasted in a post.
    using ojson = nlohmann::ordered_json;
    auto ordered = [](const json& j) { return ojson::parse(j.dump()); };
    ojson nodes = ojson::array(), links = ojson::array(), inputs = ojson::array();
    for (const GNode* n : picked)
    {
        glm::dvec2 p = n->position - centre;
        ojson o = {{"name", n->name}, {"type", n->type}, {"position", {std::round(p.x), std::round(p.y)}}};
        if (!n->values.empty())
            o["values"] = ordered(n->values);
        putNodeFlagsTo(*n, o);
        nodes.push_back(std::move(o));
    }
    std::set<std::string> fed;
    for (const GLink& l : g.links)
        if (set.contains(l.fromNode) && set.contains(l.toNode))
        {
            links.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
            if (g.node(l.fromNode)->type == "GroupInput")
                fed.insert(l.fromSocket);
        }
    for (const GInput& in : g.inputs)
        if (fed.contains(in.name))
            inputs.push_back({{"name", in.name}, {"type", socketTypeName(in.type)}, {"default", ordered(in.defaultValue)}});

    ojson out = {{"format", kClipboardFormat}, {"version", kClipboardVersion}, {"nodes", nodes}, {"links", links}};
    if (!inputs.empty())
        out["inputs"] = inputs;
    if (std::vector<GTie> ties = tiesAmong(g.ties, set, {}, -centre); !ties.empty())
    {
        for (GTie& t : ties)
            t.position = glm::round(t.position);
        out["ties"] = ordered(tiesToJson(ties));
    }
    if (std::vector<GFrame> frames = framesAmong(g.frames, set, {}, -centre); !frames.empty())
    {
        for (GFrame& f : frames)
            f.position = glm::round(f.position);
        out["frames"] = ordered(framesToJson(frames));
    }
    return out.dump(2);
}

std::vector<std::string> pasteNodesText(Group& g, std::string_view text, glm::dvec2 at)
{
    LILA_ZONE();

    json j = extractJson(text);
    if (!j.is_object())
        throw std::invalid_argument("can't paste: the node data must be a JSON object");
    if (j.contains("format"))
    {
        if (!j["format"].is_string() || j["format"].get<std::string>() != kClipboardFormat)
            throw std::invalid_argument(std::format("can't paste: not Lila geometry nodes (format {})", j["format"].dump()));
        if (!j.contains("version") || !j["version"].is_number_integer())
            throw std::invalid_argument("can't paste: the version number is missing");
        if (int v = j["version"].get<int>(); v > kClipboardVersion)
            throw std::invalid_argument(std::format("can't paste: made by a newer Lila (version {}, this one reads up to {})", v, kClipboardVersion));
    }
    else if (!j.contains("nodes") || !j.contains("typeVersion"))
        throw std::invalid_argument("can't paste: not Lila geometry nodes (no \"format\": \"lila.geometryNodes\" header)");
    if (!j.contains("nodes") || !j["nodes"].is_array())
        throw std::invalid_argument("can't paste: \"nodes\" must be a list");

    std::vector<std::string> errors;
    Group next = g;

    for (const json& in : j.value("inputs", json::array()))
    {
        if (!in.is_object() || !in.contains("name") || !in["name"].is_string())
        {
            errors.push_back("an input has no name");
            continue;
        }
        std::string name = in["name"].get<std::string>();
        SocketType type = SocketType::Float;
        if (!validInputName(name))
            errors.push_back(std::format("input '{}' isn't a usable parameter name", name));
        else if (!in.value("type", json("float")).is_string() || !parseSocketType(in.value("type", "float"), &type) || type == SocketType::Geometry)
            errors.push_back(std::format("input '{}': unknown type {}", name, in.value("type", json()).dump()));
        else if (const GInput* have = next.input(name))
        {
            if (have->type != type)
                errors.push_back(std::format("input '{}' is a {} here but a {} in the pasted nodes", name, socketTypeName(have->type), socketTypeName(type)));
        }
        else
            next.inputs.push_back({name, type, in.value("default", json())});
    }

    std::vector<GNode> pasted;
    std::set<std::string> names, dropped;
    for (const json& n : j["nodes"])
    {
        if (!n.is_object() || !n.contains("name") || !n["name"].is_string() || !n.contains("type") || !n["type"].is_string())
        {
            errors.push_back("a node has no name or type");
            continue;
        }
        GNode gn{n["name"].get<std::string>(), n["type"].get<std::string>(), {0.0, 0.0}, json::object()};
        getNodeFlags(gn, n);
        if (!validIdentifier(gn.name))
            errors.push_back(std::format("'{}' isn't a valid node name", gn.name));
        else if (!names.insert(gn.name).second)
            errors.push_back(std::format("two nodes are named '{}'", gn.name));
        if (gn.type == "GroupOutput")
        {
            dropped.insert(gn.name);
            continue;
        }
        if (!findDecl(gn.type))
            errors.push_back(std::format("node '{}': unknown type '{}'", gn.name, gn.type));
        if (n.contains("position"))
        {
            if (isVec2(n["position"]))
                gn.position = {n["position"][0].get<double>(), n["position"][1].get<double>()};
            else
                errors.push_back(std::format("node '{}': position must be [x, y]", gn.name));
        }
        if (n.contains("values"))
        {
            if (n["values"].is_object())
                gn.values = n["values"];
            else
                errors.push_back(std::format("node '{}': values must be an object", gn.name));
        }
        pasted.push_back(std::move(gn));
    }
    if (pasted.empty() && errors.empty())
        errors.push_back("there are no nodes to paste");

    // Sockets can depend on the group and on other values, so they're resolved against the group as it will be.
    Group probe = next;
    probe.nodes = pasted;
    std::map<std::string, Sockets> sockets;
    for (const GNode& n : pasted)
    {
        const NodeDecl* d = findDecl(n.type);
        if (!d)
            continue;
        Sockets s = d->socketsFor(probe, n);
        for (auto& [key, v] : n.values.items())
        {
            if (isStructuralValue(n.type, key))
                continue;
            if (const PropDecl* p = d->prop(key))
            {
                if (!v.is_string())
                    errors.push_back(std::format("node '{}': {} takes a string", n.name, key));
                else if (!p->options.empty() && std::find(p->options.begin(), p->options.end(), v.get<std::string>()) == p->options.end())
                    errors.push_back(std::format("node '{}': {} can't be '{}'", n.name, key, v.get<std::string>()));
                continue;
            }
            int ii = s.inputIndex(key);
            if (ii < 0)
                errors.push_back(std::format("node '{}' ({}) has no input or property '{}'", n.name, n.type, key));
            else if (std::string why = valueProblem(s.inputs[ii], v); !why.empty())
                errors.push_back(std::format("node '{}': {}", n.name, why));
        }
        sockets[n.name] = std::move(s);
    }

    std::vector<GLink> links;
    std::set<std::pair<std::string, std::string>> linkedInputs;
    if (j.contains("links") && !j["links"].is_array())
        errors.push_back("\"links\" must be a list");
    for (const json& l : j.value("links", json::array()))
    {
        if (!l.is_array() || l.size() != 4 || !l[0].is_string() || !l[1].is_string() || !l[2].is_string() || !l[3].is_string())
        {
            errors.push_back(std::format("bad link {}: expected [fromNode, fromSocket, toNode, toSocket]", l.dump()));
            continue;
        }
        GLink link{l[0].get<std::string>(), l[1].get<std::string>(), l[2].get<std::string>(), l[3].get<std::string>()};
        if (dropped.contains(link.fromNode) || dropped.contains(link.toNode))
            continue;
        auto from = sockets.find(link.fromNode), to = sockets.find(link.toNode);
        if (!names.contains(link.fromNode) || !names.contains(link.toNode))
        {
            errors.push_back(std::format("link {}.{} -> {}.{} names a node that isn't pasted", link.fromNode, link.fromSocket, link.toNode, link.toSocket));
            continue;
        }
        if (from == sockets.end() || to == sockets.end())
            continue; // the node's own error is reported already
        int oi = from->second.outputIndex(link.fromSocket), ii = to->second.inputIndex(link.toSocket);
        if (oi < 0)
            errors.push_back(std::format("{} has no output '{}'", link.fromNode, link.fromSocket));
        if (ii < 0)
            errors.push_back(std::format("{} has no input '{}'", link.toNode, link.toSocket));
        if (oi < 0 || ii < 0)
            continue;
        if (!linkable(from->second.outputs[oi].type, to->second.inputs[ii].type))
            errors.push_back(std::format("can't link {}.{} ({}) to {}.{} ({})", link.fromNode, link.fromSocket, socketTypeName(from->second.outputs[oi].type),
                                         link.toNode, link.toSocket, socketTypeName(to->second.inputs[ii].type)));
        else if (!to->second.inputs[ii].multi && !linkedInputs.insert({link.toNode, link.toSocket}).second)
            errors.push_back(std::format("{}.{} has more than one link into it", link.toNode, link.toSocket));
        else
            links.push_back(link);
    }
    if (hasCycle(links))
        errors.push_back("the links make a cycle");

    if (!errors.empty())
    {
        std::string msg = "can't paste: ";
        size_t shown = std::min<size_t>(errors.size(), 8);
        for (size_t i = 0; i < shown; ++i)
            msg += (i ? "; " : "") + errors[i];
        if (errors.size() > shown)
            msg += std::format("; and {} more", errors.size() - shown);
        throw std::invalid_argument(msg);
    }

    glm::dvec2 centre(0.0);
    for (const GNode& n : pasted)
        centre += n.position;
    centre /= double(pasted.size());
    std::map<std::string, std::string> renamed;
    std::vector<std::string> out;
    const size_t firstPasted = next.nodes.size();
    for (GNode n : pasted)
    {
        std::string old = n.name;
        n.name = next.uniqueNodeName(old);
        n.position = n.position - centre + at;
        renamed[old] = n.name;
        out.push_back(n.name);
        next.nodes.push_back(std::move(n));
    }
    for (size_t k = firstPasted; k < next.nodes.size(); ++k)
        retargetZoneCopy(next.nodes[k], renamed);
    for (const GLink& l : links)
        next.links.push_back({renamed[l.fromNode], l.fromSocket, renamed[l.toNode], l.toSocket});
    std::set<std::string> kept;
    for (const GNode& n : pasted)
        kept.insert(n.name);
    for (GTie& t : tiesAmong(tiesFromJson(j.value("ties", json::array())), kept, renamed, at - centre))
        next.ties.push_back(std::move(t));
    pruneTies(next.ties, next.links);
    for (GFrame& f : framesAmong(framesFromJson(j.value("frames", json::array())), kept, renamed, at - centre))
        next.frames.push_back(std::move(f));
    g = std::move(next);
    return out;
}
} // namespace lila::gn
