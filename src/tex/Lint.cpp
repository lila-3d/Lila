#include "tex/Lint.h"

#include "core/Profile.h"
#include "tex/Evaluate.h"
#include "tex/NodeDef.h"
#include "tex/Plan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <sstream>
#include <stdexcept>

namespace lila::tex::lint
{
namespace
{
using Rgb = std::array<double, 3>;

constexpr double kDielectricLow = 30.0 / 255.0;
constexpr double kDielectricHigh = 240.0 / 255.0;
constexpr double kMetalWarn = 150.0 / 255.0;
constexpr double kMetalFail = 100.0 / 255.0;
constexpr double kRoughFloor = 0.05;

/** Measured reflectance (F0) of common metals, linear. Matched against the meta's name, tags and category, first wins. */
const std::vector<std::pair<const char*, Rgb>>& metalTable()
{
    static const std::vector<std::pair<const char*, Rgb>> t = {
        {"galvanised", {0.664, 0.824, 0.850}}, {"galvanized", {0.664, 0.824, 0.850}}, {"zinc", {0.664, 0.824, 0.850}},
        {"gold", {1.000, 0.766, 0.336}},       {"silver", {0.972, 0.960, 0.915}},     {"aluminium", {0.913, 0.922, 0.924}},
        {"aluminum", {0.913, 0.922, 0.924}},   {"copper", {0.955, 0.638, 0.538}},     {"brass", {0.910, 0.778, 0.423}},
        {"bronze", {0.820, 0.580, 0.370}},     {"chrome", {0.550, 0.556, 0.554}},     {"nickel", {0.660, 0.609, 0.526}},
        {"titanium", {0.542, 0.497, 0.449}},   {"cobalt", {0.662, 0.655, 0.634}},     {"platinum", {0.673, 0.637, 0.585}},
        {"steel", {0.560, 0.570, 0.580}},      {"iron", {0.560, 0.570, 0.580}},
    };
    return t;
}

const char* kChannels[] = {"baseColor", "roughness", "metallic", "normal", "height"};

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string haystack(const library::Meta& m)
{
    std::string h = lower(m.name + " " + m.category + " " + m.description);
    for (const std::string& t : m.tags)
        h += " " + lower(t);
    return h;
}

bool mentions(const library::Meta& m, std::initializer_list<const char*> words)
{
    std::string h = haystack(m);
    return std::any_of(words.begin(), words.end(), [&](const char* w) { return h.find(w) != std::string::npos; });
}

/** Whether one of words is a whole word of the meta's name, category or tags. */
bool namedAs(const library::Meta& m, std::initializer_list<const char*> words)
{
    std::string h = lower(m.name + " " + m.category);
    for (const std::string& t : m.tags)
        h += " " + lower(t);
    for (char& c : h)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = ' ';
    std::istringstream in(h);
    for (std::string w; in >> w;)
        for (const char* x : words)
            if (w == x)
                return true;
    return false;
}

struct Img
{
    const image::Image* img = nullptr;
    int w = 0, h = 0;

