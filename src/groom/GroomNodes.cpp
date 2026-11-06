#include "groom/GroomNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/NodeHelpers.h"
#include "curve/Pipe.h"
#include "groom/Attrs.h"
#include "groom/GroomLab.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <sstream>

namespace lila::groom
{
using namespace scene;
using namespace curve::helpers;
using curve::makeCurveData;
using curve::splinesOf;

namespace
{
std::vector<std::string> profileNames()
{
    std::vector<std::string> out;
    for (int p = 0; p < int(LockProfile::Count); ++p)
        out.push_back(profileName(LockProfile(p)));
    return out;
}

/** "x,y x,y ..." pairs. */
std::vector<glm::dvec2> parsePairs(const std::string& text)
{
    std::vector<double> n = curve::parseNumbers(text);
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i + 1 < n.size(); i += 2)
        out.push_back({n[i], n[i + 1]});
    return out;
}

geo::Mesh transformedMesh(const geo::Mesh& in, const glm::dmat4& m)
{
    geo::Mesh out = in;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, D3(m * glm::dvec4(out.position(p), 1.0)));
    return out;
}

/** The scalp in world space from a node's inMesh and meshMatrix, rebuilt only when either changes. */
struct ScalpCache
{
    const geo::Mesh* mesh = nullptr;
    glm::dmat4 matrix{0.0};
    std::unique_ptr<Surface> surface;

    const Surface* get(const Node& n)
    {
        const MeshPtr& m = n.as<MeshPtr>(idx(n, "inMesh"));
        const glm::dmat4& mm = n.as<glm::dmat4>(idx(n, "meshMatrix"));
        if (m.get() != mesh || mm != matrix)
        {
            mesh = m.get();
            matrix = mm;
            surface.reset();
            if (m && m->numFaces() > 0)
                surface = std::make_unique<Surface>(transformedMesh(*m, mm));
        }
        return surface.get();
    }
};

/** A node with a warning output it logs when it changes. */
class WarningNode : public Node
{
protected:
    void report(const std::vector<std::string>& warnings)
    {
        std::string w;
        for (const std::string& s : warnings)
            w += (w.empty() ? "" : "; ") + s;
        if (!w.empty() && w != as<std::string>(idx(*this, "warning")))
            log::warn("{}: {}", name(), w);
        setOutput(idx(*this, "warning"), w);
    }
};

/** Drops a curve that repeats an earlier one with the same id: the mirror copy of a lock drawn on the mirror plane. */
Splines withoutTwins(const Splines& in)
{
    Splines out;
    out.reserve(in.size());
    for (const Spline& s : in)
    {
        const bool twin = s.attrs.contains(std::string(attr::kId)) && std::ranges::any_of(out, [&](const Spline& o) {
            if (o.size() != s.size() || attr::get(o, attr::kId, -1.0) != attr::get(s, attr::kId, -2.0))
                return false;
            for (size_t i = 0; i < s.size(); ++i)
                if (glm::distance(o.points[i], s.points[i]) > 1e-4)
                    return false;
            return true;
        });
        if (!twin)
            out.push_back(s);
    }
    return out;
}

/** The groom node: resolves the stack's curves into locks and sweeps the lock mesh. */
class GroomNode final : public WarningNode
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        try
        {
            const Surface* scalp = mScalp.get(*this);
            Splines curves = withoutTwins(splinesOf(value(idx(*this, "inCurve"))));
            attr::ensureIds(curves);
            const LockOptions o = lockOptionsOf(*this);
            std::vector<std::string> warnings;
            Splines resolved = resolveLocks(curves, scalp, o, &warnings);
            LockMesh lm = lockMesh(resolved, o);
            setOutput(idx(*this, "lockCount"), int64_t(lm.locks));
            setOutput(idx(*this, "outCurve"), makeCurveData(std::move(resolved)));
            setOutput(idx(*this, "outMesh"), MeshPtr(std::make_shared<geo::Mesh>(std::move(lm.mesh))));
            report(warnings);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(idx(*this, "outMesh"), MeshPtr());
        }
    }

private:
    ScalpCache mScalp;
};

/** A style modifier: curves in, styled curves out; passes them through when disabled. */
class StyleNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        Splines curves = splinesOf(value(idx(*this, "inCurve")));
        try
        {
            if (b(*this, "enabled"))
            {
                attr::ensureIds(curves);
                applyStyle(curves, styleOptionsOf(*this), mScalp.get(*this));
            }
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
        }
        setOutput(idx(*this, "outCurve"), makeCurveData(std::move(curves)));
    }

private:
    ScalpCache mScalp;
};

