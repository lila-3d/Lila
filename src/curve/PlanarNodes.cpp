#include "curve/PlanarNodes.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "curve/CurveBoolean.h"
#include "curve/CurveNodes.h"
#include "curve/Pipe.h"
#include "curve/Roof.h"
#include "curve/Svg.h"
#include "curve/Text.h"
#include "ops/Command.h"
#include "io/FileTranslators.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D2 = glm::dvec2;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
constexpr uint32_t kIn = AttrInput | AttrStorable;
constexpr uint32_t kKey = AttrInput | AttrStorable | AttrKeyable;

AttrDef fAttr(std::string name, double def, std::string doc, std::string cat = {}, std::optional<double> lo = std::nullopt,
              std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    a.category = std::move(cat);
    return a;
}

AttrDef iAttr(std::string name, int64_t def, std::string doc, std::string cat = {}, double lo = 0.0)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.category = std::move(cat);
    return a;
}

AttrDef bAttr(std::string name, bool def, std::string doc, std::string cat = {})
{
    AttrDef a{.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.category = std::move(cat);
    return a;
}

AttrDef eAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc, std::string cat = {})
{
    AttrDef a{.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kIn, .enumNames = std::move(names), .doc = std::move(doc)};
    a.category = std::move(cat);
    return a;
}

AttrDef sAttr(std::string name, std::string def, std::string doc, std::string cat = {}, uint32_t extra = 0)
{
    AttrDef a{.name = std::move(name), .type = PlugType::String, .defaultValue = std::move(def), .flags = kIn | extra, .doc = std::move(doc)};
    a.category = std::move(cat);
    return a;
}

AttrDef vAttr(std::string name, const D3& def, std::string doc, std::string cat = {})
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    for (int i = 0; i < 3; ++i)
        a.childNames[size_t(i)] = name + char('X' + i);
    a.category = std::move(cat);
    return a;
}

AttrDef dataIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}