    explicit operator bool() const { return img && w > 0 && h > 0; }
    const float* at(int x, int y) const
    {
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
        return &img->rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
    }
    size_t count() const { return size_t(w) * size_t(h); }
};

double luminance(const float* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

Check albedo(const Img& base, const Img& metal, const library::Meta& m)
{
    Check c{.id = "albedo"};
    if (!base)
        return {.id = "albedo", .status = "skip", .message = "no base colour channel"};
    const bool relaxed = m.physical == "stylised" || m.physical == "emissive";
    // Fresh snow reflects about 90%, and clear glass and ice take their look from transmission, not base colour.
    const bool bright = namedAs(m, {"snow", "glass", "ice"});
    size_t n = 0, dark = 0, light = 0;
    double lo = 1.0, hi = 0.0;
    for (int y = 0; y < base.h; ++y)
        for (int x = 0; x < base.w; ++x)
        {
            if (metal && metal.at(x, y)[0] > 0.5f)
                continue;
            const float* p = base.at(x, y);
            double l = toSrgb(luminance(p));
            double mx = toSrgb(std::max({p[0], p[1], p[2]}));
            lo = std::min(lo, l);
            hi = std::max(hi, mx);
            dark += l < kDielectricLow - 1e-4;
            light += !bright && mx > kDielectricHigh + 1e-4;
            ++n;
        }
    if (n == 0)
        return {.id = "albedo", .status = "skip", .message = "every texel is metal"};
    double fd = double(dark) / double(n), fb = double(light) / double(n);
    c.data = {{"darkFraction", fd}, {"brightFraction", fb}, {"minSrgb", std::lround(lo * 255.0)}, {"maxSrgb", std::lround(hi * 255.0)}};
    double worst = std::max(fd, fb);
    if (worst <= 0.02)
    {
        c.message = std::format("non-metal base colour {} to {} sRGB", std::lround(lo * 255.0), std::lround(hi * 255.0));
        return c;
    }
    std::string what = std::format("{:.0f}% of non-metal base colour is darker than 30 sRGB and {:.0f}% brighter than 240", fd * 100.0, fb * 100.0);
    if (relaxed)
    {
        c.message = what + " (allowed: " + m.physical + ")";
        return c;
    }
    c.status = worst > 0.10 ? "fail" : "warn";
    c.message = what;
    return c;
}

Check blackWhite(const Img& base, const library::Meta& m)
{
    if (!base)
        return {.id = "blackWhite", .status = "skip", .message = "no base colour channel"};
    size_t black = 0, white = 0;
    for (int y = 0; y < base.h; ++y)
        for (int x = 0; x < base.w; ++x)
        {
            const float* p = base.at(x, y);
            double mx = toSrgb(std::max({p[0], p[1], p[2]})), mn = toSrgb(std::min({p[0], p[1], p[2]}));
            black += mx <= 2.0 / 255.0;
            white += mn >= 253.0 / 255.0;
        }
    double fb = double(black) / double(base.count()), fw = double(white) / double(base.count());
    Check c{.id = "blackWhite", .data = {{"blackFraction", fb}, {"whiteFraction", fw}}};
    if (std::max(fb, fw) <= 0.005)
        return c;
    c.message = std::format("{:.1f}% pure black and {:.1f}% pure white base colour", fb * 100.0, fw * 100.0);
    if (m.physical == "emissive" && fw <= 0.005)
        c.message += " (allowed: emissive)";
    else
        c.status = m.physical == "stylised" ? "warn" : "fail";
    return c;
}

Check metalColour(const Img& base, const Img& metal, const library::Meta& m)
{
    if (!base || !metal)
        return {.id = "metal", .status = "skip", .message = "no base colour or metallic channel"};
    size_t n = 0, dim = 0, veryDim = 0;
    Rgb sum{0.0, 0.0, 0.0};
    for (int y = 0; y < base.h; ++y)
        for (int x = 0; x < base.w; ++x)
        {
            if (metal.at(x, y)[0] <= 0.5f)
                continue;
            const float* p = base.at(x, y);
            double l = toSrgb(luminance(p));
            dim += l < kMetalWarn;
            veryDim += l < kMetalFail;
            for (int k = 0; k < 3; ++k)
                sum[k] += p[k];
            ++n;
        }
    if (n < base.count() / 50)
        return {.id = "metal", .status = "skip", .message = "little or no metal"};
    Check c{.id = "metal"};
    Rgb mean{sum[0] / double(n), sum[1] / double(n), sum[2] / double(n)};
    Rgb meanSrgb{toSrgb(mean[0]) * 255.0, toSrgb(mean[1]) * 255.0, toSrgb(mean[2]) * 255.0};
    c.data = {{"meanSrgb", {std::lround(meanSrgb[0]), std::lround(meanSrgb[1]), std::lround(meanSrgb[2])}},
              {"dimFraction", double(dim) / double(n)}};
    if (m.physical == "stylised")
    {
        c.message = "stylised: metal reflectance not checked";
        return c;
    }
    double fdim = double(dim) / double(n), fvery = double(veryDim) / double(n);
    if (fvery > 0.10)
    {
        c.status = "fail";
        c.message = std::format("{:.0f}% of metal texels are darker than 100 sRGB; metals reflect at least about 50%", fvery * 100.0);
        return c;
    }
    if (fdim > 0.10)
    {
        c.status = "warn";
        c.message = std::format("{:.0f}% of metal texels are darker than 150 sRGB", fdim * 100.0);
    }
    std::string h = haystack(m);
    for (const auto& [name, f0] : metalTable())
    {
        if (h.find(name) == std::string::npos)
            continue;
        Rgb ref{toSrgb(f0[0]) * 255.0, toSrgb(f0[1]) * 255.0, toSrgb(f0[2]) * 255.0};
        double d = (std::abs(ref[0] - meanSrgb[0]) + std::abs(ref[1] - meanSrgb[1]) + std::abs(ref[2] - meanSrgb[2])) / 3.0;
        c.data["metal"] = name;
        c.data["referenceSrgb"] = {std::lround(ref[0]), std::lround(ref[1]), std::lround(ref[2])};
        if (d > 40.0 && c.status == "pass")
        {
            c.status = "warn";
            c.message = std::format("metal averages ({:.0f}, {:.0f}, {:.0f}) sRGB, {:.0f} away from {} ({:.0f}, {:.0f}, {:.0f})", meanSrgb[0], meanSrgb[1],
                                    meanSrgb[2], d, name, ref[0], ref[1], ref[2]);
        }
        else if (c.message.empty())
            c.message = std::format("metal within {:.0f} of {}", d, name);
        break;
    }
    return c;
}

Check metallicBinary(const Img& metal)
{
    if (!metal)
        return {.id = "metallic", .status = "skip", .message = "no metallic channel"};
    size_t mid = 0;
    double lo = 1.0, hi = 0.0;
    for (int y = 0; y < metal.h; ++y)
        for (int x = 0; x < metal.w; ++x)
        {
            double v = metal.at(x, y)[0];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            if (v <= 0.1 || v >= 0.9)
                continue;
            // A mid value next to a metal edge is the transition between metal and non-metal, which is fine.
            double nlo = 1.0, nhi = 0.0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    double nv = metal.at(x + i, y + j)[0];
                    nlo = std::min(nlo, nv);
                    nhi = std::max(nhi, nv);
                }
            mid += nhi - nlo < 0.3;
        }
    double f = double(mid) / double(metal.count());
    Check c{.id = "metallic", .data = {{"midFraction", f}, {"min", lo}, {"max", hi}}};
    if (f <= 0.01)
        return c;
    c.status = f > 0.05 ? "fail" : "warn";
    c.message = std::format("{:.0f}% of metallic is neither 0 nor 1 away from metal edges; use 0 or 1 and a mask", f * 100.0);
    return c;
}

Check roughness(const Img& r, const library::Meta& m)
{
    if (!r)
        return {.id = "roughness", .status = "skip", .message = "no roughness channel"};
    size_t out = 0, mirror = 0;
    double lo = 1e9, hi = -1e9;
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
        {
            double v = r.at(x, y)[0];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            out += v < -1e-3 || v > 1.0 + 1e-3;
            mirror += v < 0.02;
        }
    double fo = double(out) / double(r.count()), fm = double(mirror) / double(r.count());
    Check c{.id = "roughness", .data = {{"min", lo}, {"max", hi}, {"mirrorFraction", fm}}};
    if (fo > 0.0)
    {
        c.status = "fail";
        c.message = std::format("roughness runs {:.2f} to {:.2f}, outside 0 to 1", lo, hi);
        return c;
    }
    bool glossy = mentions(m, {"glass", "chrome", "mirror", "polish", "water", "ice", "gloss", "lacquer", "varnish", "wet", "clear coat", "clearcoat"});
    if (fm > 0.25 && !glossy)
    {
        c.status = "warn";
        c.message = std::format("{:.0f}% of roughness is below 0.02, a perfect mirror; real surfaces rarely go under 0.05", fm * 100.0);
        return c;
    }
    c.message = std::format("roughness {:.2f} to {:.2f}", lo, hi);
    return c;
}

/** Pearson correlation of two series. */
double correlation(const std::vector<double>& a, const std::vector<double>& b)
{
    size_t n = a.size();
    if (n < 2)
        return 0.0;
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= double(n);
    mb /= double(n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return saa > 1e-12 && sbb > 1e-12 ? sab / std::sqrt(saa * sbb) : 0.0;
}

Check normal(const Img& nrm, const Img& height)
{
    if (!nrm)
        return {.id = "normal", .status = "skip", .message = "no normal channel"};
    size_t bad = 0, inward = 0;
    for (int y = 0; y < nrm.h; ++y)
        for (int x = 0; x < nrm.w; ++x)
        {
            const float* p = nrm.at(x, y);
            double nx = p[0] * 2.0 - 1.0, ny = p[1] * 2.0 - 1.0, nz = p[2] * 2.0 - 1.0;
            double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            bad += std::abs(len - 1.0) > 0.1;
            inward += nz < -1e-3;
        }
    double fb = double(bad) / double(nrm.count()), fi = double(inward) / double(nrm.count());
    Check c{.id = "normal", .data = {{"nonUnitFraction", fb}, {"inwardFraction", fi}}};
    // Convention: with height rising toward +v, an OpenGL normal leans toward -v (green below mid), so its green
    // correlates with -dh/dv. Rows run top first, so +v is the row above.
    if (height)
    {
        std::vector<double> gx, gy, nx, ny;
        for (int y = 0; y < nrm.h; ++y)
            for (int x = 0; x < nrm.w; ++x)
            {
                double ddu = (height.at(x + 1, y)[0] - height.at(x - 1, y)[0]) * 0.5;
                double ddv = (height.at(x, y - 1)[0] - height.at(x, y + 1)[0]) * 0.5;
                gx.push_back(-ddu);
                gy.push_back(-ddv);
                nx.push_back(nrm.at(x, y)[0]);
                ny.push_back(nrm.at(x, y)[1]);
            }
        double cx = correlation(gx, nx), cy = correlation(gy, ny);
        c.data["heightCorrelation"] = {cx, cy};
        if (cx > 0.3 && cy < -0.3)
        {
            c.status = "warn";
            c.message = "green runs opposite to the height: a DirectX (green down) normal; graphs make OpenGL normals and exports flip them";
        }
    }
    if (fb > 0.01 || fi > 0.01)
    {
        c.status = fb > 0.10 || fi > 0.01 ? "fail" : "warn";
        c.message = std::format("{:.0f}% of normals aren't unit length and {:.0f}% point into the surface", fb * 100.0, fi * 100.0);
    }
    return c;
}

Check heightRange(const Img& h)
{
    if (!h)
        return {.id = "height", .status = "skip", .message = "no height channel"};
    size_t out = 0;
    double lo = 1e9, hi = -1e9;
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x)
        {
            double v = h.at(x, y)[0];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            out += v < -1e-3 || v > 1.0 + 1e-3;
        }
    double f = double(out) / double(h.count());
    Check c{.id = "height", .data = {{"min", lo}, {"max", hi}}};
    if (f > 0.0)
    {
        c.status = f > 0.01 ? "fail" : "warn";
        c.message = std::format("height runs {:.2f} to {:.2f}, outside 0 to 1", lo, hi);
    }
    else
        c.message = hi - lo < 0.01 ? "height is flat" : std::format("height {:.2f} to {:.2f}", lo, hi);
    return c;
}

/**
 * How much a channel jumps across the texture's wrap compared with the sharpest lines inside it: per line of texels, the
 * mean step to the next one; the wrap's step against the 98th percentile of the others. Patterns with hard lines (bricks,
 * panels) have many sharp steps inside, so a line that falls on the edge isn't mistaken for a seam.
 */
std::pair<double, double> seamRatio(const Img& im, bool horizontal)
{
    int along = horizontal ? im.h : im.w, across = horizontal ? im.w : im.h;
    std::vector<double> steps(size_t(across), 0.0);
    for (int a = 0; a < along; ++a)
        for (int i = 0; i < across; ++i)
        {
            const float* p = horizontal ? im.at(i, a) : im.at(a, i);
            const float* q = horizontal ? im.at(i + 1, a) : im.at(a, i + 1);
            steps[size_t(i)] += std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]);
        }
    for (double& d : steps)
        d /= double(std::max(along, 1));
    const double edge = steps.back();
    steps.pop_back();
    if (steps.empty())
        return {edge, 0.0};
    std::sort(steps.begin(), steps.end());
    const double sharp = steps[std::min(steps.size() - 1, size_t(double(steps.size()) * 0.98))];
    return {edge, edge / (sharp + 1e-3)};
}

