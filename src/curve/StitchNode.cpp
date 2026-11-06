#include "curve/EnvironmentNodes.h"
#include "curve/NodeHelpers.h"
#include "curve/Stitch.h"
#include "curve/Tessellate.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "math/MathCore.h"
#include "ops/CommandOptions.h"
#include "scene/MeshNodes.h"

#include <memory>

namespace lila::curve
{
using namespace scene;
using namespace helpers;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
const std::vector<std::string> kPatterns{"running", "zigzag", "cross", "blanket"};
const std::vector<std::string> kAligns{"center", "left", "right"};

StitchOptions optionsOf(const Node& n)
{
    StitchOptions o;
    o.pattern = StitchPattern(std::clamp<int64_t>(i(n, "pattern"), 0, 3));
    o.length = f(n, "stitchLength");
    o.gap = f(n, "stitchGap");
    o.rotation = f(n, "rotation");
    o.width = f(n, "stitchWidth");
    o.rows = int(i(n, "rows"));
    o.rowSpacing = f(n, "rowSpacing");
    o.align = StitchAlign(std::clamp<int64_t>(i(n, "rowAlign"), 0, 2));
    o.offset = f(n, "offset");
    o.diameter = f(n, "diameter");
    o.flatten = f(n, "flatten");
    o.plies = int(i(n, "plies"));
    o.twist = f(n, "twist");
    o.restHeight = f(n, "restHeight");
    o.inset = f(n, "insetDepth");
    o.sides = int(i(n, "sides"));
    o.segments = int(i(n, "segments"));
    o.jitter = f(n, "jitter");
    o.seed = uint32_t(i(n, "seed"));
    o.continuousUv = b(n, "continuousUv");
    o.uvScale = f(n, "uvScale");
    o.uvRandom = f(n, "uvRandom");
    return o;
}

/** Attr values for a preset's options, by attr name. */
json presetValues(const StitchOptions& o)
{
    return {{"pattern", stitchPatternName(o.pattern)},
            {"stitchLength", o.length},
            {"stitchGap", o.gap},
            {"rotation", o.rotation},
            {"stitchWidth", o.width},
            {"rows", o.rows},
            {"rowSpacing", o.rowSpacing},
            {"rowAlign", kAligns[size_t(o.align)]},
            {"diameter", o.diameter},
            {"flatten", o.flatten},
            {"plies", o.plies},
            {"twist", o.twist},
            {"restHeight", o.restHeight},
            {"insetDepth", o.inset},
            {"jitter", o.jitter}};
}

/** Stitches along the input curves, on the surface mesh when one is connected. */
class StitchNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int out = idx(*this, "outMesh");
        try
        {
            const StitchOptions o = optionsOf(*this);
            ClosestSurface snap;
            if (const MeshPtr& mesh = as<MeshPtr>(idx(*this, "surface")); mesh && mesh->numFaces() > 0)
            {
                const glm::dmat4 world = as<glm::dmat4>(idx(*this, "surfaceMatrix")), toLocal = glm::inverse(world);
                const glm::dmat3 normalToWorld = glm::transpose(glm::inverse(glm::dmat3(world)));
                auto bvh = std::make_shared<geo::MeshBvh>(*mesh);
                snap = [mesh, bvh, world, toLocal, normalToWorld](const D3& p) -> std::optional<SurfacePoint> {
                    geo::BvhClosest hit;
                    if (!bvh->closestPoint(D3(toLocal * glm::dvec4(p, 1.0)), hit) || !mesh->faceValid(hit.face))
                        return std::nullopt;
                    const D3 n = normalToWorld * mesh->faceNormal(hit.face);
                    return SurfacePoint{D3(world * glm::dvec4(hit.point, 1.0)), glm::length(n) > 1e-12 ? glm::normalize(n) : D3(0, 1, 0)};
                };
            }

            geo::Mesh all;
            int count = 0;
            for (const Spline& s : in(*this, "inCurve"))
            {
                const Polyline pl = tessellate(s);
                if (pl.size() < 2)
                    continue;
                std::vector<D3> normals;
                if (snap)
                    for (const D3& p : pl.points)
                    {
                        const std::optional<SurfacePoint> hit = snap(p);
                        normals.push_back(hit ? hit->normal : D3(0, 1, 0));
                    }
                else
                {
                    FrameOptions fo;
                    fo.up = v(*this, "up");
                    fo.startNormal = fo.up;
                    for (const Frame& fr : frames(pl, fo))
                        normals.push_back(fr.n);
                }
                StitchResult r = buildStitches(pl.length, pl.closed, polylineSurface(pl.points, std::move(normals), pl.closed, snap), o);
                count += r.stitches;
                all.append(r.mesh);
            }
            setOutput(idx(*this, "stitchCount"), int64_t(count));
            setOutput(out, meshValue(std::move(all)));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, MeshPtr());
        }
    }
};

