#include "curve/AlongNodes.h"
#include "curve/CurveDeform.h"
#include "curve/CurveNodes.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"

#include <algorithm>
#include <map>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
/** One spline edit: the spline and its point mask (empty for every point). */
using SplineEdit = std::function<Spline(const Spline&, std::span<const uint8_t> mask)>;

/** Curves named, else selected, plus curves holding selected points. */
std::vector<Node*> targetCurves(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
    {
        Node* n = g.find(p.get<std::string>());
        if (!n || !n->type().isA("curve"))
            throw CommandError(std::string(cmd) + ": " + p.get<std::string>() + " is not a curve");
        out.push_back(n);
    }
    if (!out.empty())
        return out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("curve"))
            out.push_back(n);
    for (auto& [id, ids] : components::selected(ComponentMode::Vertex))
        if (Node* n = g.node(id); n && n->type().isA("curve") && !ids.empty() && std::ranges::find(out, n) == out.end())
            out.push_back(n);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select curves or curve points");
    return out;
}

/** Selected points per spline (anchors and control points; a selected handle counts for its point). */
std::map<size_t, std::vector<uint8_t>> pointMasks(NodeId id, const Splines& s)
{
    std::map<size_t, std::vector<uint8_t>> out;
    for (uint32_t c : components::selected(id, ComponentMode::Vertex))
    {
        size_t si, p;
        PointRole r;
        decodeComponent(c, &si, &p, &r);
        if (si >= s.size() || p >= s[si].size() || r == PointRole::Edit)
            continue;
        auto& m = out[si];
        m.resize(s[si].size(), 0);
        m[p] = 1;
    }
    return out;
}

/** Runs the edit over each curve's splines (only those with selected points, when any are), in object space. */
json editCurves(const Args& a, const char* cmd, const SplineEdit& edit)
{
    Graph& g = Graph::current();
    g.evaluate();
    json names = json::array();
    for (Node* c : targetCurves(g, a, cmd))
    {
        Splines s = splinesOf(g.value({c->id(), CurveShape::kLocal, -1}));
        if (s.empty())
            continue;
        auto masks = pointMasks(c->id(), s);
        for (size_t si = 0; si < s.size(); ++si)
        {
            if (!masks.empty() && !masks.contains(si))
                continue;
            std::vector<uint8_t> mask = masks.empty() ? std::vector<uint8_t>() : masks[si];
            Spline r = edit(s[si], mask);
            std::string why;
            if (!isValid(r, &why))
                throw CommandError(std::string(cmd) + ": " + why);
            s[si] = std::move(r);
        }
        PlugRef create{c->id(), CurveShape::kCreate, -1};
        if (g.source(create))
            g.disconnect(create);
        g.setValue(create, makeCurveData(std::move(s)));
        names.push_back(c->path());
    }
    return names;
}

D3 vec3Flag(const Args& a, const char* flag)
{
    if (!a.has(flag))
        return D3(0.0);
    std::vector<double> v = a.flags[flag].get<std::vector<double>>();
    if (v.size() != 3)
        throw CommandError(std::string(flag) + ": expected [x, y, z]");
    return {v[0], v[1], v[2]};
}

void registerDeformOptionLayouts()
{
    namespace opt = ops::options;
    opt::Override bend;
    bend.ranges["angle"] = {-3600.0, 3600.0, 5.0, " deg"};
    opt::setOverride("bendCurve", std::move(bend));
    opt::Override curl;
    curl.ranges["amplitude"] = {0.0, 1e5, 0.1, " cm"};
    opt::setOverride("curlCurve", std::move(curl));
    opt::Override smooth;
    smooth.ranges["strength"] = {0.0, 1.0, 0.05, ""};
    opt::setOverride("smoothCurve", std::move(smooth));
}
} // namespace

void registerCurveDeformCommands()
{
    ops::registerCommand({
        .name = "bendCurve",
        .doc = "Bend curves around an axis by an angle, like a bend deformer laid along each curve's chord. In vertex mode only the "
               "selected points move. Returns the curves.",
        .positionalDoc = "curves",
        .flags = {{"angle", "a", T::Float, 90.0, "Total bend, degrees."},
                  {"axis", "ax", T::Vec3, nullptr, "Bend axis, object space; default the curve's plane normal (or up for a straight curve)."},
                  {"fromCenter", "fc", T::Bool, false, "Bend both halves about the middle instead of from the start."}},
        .run = [](const Args& a) -> json {
            BendCurveOptions o;
            o.angle = glm::radians(a.get<double>("angle", 90.0));
            o.axis = vec3Flag(a, "axis");
            o.fromCenter = a.get<bool>("fromCenter");
            return editCurves(a, "bendCurve", [&](const Spline& s, std::span<const uint8_t> m) { return bendCurve(s, o, m); });
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "curlCurve",
        .doc = "Coil curves around themselves (a helix about the curve). The whole curve is resampled so the coils are there; in "
               "vertex mode only the selected points move. Returns the curves.",
        .positionalDoc = "curves",
        .flags = {{"amplitude", "amp", T::Float, 2.0, "Curl radius, cm.", {}, {}, 0.0},
                  {"turns", "t", T::Float, 6.0, "Turns over the curve."},
                  {"keepEnds", "ke", T::Bool, true, "Ease the curl in and out so the ends stay put."},
                  {"samplesPerTurn", "spt", T::Int, 12, "Points per turn when resampling.", {}, {}, 3.0, 256.0}},
        .run = [](const Args& a) -> json {
            CurlCurveOptions o;
            o.amplitude = a.get<double>("amplitude", 2.0);
            o.turns = a.get<double>("turns", 6.0);
            o.keepEnds = a.get<bool>("keepEnds", true);
            o.samplesPerTurn = std::clamp(a.get<int>("samplesPerTurn", 12), 3, 256);
            return editCurves(a, "curlCurve", [&](const Spline& s, std::span<const uint8_t> m) { return curlCurve(s, o, m); });
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "scaleCurvature",
        .doc = "Multiply curves' curvature: below 1 flattens them toward a straight line, 0 straightens them, above 1 bends them "
               "tighter. Lengths along the curve and its start stay. In vertex mode only the selected points move. Returns the curves.",
        .positionalDoc = "curves",
        .flags = {{"factor", "f", T::Float, 0.5, "Curvature multiplier."}},
        .run = [](const Args& a) -> json {
            double factor = a.get<double>("factor", 0.5);
            return editCurves(a, "scaleCurvature", [&](const Spline& s, std::span<const uint8_t> m) { return scaleCurvature(s, factor, m); });
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "smoothCurve",
        .doc = "Smooth curves' points toward their neighbours (Bezier handles follow their points). In vertex mode only the selected "
               "points move. Returns the curves.",
        .positionalDoc = "curves",
        .flags = {{"strength", "s", T::Float, 0.5, "How far each pass moves a point toward its neighbours' average, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"iterations", "i", T::Int, 3, "Passes.", {}, {}, 1.0, 1000.0},
                  {"keepEnds", "ke", T::Bool, true, "Open curves keep their end points."}},
        .run = [](const Args& a) -> json {
            SmoothCurveOptions o;
            o.strength = std::clamp(a.get<double>("strength", 0.5), 0.0, 1.0);
            o.iterations = std::clamp(a.get<int>("iterations", 3), 1, 1000);
            o.keepEnds = a.get<bool>("keepEnds", true);
            return editCurves(a, "smoothCurve", [&](const Spline& s, std::span<const uint8_t> m) { return smoothCurve(s, o, m); });
        },
        .adjustable = true,
    });
    registerDeformOptionLayouts();
}
} // namespace lila::curve