Check seams(const std::map<std::string, Img>& imgs, const library::Meta& m)
{
    if (!m.tileable)
        return {.id = "seams", .status = "skip", .message = "not tileable"};
    Check c{.id = "seams"};
    double worst = 0.0;
    for (const auto& [name, im] : imgs)
    {
        if (!im)
            continue;
        for (bool horizontal : {true, false})
        {
            auto [edge, ratio] = seamRatio(im, horizontal);
            std::string status = ratio > 5.0 && edge > 0.05 ? "fail" : ratio > 2.5 && edge > 0.02 ? "warn" : "pass";
            if (status != "pass")
            {
                c.data["channels"][name + (horizontal ? ".u" : ".v")] = {{"edge", edge}, {"ratio", ratio}};
                if (status == "fail" || c.status == "pass")
                    c.status = status;
            }
            worst = std::max(worst, ratio);
        }
    }
    c.data["worstRatio"] = worst;
    if (c.status != "pass")
    {
        std::string which;
        for (auto& [k, v] : c.data["channels"].items())
            which += (which.empty() ? "" : ", ") + k;
        c.message = "visible tile seams in " + which + "; the graph isn't periodic (Make Tileable, or tile the noise)";
    }
    return c;
}

const GNode* outputNode(const Graph& g) { return g.firstOfType(kMaterialOutputType); }

