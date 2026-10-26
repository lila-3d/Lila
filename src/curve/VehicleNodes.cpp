#include "curve/VehicleNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Network.h"
#include "geo/Topology.h"
#include "scene/Graph.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
constexpr uint32_t kIn = AttrInput | AttrStorable;
constexpr uint32_t kKey = AttrInput | AttrStorable | AttrKeyable;

const std::vector<std::string> kContinuity = {"g0", "g1", "g2", "g3"};
const std::vector<std::string> kBlendEnds = {"start", "end", "parameter"};

AttrDef dataIn(std::string name, std::string doc, bool stored = true)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = DataPtr(), .flags = (stored ? kIn : AttrInput) | AttrHidden,
            .doc = std::move(doc)};
}

/** Curves in any number, one per element; fileAlias reads the numbered attrs it replaced. */
AttrDef dataArray(std::string name, std::string doc, std::string fileAlias)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden | AttrArray,
            .doc = std::move(doc), .fileAlias = std::move(fileAlias)};
}

AttrDef meshIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}

AttrDef matrixIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}

AttrDef fAttr(std::string name, double def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

AttrDef iAttr(std::string name, int64_t def, std::string doc, double lo = 0.0)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    return a;
}

AttrDef bAttr(std::string name, bool def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
}

AttrDef eAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kIn, .enumNames = std::move(names), .doc = std::move(doc)};
}

AttrDef sAttr(std::string name, std::string doc, uint32_t extra = 0)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = kIn | extra, .doc = std::move(doc)};
}

AttrDef warningOut()
{
    return {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput,
            .doc = "Problems with the last build (curves left out, sides that touch)."};
}

int idx(const Node& n, const char* a) { return n.type().attrIndex(a); }
double f(const Node& n, const char* a) { return n.as<double>(idx(n, a)); }
int64_t i(const Node& n, const char* a) { return n.as<int64_t>(idx(n, a)); }
bool b(const Node& n, const char* a) { return n.as<bool>(idx(n, a)); }
const std::string& s(const Node& n, const char* a) { return n.as<std::string>(idx(n, a)); }
const Splines& in(const Node& n, const char* a) { return splinesOf(n.value(idx(n, a))); }

/** The mesh plug carried into world space by its matrix plug. */
std::optional<geo::Mesh> worldMesh(const Node& n, const char* meshAttr, const char* matrixAttr)
{
    const MeshPtr& m = n.as<MeshPtr>(idx(n, meshAttr));
    if (!m || m->numFaces() == 0)
        return std::nullopt;
    geo::Mesh w = *m;
    glm::dmat4 mat = n.as<glm::dmat4>(idx(n, matrixAttr));
    for (geo::Index p = 0; p < w.pointCapacity(); ++p)
        if (w.pointValid(p))
            w.setPosition(p, D3(mat * glm::dvec4(w.position(p), 1.0)));
    return w;
}

std::string joined(const std::vector<std::string>& w)
{
    std::string out;
    for (const std::string& x : w)
        out += (out.empty() ? "" : "; ") + x;
    return out;
}

/** A history node: run builds the output from the inputs; problems go to its warning output and the log. */
class VehicleNode final : public Node
{
public:
    std::function<PlugValue(const Node&, std::vector<std::string>&)> run;
    int outAttr = -1;

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        std::vector<std::string> warnings;
        int warnAttr = type().attrIndex("warning");
        try
        {
            setOutput(outAttr, run(*this, warnings));
        }
        catch (const std::exception& e)
        {
            warnings.push_back(e.what());
            setOutput(outAttr, type().attrs[size_t(outAttr)].type == PlugType::Mesh ? PlugValue(MeshPtr()) : PlugValue(DataPtr()));
        }
        std::string w = joined(warnings);
        if (warnAttr >= 0)
        {
            if (!w.empty() && w != as<std::string>(warnAttr))
                log::warn("{}: {}", name(), w);
            setOutput(warnAttr, w);
        }
        else if (!w.empty())
            log::warn("{}: {}", name(), w);
    }
};