AttrDef curveOut() { return {.name = "outCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden, .doc = "The curves it makes."}; }

AttrDef meshOut() { return {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "The mesh it makes."}; }

/** The node's curves, one element per input curve; fileAlias reads the numbered inCurve0 ... attrs it replaced. */
std::vector<AttrDef> curveInputs(const std::string& doc)
{
    return {{.name = "inCurves", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden | AttrArray, .doc = doc,
             .fileAlias = "inCurve"}};
}

int idx(const Node& n, const char* a) { return n.type().attrIndex(a); }
double f(const Node& n, const char* a) { return n.as<double>(idx(n, a)); }
int64_t i(const Node& n, const char* a) { return n.as<int64_t>(idx(n, a)); }
bool b(const Node& n, const char* a) { return n.as<bool>(idx(n, a)); }
D3 v(const Node& n, const char* a) { return n.as<D3>(idx(n, a)); }
const std::string& s(const Node& n, const char* a) { return n.as<std::string>(idx(n, a)); }

/** Each connected input's splines: one operand per input. */
std::vector<Splines> operandsOf(const Node& n)
{
    std::vector<Splines> out;
    for (const auto& [k, v] : n.elements(idx(n, "inCurves")))
        if (const Splines& sp = splinesOf(v); !sp.empty())
            out.push_back(sp);
    return out;
}

Splines allCurvesOf(const Node& n)
{
    Splines out;
    for (Splines& op : operandsOf(n))
        out.insert(out.end(), std::make_move_iterator(op.begin()), std::make_move_iterator(op.end()));
    return out;
}

/** A history node whose run also reports problems, shown in its warning attr and the log. */
class PlanarNode final : public Node
{
public:
    std::function<PlugValue(const Node&, std::vector<std::string>&)> run;
    int outAttr = -1;
    int warnAttr = -1;

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        std::vector<std::string> warnings;
        try
        {
            setOutput(outAttr, run(*this, warnings));
        }
        catch (const std::exception& e)
        {
            warnings.push_back(e.what());
            setOutput(outAttr, type().attrs[size_t(outAttr)].type == PlugType::Mesh ? PlugValue(MeshPtr()) : PlugValue(DataPtr()));
        }
        std::string text;
        for (const std::string& w : warnings)
            text += (text.empty() ? "" : "; ") + w;
        if (!text.empty() && text != as<std::string>(warnAttr))
            log::warn("{}: {}", name(), text);
        setOutput(warnAttr, text);
    }
};

void registerPlanar(std::string name, std::string icon, std::string doc, std::vector<AttrDef> attrs,
                    std::function<PlugValue(const Node&, std::vector<std::string>&)> run)
{
    if (types::find(name))
        return;
    attrs.push_back({.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."});
    NodeType t;
    t.name = std::move(name);
    t.icon = std::move(icon);
    t.doc = std::move(doc);
    t.ownAttrs = std::move(attrs);
    int out = -1, warn = -1;
    for (size_t k = 0; k < t.ownAttrs.size(); ++k)
    {
        if (t.ownAttrs[k].name == "outCurve" || t.ownAttrs[k].name == "outMesh")
            out = int(k);
        if (t.ownAttrs[k].name == "warning")
            warn = int(k);
    }
    t.create = [run, out, warn] {
        auto n = std::make_unique<PlanarNode>();
        n->run = run;
        n->outAttr = out;
        n->warnAttr = warn;
        return n;
    };
    types::registerType(std::move(t));
}

PlugValue meshValue(geo::Mesh m) { return MeshPtr(std::make_shared<geo::Mesh>(std::move(m))); }

void append(std::vector<std::string>& to, const std::vector<std::string>& from) { to.insert(to.end(), from.begin(), from.end()); }

const std::vector<std::string> kOps = {"union", "subtract", "intersect", "xor"};
const std::vector<std::string> kJoins = {"round", "miter", "square"};
const std::vector<std::string> kProfiles = {"straight", "convex", "concave", "step", "custom"};

PlugValue booleanRun(const Node& n, std::vector<std::string>& warnings)
{
    CurveBooleanOptions o;
    o.op = BooleanOp(std::clamp<int64_t>(i(n, "operation"), 0, 3));
    o.normal = v(n, "normal");
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    std::vector<Splines> ops = operandsOf(n);
    if (ops.empty())
        return makeCurveData({});
    CurveBooleanResult r = curveBoolean(ops, o);
    append(warnings, r.warnings);
    return makeCurveData(std::move(r.curves));
}

PlugValue offsetRegionRun(const Node& n, std::vector<std::string>& warnings)
{
    RegionOffsetOptions o;
    o.distance = f(n, "distance");
    o.join = JoinType(std::clamp<int64_t>(i(n, "join"), 0, 2));
    o.miterLimit = std::max(1.0, f(n, "miterLimit"));
    o.normal = v(n, "normal");
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    Splines in = allCurvesOf(n);
    if (in.empty())
        return makeCurveData({});
    CurveBooleanResult r = regionOffset(in, o);
    append(warnings, r.warnings);
    return makeCurveData(std::move(r.curves));
}

/** "x:y x:y ..." pairs. */
std::vector<D2> parsePairs(std::string_view text)
{
    std::vector<D2> out;
    std::vector<double> nums = parseNumbers(text);
    for (size_t k = 0; k + 1 < nums.size(); k += 2)
        out.push_back(D2(nums[k], nums[k + 1]));
    return out;
}

/** A profile curve's points: x is the inset, y the rise (drawn in the front view); a curve on another plane uses its own two axes. */
std::vector<D2> profileFromCurve(const Splines& sp)
{
    std::vector<D2> out;
    if (sp.empty() || spanCount(sp.front()) == 0)
        return out;
    TessOptions o;
    o.chordError = 0.005;
    Polyline pl = tessellate(sp.front(), o);
    D3 n = planeNormal(pl.points, 1e30);
    int drop = 2;
    if (glm::length(n) > 0.5)
        for (int k = 0; k < 3; ++k)
            if (std::abs(n[k]) > std::abs(n[drop]))
                drop = k;
    int ax = drop == 0 ? 2 : 0, ay = drop == 1 ? 2 : 1;
    for (const D3& p : pl.points)
        out.push_back(D2(p[ax], p[ay]));
    return out;
}
} // namespace

BevelPlusOptions bevelPlusOptionsOf(const Node& n)
{
    BevelPlusOptions o;
    o.extrudeDepth = std::max(0.0, f(n, "extrudeDepth"));
    o.extrudeDivisions = int(std::max<int64_t>(1, i(n, "extrudeDivisions")));
    auto side = [&](const char* prefix, bool on) {
        std::string p(prefix);
        BevelSide s;
        s.width = on ? std::max(0.0, f(n, (p + "Width").c_str())) : 0.0;
        s.depth = std::max(0.0, f(n, (p + "Depth").c_str()));
        s.profile = BevelProfile(std::clamp<int64_t>(i(n, (p + "Profile").c_str()), 0, 4));
        s.segments = int(std::max<int64_t>(1, i(n, (p + "Segments").c_str())));
        return s;
    };
    o.front = side("front", b(n, "frontBevel"));
    o.back = side("back", b(n, "backBevel"));
    std::vector<D2> custom = profileFromCurve(splinesOf(n.value(idx(n, "profileCurve"))));
    if (custom.size() < 2)
        custom = parsePairs(s(n, "customProfile"));
    o.front.custom = o.back.custom = custom;
    o.frontCap = b(n, "frontCap");
    o.backCap = b(n, "backCap");
    o.capSpacing = std::max(0.0, f(n, "capSpacing"));
    o.capQuads = b(n, "capQuads");
    o.smoothingAngle = std::clamp(f(n, "smoothingAngle"), 0.0, 180.0);
    o.uvScale = std::max(0.0, f(n, "uvScale"));
    o.normal = v(n, "normal");
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    o.maxAngle = std::clamp(f(n, "maxAngle"), 0.5, 45.0);
    return o;
}

namespace
{
PlugValue bevelRun(const Node& n, std::vector<std::string>& warnings)
{
    Splines in = allCurvesOf(n);
    if (in.empty())
        return meshValue({});
    BevelPlusOptions o = bevelPlusOptionsOf(n);
    if (o.front.profile == BevelProfile::Custom && o.front.custom.size() < 2)
        warnings.push_back("custom profile: connect a profile curve or set customProfile; straight is used");
    BevelPlusResult r = bevelPlus(in, o);
    append(warnings, r.warnings);
    return meshValue(std::move(r.mesh));
}

PlugValue textRun(const Node& n, std::vector<std::string>& warnings)
{
    TextOptions o;
    o.text = s(n, "text");
    o.font = s(n, "font");
    o.size = std::max(1e-4, f(n, "size"));
    o.letterSpacing = f(n, "letterSpacing");
    o.wordSpacing = f(n, "wordSpacing");
    o.lineSpacing = f(n, "lineSpacing");
    o.align = TextAlign(std::clamp<int64_t>(i(n, "alignment"), 0, 3));
    o.anchor = TextAnchor(std::clamp<int64_t>(i(n, "anchor"), 0, 3));
    o.wrapWidth = std::max(0.0, f(n, "wrapWidth"));
    o.textCase = TextCase(std::clamp<int64_t>(i(n, "textCase"), 0, 3));
    o.width = std::max(0.01, f(n, "width"));
    o.slant = std::clamp(f(n, "slant"), -60.0, 60.0);
    o.weight = f(n, "weight");
    o.underline = b(n, "underline");
    o.strikethrough = b(n, "strikethrough");
    o.kerning = b(n, "kerning");
    o.mergeOverlaps = b(n, "mergeOverlaps");
    TextResult r = textCurves(o);
    append(warnings, r.warnings);
    return makeCurveData(std::move(r.curves));
}

PlugValue roofRun(const Node& n, std::vector<std::string>& warnings)
{
    Splines in = allCurvesOf(n);
    if (in.empty())
        return meshValue({});
    RoofOptions o;
    o.pitch = std::clamp(f(n, "pitch"), 1.0, 89.0);
    o.height = std::max(0.0, f(n, "height"));
    o.overhang = std::max(0.0, f(n, "overhang"));
    for (double g : parseNumbers(s(n, "gableEdges")))
        o.gables.push_back(int(g));
    o.gableEnds = b(n, "gableEnds");
    o.thickness = std::max(0.0, f(n, "thickness"));
    o.uvScale = std::max(1e-3, f(n, "uvScale"));
    o.normal = v(n, "normal");
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    RoofResult r = roof(in, o);
    append(warnings, r.warnings);
    return meshValue(std::move(r.mesh));
}

PlugValue insetRun(const Node& n, std::vector<std::string>& warnings)
{
    Splines in = allCurvesOf(n);
    if (in.empty())
        return makeCurveData({});
    SkeletonCurvesResult r = insetCurves(in, f(n, "distance"), int(std::max<int64_t>(1, i(n, "steps"))), v(n, "normal"), std::max(1e-5, f(n, "tolerance")));
    append(warnings, r.warnings);
    return makeCurveData(std::move(r.curves));
}

PlugValue centerRun(const Node& n, std::vector<std::string>& warnings)
{
    Splines in = allCurvesOf(n);
    if (in.empty())
        return makeCurveData({});
    SkeletonCurvesResult r = centerLines(in, v(n, "normal"), std::max(1e-5, f(n, "tolerance")));
    append(warnings, r.warnings);
    return makeCurveData(std::move(r.curves));
}

void registerTypesHere()
{
    const std::string ext = "Extrude", fb = "Front Bevel", bb = "Back Bevel", cap = "Caps", outp = "Output", tess = "Tessellation";
    std::vector<AttrDef> bo = curveInputs("Target curves (a letter and its counters fill even-odd), world space.");
    for (AttrDef a : {eAttr("operation", kOps, 0,
                            "union joins every input, subtract cuts the others out of the first, intersect keeps what all share, xor what "
                            "an odd number cover."),
                      vAttr("normal", D3(0.0), "Plane normal; zero fits the plane to the curves."),
                      fAttr("tolerance", 0.002, "Where the cuts fall: chord error of the clip, cm. The pieces are cut from the input curves.", {}, 1e-5),
                      curveOut()})
        bo.push_back(a);
    registerPlanar("curveBoolean", "layers-union", "2D boolean of closed planar curves, each piece cut exactly from the input it lies on.", bo,
                   booleanRun);

    std::vector<AttrDef> ro = curveInputs("Closed curves bounding the region (holes even-odd), world space.");
    for (AttrDef a : {fAttr("distance", 1.0, "cm; positive grows the region, negative insets it."),
                      eAttr("join", kJoins, 0, "Corners on the outside: round, miter or square."),
                      fAttr("miterLimit", 4.0, "Longest miter, in distances.", {}, 1.0),
                      vAttr("normal", D3(0.0), "Plane normal; zero fits the plane to the curves."),
                      fAttr("tolerance", 0.01, "Fitting tolerance for curved results, cm.", {}, 1e-5), curveOut()})
        ro.push_back(a);
    registerPlanar("offsetRegion", "ripple", "Offsets the region closed curves bound: loops that run into each other merge, holes shrink as it grows.", ro,
                   offsetRegionRun);

    std::vector<AttrDef> bp = curveInputs("Closed planar curves; loops inside others are holes. World space.");
    bp.push_back(dataIn("profileCurve", "A curve drawn in the front view as the custom bevel profile: x inset, y rise."));
    for (AttrDef a : {fAttr("extrudeDepth", 2.0, "Length of the straight sides along the normal, cm.", ext, 0.0),
                      iAttr("extrudeDivisions", 1, "Rows of faces along the sides.", ext, 1),
                      bAttr("frontBevel", true, "Bevel the front.", fb),
                      fAttr("frontWidth", 0.5, "How far in from the outline the front bevel reaches, cm.", fb, 0.0),
                      fAttr("frontDepth", 0.5, "How far the front bevel rises past the sides, cm.", fb, 0.0),
                      eAttr("frontProfile", kProfiles, 0, "Shape across the front bevel: straight, convex round, concave cove, steps or custom.", fb),
                      iAttr("frontSegments", 1, "Segments across the front bevel (steps for step).", fb, 1),
                      bAttr("backBevel", true, "Bevel the back.", bb),
                      fAttr("backWidth", 0.5, "How far in from the outline the back bevel reaches, cm.", bb, 0.0),
                      fAttr("backDepth", 0.5, "How far the back bevel reaches past the sides, cm.", bb, 0.0),
                      eAttr("backProfile", kProfiles, 0, "Shape across the back bevel.", bb),
                      iAttr("backSegments", 1, "Segments across the back bevel.", bb, 1),
                      sAttr("customProfile", "", "Custom profile points \"inset:rise ...\" from the side to the cap, e.g. \"0:0 0.2:0.7 1:1\".", fb),
                      bAttr("frontCap", true, "Close the front.", cap),
                      bAttr("backCap", true, "Close the back.", cap),
                      fAttr("capSpacing", 0.0, "Quad size on the caps and longest side edge, cm; 0 picks it from the outline.", cap, 0.0),
                      bAttr("capQuads", true, "Caps as quads where they fit; off leaves triangles.", cap),
                      fAttr("smoothingAngle", 30.0, "Edges where faces meet sharper than this (degrees) are hard.", outp, 0.0, 180.0),
                      fAttr("uvScale", 0.0, "cm per UV unit; 0 fits the caps into 0..1 with the sides to match.", outp, 0.0),
                      vAttr("normal", D3(0.0), "Extrusion direction; zero fits the plane to the curves (facing up or out).", outp),
                      fAttr("tolerance", 0.01, "Largest gap between the curves and the outline, cm.", tess, 1e-5),
                      fAttr("maxAngle", 8.0, "Largest turn between outline segments, degrees.", tess, 0.5, 45.0), meshOut()})
        bp.push_back(a);
    const std::string rf = "Roof";
    std::vector<AttrDef> ro2 = curveInputs("Closed footprint curves (inner loops are courtyards), world space.");
    for (AttrDef a : {fAttr("pitch", 30.0, "Slope of the roof planes, degrees.", rf, 1.0, 89.0),
                      fAttr("height", 0.0, "Ridge height above the footprint, cm; 0 uses the pitch.", rf, 0.0),
                      fAttr("overhang", 0.0, "How far the eaves reach past the footprint, cm.", rf, 0.0),
                      sAttr("gableEdges", "", "Footprint edges that end in a gable, e.g. \"0 2\" (edge i runs from point i to i + 1).", rf),
                      bAttr("gableEnds", false, "Gable every end: each edge between two convex corners shorter than both neighbours.", rf),
                      fAttr("thickness", 0.0, "Roof slab thickness, cm; 0 is a single surface.", rf, 0.0),
                      fAttr("uvScale", 100.0, "cm per UV unit along the roof planes.", outp, 1e-3),
                      vAttr("normal", D3(0.0), "Up for the roof; zero fits the plane to the footprint.", outp),
                      fAttr("tolerance", 0.01, "Tessellation of curved footprints, cm.", tess, 1e-5), meshOut()})
        ro2.push_back(a);
    registerPlanar("roof", "building-cottage", "A roof over closed footprint curves from the straight skeleton: hips, valleys and ridges at one pitch, gables where asked.",
                   ro2, roofRun);

    std::vector<AttrDef> in2 = curveInputs("Closed curves bounding the region (holes by containment), world space.");
    for (AttrDef a : {fAttr("distance", 1.0, "How far in, cm.", {}, 0.0), iAttr("steps", 1, "Evenly spaced insets out to the distance.", {}, 1),
                      vAttr("normal", D3(0.0), "Plane normal; zero fits the plane to the curves."),
                      fAttr("tolerance", 0.01, "Tessellation of curved input, cm.", {}, 1e-5), curveOut()})
        in2.push_back(a);
    registerPlanar("insetCurve", "border-inner",
                   "Insets the region closed curves bound along the straight skeleton: mitred corners, narrow parts vanish, pinched loops split.", in2,
                   insetRun);

    std::vector<AttrDef> cl = curveInputs("Closed curves bounding the region (holes by containment), world space.");
    for (AttrDef a : {vAttr("normal", D3(0.0), "Plane normal; zero fits the plane to the curves."),
                      fAttr("tolerance", 0.01, "Tessellation of curved input, cm.", {}, 1e-5), curveOut()})
        cl.push_back(a);
    registerPlanar("curveCenterLine", "line", "Centre lines of the region closed curves bound: the straight skeleton's inner arcs.", cl, centerRun);

    const std::string txt = "Text", lay = "Layout", sty = "Style";
    registerPlanar("curveText", "typography", "Text as closed Bezier curves, the glyph outlines of a TrueType or OpenType font, live.",
                   {sAttr("text", "Lila", "The text; new lines start new lines.", txt, AttrMultiline),
                    sAttr("font", "Inter", "A font file, or a family and style (\"Inter\", \"Inter SemiBold\", \"Arial Bold\") among the bundled and installed fonts.",
                          txt, AttrFile | AttrFont),
                    fAttr("size", 10.0, "Em size, cm.", txt, 0.0),
                    eAttr("alignment", {"left", "center", "right", "justify"}, 0,
                          "Where lines sit against x = 0. Justify stretches wrapped lines to the wrap width (or the widest line).", lay),
                    eAttr("anchor", {"baseline", "top", "middle", "bottom"}, 0, "Which part of the block sits on y = 0: the first baseline, the top, the middle or the bottom.", lay),
                    fAttr("wrapWidth", 0.0, "Lines break between words to fit this width, cm; 0 breaks only at new lines.", lay, 0.0),
                    fAttr("letterSpacing", 0.0, "Extra space after each character, cm.", lay),
                    fAttr("wordSpacing", 0.0, "Extra space after each space, cm.", lay),
                    fAttr("lineSpacing", 1.0, "Baseline to baseline, in the font's line heights.", lay),
                    eAttr("textCase", {"as typed", "upper", "lower", "title"}, 0, "Changes the letters' case without editing the text.", sty),
                    fAttr("width", 1.0, "Horizontal scale of the glyphs: below 1 condenses, above 1 widens.", sty, 0.01),
                    fAttr("slant", 0.0, "Oblique slant, degrees; positive leans right. For fonts without an italic.", sty, -60.0, 60.0),
                    fAttr("weight", 0.0, "Grows each letter's outline by this much, cm (negative thins it). For fonts without a bolder weight.", sty),
                    bAttr("underline", false, "A bar under each line, joined to the letters it touches.", sty),
                    bAttr("strikethrough", false, "A bar through each line, joined to the letters.", sty),
                    bAttr("kerning", true, "Use the font's kerning.", lay),
                    bAttr("mergeOverlaps", true, "Join a glyph's overlapping contours so each area fills once (variable fonts overlap them).", lay),
                    curveOut()},
                   textRun);

    registerPlanar("bevelPlus", "text-size",
                   "Bevel Plus: closed planar curves (holes by containment) into a solid with sides, front and back bevels mitred along "
                   "the straight skeleton, and quad caps.",
                   bp, bevelRun);
}

Node& curveNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a curve");
    return *n;
}

/** The given curves, or the selected ones in selection order. */
std::vector<Node*> curvesArg(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
        out.push_back(&curveNamed(g, p.get<std::string>(), cmd));
    if (out.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select closed curves");
    return out;
}

/** Flags for every editable attr of a node type (enums by name), with no defaults so only given ones apply. */
std::vector<FlagDef> attrFlags(const std::string& type)
{
    std::vector<FlagDef> out;
    for (const AttrDef& d : types::find(type)->ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden))
            continue;
        FlagDef fd{d.name, "", T::Float, nullptr, d.doc};
        switch (d.type)
        {
        case PlugType::Bool: fd.type = T::Bool; break;
        case PlugType::Int: fd.type = T::Int; break;
        case PlugType::String: fd.type = T::String; break;
        case PlugType::Vec3: fd.type = T::Vec3; break;
        case PlugType::Enum:
            fd.type = T::String;
            fd.choices = d.enumNames;
            break;
        default: break;
        }
        out.push_back(std::move(fd));
    }
    return out;
}

std::vector<FlagDef> withFlags(std::vector<FlagDef> base, std::vector<FlagDef> extra)
{
    base.insert(base.end(), extra.begin(), extra.end());
    return base;
}

void applyAttrFlags(Graph& g, Node& n, const Args& a, const char* cmd)
{
    for (const AttrDef& d : n.type().ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden) || !a.has(d.name))
            continue;
        int at = n.type().attrIndex(d.name);
        const json& val = a.flags[d.name];
        if (d.type == PlugType::Enum && val.is_string())
        {
            auto it = std::ranges::find(d.enumNames, val.get<std::string>());
            if (it == d.enumNames.end())
                throw CommandError(std::string(cmd) + ": " + d.name + ": unknown value '" + val.get<std::string>() + "'");
            g.setValue({n.id(), at, -1}, int64_t(it - d.enumNames.begin()));
            continue;
        }
        try
        {
            g.setValue({n.id(), at, -1}, fromJson(d, val));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + d.name + ": " + e.what());
        }
    }
}