/** Where a channel's value enters the material: (node, socket) whose link or value to change; empty node when it can't be reached. */
std::pair<std::string, std::string> channelTarget(const Graph& g, const std::string& channel)
{
    const GNode* out = outputNode(g);
    if (!out)
        return {};
    if (!g.linksInto(out->name, channel).empty())
        return {out->name, channel};
    std::vector<GLink> m = g.linksInto(out->name, "material");
    if (m.empty())
        return {out->name, channel};
    std::string node = m.front().fromNode;
    for (int guard = 0; guard < 16; ++guard)
    {
        const GNode* n = g.node(node);
        if (!n)
            return {};
        if (n->type == "material")
            return {n->name, channel};
        if (n->type != library::kTilingType)
            return {};
        std::vector<GLink> in = g.linksInto(n->name, "input");
        if (in.empty())
            return {};
        node = in.front().fromNode;
    }
    return {};
}

json defaultOf(const Graph& g, const std::string& node, const std::string& socket)
{
    const GNode* n = g.node(node);
    if (!n)
        return nullptr;
    if (auto it = n->values.find(socket); it != n->values.end())
        return *it;
    const NodeDef* d = findNode(n->type);
    const PortDecl* p = d ? d->input(socket) : nullptr;
    return p ? p->defaultValue : json();
}