/** The shell: resolved locks into one surface, re-meshing only what a change touched. */
class ShellNode final : public WarningNode
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        try
        {
            const Splines& resolved = splinesOf(value(idx(*this, "inCurve")));
            std::shared_ptr<const geo::Mesh> m = mBuilder.update(resolved, shellOptionsOf(*this));
            setOutput(idx(*this, "outMesh"), MeshPtr(std::const_pointer_cast<geo::Mesh>(m)));
            report({});
        }
        catch (const std::exception& e)
        {
            report({e.what()});
            setOutput(idx(*this, "outMesh"), MeshPtr());
        }
    }

private:
    ShellBuilder mBuilder;
};

std::vector<AttrDef> commonIns()
{
    return {
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Curves in, world space."},
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The scalp."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The scalp's world matrix."},
    };
}

const char* kRampDoc = "Strength from root to tip: keys \"t:value\", t 0 at the root to 1 at the tip; empty uses the modifier's own.";

std::vector<AttrDef> styleAttrs(Style k)
{
    const std::string c = "Style";
    std::vector<AttrDef> a = commonIns();
    a.push_back(bAttr("enabled", true, "Off passes the curves through unchanged.", c));
    switch (k)
    {
    case Style::Split:
        a.push_back(iAttr("count", 3, "Sub-locks per lock.", c, 1));
        a.push_back(fAttr("spread", 1.0, "How far the sub-locks fan out, cm.", c, 0.0));
        a.push_back(fAttr("widthScale", 0.55, "Sub-lock width as a fraction of its lock's.", c, 0.01));
        break;
    case Style::Clump:
        a.push_back(fAttr("amount", 0.6, "How far curves pull toward their clump's middle, 0 to 1; painted clump adds to it.", c, 0.0, 1.0));
        a.push_back(iAttr("count", 0, "Clumps; 0 groups by the clump attr (each split lock's sub-locks).", c, 0));
        break;
    case Style::Curl:
        a.push_back(fAttr("amount", 1.0, "Strength, 0 to 1; painted curl adds to it.", c, 0.0));
        a.push_back(fAttr("radius", 1.0, "Curl radius, cm.", c, 0.0));
        a.push_back(fAttr("frequency", 0.25, "Turns per cm.", c, 0.0));
        break;
    case Style::Wave:
        a.push_back(fAttr("amount", 1.0, "Wave height, cm; painted wave adds to it.", c));
        a.push_back(fAttr("frequency", 0.15, "Waves per cm.", c, 0.0));
        break;
    case Style::Frizz:
        a.push_back(fAttr("amount", 0.3, "Kink size, cm; painted frizz adds to it.", c));
        a.push_back(fAttr("frequency", 1.5, "Kinks per cm.", c, 0.0));
        break;
    case Style::Noise:
        a.push_back(fAttr("amount", 1.0, "Displacement, cm.", c));
        a.push_back(fAttr("frequency", 0.08, "Features per cm: low bends whole locks together.", c, 0.0));
        break;
    case Style::Length:
        a.push_back(fAttr("amount", 0.3, "Most a curve is shortened, as a fraction of its length.", c, 0.0, 0.95));
        break;
    case Style::Cut:
        a.push_back(fAttr("length", 20.0, "Longest a curve may be, cm.", c, 0.0));
        break;
    case Style::Flyaways:
        a.push_back(fAttr("amount", 0.5, "Strays per lock.", c, 0.0));
        a.push_back(fAttr("spread", 2.0, "How far strays stray from their lock, cm.", c, 0.0));
        a.push_back(fAttr("length", 0.6, "Stray length, fraction of its lock.", c, 0.0, 1.0));
        break;
    default: break;
    }
    if (k != Style::Cut && k != Style::Length)
        a.push_back(sAttrDefault("ramp", "", kRampDoc, c));
    if (k != Style::Cut && k != Style::Noise && k != Style::Length)
        a.push_back(fAttr("randomness", 0.5, "Variation between curves, 0 to 1.", c, 0.0, 1.0));
    if (k != Style::Cut)
        a.push_back(iAttr("seed", 1, "Random seed.", c));
    a.push_back(curveOut());
    return a;
}

const char* styleDoc(Style k)
{
    switch (k)
    {
    case Style::Split: return "Splits each lock into sub-locks fanned out around it: chunks of chunks.";
    case Style::Clump: return "Pulls curves toward the middle of their clump, so sub-locks and strands gather into locks.";
    case Style::Curl: return "Winds curves into a helix: curls, ringlets and coils.";
    case Style::Wave: return "Waves curves side to side in the lock's flat plane.";
    case Style::Frizz: return "Fine random kinks along each curve, stronger toward the tips.";
    case Style::Noise: return "Smooth noise in space, so neighbouring locks bend together.";
    case Style::Length: return "Shortens curves at random for an uneven hem.";
    case Style::Cut: return "Trims curves to a length.";
    case Style::Flyaways: return "Adds stray strands off locks.";
    default: return "";
    }
}