FlagDef chFlag() { return {"constructionHistory", "ch", T::Bool, true, "Keep the history node so its settings stay live."}; }

void bake(Graph& g, Node& op, const PlugRef& dst)
{
    g.evaluate();
    PlugValue val = g.value(dst);
    g.disconnect(dst);
    g.setValue(dst, val);
    g.deleteNode(op.id());
}

void connectInputs(Graph& g, Node& op, const std::vector<Node*>& curves)
{
    for (Node* c : curves)
        g.connect({c->id(), CurveShape::kWorldSpace, -1}, {op.id(), op.type().attrIndex("inCurves")});
}

/** A node of type on the curves, feeding a new curve. Returns [curve, node]. */
json curveResult(Graph& g, const std::string& type, const Args& a, const std::vector<Node*>& curves, const char* defaultName)
{
    Node* op = g.createNode(*types::find(type), {}, kNoNode, true);
    applyAttrFlags(g, *op, a, type.c_str());
    connectInputs(g, *op, curves);
    Node* c = g.createNode(*types::find("curve"), a.get<std::string>("name", defaultName), kNoNode, true);
    g.connect({op->id(), op->type().attrIndex("outCurve"), -1}, {c->id(), CurveShape::kCreate, -1});
    json opName = op->path();
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, *op, {c->id(), CurveShape::kCreate, -1});
        opName = nullptr;
    }
    if (a.get<bool>("hideInputs", false))
        for (Node* in : curves)
            g.setValue({in->id(), Transform::kVisibility, -1}, false);
    g.select({c->id()});
    return json::array({c->path(), opName});
}

