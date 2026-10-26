#include "curve/Contour.h"
#include "curve/EnvAttrs.h"
#include "curve/EnvironmentNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "ops/CommandOptions.h"
#include "scene/MeshNodes.h"

#include <algorithm>

namespace lila::curve
{
using namespace scene;
using namespace env;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{

/** Slices the connected meshes: contours along an axis, or one section through a plane. */
class ContourNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int out = idx(*this, "outCurve");
        try
        {
            std::vector<geo::SectionMesh> meshes;
            const auto& matrices = elements(idx(*this, "meshMatrices"));
            for (const auto& [k, v] : elements(idx(*this, "inMeshes")))
            {
                const MeshPtr& m = std::get<MeshPtr>(v);
                auto mx = std::ranges::find(matrices, k, [](const auto& e) { return e.first; });
                if (m && m->numFaces() > 0)
                    meshes.push_back({m.get(), mx != matrices.end() ? std::get<glm::dmat4>(mx->second) : glm::dmat4(1.0)});
            }
            SliceOptions so;
            so.tolerance = std::max(0.0, f(*this, "tolerance"));
            so.smooth = b(*this, "smooth");
            so.bridge = b(*this, "bridge");
            std::string warning;
            Splines curves;
            if (i(*this, "mode") == 1)
            {
                if (!(glm::length(v(*this, "axis")) > 1e-12))
                    warning = "the section plane's normal has no length";
                else
                    curves = sectionCurves(meshes, v(*this, "origin"), v(*this, "axis"), so);
            }
            else
            {
                ContourOptions co;
                co.slice = so;
                co.axis = v(*this, "axis");
                co.origin = v(*this, "origin");
                co.spacing = f(*this, "spacing");
                co.count = int(std::max<int64_t>(0, i(*this, "count")));
                ContourResult r = contour(meshes, co);
                curves = std::move(r.curves);
                for (const std::string& w : r.warnings)
                    warning += (warning.empty() ? "" : "; ") + w;
            }
            if (meshes.empty())
                warning = "no mesh to slice";
            else if (curves.empty() && warning.empty())
                warning = "the plane misses the meshes";
            if (warning != as<std::string>(idx(*this, "warning")) && !warning.empty())
                log::warn("{}: {}", name(), warning);
            setOutput(idx(*this, "warning"), warning);
            setOutput(out, makeCurveData(std::move(curves)));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, DataPtr());
        }
    }
};

std::vector<Node*> meshesOf(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
    {
        Node* n = g.find(p.get<std::string>());
        if (!n || !n->type().isA("mesh"))
            throw CommandError(std::string(cmd) + ": " + p.get<std::string>() + " is not a mesh");
        out.push_back(n);
    }
    if (a.positional.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                out.push_back(n);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select one or more meshes");
    return out;
}

/** A contour node on the meshes feeding a new curve. Returns [curve, node]. */
json build(const Args& a, const char* cmd, int64_t mode, const std::function<void(Graph&, Node&)>& configure)
{
    Graph& g = Graph::current();
    std::vector<Node*> meshes = meshesOf(g, a, cmd);
    Node* op = g.createNode(*types::find("curveContour"), {}, kNoNode, true);
    g.setValue({op->id(), idx(*op, "mode"), -1}, mode);
    configure(g, *op);
    for (size_t k = 0; k < meshes.size(); ++k)
    {
        g.connect({meshes[k]->id(), MeshNode::kOutMesh, -1}, {op->id(), idx(*op, "inMeshes"), -1, int(k)});
        g.connect({meshes[k]->id(), Transform::kWorldMatrix, -1}, {op->id(), idx(*op, "meshMatrices"), -1, int(k)});
    }
    Node* curve = g.createNode(*types::find("curve"), a.get<std::string>("name", mode == 1 ? "Section" : "Contour"), kNoNode, true);
    g.connect({op->id(), idx(*op, "outCurve"), -1}, {curve->id(), CurveShape::kCreate, -1});
    json opName = op->path();
    g.evaluate();
    const std::string warning = op->as<std::string>(idx(*op, "warning"));
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, *op, {curve->id(), CurveShape::kCreate, -1});
        opName = nullptr;
    }
    if (splinesOf(g.value({curve->id(), CurveShape::kLocal, -1})).empty())
        log::warn("{}: no curves made{}", cmd, warning.empty() ? "" : ": " + warning);
    g.select({curve->id()});
    return json::array({curve->path(), opName});
}

void registerOptionLayout()
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = {"axis", "spacing", "count", "origin", "tolerance", "smooth", "bridge"};
    o.hidden = {"name", "mode"};
    o.ranges["spacing"] = {0.001, 1e6, 1.0, " cm"};
    o.ranges["tolerance"] = {0.0, 1e3, 0.01, " cm"};
    o.labels = {{"count", "Count (0 = use spacing)"}, {"origin", "Base Point"}, {"bridge", "Close Open Cuts"}};
    opt::setOverride("contour", std::move(o));
}
} // namespace