double numberOr(const Node& n, const char* a, double def)
{
    const int k = idx(n, a);
    if (k < 0)
        return def;
    const AttrDef& d = n.type().attrs[size_t(k)];
    return d.type == PlugType::Int ? double(n.as<int64_t>(k)) : n.as<double>(k);
}
} // namespace

std::string groomSurfaceOf(const Node& curve)
{
    const int a = curve.type().attrIndex("groomSurface");
    return a < 0 ? std::string() : curve.as<std::string>(a);
}

LockOptions lockOptionsOf(const Node& n)
{
    LockOptions o;
    o.width = std::max(1e-3, f(n, "width"));
    o.thickness = std::clamp(f(n, "thickness"), 0.02, 4.0);
    o.profile = LockProfile(std::clamp<int64_t>(i(n, "profile"), 0, int(LockProfile::Count) - 1));
    o.customProfile = parsePairs(s(n, "customProfile"));
    o.widthRamp = curve::parseRamp(s(n, "widthRamp"));
    o.thicknessRamp = curve::parseRamp(s(n, "thicknessRamp"));
    o.twist = glm::radians(f(n, "twist"));
    o.twistRamp = curve::parseRamp(s(n, "twistRamp"));
    o.flatten = std::clamp(f(n, "flatten"), 0.0, 1.0);
    o.rest = b(n, "rest");
    o.lift = f(n, "lift");
    o.strayWidth = std::max(1e-3, f(n, "strayWidth"));
    o.segments = int(std::clamp<int64_t>(i(n, "segments"), 0, 512));
    o.segmentLength = std::max(0.05, f(n, "segmentLength"));
    o.sides = int(std::clamp<int64_t>(i(n, "sides"), 3, 64));
    o.caps = b(n, "caps");
    o.columns = int(std::clamp<int64_t>(i(n, "atlasColumns"), 1, 64));
    o.rows = int(std::clamp<int64_t>(i(n, "atlasRows"), 1, 64));
    o.seed = uint32_t(i(n, "seed"));
    return o;
}

ShellOptions shellOptionsOf(const Node& n)
{
    ShellOptions o;
    o.voxelSize = std::max(0.02, f(n, "voxelSize"));
    o.blend = std::max(0.0, f(n, "blend"));
    o.inflate = f(n, "inflate");
    o.segments = int(std::clamp<int64_t>(i(n, "segments"), 0, 256));
    o.skipStrays = b(n, "skipStrays");
    o.projectSteps = int(std::clamp<int64_t>(i(n, "projectSteps"), 0, 4));
    o.gpu = b(n, "gpu");
    return o;
}

bool isStyleNode(const Node& n)
{
    Style k;
    const std::string& t = n.type().registered().name;
    return t.size() > 5 && t.starts_with("groom") && parseStyle(std::string(1, char(std::tolower(t[5]))) + t.substr(6), &k);
}

StyleOptions styleOptionsOf(const Node& n)
{
    StyleOptions o;
    const std::string& t = n.type().registered().name;
    if (t.size() > 5)
        parseStyle(std::string(1, char(std::tolower(t[5]))) + t.substr(6), &o.kind);
    o.amount = numberOr(n, "amount", o.amount);
    o.frequency = numberOr(n, "frequency", o.frequency);
    o.radius = numberOr(n, "radius", o.radius);
    o.count = int(numberOr(n, "count", o.count));
    o.spread = numberOr(n, "spread", o.spread);
    o.length = numberOr(n, "length", o.length);
    o.widthScale = numberOr(n, "widthScale", o.widthScale);
    o.randomness = numberOr(n, "randomness", o.randomness);
    o.seed = uint32_t(numberOr(n, "seed", 1.0));
    if (int k = idx(n, "ramp"); k >= 0)
        o.ramp = curve::parseRamp(n.as<std::string>(k));
    return o;
}

std::optional<std::pair<D3, D3>> mirrorPlane(const Node& curve)
{
    const int axis = curve::symmetryAxis(curve);
    if (axis < 0)
        return std::nullopt;
    const int space = curve.type().attrIndex("symmetrySpace");
    if (space < 0 || curve.as<int64_t>(space) == 0)
    {
        D3 n(0.0);
        n[axis] = 1.0;
        return std::pair{D3(0.0), n};
    }
    const glm::dmat4 w = curve.as<glm::dmat4>(Transform::kWorldMatrix);
    const D3 n = glm::cross(D3(w[(axis + 1) % 3]), D3(w[(axis + 2) % 3]));
    if (glm::length(n) < 1e-12)
        return std::nullopt;
    return std::pair{D3(w[3]), glm::normalize(n)};
}

