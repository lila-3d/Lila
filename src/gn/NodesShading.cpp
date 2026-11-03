#include "gn/NodesCommon.h"

#include "geo/Normals.h"

namespace lila::gn::nodes
{
namespace
{
using D4 = glm::dvec4;

D3 hsvToRgb(D3 hsv)
{
    double h = hsv.x, s = hsv.y, v = hsv.z;
    if (s == 0.0)
        return D3(v);
    h = (h - std::floor(h)) * 6.0;
    int i = int(std::floor(h));
    double f = h - double(i);
    double p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    switch (i % 6)
    {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

D3 rgbToHsv(D3 rgb)
{
    double mx = std::max({rgb.x, rgb.y, rgb.z}), mn = std::min({rgb.x, rgb.y, rgb.z}), d = mx - mn;
    double h = 0.0, s = mx != 0.0 ? d / mx : 0.0;
    if (d != 0.0)
    {
        if (mx == rgb.x)
            h = (rgb.y - rgb.z) / d;
        else if (mx == rgb.y)
            h = 2.0 + (rgb.z - rgb.x) / d;
        else
            h = 4.0 + (rgb.x - rgb.y) / d;
        h /= 6.0;
        if (h < 0.0)
            h += 1.0;
    }
    return {h, s, mx};
}

D3 hslToRgb(D3 hsl)
{
    double h = hsl.x, s = hsl.y, l = hsl.z;
    double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
    D3 hue = hsvToRgb({h, 1.0, 1.0});
    return (hue - 0.5) * c + l;
}

D3 rgbToHsl(D3 rgb)
{
    double mx = std::max({rgb.x, rgb.y, rgb.z}), mn = std::min({rgb.x, rgb.y, rgb.z});
    double l = (mx + mn) / 2.0, d = mx - mn;
    double s = d == 0.0 ? 0.0 : d / (1.0 - std::abs(2.0 * l - 1.0));
    return {rgbToHsv(rgb).x, s, l};
}

std::vector<std::string> channelNames(const std::string& mode)
{
    if (mode == "hsv")
        return {"hue", "saturation", "value"};
    if (mode == "hsl")
        return {"hue", "saturation", "lightness"};
    return {"red", "green", "blue"};
}

void combineSeparate()
{
    registerDecl({
        .type = "CombineColor",
        .label = "Combine Color",
        .category = "Color",
        .doc = "A colour from RGB, HSV or HSL channels and alpha (all 0..1, linear).",
        .props = {prop("mode", {"rgb", "hsv", "hsl"}, "rgb")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                for (const std::string& ch : channelNames(findDecl("CombineColor")->propValue(n, "mode")))
                    s.inputs.push_back(fin(ch, T::Float, 0.0));
                s.inputs.push_back(bounded(fin("alpha", T::Float, 1.0), 0.0, 1.0));
                s.outputs = {fout("color", T::Color)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::string mode = c.prop("mode");
                std::vector<std::string> ch = channelNames(mode);
                c.out("color", map<D4, double, double, double, double>(
                                   T::Color,
                                   [mode](double a, double b, double cc, double alpha) {
                                       D3 v(a, b, cc);
                                       D3 rgb = mode == "hsv" ? hsvToRgb(v) : mode == "hsl" ? hslToRgb(v) : v;
                                       return D4(rgb, alpha);
                                   },
                                   {c.field(ch[0]), c.field(ch[1]), c.field(ch[2]), c.field("alpha")}));
            },
    });
    registerDecl({
        .type = "SeparateColor",
        .label = "Separate Color",
        .category = "Color",
        .doc = "A colour's RGB, HSV or HSL channels and alpha.",
        .props = {prop("mode", {"rgb", "hsv", "hsl"}, "rgb")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                s.inputs = {fin("color", T::Color, json::array({1, 1, 1, 1}))};
                for (const std::string& ch : channelNames(findDecl("SeparateColor")->propValue(n, "mode")))
                    s.outputs.push_back(fout(ch, T::Float));
                s.outputs.push_back(fout("alpha", T::Float));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::string mode = c.prop("mode");
                std::vector<std::string> ch = channelNames(mode);
                FieldPtr col = c.field("color");
                for (int k = 0; k < 4; ++k)
                    c.out(k < 3 ? ch[k] : std::string("alpha"), map<double, D4>(
                                                                    T::Float,
                                                                    [mode, k](D4 v) {
                                                                        if (k == 3)
                                                                            return v.w;
                                                                        D3 rgb(v);
                                                                        D3 x = mode == "hsv" ? rgbToHsv(rgb) : mode == "hsl" ? rgbToHsl(rgb) : rgb;
                                                                        return x[k];
                                                                    },
                                                                    {col}));
            },
    });
}

/** Writes per-corner custom normals and lets hard edges follow them, so exporters and the viewport agree. */
void writeCustomNormals(geo::Mesh& m, const std::vector<D3>& corner)
{
    auto dst = m.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
    for (Index c = 0; c < m.cornerCapacity() && c < dst.size(); ++c)
    {
        D3 n = c < corner.size() ? corner[c] : D3(0.0);
        double l = glm::length(n);
        dst[c] = l > 1e-12 ? glm::vec3(n / l) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    m.edgeAttrs().remove(geo::attr::kHard);
}

void setMeshNormal()
{
    registerDecl({
        .type = "SetMeshNormal",
        .label = "Set Mesh Normal",
        .category = "Mesh",
        .doc = "Custom normals (free: any direction per point, face or corner), or smoothing from sharp edges and faces, or smoothing by "
               "angle. Custom normals are the corner normal layer that FBX exports and the viewport shades with.",
        .props = {prop("mode", {"free", "sharpness", "smoothByAngle"}, "free"), prop("domain", {"point", "face", "corner"}, "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                std::string mode = findDecl("SetMeshNormal")->propValue(n, "mode");
                Sockets s{{geoIn("mesh")}, {geoOut("mesh")}};
                if (mode == "sharpness")
                {
                    s.inputs.push_back(fin("edgeSharpness", T::Bool, false, Implicit::None, "Edges to keep hard."));
                    s.inputs.push_back(fin("faceSharpness", T::Bool, false, Implicit::None, "Faces to shade flat."));
                }
                else if (mode == "smoothByAngle")
                    s.inputs.push_back(bounded(fin("angle", T::Float, 30.0, Implicit::None, "Degrees; edges sharper than this stay hard."), 0.0, 180.0));
                else
                    s.inputs.push_back(fin("customNormal", T::Vector, nullptr, Implicit::Normal, "The normal; normalized. Defaults to the current normal."));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::string mode = c.prop("mode");
                Domain d = domainOf(c.prop("domain"));
                FieldPtr customNormal = c.sockets.inputIndex("customNormal") >= 0 ? c.field("customNormal") : nullptr;
                ComponentEdits e;
                e.mesh = [&](geo::Mesh& m) {
                    if (mode == "free")
                    {
                        Column col = evaluate(customNormal, contextFor(m, d), T::Vector);
                        Column corners = adaptDomain(m, col, d, Domain::Corner);
                        writeCustomNormals(m, expand<D3>(corners, m.cornerCapacity()));
                        return;
                    }
                    m.cornerAttrs().remove(geo::attr::kNormal);
                    if (mode == "smoothByAngle")
                    {
                        geo::setSoftEdges(m);
                        geo::setHardEdgesByAngle(m, c.f("angle"));
                        return;
                    }
                    FieldContext ectx = FieldContext::forMesh(m, Domain::Edge), fctx = FieldContext::forMesh(m, Domain::Face);
                    std::vector<uint8_t> sharpE = expand<uint8_t>(evaluate(c.field("edgeSharpness"), ectx, T::Bool), ectx.size);
                    std::vector<uint8_t> sharpF = expand<uint8_t>(evaluate(c.field("faceSharpness"), fctx, T::Bool), fctx.size);
                    m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
                    auto hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
                    for (Index ed = 0; ed < m.edgeCapacity() && ed < hard.size(); ++ed)
                        hard[ed] = sharpE[ed] != 0;
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f) && sharpF[f])
                            m.forEachFaceCorner(f, [&](Index cc) { hard[m.cornerEdge(cc)] = true; });
                };
                c.out("mesh", editComponents(c.geometry("mesh"), e));
            },
    });
}
} // namespace

void registerShadingNodes()
{
    combineSeparate();
    setMeshNormal();
}
} // namespace lila::gn::nodes
