#include "tex/Ports.h"

namespace lila::tex
{
namespace
{
template <class T> const T* byId(const std::vector<T>& v, std::string_view id)
{
    for (const T& x : v)
        if (x.id == id)
            return &x;
    return nullptr;
}

template <class T> int indexOf(const std::vector<T>& v, std::string_view id)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].id == id)
            return int(i);
    return -1;
}

void interfaceOf(const Graph& inner, NodePorts& out)
{
    for (const GraphInput& in : inner.inputs)
    {
        if (in.param)
        {
            ParamDecl p;
            p.id = in.name;
            p.type = in.paramType;
            p.defaultValue = in.defaultValue;
            p.label = in.label;
            p.doc = in.description;
            p.panel = in.panel;
            p.options = in.options;
            p.min = in.min;
            p.max = in.max;
            p.softMin = in.softMin;
            p.softMax = in.softMax;
            p.step = in.step;
            out.params.push_back(std::move(p));
        }
        else
            out.inputs.push_back({in.name, in.port, in.defaultValue, in.label, in.description, 1});
    }
    for (const GraphOutput& o : inner.outputs)
        out.outputs.push_back({o.name, o.type, json(), {}, o.description, 1});
}
} // namespace

const PortDecl* NodePorts::input(std::string_view id) const { return byId(inputs, id); }
const PortDecl* NodePorts::output(std::string_view id) const { return byId(outputs, id); }
const ParamDecl* NodePorts::param(std::string_view id) const { return byId(params, id); }
int NodePorts::inputIndex(std::string_view id) const { return indexOf(inputs, id); }
int NodePorts::outputIndex(std::string_view id) const { return indexOf(outputs, id); }

NodePorts portsOf(const Graph& g, const GNode& n, const ChannelList* extra)
{
    NodePorts out;
    out.def = findNode(n.type);
    if (!out.def)
    {
        out.error = "unknown node type " + n.type;
        return out;
    }
    const NodeDef& d = *out.def;
    switch (d.kind)
    {
    case NodeKind::GroupInput:
        for (const GraphInput& in : g.inputs)
            if (!in.param)
                out.outputs.push_back({in.name, in.port, json(), in.label, in.description, 1});
        return out;
    case NodeKind::GroupOutput:
        for (const GraphOutput& o : g.outputs)
            out.inputs.push_back({o.name, o.type, json(), {}, o.description, 1});
        return out;
    case NodeKind::Group:
    case NodeKind::Graph:
    {
        if (d.kind == NodeKind::Group)
        {
            std::string name = n.values.value("group", std::string());
            auto it = g.subgroups.find(name);
            if (it == g.subgroups.end())
            {
                out.error = name.empty() ? "no group chosen" : "no subgroup named " + name;
                return out;
            }
            out.graph = it->second;
        }
        else
            out.graph = d.graph;
        try
        {
            interfaceOf(Graph::fromJson(out.graph), out);
        }
        catch (const std::exception& e)
        {
            out.error = e.what();
        }
        return out;
    }
    default: break;
    }
    out.inputs = d.inputs;
    out.outputs = d.outputs;
    out.params = d.params;
    if (d.kind == NodeKind::Output && extra)
        for (const auto& [name, type] : *extra)
            if (!out.input(name))
                out.inputs.push_back({name, type, json(), {}, {}, 1});
    return out;
}

json paramValue(const GNode& n, const ParamDecl& p)
{
    auto it = n.values.find(p.id);
    return it != n.values.end() ? *it : p.defaultValue;
}
} // namespace lila::tex