void registerNode(std::string name, std::string icon, std::string doc, std::vector<AttrDef> attrs,
                  std::function<PlugValue(const Node&, std::vector<std::string>&)> run)
{
    if (types::find(name))
        return;
    NodeType t;
    t.name = std::move(name);
    t.icon = std::move(icon);
    t.doc = std::move(doc);
    t.ownAttrs = std::move(attrs);
    int out = -1;
    for (size_t k = 0; k < t.ownAttrs.size(); ++k)
        if ((t.ownAttrs[k].flags & AttrOutput) && t.ownAttrs[k].name != "warning")
            out = int(k);
    t.create = [run, out] {
        auto n = std::make_unique<VehicleNode>();
        n->run = run;
        n->outAttr = out;
        return n;
    };
    types::registerType(std::move(t));
}

PlugValue meshValue(geo::Mesh m) { return MeshPtr(std::make_shared<geo::Mesh>(std::move(m))); }

/** A blend end on a curve: one of its ends, or a point heading toward `toward` (flip reverses either). */
Geometry curveEnd(const Spline& s, int64_t mode, double u, bool flip, const D3* toward)
{
    Geometry g;
    if (mode == 0)
        g = endGeometry(s, false);
    else if (mode == 1)
        g = endGeometry(s, true);
    else
    {
        g = geometryAt(s, std::clamp(u, 0.0, double(spanCount(s))), true);
        if (toward)
            g = facing(g, *toward - g.p);
    }
    return flip ? reversed(g) : g;
}

D3 curveEndPoint(const Spline& s, int64_t mode, double u)
{
    return mode == 0 ? pointAt(s, 0.0) : mode == 1 ? pointAt(s, spanCount(s)) : pointAt(s, std::clamp(u, 0.0, double(spanCount(s))));
}

PlugValue blendRun(const Node& n, std::vector<std::string>&)
{
    BlendSetup st = blendSetupOf(n);
    return makeCurveData({blendCurve(st.a, st.b)});
}

PlugValue networkRun(const Node& n, std::vector<std::string>& warnings)
{
    Splines curves;
    std::vector<int> slotOf;
    for (const auto& [k, v] : n.elements(idx(n, "curves")))
        for (const Spline& sp : splinesOf(v))
        {
            curves.push_back(sp);
            slotOf.push_back(k);
        }
    if (curves.empty())
        return meshValue(geo::Mesh());
    auto slots = [&](const char* attr) {
        std::vector<int> out;
        std::istringstream ss(s(n, attr));
        for (int v; ss >> v;)
            out.push_back(v);
        return out;
    };
    std::vector<int> uSlots = slots("uSlots"), vSlots = slots("vSlots");
    NetworkOptions o;
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    o.spansU = int(std::max<int64_t>(1, i(n, "spansU")));
    o.spansV = int(std::max<int64_t>(1, i(n, "spansV")));
    o.flip = b(n, "flip");
    for (int slot : slotOf)
        o.families.push_back(std::ranges::find(uSlots, slot) != uSlots.end() ? 0 : std::ranges::find(vSlots, slot) != vSlots.end() ? 1 : -1);
    try
    {
        NetworkResult r = curveNetwork(curves, o);
        // Warnings name curves by input slot, which is what the user can find.
        for (std::string w : r.warnings)
        {
            for (size_t c = curves.size(); c-- > 0;)
            {
                std::string from = "curve " + std::to_string(c), to = "curves[" + std::to_string(slotOf[c]) + "] input";
                if (size_t at = w.find(from); at != std::string::npos && (at + from.size() == w.size() || !std::isdigit(w[at + from.size()])))
                    w.replace(at, from.size(), to);
            }
            warnings.push_back(w);
        }
        return meshValue(std::move(r.mesh));
    }
    catch (const std::invalid_argument& e)
    {
        throw std::invalid_argument(std::string(e.what()) + " (curve numbers count the connected curves from the first element)");
    }
}