json meshResult(Graph& g, Node& op, const Args& a, const char* defaultName)
{
    Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", defaultName), kNoNode, true);
    g.connect({op.id(), op.type().attrIndex("outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
    json opName = op.path();
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, op, {mesh->id(), MeshNode::kInMesh, -1});
        opName = nullptr;
    }
    g.select({mesh->id()});
    return json::array({mesh->path(), opName});
}

void layout(const std::string& command, const std::string& type, std::vector<std::string> order, std::map<std::string, ops::options::Override::Range> ranges,
            std::vector<std::string> hidden = {})
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = std::move(order);
    o.hidden = std::move(hidden);
    o.hidden.push_back("name");
    for (const AttrDef& d : types::find(type)->ownAttrs)
        if (!d.category.empty())
            o.sections[d.name] = d.category;
    o.ranges = std::move(ranges);
    opt::setOverride(command, std::move(o));
}

std::string readText(const std::string& path, const char* cmd)
{
    std::ifstream in(paths::fromUtf8(path), std::ios::binary);
    if (!in)
        throw CommandError(std::string(cmd) + ": cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void registerSkeletonCommands()
{
    ops::registerCommand({
        .name = "roof",
        .doc = "Build a roof over closed footprint curves with live history (Houdini's straight skeleton roof): a plane per footprint "
               "edge at the pitch (or reaching the given ridge height), hips and valleys between them, ridges where they meet, "
               "gables on the edges asked for, an overhang and a slab thickness. Returns [mesh, roof].",
        .positionalDoc = "footprints",
        .flags = withFlags(attrFlags("roof"), {chFlag(), {"name", "n", T::String, nullptr, "Mesh name; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves = curvesArg(g, a, "roof");
            Node* op = g.createNode(*types::find("roof"), {}, kNoNode, true);
            applyAttrFlags(g, *op, a, "roof");
            if (!a.has("normal"))
                g.setValue({op->id(), op->type().attrIndex("normal"), -1}, sceneUp());
            connectInputs(g, *op, curves);
            return meshResult(g, *op, a, "Roof");
        },
        .adjustable = true,
    });
    layout("roof", "roof", {"pitch", "height", "overhang", "gableEnds", "gableEdges", "thickness"},
           {{"pitch", {1.0, 89.0, 1.0, " deg"}}, {"height", {0.0, 1e5, 10.0, " cm"}}, {"overhang", {0.0, 1e4, 5.0, " cm"}}, {"thickness", {0.0, 1e4, 1.0, " cm"}}});

    ops::registerCommand({
        .name = "insetCurve",
        .doc = "Inset the region closed planar curves bound along the straight skeleton, with live history: corners stay mitred, "
               "parts narrower than twice the distance vanish and loops pinched apart come out as separate curves. steps gives "
               "several evenly spaced insets. Returns [curve, insetCurve].",
        .positionalDoc = "curves",
        .flags = withFlags(attrFlags("insetCurve"), {chFlag(), {"name", "n", T::String, nullptr, "Name of the result curve; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return curveResult(g, "insetCurve", a, curvesArg(g, a, "insetCurve"), "Inset");
        },
        .adjustable = true,
    });
    layout("insetCurve", "insetCurve", {"distance", "steps"}, {{"distance", {0.0, 1e5, 0.1, " cm"}}});

    ops::registerCommand({
        .name = "curveCenterLine",
        .doc = "Centre lines (the medial axis of the straight skeleton) of the region closed planar curves bound, as poly curves "
               "with live history. Returns [curve, curveCenterLine].",
        .positionalDoc = "curves",
        .flags = withFlags(attrFlags("curveCenterLine"), {chFlag(), {"name", "n", T::String, nullptr, "Name of the result curve; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return curveResult(g, "curveCenterLine", a, curvesArg(g, a, "curveCenterLine"), "CenterLine");
        },
        .adjustable = true,
    });
}

void registerInsetRegionCommand()
{
    ops::registerCommand({
        .name = "polyInsetRegion",
        .doc = "Inset each flat, connected region of the selected faces as one, along the straight skeleton: the band follows the "
               "region's outline and holes with mitred corners, closes in a ridge where the inset meets itself, and parts it pinches "
               "off become separate faces. The region's faces are replaced by the band and one inner face per part (quad-filled "
               "around holes), which end up selected. UVs carry on from the region. Returns the meshes changed.",
        .flags = {{"distance", "d", T::Float, 1.0, "Inward distance, cm."},
                  {"depth", "", T::Float, 0.0, "Push of the inner faces along the normal, cm; the band ramps to it."},
                  {"planarAngle", "", T::Float, 1.0, "Degrees a region's faces may bend and still count as flat."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            std::map<NodeId, components::Ids> targets = components::selected(ComponentMode::Face);
            std::erase_if(targets, [&](const auto& t) {
                Node* n = g.node(t.first);
                return !n || !n->type().isA("mesh") || t.second.empty();
            });
            if (targets.empty())
                throw CommandError("polyInsetRegion: select faces");
            RegionInsetOptions o;
            o.distance = a.get<double>("distance", 1.0);
            o.depth = a.get<double>("depth", 0.0);
            o.planarAngle = a.get<double>("planarAngle", 1.0);
            if (!(o.distance > 0.0))
                throw CommandError("polyInsetRegion: distance must be above 0");
            json out = json::array();
            for (auto& [id, ids] : targets)
            {
                Node& n = *g.node(id);
                PlugRef base = modifiers::editPlug(g, id);
                g.evaluate();
                const MeshPtr& stored = std::get<MeshPtr>(g.storedValue(base));
                geo::Mesh mesh = stored ? *stored : geo::Mesh();
                RegionInsetResult r = insetRegion(mesh, ids, o);
                for (const std::string& w : r.warnings)
                    log::warn("polyInsetRegion: {}: {}", n.path(), w);
                if (r.inner.empty() && r.band.empty())
                    continue;
                geo::MeshRemap remap;
                mesh.compact(&remap);
                components::Ids inner;
                for (geo::Index f : r.inner)
                    if (f < remap.faces.size() && remap.faces[f] != geo::kInvalid)
                        inner.push_back(remap.faces[f]);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
                components::select(id, ComponentMode::Face, inner, components::Op::Replace);
                out.push_back(n.path());
            }
            return out;
        },
        .adjustable = true,
    });
}

void registerSvgCommands()
{
    ops::registerCommand({
        .name = "importSvg",
        .doc = "Import an SVG file as Bezier curves (paths, rect, circle, ellipse, line, polyline, polygon, with transforms, groups "
               "and the viewBox) on a plane at a scale. One curve per shape under a group named after the file, or all in one "
               "curve. Returns the curve names.",
        .positionalDoc = "path",
        .flags = {{"scale", "s", T::Float, 0.1, "cm per SVG unit (px)."},
                  {"plane", "", T::String, "xy", "Plane the drawing lies on: xy (front), xz (ground) or yz (side).", {"xy", "xz", "yz"}},
                  {"center", "", T::Bool, true, "Centre the drawing on the origin; off puts its top left corner there."},
                  {"merge", "", T::Bool, false, "One curve for the whole drawing instead of one per shape."},
                  {"name", "n", T::String, nullptr, "Name of the group (or the merged curve); the file name by default."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("importSvg: path required");
            std::string path = a.positional[0].get<std::string>();
            SvgImportOptions o;
            o.scale = a.get<double>("scale", 0.1);
            if (!(o.scale > 0.0))
                throw CommandError("importSvg: scale must be positive");
            if (!parseSvgPlane(a.get<std::string>("plane", "xy"), &o.plane))
                throw CommandError("importSvg: plane is xy, xz or yz");
            o.center = a.get<bool>("center", true);
            SvgDocument doc = readSvg(readText(path, "importSvg"), o);
            for (const std::string& w : doc.warnings)
                log::warn("importSvg: {}", w);
            if (doc.shapes.empty())
                throw CommandError("importSvg: " + path + " has no paths or shapes to import");
            Graph& g = Graph::current();
            Graph::Batch batch(g);
            std::string stem = paths::toUtf8(paths::fromUtf8(path).stem());
            std::string base = a.get<std::string>("name", Graph::validName(stem) ? stem : std::string("Svg"));
            json names = json::array();
            std::vector<NodeId> made;
            if (a.get<bool>("merge", false))
            {
                Splines all;
                for (SvgShape& sh : doc.shapes)
                    all.insert(all.end(), sh.curves.begin(), sh.curves.end());
                Node* c = g.createNode(*types::find("curve"), base, kNoNode, true);
                g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(all)));
                names.push_back(c->path());
                made.push_back(c->id());
            }
            else
            {
                Node* group = g.createNode(*types::find("transform"), base, kNoNode, true);
                for (SvgShape& sh : doc.shapes)
                {
                    Node* c = g.createNode(*types::find("curve"), Graph::validName(sh.id) ? sh.id : std::string("Path"), group->id(), true);
                    g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(sh.curves)));
                    names.push_back(c->path());
                    made.push_back(c->id());
                }
            }
            g.select(made);
            return names;
        },
    });

    ops::registerCommand({
        .name = "exportSvg",
        .doc = "Write curves to an SVG file, one path per curve, seen on a plane (xy front, xz ground, yz side) at a scale. Beziers "
               "go out exactly, other curves as Beziers within tolerance. Defaults to the selected curves.",
        .positionalDoc = "path curves...",
        .flags = {{"scale", "s", T::Float, 0.1, "cm per SVG unit."},
                  {"plane", "", T::String, "xy", "Plane to draw on: xy (front), xz (ground) or yz (side).", {"xy", "xz", "yz"}},
                  {"tolerance", "", T::Float, 0.01, "cm, for curves that are not Beziers."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("exportSvg: path required");
            std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            g.evaluate();
            std::vector<Node*> curves;
            for (size_t k = 1; k < a.positional.size(); ++k)
                curves.push_back(&curveNamed(g, a.positional[k].get<std::string>(), "exportSvg"));
            if (curves.empty())
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("curve"))
                        curves.push_back(n);
            if (curves.empty())
                throw CommandError("exportSvg: select curves to export");
            SvgExportOptions o;
            o.scale = a.get<double>("scale", 0.1);
            o.tolerance = std::max(1e-5, a.get<double>("tolerance", 0.01));
            if (!(o.scale > 0.0))
                throw CommandError("exportSvg: scale must be positive");
            if (!parseSvgPlane(a.get<std::string>("plane", "xy"), &o.plane))
                throw CommandError("exportSvg: plane is xy, xz or yz");
            std::vector<Splines> groups;
            for (Node* c : curves)
                groups.push_back(splinesOf(g.value({c->id(), CurveShape::kWorldSpace, -1})));
            std::ofstream out(paths::fromUtf8(path), std::ios::binary);
            if (!out)
                throw CommandError("exportSvg: cannot write " + path);
            out << writeSvg(groups, o);
            return path;
        },
    });

    if (!io::translators::find("svg"))
        io::translators::add({.id = "svg",
                              .label = "SVG",
                              .doc = "Scalable Vector Graphics: paths and shapes in as Bezier curves, curves out as paths.",
                              .extensions = {".svg"},
                              .import = {.command = "importSvg"},
                              .exportSelection = {.command = "exportSvg"}});
}

void registerCommandsHere()
{
    ops::registerCommand({
        .name = "textCurves",
        .doc = "Make text as curves (Maya's textCurves): closed Bezier outlines of each glyph, exact, in the XY plane with the first "
               "baseline on the origin. The curveText node keeps the text, font and layout live, so editing it updates anything built "
               "on the curve (a bevelPlus, say). Returns [curve, curveText].",
        .flags = withFlags(attrFlags("curveText"), {chFlag(), {"name", "n", T::String, nullptr, "Curve name; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return curveResult(g, "curveText", a, {}, "Text");
        },
        .adjustable = true,
    });
    layout("textCurves", "curveText", {"text", "font", "size", "alignment", "anchor", "wrapWidth"},
           {{"size", {0.0, 1e5, 0.5, " cm"}}, {"letterSpacing", {-1e4, 1e4, 0.1, " cm"}}, {"wordSpacing", {-1e4, 1e4, 0.1, " cm"}},
            {"wrapWidth", {0.0, 1e5, 1.0, " cm"}}, {"weight", {-1e4, 1e4, 0.01, " cm"}}, {"slant", {-60.0, 60.0, 1.0, " deg"}}});
    if (const ops::options::Override* ov = ops::options::findOverride("textCurves"))
    {
        ops::options::Override o = *ov;
        o.editors = {{"text", "multiline"}, {"font", "font"}};
        ops::options::setOverride("textCurves", std::move(o));
    }

    ops::registerCommand({
        .name = "fontList",
        .doc = "The fonts textCurves can use by name: \"Family Style\" of every bundled and installed font, sorted.",
        .undoable = false,
        .run = [](const Args&) -> json { return fontNames(); },
    });

    ops::registerCommand({
        .name = "curveBoolean",
        .doc = "2D boolean of closed planar curves into a new curve with live history (C4D's Spline Mask, Rhino's CurveBoolean): union, "
               "subtract (the rest from the first), intersect or xor. Each curve's loops fill even-odd, so a letter's counter stays "
               "a hole. Pieces are cut exactly from the inputs, with sharp corners where they cross. Returns [curve, curveBoolean].",
        .positionalDoc = "target tools...",
        .flags = withFlags(attrFlags("curveBoolean"), {chFlag(),
                                                       {"hideInputs", "", T::Bool, false, "Hide the input curves (they still drive the result)."},
                                                       {"name", "n", T::String, nullptr, "Name of the result curve; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves = curvesArg(g, a, "curveBoolean");
            if (curves.size() < 2 && a.get<std::string>("operation", "union") != "union")
                throw CommandError("curveBoolean: select the target and at least one more curve");
            return curveResult(g, "curveBoolean", a, curves, "Boolean");
        },
        .adjustable = true,
    });
    layout("curveBoolean", "curveBoolean", {"operation", "hideInputs", "tolerance"}, {{"tolerance", {1e-5, 1.0, 0.001, " cm"}}});

    ops::registerCommand({
        .name = "offsetRegion",
        .doc = "Offset the region closed planar curves bound (holes even-odd) into new curves with live history: loops that run "
               "into each other merge, parts that vanish drop, holes shrink as the outline grows. Returns [curve, offsetRegion].",
        .positionalDoc = "curves",
        .flags = withFlags(attrFlags("offsetRegion"), {chFlag(), {"name", "n", T::String, nullptr, "Name of the result curve; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return curveResult(g, "offsetRegion", a, curvesArg(g, a, "offsetRegion"), "Offset");
        },
        .adjustable = true,
    });
    layout("offsetRegion", "offsetRegion", {"distance", "join", "miterLimit"}, {{"distance", {-1e5, 1e5, 0.1, " cm"}}});

    ops::registerCommand({
        .name = "bevelPlus",
        .doc = "Bevel Plus (Maya's): closed planar curves into a solid mesh with live history. Loops inside others are holes; "
               "front and back bevels follow a profile (straight, convex, concave, step or a custom curve) and mitre along the "
               "straight skeleton, meeting in a ridge where the shape is thinner than the bevel. Quad caps, hard edges by angle, "
               "UVs (caps planar, sides along the outline). Returns [mesh, bevelPlus].",
        .positionalDoc = "curves",
        .flags = withFlags(attrFlags("bevelPlus"), {{"profileCurve", "pc", T::String, nullptr, "Curve to use as the custom profile (sets both profiles to custom)."},
                                                   chFlag(),
                                                   {"name", "n", T::String, nullptr, "Mesh name; made unique."}}),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves = curvesArg(g, a, "bevelPlus");
            Node* prof = a.has("profileCurve") ? &curveNamed(g, a.get<std::string>("profileCurve"), "bevelPlus") : nullptr;
            if (prof)
                std::erase(curves, prof);
            Node* op = g.createNode(*types::find("bevelPlus"), {}, kNoNode, true);
            applyAttrFlags(g, *op, a, "bevelPlus");
            connectInputs(g, *op, curves);
            if (prof)
            {
                g.connect({prof->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("profileCurve"), -1});
                if (!a.has("frontProfile"))
                    g.setValue({op->id(), op->type().attrIndex("frontProfile"), -1}, int64_t(BevelProfile::Custom));
                if (!a.has("backProfile"))
                    g.setValue({op->id(), op->type().attrIndex("backProfile"), -1}, int64_t(BevelProfile::Custom));
            }
            return meshResult(g, *op, a, "BevelPlus");
        },
        .adjustable = true,
    });
    layout("bevelPlus", "bevelPlus",
           {"extrudeDepth", "extrudeDivisions", "frontBevel", "frontWidth", "frontDepth", "frontProfile", "frontSegments", "customProfile", "backBevel",
            "backWidth", "backDepth", "backProfile", "backSegments"},
           {{"extrudeDepth", {0.0, 1e5, 0.1, " cm"}},
            {"frontWidth", {0.0, 1e4, 0.05, " cm"}},
            {"frontDepth", {0.0, 1e4, 0.05, " cm"}},
            {"backWidth", {0.0, 1e4, 0.05, " cm"}},
            {"backDepth", {0.0, 1e4, 0.05, " cm"}},
            {"capSpacing", {0.0, 1e4, 0.1, " cm"}},
            {"smoothingAngle", {0.0, 180.0, 1.0, " deg"}},
            {"maxAngle", {0.5, 45.0, 0.5, " deg"}}},
           {"profileCurve"});
}
} // namespace

void registerPlanarTypes() { registerTypesHere(); }

void registerPlanarCommands()
{
    registerCommandsHere();
    registerSkeletonCommands();
    registerInsetRegionCommand();
    registerSvgCommands();
    registerDxfCommands();
}
} // namespace lila::curve
