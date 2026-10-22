#include "gn/Group.h"

#include <set>
#include <stdexcept>

namespace lila::gn
{
bool validIdentifier(std::string_view s)
{
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!alpha && !(digit && i > 0))
            return false;
    }
    return true;
}

bool validInputName(std::string_view s)
{
    static const std::set<std::string, std::less<>> reserved = {"inMesh", "enabled", "groupDefinition", "outMesh", "geometry", "message", "caching"};
    return validIdentifier(s) && !reserved.contains(s) && !s.starts_with("obj_");
}

json Group::toJson() const
{
    json jin = json::array();
    for (const GInput& in : inputs)
        jin.push_back({{"name", in.name}, {"type", socketTypeName(in.type)}, {"default", in.defaultValue}});
    json jn = json::array();
    for (const GNode& n : nodes)
    {
        json o = {{"name", n.name}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
        if (!n.values.empty())
            o["values"] = n.values;
        jn.push_back(std::move(o));
    }
    json jl = json::array();
    for (const GLink& l : links)
        jl.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
    return {{"name", name}, {"typeVersion", typeVersion}, {"inputs", jin}, {"nodes", jn}, {"links", jl}};
}

Group Group::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("group definition must be an object");
    Group g;
    try
    {
        g.name = j.at("name").get<std::string>();
        g.typeVersion = j.value("typeVersion", 1);
        for (const json& in : j.value("inputs", json::array()))
        {
            GInput gi;
            gi.name = in.at("name").get<std::string>();
            if (!parseSocketType(in.value("type", "float"), &gi.type))
                throw std::invalid_argument("input " + gi.name + ": unknown type " + in.value("type", ""));
            gi.defaultValue = in.value("default", json());
            g.inputs.push_back(std::move(gi));
        }
        for (const json& n : j.value("nodes", json::array()))
        {
            GNode gn;
            gn.name = n.at("name").get<std::string>();
            gn.type = n.at("type").get<std::string>();
            if (n.contains("position") && n["position"].is_array() && n["position"].size() == 2)
                gn.position = {n["position"][0].get<double>(), n["position"][1].get<double>()};
            gn.values = n.value("values", json::object());
            g.nodes.push_back(std::move(gn));
        }
        for (const json& l : j.value("links", json::array()))
            g.links.push_back({l.at(0).get<std::string>(), l.at(1).get<std::string>(), l.at(2).get<std::string>(), l.at(3).get<std::string>()});
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad group definition: ") + e.what());
    }
    return g;
}

GNode* Group::node(std::string_view n)
{
    for (GNode& x : nodes)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GNode* Group::node(std::string_view n) const
{
    for (const GNode& x : nodes)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GInput* Group::input(std::string_view n) const
{
    for (const GInput& x : inputs)
        if (x.name == n)
            return &x;
    return nullptr;
}

const GNode* Group::firstOfType(std::string_view type) const
{
    for (const GNode& x : nodes)
        if (x.type == type)
            return &x;
    return nullptr;
}

std::string Group::uniqueNodeName(std::string_view base) const
{
    std::string b(base);
    if (!node(b))
        return b;
    for (int i = 1;; ++i)
    {
        std::string c = b + std::to_string(i);
        if (!node(c))
            return c;
    }
}

std::vector<GLink> Group::linksInto(std::string_view n, std::string_view socket) const
{
    std::vector<GLink> out;
    for (const GLink& l : links)
        if (l.toNode == n && l.toSocket == socket)
            out.push_back(l);
    return out;
}

Group Group::makeDefault(std::string name)
{
    Group g;
    g.name = std::move(name);
    g.nodes.push_back({"GroupInput", "GroupInput", {-300.0, 0.0}, json::object()});
    g.nodes.push_back({"GroupOutput", "GroupOutput", {300.0, 0.0}, json::object()});
    g.links.push_back({"GroupInput", "geometry", "GroupOutput", "geometry"});
    return g;
}
} // namespace lila::gn
