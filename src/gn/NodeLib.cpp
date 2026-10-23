#include "gn/NodeLib.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"

#include <map>
#include <memory>

namespace lila::gn
{
namespace
{
/** Removed decls stay allocated: evaluation plans hold pointers to them. */
std::vector<std::unique_ptr<NodeDecl>>& retired()
{
    static std::vector<std::unique_ptr<NodeDecl>> r;
    return r;
}

void retire(std::unique_ptr<NodeDecl> d)
{
    if (!d)
        return;
    // Its functions may live in an extension library that is about to be unloaded.
    std::string type = d->type;
    d->dynamicSockets = nullptr;
    d->exec = [type](ExecContext&) { throw std::runtime_error("node type " + type + " was unloaded"); };
    retired().push_back(std::move(d));
}

const Value& emptyValue()
{
    static const Value v;
    return v;
}

glm::dvec3 vec3(const json& j, glm::dvec3 fallback)
{
    if (j.is_array() && j.size() == 3)
        return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
    if (j.is_number())
        return glm::dvec3(j.get<double>());
    return fallback;
}
} // namespace

int Sockets::inputIndex(std::string_view id) const
{
    for (size_t i = 0; i < inputs.size(); ++i)
        if (inputs[i].id == id)
            return int(i);
    return -1;
}

int Sockets::outputIndex(std::string_view id) const
{
    for (size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i].id == id)
            return int(i);
    return -1;
}

Sockets NodeDecl::socketsFor(const Group& g, const GNode& n) const
{
    return dynamicSockets ? dynamicSockets(g, n) : sockets;
}

const PropDecl* NodeDecl::prop(std::string_view id) const
{
    for (const PropDecl& p : props)
        if (p.id == id)
            return &p;
    return nullptr;
}

std::string NodeDecl::propValue(const GNode& n, std::string_view id) const
{
    auto it = n.values.find(std::string(id));
    if (it != n.values.end() && it->is_string())
        return it->get<std::string>();
    const PropDecl* p = prop(id);
    return p ? p->defaultValue : std::string();
}

Registry<NodeDecl>& declRegistry()
{
    static Registry<NodeDecl> r;
    return r;
}

void registerDecl(NodeDecl d)
{
    std::string type = d.type;
    retire(declRegistry().take(type));
    declRegistry().add(std::move(type), std::move(d));
}

const NodeDecl* registerDecl(ext::Context& ctx, NodeDecl d)
{
    std::string type = d.type;
    if (declRegistry().contains(type))
    {
        log::error("extension {}: geometry node type '{}' is already registered", ctx.owner(), type);
        return nullptr;
    }
    declRegistry().add(type, std::move(d), ctx.owner());
    std::string owner = ctx.owner();
    ctx.track("gnNode:" + type, [type, owner] {
        if (const std::string* o = declRegistry().ownerOf(type); o && *o == owner)
            unregisterDecl(type);
    });
    return declRegistry().find(type);
}

void unregisterDecl(std::string_view type)
{
    retire(declRegistry().take(type));
}

const NodeDecl* findDecl(std::string_view type)
{
    return declRegistry().find(type);
}

bool linkable(SocketType from, SocketType to)
{
    if (from == to)
        return true;
    if (from == SocketType::Geometry || to == SocketType::Geometry)
        return false;
    if (to == SocketType::String)
        return isDataType(from);
    return from != SocketType::String;
}

std::string compatibleSocket(const Group& g, const NodeDecl& d, SocketType type, bool wantInput)
{
    GNode probe{d.type, d.type, {0.0, 0.0}, json::object()};
    Sockets s = d.socketsFor(g, probe);
    const std::vector<SocketDecl>& list = wantInput ? s.inputs : s.outputs;
    for (const SocketDecl& sd : list)
        if (sd.type == type)
            return sd.id;
    for (const SocketDecl& sd : list)
        if (wantInput ? linkable(type, sd.type) : linkable(sd.type, type))
            return sd.id;
    return {};
}

std::vector<const NodeDecl*> allDecls()
{
    std::vector<const NodeDecl*> out;
    for (NodeDecl* d : declRegistry().all())
        out.push_back(d);
    return out;
}