void registerStitchOptionLayout()
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = {"preset",   "surface",    "pattern",  "stitchLength", "stitchGap", "rotation",   "stitchWidth", "rows",
               "rowSpacing", "rowAlign", "offset",   "diameter",     "flatten",   "plies",      "twist",       "restHeight",
               "insetDepth", "jitter",   "seed",     "continuousUv", "uvScale",   "uvRandom",   "sides",       "segments"};
    o.hidden = {"name"};
    o.sections = {{"rows", "Rows"},           {"rowSpacing", "Rows"},     {"rowAlign", "Rows"},    {"offset", "Rows"},
                  {"diameter", "Thread"},     {"flatten", "Thread"},      {"plies", "Thread"},     {"twist", "Thread"},
                  {"restHeight", "Thread"},   {"insetDepth", "Thread"},   {"jitter", "Hand Sewn"}, {"seed", "Hand Sewn"},
                  {"continuousUv", "UVs"},    {"uvScale", "UVs"},         {"uvRandom", "UVs"},     {"sides", "Quality"},
                  {"segments", "Quality"},    {"up", "Quality"}};
    for (const char* cm : {"stitchLength", "stitchGap", "stitchWidth", "rowSpacing", "offset", "diameter", "restHeight", "insetDepth"})
        o.ranges[cm] = {0.0, 10.0, 0.01, " cm"};
    o.ranges["offset"] = {-10.0, 10.0, 0.01, " cm"};
    o.ranges["restHeight"] = {-1.0, 1.0, 0.005, " cm"};
    o.ranges["rotation"] = {-90.0, 90.0, 1.0, "\xC2\xB0"};
    o.labels = {{"stitchLength", "Length"}, {"stitchGap", "Gap"}, {"stitchWidth", "Width"}, {"rowAlign", "Centering"},
                {"insetDepth", "Inset Depth"}, {"continuousUv", "Continuous UV"}, {"uvRandom", "Random UV Offset"}};
    opt::setOverride("stitch", std::move(o));
}
} // namespace