bool mirrorDabToSource(const Node& curve, const Splines& ownLocal, D3& center, D3& normal, D3& delta, D3& view)
{
    const auto plane = mirrorPlane(curve);
    if (!plane)
        return false;
    const auto [at, n] = *plane;
    auto point = [&](const D3& p) { return p - n * (2.0 * glm::dot(p - at, n)); };
    auto dir = [&](const D3& v) { return v - n * (2.0 * glm::dot(v, n)); };
    const glm::dmat4 w = curve.as<glm::dmat4>(Transform::kWorldMatrix);
    const D3 flipped = point(center);
    double direct = 1e300, mirrored = 1e300;
    for (const Spline& s : ownLocal)
        for (const D3& p : s.points)
        {
            const D3 wp(w * glm::dvec4(p, 1.0));
            direct = std::min(direct, glm::distance(wp, center));
            mirrored = std::min(mirrored, glm::distance(wp, flipped));
        }
    if (!(mirrored < direct))
        return false;
    center = flipped;
    normal = dir(normal);
    delta = dir(delta);
    view = dir(view);
    return true;
}

Node* groomOf(Graph& g, const Node& curve)
{
    PlugRef out{curve.id(), curve::CurveShape::kWorldSpace, -1};
    for (int depth = 0; depth < 64; ++depth)
    {
        Node* next = nullptr;
        for (const PlugRef& d : g.destinations(out))
            if (Node* n = g.node(d.node))
            {
                if (n->type().isA("groom"))
                    return n;
                if (isStyleNode(*n) && d.attr == idx(*n, "inCurve"))
                    next = n;
            }
        if (!next)
            return nullptr;
        out = {next->id(), idx(*next, "outCurve"), -1};
    }
    return nullptr;
}

std::vector<Node*> styleStack(Graph& g, const Node& groom)
{
    std::vector<Node*> out;
    PlugRef in{groom.id(), idx(groom, "inCurve"), -1};
    for (int depth = 0; depth < 64; ++depth)
    {
        auto src = g.source(in);
        Node* n = src ? g.node(src->node) : nullptr;
        if (!n || !isStyleNode(*n))
            break;
        out.push_back(n);
        in = {n->id(), idx(*n, "inCurve"), -1};
    }
    std::ranges::reverse(out);
    return out;
}

Node* guidesOf(Graph& g, const Node& groom)
{
    std::vector<Node*> stack = styleStack(g, groom);
    PlugRef in = stack.empty() ? PlugRef{groom.id(), idx(groom, "inCurve"), -1} : PlugRef{stack.front()->id(), idx(*stack.front(), "inCurve"), -1};
    auto src = g.source(in);
    Node* n = src ? g.node(src->node) : nullptr;
    return n && n->type().isA("curve") ? n : nullptr;
}

Node* findGroom(Graph& g, const Node& part)
{
    if (part.type().isA("groom"))
        return const_cast<Node*>(&part);
    if (part.type().isA("curve"))
        return groomOf(g, part);
    if (isStyleNode(part))
    {
        PlugRef out{part.id(), idx(part, "outCurve"), -1};
        for (int depth = 0; depth < 64; ++depth)
        {
            Node* next = nullptr;
            for (const PlugRef& d : g.destinations(out))
                if (Node* n = g.node(d.node))
                {
                    if (n->type().isA("groom"))
                        return n;
                    if (isStyleNode(*n))
                        next = n;
                }
            if (!next)
                return nullptr;
            out = {next->id(), idx(*next, "outCurve"), -1};
        }
        return nullptr;
    }
    if (part.type().isA("mesh") || part.type().isA("groomShell"))
    {
        const NodeId start = part.id();
        PlugRef in = part.type().isA("mesh") ? PlugRef{start, MeshNode::kInMesh, -1} : PlugRef{start, idx(part, "inCurve"), -1};
        for (int depth = 0; depth < 4; ++depth)
        {
            auto src = g.source(in);
            Node* n = src ? g.node(src->node) : nullptr;
            if (!n)
                return nullptr;
            if (n->type().isA("groom"))
                return n;
            if (!n->type().isA("groomShell"))
                return nullptr;
            in = {n->id(), idx(*n, "inCurve"), -1};
        }
    }
    return nullptr;
}

void registerHairCardsType();