/** Remaps a value as Levels would: (v - inLow) / (inHigh - inLow), saturated, to outLow..outHigh. */
json levelsValue(const json& v, const json& levels)
{
    double il = levels.value("inLow", 0.0), ih = levels.value("inHigh", 1.0), ol = levels.value("outLow", 0.0), oh = levels.value("outHigh", 1.0);
    auto f = [&](double x) { return ol + (oh - ol) * std::clamp((x - il) / std::max(ih - il, 1e-5), 0.0, 1.0); };
    if (v.is_number())
        return f(v.get<double>());
    if (v.is_array())
    {
        json out = v;
        for (size_t i = 0; i < std::min<size_t>(3, out.size()); ++i)
            if (out[i].is_number())
                out[i] = f(out[i].get<double>());
        return out;
    }
    return v;
}

std::string fixChannel(Graph& g, const std::string& channel, const json& levels)
{
    auto [node, socket] = channelTarget(g, channel);
    if (node.empty())
        return "can't fix " + channel + ": it comes through a material blend";
    const std::string name = "LintFix_" + channel;
    std::vector<GLink> in = g.linksInto(node, socket);
    if (in.empty())
    {
        json v = levelsValue(defaultOf(g, node, socket), levels);
        g.node(node)->values[socket] = v;
        return "clamped " + node + "." + socket + " to " + v.dump();
    }
    if (GNode* existing = g.node(name); existing && in.front().fromNode == name)
    {
        for (auto& [k, v] : levels.items())
            existing->values[k] = v;
        return "updated " + name;
    }
    const GLink src = in.front();
    std::erase_if(g.links, [&](const GLink& l) { return l.toNode == node && l.toSocket == socket; });
    GNode lv{g.uniqueNodeName(name), "levels", g.node(node)->position + glm::dvec2(-180.0, 60.0), levels};
    g.links.push_back({src.fromNode, src.fromSocket, lv.name, "input"});
    g.links.push_back({lv.name, "out", node, socket});
    std::string made = lv.name;
    g.nodes.push_back(std::move(lv));
    return "added " + made + " on " + node + "." + socket;
}
} // namespace