void registerStitchType()
{
    if (types::find("curveStitch"))
        return;
    NodeType t;
    t.name = "curveStitch";
    t.icon = "line-dashed";
    t.doc = "Sewn thread along curves: running, zigzag, cross or blanket stitches arching in and out of the surface, in twisted "
            "plies, sitting on a mesh when one is connected.";
    t.ownAttrs.push_back(dataIn("inCurve", "The stitch lines, world space.", false));
    t.ownAttrs.push_back(meshIn("surface", "The mesh the stitches sit on."));
    t.ownAttrs.push_back(matrixIn("surfaceMatrix", "That mesh's world matrix."));
    const StitchOptions d;
    for (AttrDef a : {eAttr("pattern", kPatterns, 0, "How the thread runs: dashes, side-to-side diagonals, an X per stitch, or legs across joined along the line."),
                      fAttr("stitchLength", d.length, "Each stitch's length along the line, cm.", 0.01),
                      fAttr("stitchGap", d.gap, "Between stitches, cm.", 0.0),
                      fAttr("rotation", d.rotation, "Running: each stitch turned in the surface, degrees (a saddle stitch's slant).", -90.0, 90.0),
                      fAttr("stitchWidth", d.width, "Zigzag, cross and blanket: how far across the line a stitch reaches, cm.", 0.0),
                      iAttr("rows", d.rows, "Parallel rows of stitches.", 1, 16.0),
                      fAttr("rowSpacing", d.rowSpacing, "Between rows, cm.", 0.0),
                      eAttr("rowAlign", kAligns, 0, "Where extra rows go: centred on the line or to one side."),
                      fAttr("offset", d.offset, "The stitching moved across the line, cm."),
                      fAttr("diameter", d.diameter, "Thread thickness, cm.", 0.001),
                      fAttr("flatten", d.flatten, "0 round, toward 1 a flat ribbon pressed into the surface.", 0.0, 0.95),
                      iAttr("plies", d.plies, "Strands twisted into the thread.", 1, 3.0),
                      fAttr("twist", d.twist, "Turns of the plies per cm of thread.", 0.0),
                      fAttr("restHeight", d.restHeight, "How far the thread's underside sits off the surface, cm; negative sinks it in."),
                      fAttr("insetDepth", d.inset, "How deep the thread dives into its holes, cm.", 0.0),
                      fAttr("jitter", d.jitter, "Hand-sewn wobble in spacing, angle and length; 0 is machine-even.", 0.0, 1.0),
                      iAttr("seed", d.seed, "The wobble's seed."),
                      bAttr("continuousUv", d.continuousUv, "V runs on along the line from stitch to stitch; off, each stitch starts at 0."),
                      fAttr("uvScale", d.uvScale, "Texture repeats per thread circumference along V.", 0.001),
                      fAttr("uvRandom", d.uvRandom, "Each stitch's V shifted by up to this much at random.", 0.0),
                      iAttr("sides", d.sides, "Sides round each ply.", 3, 32.0),
                      iAttr("segments", d.segments, "Rings along each stitch.", 2, 64.0),
                      vAttr("up", D3(0, 1, 0), "With no surface mesh: which way is out of the surface along the curve.")})
        t.ownAttrs.push_back(std::move(a));
    t.ownAttrs.push_back(meshOut());
    t.ownAttrs.push_back({.name = "stitchCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = scene::AttrOutput, .doc = "Stitches built."});
    t.create = [] { return std::make_unique<StitchNode>(); };
    types::registerType(std::move(t));
}

void registerStitchCommands()
{
    std::vector<FlagDef> flags = attrFlags("curveStitch");
    for (FlagDef fd : std::vector<FlagDef>{
             {"preset", "", T::String, nullptr, "Starting settings (flags given as well win).", stitchPresetNames()},
             {"surface", "srf", T::String, nullptr, "The mesh the stitches sit on (default: the selected mesh). Leave out for none."},
             {"constructionHistory", "ch", T::Bool, true, "Keep the stitch node so editing the curve or settings updates the stitches."},
             {"name", "n", T::String, nullptr, "Mesh name; made unique."}})
        flags.push_back(std::move(fd));

    ops::registerCommand({
        .name = "stitch",
        .doc = "Sew thread along curves: running (saddle with rotation), zigzag, cross or blanket stitches, in rows, as real "
               "geometry arching in and out of the surface with twisted plies and UVs along the thread. Sits on a mesh when one "
               "is given. Live: editing the curve, the mesh or the settings updates the stitches. Returns [mesh, curveStitch] "
               "per curve.",
        .positionalDoc = "curves (default: the selected curves)",
        .flags = flags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves;
            for (const json& p : a.positional)
            {
                Node* n = g.find(p.get<std::string>());
                if (!n || !n->type().isA("curve"))
                    throw CommandError("stitch: " + p.get<std::string>() + " is not a curve");
                curves.push_back(n);
            }
            Node* surface = a.has("surface") ? &meshNamed(g, a.get<std::string>("surface"), "stitch") : nullptr;
            for (NodeId id : g.selection())
                if (Node* n = g.node(id))
                {
                    if (a.positional.empty() && n->type().isA("curve"))
                        curves.push_back(n);
                    else if (!surface && !a.has("surface") && n->type().isA("mesh"))
                        surface = n;
                }
            if (curves.empty())
                throw CommandError("stitch: select the curves to stitch along");

            json values = json::object();
            if (a.has("preset"))
            {
                const std::optional<StitchOptions> p = stitchPreset(a.get<std::string>("preset"));
                if (!p)
                    throw CommandError("stitch: unknown preset " + a.get<std::string>("preset"));
                values = presetValues(*p);
            }
            for (auto& [k, val] : a.flags.items())
                if (!val.is_null())
                    values[k] = val;
            Args merged = a;
            merged.flags = values;

            json out = json::array();
            std::vector<NodeId> made;
            for (Node* curve : curves)
            {
                Node* op = g.createNode(*types::find("curveStitch"), {}, kNoNode, true);
                applyFlags(g, *op, merged, "stitch");
                g.connect({curve->id(), CurveShape::kWorldSpace, -1}, {op->id(), idx(*op, "inCurve"), -1});
                if (surface)
                {
                    g.connect({surface->id(), MeshNode::kOutMesh, -1}, {op->id(), idx(*op, "surface"), -1});
                    g.connect({surface->id(), Transform::kWorldMatrix, -1}, {op->id(), idx(*op, "surfaceMatrix"), -1});
                }
                else if (math::upAxis() == math::UpAxis::Z)
                    g.setValue({op->id(), idx(*op, "up"), -1}, D3(0, 0, 1));
                Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "Stitches"), kNoNode, true);
                g.connect({op->id(), idx(*op, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
                g.evaluate();
                if (op->as<int64_t>(idx(*op, "stitchCount")) == 0)
                    log::warn("stitch: no stitches along {} (shorter than one stitch?)", curve->path());
                json opName = op->path();
                if (!a.get<bool>("constructionHistory", true))
                {
                    bake(g, *op, {mesh->id(), MeshNode::kInMesh, -1});
                    opName = nullptr;
                }
                out.push_back(json::array({mesh->path(), opName}));
                made.push_back(mesh->id());
            }
            g.select(made);
            return out;
        },
        .examples = {"stitch('SeamCurve', surface='Shoe', preset='heavyLeather')", "stitch('Hem', pattern='zigzag', stitchWidth=0.6, rows=2)"},
    });
    registerStitchOptionLayout();
}
} // namespace lila::curve