void registerTypes()
{
    if (types::find("groom"))
        return;
    registerHairCardsType();
    registerGroomLabType();
    const std::string lock = "Lock", mesh = "Mesh", uv = "Atlas";
    NodeType t;
    t.name = "groom";
    t.icon = "feather";
    t.doc = "Locks swept along the groom's curves: a profile with width, thickness and twist ramps from root to tip, lying flat "
            "on the scalp near it, as one mesh with uvs along each lock.";
    t.ownAttrs = commonIns();
    for (AttrDef d : std::vector<AttrDef>{
             fAttr("width", 2.0, "Lock width, cm, times each lock's width and its points' radius.", lock, 0.0),
             fAttr("thickness", 0.35, "Thickness as a fraction of width; 1 is round.", lock, 0.02, 4.0),
             eAttr("profile", profileNames(), int64_t(LockProfile::Ribbon), "Cross-section of every lock without one of its own.", lock),
             sAttrDefault("customProfile", "", "Custom profile: \"x,y x,y ...\" around a closed outline, x across the width.", lock),
             sAttrDefault("widthRamp", "0:0.55 0.12:1 0.7:0.8 1:0.08", "Width from root to tip: keys \"t:scale\".", lock),
             sAttrDefault("thicknessRamp", "", "Thickness from root to tip: keys \"t:scale\"; empty keeps it.", lock),
             fAttr("twist", 0.0, "Twist from root to tip, degrees.", lock),
             sAttrDefault("twistRamp", "", "How the twist builds from root to tip: keys \"t:fraction\"; empty is even.", lock),
             fAttr("flatten", 1.0, "How strongly locks turn their flat side to the scalp where they lie on it, 0 to 1.", lock, 0.0, 1.0),
             bAttr("rest", true, "Locks rest on the scalp instead of sinking into it.", lock),
             fAttr("lift", 0.0, "Gap between the locks and the scalp, cm.", lock),
             fAttr("strayWidth", 0.08, "Width of stray strands, cm.", lock, 0.0),
             fAttr("segmentLength", 0.75, "Spacing of rings along each lock, cm.", mesh, 0.05),
             iAttr("segments", 0, "Rings along each lock; 0 follows segmentLength.", mesh, 0),
             iAttr("sides", 10, "Points around each lock.", mesh, 3),
             bAttr("caps", true, "Close the root and tip.", mesh),
             iAttr("atlasColumns", 1, "Texture atlas cells across; each lock takes one by its id.", uv, 1),
             iAttr("atlasRows", 1, "Texture atlas cells down.", uv, 1),
             iAttr("seed", 1, "Seed for picking atlas cells.", uv),
         })
        t.ownAttrs.push_back(std::move(d));
    t.ownAttrs.push_back(meshOut());
    t.ownAttrs.push_back(dataOut("outCurve", "The resolved locks (curves with their cross-sections), world space."));
    t.ownAttrs.push_back({.name = "lockCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Locks made."});
    t.ownAttrs.push_back(stringOut("warning", "Problems with the last build."));
    t.create = [] { return std::make_unique<GroomNode>(); };
    types::registerType(std::move(t));

    for (int k = 0; k < int(Style::Count); ++k)
    {
        NodeType st;
        st.name = styleNodeType(Style(k));
        st.icon = "wave-sine";
        st.doc = styleDoc(Style(k));
        st.ownAttrs = styleAttrs(Style(k));
        st.create = [] { return std::make_unique<StyleNode>(); };
        types::registerType(std::move(st));
    }

    NodeType sh;
    sh.name = "groomShell";
    sh.icon = "blob";
    sh.doc = "One watertight surface over a groom's locks, smooth-unioned and meshed with surface nets, ready for retopology.";
    const std::string c = "Shell";
    sh.ownAttrs = {
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Resolved locks (a groom's outCurve)."},
        fAttr("voxelSize", 0.3, "Mesh resolution, cm.", c, 0.02),
        fAttr("blend", 0.6, "Smooth union between locks, cm: larger fills the gaps between them.", c, 0.0),
        fAttr("inflate", 0.0, "Grows every lock, cm.", c),
        iAttr("segments", 16, "Round cones per lock; 0 uses every ring.", c, 0),
        bAttr("skipStrays", true, "Leave stray strands out.", c),
        iAttr("projectSteps", 1, "Steps moving vertices onto the surface.", c, 0),
        bAttr("gpu", false, "Sample the field on the GPU when one is available.", c),
        meshOut(),
        stringOut("warning", "Problems with the last build."),
    };
    sh.create = [] { return std::make_unique<ShellNode>(); };
    types::registerType(std::move(sh));
}
} // namespace lila::groom