void registerContourType()
{
    if (types::find("curveContour"))
        return;
    NodeType t;
    t.name = "curveContour";
    t.icon = "stack-2";
    t.doc = "Slices meshes into curves: contours at evenly spaced planes along an axis, or one section through a plane. Loops "
            "wind counter-clockwise about the axis; open cuts stay open unless bridged.";
    // Paired by element: meshMatrices[k] places inMeshes[k].
    t.ownAttrs.push_back({.name = "inMeshes", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrArray,
                          .doc = "The meshes to slice.", .fileAlias = "inMesh"});
    t.ownAttrs.push_back({.name = "meshMatrices", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                          .flags = AttrInput | AttrHidden | AttrArray, .doc = "Each mesh's world matrix, by the same element.",
                          .fileAlias = "meshMatrix"});
    for (AttrDef a : {eAttr("mode", {"contour", "section"}, 0, "Contours along the axis, or a single section through origin."),
                      vAttr("axis", D3(0, 1, 0), "The planes' normal: contours step along it; a section's plane faces it."),
                      vAttr("origin", D3(0.0), "A point every contour plane is a whole number of spacings from; the section plane passes through it."),
                      fAttr("spacing", 10.0, "cm between contour planes (count 0).", 1e-6),
                      iAttr("count", 0, "Contour planes spread evenly through the meshes, their ends left out; 0 uses spacing."),
                      fAttr("tolerance", 0.01, "Simplify each curve within this many cm; 0 keeps every crossing.", 0.0),
                      bAttr("smooth", false, "Fit smooth Beziers (corners kept) instead of poly lines."),
                      bAttr("bridge", false, "Close cuts through open meshes across their gaps instead of leaving them open.")})
        t.ownAttrs.push_back(std::move(a));
    t.ownAttrs.push_back(dataOut("outCurve", "The cut curves, world space."));
    t.ownAttrs.push_back(stringOut("warning", "Why the last cut made nothing, if it did."));
    t.create = [] { return std::make_unique<ContourNode>(); };
    types::registerType(std::move(t));
}

void registerContourCommands()
{
    std::vector<FlagDef> flags = attrFlags("curveContour");
    std::erase_if(flags, [](const FlagDef& fd) { return fd.name == "mode"; });
    flags.push_back({"constructionHistory", "ch", T::Bool, true, "Keep the contour node so the curves follow the meshes and settings."});
    flags.push_back({"name", "n", T::String, nullptr, "Curve name; made unique."});
    ops::registerCommand({
        .name = "contour",
        .doc = "Slice meshes into curves at evenly spaced parallel planes (Rhino's Contour): every spacing cm along axis through "
               "origin, or count planes spread through the meshes. One curve node holds every cut, kept live while history is on. "
               "Returns [curve, curveContour].",
        .positionalDoc = "meshes (default: the selected meshes)",
        .flags = flags,
        .run = [](const Args& a) -> json { return build(a, "contour", 0, [&](Graph& g, Node& op) { applyFlags(g, op, a, "contour"); }); },
    });

    ops::registerCommand({
        .name = "section",
        .doc = "Cut meshes with one plane into curves (Rhino's Section): the plane through point facing normal. Loops come out "
               "closed and cuts through open meshes open. Kept live while history is on. Returns [curve, curveContour].",
        .positionalDoc = "meshes (default: the selected meshes)",
        .flags = {{"point", "p", T::Vec3, nullptr, "A point on the plane, world cm (default the origin)."},
                  {"normal", "nm", T::Vec3, nullptr, "The plane's normal (default up)."},
                  {"tolerance", "tol", T::Float, nullptr, "Simplify each curve within this many cm; 0 keeps every crossing."},
                  {"smooth", "sm", T::Bool, nullptr, "Fit smooth Beziers instead of poly lines."},
                  {"bridge", "br", T::Bool, nullptr, "Close cuts through open meshes across their gaps."},
                  {"constructionHistory", "ch", T::Bool, true, "Keep the node so the section follows the meshes and the plane."},
                  {"name", "n", T::String, nullptr, "Curve name; made unique."}},
        .run = [](const Args& a) -> json {
            return build(a, "section", 1, [&](Graph& g, Node& op) {
                auto vec = [&](const char* flag) {
                    const json& j = a.flags[flag];
                    if (!j.is_array() || j.size() != 3)
                        throw CommandError(std::string("section: ") + flag + ": expected [x, y, z]");
                    return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
                };
                if (a.has("point"))
                    g.setValue({op.id(), idx(op, "origin"), -1}, vec("point"));
                if (a.has("normal"))
                {
                    D3 n = vec("normal");
                    if (!(glm::length(n) > 1e-12))
                        throw CommandError("section: normal has no length");
                    g.setValue({op.id(), idx(op, "axis"), -1}, n);
                }
                if (a.has("tolerance"))
                    g.setValue({op.id(), idx(op, "tolerance"), -1}, a.get<double>("tolerance"));
                for (const char* flag : {"smooth", "bridge"})
                    if (a.has(flag))
                        g.setValue({op.id(), idx(op, flag), -1}, a.get<bool>(flag));
            });
        },
    });
    registerOptionLayout();
}
} // namespace lila::curve