double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double toLinear(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

bool Report::passed() const { return count("fail") == 0; }

int Report::count(const std::string& status) const
{
    return int(std::count_if(checks.begin(), checks.end(), [&](const Check& c) { return c.status == status; }));
}

const Check* Report::check(const std::string& id) const
{
    for (const Check& c : checks)
        if (c.id == id)
            return &c;
    return nullptr;
}

json Report::toJson() const
{
    json cs = json::array();
    for (const Check& c : checks)
    {
        json j = {{"id", c.id}, {"status", c.status}, {"message", c.message}};
        if (!c.data.empty())
            j["data"] = c.data;
        cs.push_back(j);
    }
    return {{"passed", passed()}, {"failures", count("fail")}, {"warnings", count("warn")}, {"checks", cs}, {"backend", backend}};
}

Report run(const Graph& g, const SetSettings& s, const library::Meta& m, const Options& o)
{
    LILA_ZONE();
    std::vector<OutputRequest> requests;
    for (const OutputRequest& r : channelRequests(g, s))
        if (std::find(std::begin(kChannels), std::end(kChannels), r.name) != std::end(kChannels))
            requests.push_back(r);
    if (requests.empty())
        throw std::invalid_argument(g.firstOfType(kMaterialOutputType) ? "the set has none of the checked channels" : "the graph has no Material Output");
    PlanRequest req;
    req.width = req.height = std::clamp(o.size, 16, 2048);
    req.outputs = requests;
    Plan plan = buildPlan(g, req);
    if (!plan.error.empty())
        throw std::invalid_argument(plan.error);
    Report r;
    EvalResult res = evaluate(plan, EvalOptions{}, o.backend, &r.backend);
    std::map<std::string, Img> imgs;
    for (const char* ch : kChannels)
        if (auto it = res.outputs.find(ch); it != res.outputs.end())
            imgs[ch] = Img{&it->second, it->second.width, it->second.height};
    auto img = [&](const char* ch) { return imgs.count(ch) ? imgs[ch] : Img{}; };
    r.checks.push_back(albedo(img("baseColor"), img("metallic"), m));
    r.checks.push_back(blackWhite(img("baseColor"), m));
    r.checks.push_back(metalColour(img("baseColor"), img("metallic"), m));
    r.checks.push_back(metallicBinary(img("metallic")));
    r.checks.push_back(roughness(img("roughness"), m));
    r.checks.push_back(normal(img("normal"), img("height")));
    r.checks.push_back(heightRange(img("height")));
    r.checks.push_back(seams(imgs, m));
    return r;
}

std::vector<std::string> fix(Graph& g, const Report& r, const library::Meta& m)
{
    std::vector<std::string> out;
    auto bad = [&](const char* id) {
        const Check* c = r.check(id);
        return c && (c->status == "warn" || c->status == "fail");
    };
    if (bad("albedo") || bad("blackWhite"))
    {
        bool relaxed = m.physical == "stylised" || m.physical == "emissive";
        double lo = toLinear(relaxed ? 10.0 / 255.0 : kDielectricLow);
        double hi = m.physical == "metal" ? 1.0 : toLinear(relaxed ? 250.0 / 255.0 : kDielectricHigh);
        out.push_back(fixChannel(g, "baseColor", {{"outLow", lo}, {"outHigh", hi}}));
    }
    if (bad("metallic"))
        out.push_back(fixChannel(g, "metallic", {{"inLow", 0.5}, {"inHigh", 0.501}}));
    if (bad("roughness"))
        out.push_back(fixChannel(g, "roughness", {{"outLow", kRoughFloor}, {"outHigh", 1.0}}));
    if (bad("height"))
        out.push_back(fixChannel(g, "height", json::object()));
    if (bad("metal"))
        out.push_back("metal reflectance needs the material's own colours changed; not fixed");
    if (bad("normal"))
        out.push_back("normals need fixing in the graph (Normal Invert for green, Height to Normal to rebuild); not fixed");
    if (bad("seams"))
        out.push_back("seams need a periodic graph (Make Tileable); not fixed");
    return out;
}
} // namespace lila::tex::lint