Value defaultValue(SocketType t)
{
    Value v;
    switch (t)
    {
    case SocketType::Geometry: v.geometry = makeGeometry({}); break;
    case SocketType::String: break;
    case SocketType::Float: v.field = constantFloat(0.0); break;
    case SocketType::Int: v.field = constantInt(0); break;
    case SocketType::Bool: v.field = constantBool(false); break;
    default: v.field = constantVector(glm::dvec3(0.0), t); break;
    }
    return v;
}

Value valueFromJson(SocketType t, const json& j)
{
    if (j.is_null())
        return defaultValue(t);
    Value v;
    try
    {
        switch (t)
        {
        case SocketType::Geometry: return defaultValue(t);
        case SocketType::String: v.string = j.is_string() ? j.get<std::string>() : j.dump(); break;
        case SocketType::Float: v.field = constantFloat(j.is_boolean() ? double(j.get<bool>()) : j.get<double>()); break;
        case SocketType::Int: v.field = constantInt(j.is_boolean() ? int64_t(j.get<bool>()) : int64_t(std::llround(j.get<double>()))); break;
        case SocketType::Bool: v.field = constantBool(j.is_boolean() ? j.get<bool>() : j.get<double>() != 0.0); break;
        case SocketType::Vector: v.field = constantVector(vec3(j, glm::dvec3(0.0))); break;
        case SocketType::Rotation: v.field = constantVector(glm::radians(vec3(j, glm::dvec3(0.0))), SocketType::Rotation); break;
        }
    }
    catch (const json::exception&)
    {
        return defaultValue(t);
    }
    return v;
}

Value convertValue(const Value& v, SocketType from, SocketType to)
{
    if (from == to)
        return v;
    if (isDataType(from) && isDataType(to))
    {
        Value out;
        out.field = convertField(v.field, to);
        return out;
    }
    if (to == SocketType::String && isDataType(from) && v.field && v.field->constant())
    {
        Value out;
        Column c = evaluate(v.field, FieldContext{}, from == SocketType::Int || from == SocketType::Bool ? SocketType::Int : SocketType::Float);
        out.string = std::holds_alternative<std::vector<int64_t>>(c.data) ? std::to_string(c.at<int64_t>(0)) : std::to_string(c.at<double>(0));
        return out;
    }
    return defaultValue(to);
}

const Value& ExecContext::in(std::string_view id) const
{
    int k = sockets.inputIndex(id);
    if (k < 0 || size_t(k) >= inputs.size() || inputs[k].empty())
        return emptyValue();
    return inputs[k][0];
}

GeometryPtr ExecContext::geometry(std::string_view id) const
{
    const Value& v = in(id);
    return v.geometry ? v.geometry : makeGeometry({});
}

std::vector<GeometryPtr> ExecContext::geometries(std::string_view id) const
{
    std::vector<GeometryPtr> out;
    int k = sockets.inputIndex(id);
    if (k < 0 || size_t(k) >= inputs.size())
        return out;
    for (const Value& v : inputs[k])
        if (v.geometry)
            out.push_back(v.geometry);
    return out;
}

FieldPtr ExecContext::field(std::string_view id) const
{
    const Value& v = in(id);
    if (v.field)
        return v.field;
    int k = sockets.inputIndex(id);
    return k >= 0 ? defaultValue(sockets.inputs[k].type).field : constantFloat(0.0);
}

double ExecContext::f(std::string_view id) const { return evaluateSingle<double>(field(id)); }
int64_t ExecContext::i(std::string_view id) const { return evaluateSingle<int64_t>(field(id)); }
bool ExecContext::b(std::string_view id) const { return evaluateSingle<uint8_t>(field(id)) != 0; }
glm::dvec3 ExecContext::v(std::string_view id) const { return evaluateSingle<glm::dvec3>(field(id)); }
std::string ExecContext::s(std::string_view id) const { return in(id).string; }

void ExecContext::out(std::string_view id, GeometryPtr g)
{
    int k = sockets.outputIndex(id);
    if (k >= 0)
        outputs[k].geometry = std::move(g);
}

void ExecContext::out(std::string_view id, FieldPtr f)
{
    int k = sockets.outputIndex(id);
    if (k >= 0)
        outputs[k].field = convertField(f, sockets.outputs[k].type);
}

void ExecContext::outString(std::string_view id, std::string s)
{
    int k = sockets.outputIndex(id);
    if (k >= 0)
        outputs[k].string = std::move(s);
}
} // namespace lila::gn