PlugValue borderRun(const Node& n, std::vector<std::string>& warnings)
{
    std::optional<geo::Mesh> ma = worldMesh(n, "inMeshA", "matrixA"), mb = worldMesh(n, "inMeshB", "matrixB");
    if (!ma || !mb)
        return meshValue(geo::Mesh());
    auto chainOf = [](const geo::Mesh& m, const std::string& text, const char* side) {
        std::vector<geo::Index> edges;
        for (geo::Index e : edgeListFromJson(text))
            if (m.edgeValid(e))
                edges.push_back(e);
        std::vector<geo::EdgePath> chains = geo::chainEdges(m, edges);
        if (chains.size() != 1)
            throw std::invalid_argument(std::string("side ") + side + " needs one unbroken edge chain, it has " + std::to_string(chains.size()));
        return chains[0];
    };
    geo::EdgePath ca = chainOf(*ma, s(n, "edgesA"), "A"), cb = chainOf(*mb, s(n, "edgesB"), "B");
    auto centre = [](const geo::Mesh& m, const geo::EdgePath& c) {
        D3 sum(0.0);
        for (geo::Index p : c.points)
            sum += m.position(p);
        return sum / double(std::max<size_t>(1, c.points.size()));
    };
    BorderSide sa = borderSide(*ma, ca, centre(*mb, cb)), sb = borderSide(*mb, cb, centre(*ma, ca));
    Splines profiles;
    for (const auto& [k, v] : n.elements(idx(n, "profiles")))
        for (const Spline& sp : splinesOf(v))
            profiles.push_back(sp);
    BorderBlendOptions o;
    o.segments = int(std::max<int64_t>(1, i(n, "segments")));
    o.tensionA = std::max(0.0, f(n, "tensionA"));
    o.tensionB = std::max(0.0, f(n, "tensionB"));
    o.flip = b(n, "flip");
    BorderBlendResult r = borderBlend(sa, sb, profiles, o);
    for (const std::string& w : r.warnings)
        warnings.push_back(w);
    return meshValue(std::move(r.mesh));
}
} // namespace

std::string edgeListToJson(std::span<const geo::Index> edges)
{
    json a = json::array();
    for (geo::Index e : edges)
        a.push_back(e);
    return a.dump();
}

std::vector<geo::Index> edgeListFromJson(const std::string& text)
{
    std::vector<geo::Index> out;
    json j = json::parse(text.empty() ? "[]" : text, nullptr, false);
    if (!j.is_array())
        return out;
    for (const json& e : j)
        if (e.is_number_integer() && e.get<int64_t>() >= 0)
            out.push_back(geo::Index(e.get<int64_t>()));
    return out;
}

BlendSetup blendSetupOf(const Node& n)
{
    const Splines& sa = in(n, "curveA");
    if (sa.empty())
        throw std::invalid_argument("the first curve is missing");
    const Spline& a = sa.front();
    int64_t endA = i(n, "endA"), endB = i(n, "endB");
    double uA = f(n, "parameterA"), uB = f(n, "parameterB");
    BlendSetup st;
    st.a.continuity = Continuity(std::clamp<int64_t>(i(n, "continuityA"), 0, 3));
    st.b.continuity = Continuity(std::clamp<int64_t>(i(n, "continuityB"), 0, 3));
    st.a.bulge = std::max(0.01, f(n, "bulgeA"));
    st.b.bulge = std::max(0.01, f(n, "bulgeB"));
    int64_t edge = i(n, "meshEdge");
    if (edge >= 0)
    {
        std::optional<geo::Mesh> m = worldMesh(n, "inMesh", "meshMatrix");
        if (!m)
            throw std::invalid_argument("the mesh is missing");
        std::optional<Geometry> g = borderGeometry(*m, geo::Index(edge), f(n, "meshEdgeT"));
        if (!g)
            throw std::invalid_argument("edge " + std::to_string(edge) + " is not a border edge of the mesh");
        st.b.g = b(n, "flipB") ? reversed(*g) : *g;
    }
    else
    {
        const Splines& sb = in(n, "curveB");
        if (sb.empty())
            throw std::invalid_argument("the second curve is missing");
        D3 pa = curveEndPoint(a, endA, uA);
        st.b.g = curveEnd(sb.front(), endB, uB, b(n, "flipB"), &pa);
    }
    st.a.g = curveEnd(a, endA, uA, b(n, "flipA"), &st.b.g.p);
    // An interior point on the second curve turns to face the first once its point is known.
    if (edge < 0 && endB == 2)
        st.b.g = curveEnd(in(n, "curveB").front(), endB, uB, b(n, "flipB"), &st.a.g.p);
    return st;
}

