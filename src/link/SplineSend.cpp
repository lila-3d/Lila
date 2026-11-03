#include "link/SplineSend.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "link/UnrealLink.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::link
{
namespace
{
using D3 = glm::dvec3;

constexpr int kSplineTimeoutMs = 60 * 1000;

D3 toUnreal(const D3& v)
{
    const math::AxisBasis& b = math::axisBasis();
    return {glm::dot(v, b.forward), glm::dot(v, b.right), glm::dot(v, b.up)};
}

json cleanVec(const D3& v)
{
    auto clean = [](double x) { return std::abs(x) < 1e-12 ? 0.0 : x; };
    return json::array({clean(v.x), clean(v.y), clean(v.z)});
}

D3 unit(const D3& v, const D3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

/** World up made perpendicular to the tangent, then turned about it by tilt (right-handed, radians). */
D3 upAt(const D3& tangent, double tilt)
{
    const D3 worldUp = math::axisBasis().up;
    D3 up = worldUp - tangent * glm::dot(worldUp, tangent);
    if (glm::length(up) < 1e-9)
        up = math::axisBasis().forward - tangent * glm::dot(math::axisBasis().forward, tangent);
    up = unit(up, worldUp);
    return up * std::cos(tilt) + glm::cross(tangent, up) * std::sin(tilt);
}

/** The value of a per-point array at a Bezier anchor: directly when the anchors are the spline's own, else where it lies on it. */
double valueAt(const curve::Spline& source, const curve::Spline& bezier, size_t i, const std::vector<double>& values, double def)
{
    if (values.size() != source.size())
        return def;
    if (source.kind != curve::Kind::Nurbs && bezier.size() == source.size())
        return values[i];
    return curve::pointValueAt(source, values, def, curve::closestParam(source, bezier.points[i]));
}

json splineJson(const curve::Spline& s, const D3& origin)
{
    curve::Spline b = s.kind == curve::Kind::Bezier ? s : curve::toBezier(s, 0.01);
    b.normalize();
    const size_t n = b.size();
    json points = json::array();
    for (size_t i = 0; i < n; ++i)
    {
        const D3 p = b.points[i];
        D3 leave = 3.0 * (b.handleR[i] - p), arrive = 3.0 * (p - b.handleL[i]);
        // An open curve's outer handles shape nothing, so its ends take the tangent of the span they start or end.
        if (!b.closed && i == 0)
            arrive = leave;
        if (!b.closed && i + 1 == n)
            leave = arrive;
        D3 neighbour = i + 1 < n ? b.points[i + 1] - p : p - b.points[i - 1];
        const D3 t = unit(glm::length(leave) > 1e-12 ? leave : arrive, unit(neighbour, math::axisBasis().forward));
        const double tilt = valueAt(s, b, i, s.tilt, 0.0), radius = valueAt(s, b, i, s.radius, 1.0);
        points.push_back({{"position", cleanVec(toUnreal(p - origin))},
                          {"arriveTangent", cleanVec(toUnreal(arrive))},
                          {"leaveTangent", cleanVec(toUnreal(leave))},
                          {"up", cleanVec(toUnreal(upAt(t, tilt)))},
                          // Unreal is left-handed, so a right-handed turn about the tangent is a negative roll there.
                          {"roll", -tilt * 180.0 / math::kPi},
                          {"scale", cleanVec(D3(1.0, radius, radius))}});
    }
    return {{"closed", b.closed}, {"points", points}};
}

std::shared_ptr<RpcClient> requireClientFor(const char* cmd)
{
    try
    {
        Link::get().ensureConnected();
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string(cmd) + ": not connected to Unreal: " + e.what());
    }
    std::shared_ptr<RpcClient> c = Link::get().client();
    if (!c)
        throw CommandError(std::string(cmd) + ": not connected to Unreal");
    json features = Link::get().status().value("features", json::array());
    if (!features.is_array() || std::ranges::find(features, json(kSplineFeature)) == features.end())
        throw CommandError(std::string(cmd) + ": the LilaLink plugin in Unreal predates splines; rebuild it from Lila's ue/LilaLink");
    return c;
}

/** The mapping a curve already has, or an empty object. */
json existingMapping(const scene::Node& n)
{
    for (const json& m : mappings())
        if (m.is_object())
            try
            {
                if (scene::Graph::current().find(m.value("node", std::string())) == &n)
                    return m;
            }
            catch (const std::exception&)
            {
            }
    return json::object();
}

void report(const std::string& what, const RpcResult& r)
{
    if (!r.ok)
    {
        log::error("Unreal: sending {} failed: {}", what, r.error);
        return;
    }
    for (const json& w : r.result.value("warnings", json::array()))
        log::warn("Unreal: {}: {}", what, w.is_string() ? w.get<std::string>() : w.dump());
    log::info("Unreal: {} -> {}", what, r.result.value("actor", std::string("spline")));
}
} // namespace

json splinePoints(const curve::Splines& world, const D3& origin)
{
    LILA_ZONE();

    json out = json::array();
    for (const curve::Spline& s : world)
        if (curve::isValid(s) && s.size() >= 2)
            out.push_back(splineJson(s, origin));
    return out;
}

json splineParams(scene::Graph& g, scene::NodeId id, const json& mapping)
{
    LILA_ZONE();

    scene::Node* n = g.node(id);
    if (!n || !n->type().isA("curve"))
        throw std::invalid_argument("only curves go to Unreal as splines");
    g.evaluate();
    const curve::Splines& world = curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
    const D3 origin(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    json splines = splinePoints(world, origin);
    if (splines.empty())
        throw std::invalid_argument(n->path() + " has no curve to send");
    json params = {{"id", mapping.value("splineId", n->fullPath())},
                   {"label", n->name()},
                   {"mode", mapping.value("splineType", std::string("actor"))},
                   {"location", cleanVec(toUnreal(origin))},
                   {"splines", splines}};
    if (std::string mesh = mapping.value("splineMesh", std::string()); !mesh.empty())
    {
        // A Lila mesh mapped to Unreal goes as the asset it was sent as.
        if (!mesh.starts_with("/"))
        {
            scene::Node* m = nullptr;
            try
            {
                m = g.find(mesh);
            }
            catch (const std::exception&)
            {
            }
            std::string asset;
            if (m)
                for (const json& other : mappings())
                    if (other.is_object() && other.value("kind", std::string("static")) == "static")
                        try
                        {
                            if (g.find(other.value("node", std::string())) == m)
                                asset = other.value("contentPath", std::string());
                        }
                        catch (const std::exception&)
                        {
                        }
            if (asset.empty())
                throw std::invalid_argument("splineMesh " + mesh + " is not a mesh mapped to Unreal; map it (unrealMap) or give an asset path");
            mesh = asset;
        }
        params["mesh"] = {{"asset", mesh}, {"forwardAxis", mapping.value("splineMeshAxis", std::string("x"))}};
    }
    for (const char* key : {"landscape", "width", "deformTerrain", "folder"})
        if (mapping.contains(key) && !mapping[key].is_null())
            params[key] = mapping[key];
    return params;
}

void registerSplineCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "unrealSendCurve",
        .doc = "Send curves to the connected Unreal editor as splines: an actor with a spline component per curve (Bezier tangents, "
               "closed loops, roll from tilt and scale from radius), or with mode=landscape control points and segments on the "
               "landscape's splines, optionally with spline meshes of a sent mesh along them. The curves stay mapped, so every "
               "save sends them again and updates the same actor. Returns the setSpline replies with wait=True.",
        .positionalDoc = "curves (default: the selected curves)",
        .flags = {{"mode", "md", T::String, nullptr, "actor (default) or landscape.", {"actor", "landscape"}},
                  {"mesh", "m", T::String, nullptr, "Spline meshes along it: a Lila mesh mapped to Unreal, or an asset like /Game/Props/SM_Pipe."},
                  {"meshAxis", "ma", T::String, nullptr, "The spline mesh's forward axis (default x).", {"x", "y", "z"}},
                  {"landscape", "ls", T::String, nullptr, "mode=landscape: the landscape's label (default the first one)."},
                  {"width", "w", T::Float, nullptr, "mode=landscape: half-width of the spline at radius 1, cm (default 200).", {}, {}, 0.0},
                  {"deformTerrain", "dt", T::Bool, nullptr, "mode=landscape: the splines raise and lower the terrain (default on)."},
                  {"remove", "rm", T::Bool, false, "Unmap the curves and delete what they made in Unreal instead."},
                  {"validate", "va", T::Bool, true,
                   "Run the validators first: an issue blocking unrealSendCurve, \"unreal\" or \"*\" on a curve stops the send; "
                   "others are logged."},
                  {"wait", "wt", T::Bool, false, "Block until Unreal answers and return its replies."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<scene::Node*> curves;
            for (const std::string& name : a.objects())
                if (scene::Node* n = g.find(name); n && n->type().isA("curve"))
                    curves.push_back(n);
                else if (!a.positional.empty())
                    throw CommandError("unrealSendCurve: " + name + " is not a curve");
            if (curves.empty())
                throw CommandError("unrealSendCurve: select curves to send");
            const bool remove = a.get<bool>("remove", false), wait = a.get<bool>("wait", false);
            if (!remove)
            {
                std::vector<scene::NodeId> ids;
                for (scene::Node* c : curves)
                    ids.push_back(c->id());
                for (const json& i : preflight(g, ids, "unrealSendCurve", a.get<bool>("validate", true)))
                    log::warn("unrealSendCurve: {}: {}", i["target"].get<std::string>(), i["message"].get<std::string>());
            }
            std::shared_ptr<RpcClient> client = requireClientFor("unrealSendCurve");
            json replies = json::array();
            for (scene::Node* c : curves)
            {
                json m = existingMapping(*c);
                if (m.empty() || m.value("kind", std::string()) != "spline")
                    m = {{"node", c->fullPath()}, {"kind", "spline"}, {"splineId", c->fullPath()}};
                if (remove)
                {
                    removeMapping(c->fullPath());
                    json params = {{"id", m.value("splineId", c->fullPath())}};
                    if (wait)
                    {
                        RpcResult r = client->call("removeSpline", params, kSplineTimeoutMs);
                        if (!r.ok)
                            throw CommandError("unrealSendCurve: " + r.error);
                        replies.push_back(r.result);
                    }
                    else
                        client->callOnMain("removeSpline", params, kSplineTimeoutMs, [what = c->path()](RpcResult r) {
                            if (!r.ok)
                                log::error("Unreal: removing {} failed: {}", what, r.error);
                        });
                    continue;
                }
                if (a.has("mode"))
                    m["splineType"] = a.get<std::string>("mode");
                if (a.has("mesh"))
                    m["splineMesh"] = a.get<std::string>("mesh");
                if (a.has("meshAxis"))
                    m["splineMeshAxis"] = a.get<std::string>("meshAxis");
                if (a.has("landscape"))
                    m["landscape"] = a.get<std::string>("landscape");
                if (a.has("width"))
                    m["width"] = a.get<double>("width");
                if (a.has("deformTerrain"))
                    m["deformTerrain"] = a.get<bool>("deformTerrain");
                json params;
                try
                {
                    params = splineParams(g, c->id(), m);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("unrealSendCurve: ") + e.what());
                }
                setMapping(m);
                if (wait)
                {
                    RpcResult r = client->call("setSpline", params, kSplineTimeoutMs);
                    if (!r.ok)
                        throw CommandError("unrealSendCurve: " + r.error);
                    replies.push_back(r.result);
                }
                else
                    client->callOnMain("setSpline", params, kSplineTimeoutMs, [what = c->path()](RpcResult r) { report(what, r); });
            }
            return wait ? replies : json(int64_t(curves.size()));
        },
    });
}
} // namespace lila::link