void registerVehicleTypes()
{
    std::vector<AttrDef> blend = {
        dataIn("curveA", "First curve, world space."),
        dataIn("curveB", "Second curve, world space (unless meshEdge picks a mesh border)."),
        meshIn("inMesh", "Mesh whose border the blend meets instead of the second curve."),
        matrixIn("meshMatrix", "The mesh's world matrix."),
        eAttr("endA", kBlendEnds, 1, "Where the blend leaves the first curve: its start, its end, or parameterA."),
        fAttr("parameterA", 0.0, "Point on the first curve in spans from its start (endA parameter).", 0.0),
        eAttr("continuityA", kContinuity, 2, "Continuity with the first curve: position, tangent, curvature or curvature rate."),
        fAttr("bulgeA", 1.0, "How far the blend runs along the first curve's tangent before turning, in gaps.", 0.01, 10.0),
        bAttr("flipA", false, "Leave the first curve the other way (for a point along it)."),
        eAttr("endB", kBlendEnds, 0, "Where the blend meets the second curve: its start, its end, or parameterB."),
        fAttr("parameterB", 0.0, "Point on the second curve in spans from its start (endB parameter).", 0.0),
        eAttr("continuityB", kContinuity, 2, "Continuity with the second curve or mesh."),
        fAttr("bulgeB", 1.0, "How far the blend runs along the second end's tangent, in gaps.", 0.01, 10.0),
        bAttr("flipB", false, "Meet the second curve (or mesh) from the other side."),
        iAttr("meshEdge", -1, "Border edge of the mesh to blend to; -1 uses the second curve.", -1.0),
        fAttr("meshEdgeT", 0.5, "Where along the border edge, 0 to 1.", 0.0, 1.0),
        {.name = "outCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden, .doc = "The blend curve."},
    };
    registerNode("curveBlend", "vector-bezier-arc",
                 "A blend curve between two curves (their ends or points along them) or a curve and a mesh border, with G0 to G3 "
                 "continuity and bulge at each end; follows the curves as they move.",
                 blend, [](const Node& n, std::vector<std::string>& w) { return blendRun(n, w); });

    std::vector<AttrDef> net;
    net.push_back(dataArray("curves", "The network's curves, world space.", "curve"));
    for (AttrDef a : {fAttr("tolerance", 0.1, "Curves closer than this meet, cm.", 1e-5), iAttr("spansU", 4, "Quads along the U curves between neighbouring V curves.", 1),
                      iAttr("spansV", 4, "Quads along the V curves between neighbouring U curves.", 1), bAttr("flip", false, "Flip the faces.")})
        net.push_back(a);
    net.push_back(sAttr("uSlots", "Elements of curves forced into the U family, e.g. \"0 3\"; the rest sort themselves."));
    net.push_back(sAttr("vSlots", "Elements of curves forced into the V family."));
    net.push_back({.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
                   .doc = "The network surface."});
    net.push_back(warningOut());
    registerNode("curveNetwork", "grid",
                 "A quad surface through a network of crossing curves (Gordon surface): the curves sort into two families and become "
                 "edge loops; smooth across them; three-sided patches where one family meets at a point.",
                 net, [](const Node& n, std::vector<std::string>& w) { return networkRun(n, w); });

    std::vector<AttrDef> bb = {meshIn("inMeshA", "Mesh holding side A."),
                               matrixIn("matrixA", "Its world matrix."),
                               sAttr("edgesA", "Edges of side A, as a JSON list.", AttrHidden),
                               meshIn("inMeshB", "Mesh holding side B."),
                               matrixIn("matrixB", "Its world matrix."),
                               sAttr("edgesB", "Edges of side B, as a JSON list.", AttrHidden)};
    bb.push_back(dataArray("profiles", "Profile curves drawn across the gap, world space.", "profile"));
    for (AttrDef a : {iAttr("segments", 6, "Quads across the strip.", 1), fAttr("tensionA", 1.0, "How far the strip runs on from side A before turning.", 0.0, 10.0),
                      fAttr("tensionB", 1.0, "How far the strip runs on from side B before turning.", 0.0, 10.0), bAttr("flip", false, "Flip the faces.")})
        bb.push_back(a);
    bb.push_back({.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
                  .doc = "The blend strip."});
    bb.push_back(warningOut());
    registerNode("meshBorderBlend", "border-outer",
                 "A strip of quads between two mesh borders (or edge chains), tangent to the faces on both sides, shaped by profile "
                 "curves drawn across the gap.",
                 bb, [](const Node& n, std::vector<std::string>& w) { return borderRun(n, w); });
}
} // namespace lila::curve
