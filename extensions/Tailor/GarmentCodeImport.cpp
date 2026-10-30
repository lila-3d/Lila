#include "GarmentCodeImport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace tailor
{

namespace gcimport
{

using Json = nlohmann::ordered_json;
constexpr double kPi = 3.14159265358979323846;

struct ImportError
{
    std::string message;
};

[[noreturn]] void fail(std::string message)
{
    throw ImportError{std::move(message)};
}

struct P2
{
    double x = 0, y = 0;
};

P2 operator+(P2 a, P2 b) { return {a.x + b.x, a.y + b.y}; }
P2 operator-(P2 a, P2 b) { return {a.x - b.x, a.y - b.y}; }
P2 operator*(P2 a, double s) { return {a.x * s, a.y * s}; }
double length(P2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

double number(const Json& j, const char* what)
{
    if (!j.is_number())
        fail(std::string("expected a number for ") + what);
    return j.get<double>();
}

P2 point(const Json& j, const char* what)
{
    if (!j.is_array() || j.size() < 2)
        fail(std::string("expected [x, y] for ") + what);
    return {number(j[0], what), number(j[1], what)};
}

/** pygarment's rel_to_abs_2d: x along the edge, y along its left perpendicular, both scaled by edge length. */
P2 relToAbs(P2 s, P2 e, P2 rel)
{
    const P2 d = e - s;
    return s + d * rel.x + P2{-d.y, d.x} * rel.y;
}

/** One edge as a parametric curve on t in [0, 1]. */
struct EdgeCurve
{
    enum class Kind { Line, Quadratic, Cubic, Arc } kind = Kind::Line;
    P2 s, e, c1, c2;
    P2 center;
    double radius = 0, a0 = 0, sweep = 0;

    P2 at(double t) const
    {
        const double u = 1.0 - t;
        switch (kind)
        {
        case Kind::Line: return s * u + e * t;
        case Kind::Quadratic: return s * (u * u) + c1 * (2 * u * t) + e * (t * t);
        case Kind::Cubic: return s * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + e * (t * t * t);
        case Kind::Arc:
        {
            const double a = a0 + sweep * t;
            return center + P2{std::cos(a), std::sin(a)} * radius;
        }
        }
        return s;
    }
};

/**
 * Circular arc through s and e whose mid-chord point sits at `cy` chord lengths along the left perpendicular
 * (pygarment CircleEdge). Negative cy is its "right" flag and runs counter-clockwise about the centre.
 */
EdgeCurve arcThrough(P2 s, P2 e, double cy)
{
    EdgeCurve c;
    c.s = s;
    c.e = e;
    const P2 d = e - s;
    const double len = length(d);
    if (len <= 0 || std::abs(cy) < 1e-9)
        return c;
    const P2 n{-d.y / len, d.x / len};
    const double h = cy * len;
    const double t = (h * h - 0.25 * len * len) / (2 * h);
    c.kind = EdgeCurve::Kind::Arc;
    c.center = (s + e) * 0.5 + n * t;
    c.radius = length(s - c.center);
    c.a0 = std::atan2(s.y - c.center.y, s.x - c.center.x);
    double delta = std::atan2(e.y - c.center.y, e.x - c.center.x) - c.a0;
    if (cy < 0)
    {
        while (delta <= 0) delta += 2 * kPi;
        while (delta > 2 * kPi) delta -= 2 * kPi;
    }
    else
    {
        while (delta >= 0) delta -= 2 * kPi;
        while (delta < -2 * kPi) delta += 2 * kPi;
    }
    c.sweep = delta;
    return c;
}

EdgeCurve edgeCurve(P2 s, P2 e, const Json& edge)
{
    EdgeCurve c;
    c.s = s;
    c.e = e;
    auto it = edge.find("curvature");
    if (it == edge.end() || it->is_null())
        return c;
    const Json& cv = *it;

    // Legacy patterns store a bare quadratic control point.
    if (cv.is_array())
    {
        c.kind = EdgeCurve::Kind::Quadratic;
        c.c1 = relToAbs(s, e, point(cv, "curvature"));
        return c;
    }
    if (!cv.is_object())
        fail("edge curvature must be an object or [x, y]");

    const std::string type = cv.value("type", std::string("quadratic"));
    const Json& params = cv.contains("params") ? cv["params"] : Json::array();
    if (type == "quadratic")
    {
        if (params.empty())
            fail("quadratic curvature without params");
        c.kind = EdgeCurve::Kind::Quadratic;
        c.c1 = relToAbs(s, e, point(params[0], "quadratic control point"));
    }
    else if (type == "cubic")
    {
        if (params.size() < 2)
            fail("cubic curvature needs two control points");
        c.kind = EdgeCurve::Kind::Cubic;
        c.c1 = relToAbs(s, e, point(params[0], "cubic control point"));
        c.c2 = relToAbs(s, e, point(params[1], "cubic control point"));
    }
    else if (type == "circle")
    {
        // params: [absolute radius, large-arc flag, right flag] as SVG arcs take them.
        if (params.size() < 3)
            fail("circle curvature needs [radius, large_arc, right]");
        const double len = length(e - s);
        if (len <= 0)
            return c;
        const double rel = std::max(number(params[0], "circle radius") / len, 0.5);
        const double half = std::sqrt(std::max(rel * rel - 0.25, 0.0));
        const bool large = number(params[1], "circle large_arc") != 0;
        const bool right = number(params[2], "circle right") != 0;
        const double sagitta = large ? rel + half : rel - half;
        return arcThrough(s, e, right ? -sagitta : sagitta);
    }
    else
    {
        fail("unknown curvature type '" + type + "'");
    }
    return c;
}

/** Edge start plus interior samples spaced about `spacing` by arc length (end point excluded). */
void sampleEdge(const EdgeCurve& c, double spacing, std::vector<P2>& out)
{
    out.push_back(c.s);
    if (c.kind == EdgeCurve::Kind::Line)
        return;

    constexpr int kFine = 256;
    std::vector<P2> fine(kFine + 1);
    std::vector<double> cum(kFine + 1, 0.0);
    for (int i = 0; i <= kFine; ++i)
    {
        fine[i] = c.at(double(i) / kFine);
        if (i > 0)
            cum[i] = cum[i - 1] + length(fine[i] - fine[i - 1]);
    }
    const double total = cum.back();
    const int segments = std::max(2, int(std::lround(total / spacing)));
    int j = 1;
    for (int k = 1; k < segments; ++k)
    {
        const double target = total * k / segments;
        while (j < kFine && cum[j] < target)
            ++j;
        const double span = cum[j] - cum[j - 1];
        const double f = span > 0 ? (target - cum[j - 1]) / span : 0.0;
        out.push_back(fine[j - 1] + (fine[j] - fine[j - 1]) * f);
    }
}

using M3 = std::array<std::array<double, 3>, 3>;

M3 mul(const M3& a, const M3& b)
{
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i][j] += a[i][k] * b[k][j];
    return r;
}

/** pygarment rotation.euler_xyz_to_R: Rz * Ry * Rx, degrees. */
M3 eulerXyzToR(double xDeg, double yDeg, double zDeg)
{
    const double x = xDeg * kPi / 180, y = yDeg * kPi / 180, z = zDeg * kPi / 180;
    const M3 rx{{{1, 0, 0}, {0, std::cos(x), -std::sin(x)}, {0, std::sin(x), std::cos(x)}}};
    const M3 ry{{{std::cos(y), 0, std::sin(y)}, {0, 1, 0}, {-std::sin(y), 0, std::cos(y)}}};
    const M3 rz{{{std::cos(z), -std::sin(z), 0}, {std::sin(z), std::cos(z), 0}, {0, 0, 1}}};
    return mul(rz, mul(ry, rx));
}

Vec3 vec3(const Json& j, const char* what)
{
    if (!j.is_array() || j.size() < 3)
        fail(std::string("expected [x, y, z] for ") + what);
    return {float(number(j[0], what)), float(number(j[1], what)), float(number(j[2], what))};
}

/** Where each source edge landed on the final outline. */
struct EdgeRun
{
    int from = 0;
    int to = 0;
    bool flipped = false; // walking the run forward goes from the edge's end to its start
};

struct ImportedPanel
{
    Panel panel;
    std::vector<EdgeRun> edges;
};

ImportedPanel importPanel(const std::string& name, const Json& spec, double scale, double spacing)
{
    if (!spec.is_object())
        fail("panel '" + name + "' is not an object");
    const Json& verts = spec.contains("vertices") ? spec["vertices"] : Json();
    const Json& edges = spec.contains("edges") ? spec["edges"] : Json();
    if (!verts.is_array() || verts.size() < 2 || !edges.is_array() || edges.size() < 2)
        fail("panel '" + name + "' needs vertices and at least two edges");

    std::vector<P2> v;
    v.reserve(verts.size());
    for (const Json& p : verts)
        v.push_back(point(p, "panel vertex"));

    ImportedPanel out;
    out.panel.name = name;
    std::vector<P2> outline;
    std::vector<int> edgeStart;
    int prevEnd = -1, firstStart = -1;
    for (const Json& edge : edges)
    {
        const Json& ends = edge.contains("endpoints") ? edge["endpoints"] : Json();
        if (!ends.is_array() || ends.size() != 2 || !ends[0].is_number_integer() || !ends[1].is_number_integer())
            fail("panel '" + name + "' has an edge without integer endpoints");
        const int a = ends[0].get<int>(), b = ends[1].get<int>();
        if (a < 0 || b < 0 || a >= int(v.size()) || b >= int(v.size()))
            fail("panel '" + name + "' edge endpoint out of range");
        if (prevEnd >= 0 && a != prevEnd)
            fail("panel '" + name + "' edges do not form a loop");
        if (firstStart < 0)
            firstStart = a;
        prevEnd = b;
        edgeStart.push_back(int(outline.size()));
        sampleEdge(edgeCurve(v[a], v[b], edge), spacing, outline);
    }
    if (prevEnd != firstStart)
        fail("panel '" + name + "' edge loop is not closed");

    const int n = int(outline.size());
    out.edges.resize(edges.size());
    for (size_t i = 0; i < edges.size(); ++i)
        out.edges[i] = {edgeStart[i], edgeStart[(i + 1) % edges.size()] % n, false};

    double area2 = 0;
    for (int i = 0; i < n; ++i)
    {
        const P2& p = outline[i];
        const P2& q = outline[(i + 1) % n];
        area2 += p.x * q.y - q.x * p.y;
    }
    // Reverse clockwise loops in place, keeping index 0, so old index i maps to (n - i) % n.
    if (area2 < 0)
    {
        std::reverse(outline.begin() + 1, outline.end());
        for (EdgeRun& r : out.edges)
            r = {(n - r.to) % n, (n - r.from) % n, true};
    }

    out.panel.outline.reserve(n);
    for (const P2& p : outline)
        out.panel.outline.push_back({float(p.x * scale), float(p.y * scale)});

    const Vec3 t = spec.contains("translation") ? vec3(spec["translation"], "translation") : Vec3{0, 0, 0};
    const Vec3 r = spec.contains("rotation") ? vec3(spec["rotation"], "rotation") : Vec3{0, 0, 0};
    const M3 m = eulerXyzToR(r[0], r[1], r[2]);
    out.panel.origin = {float(t[0] * scale), float(t[1] * scale), float(t[2] * scale)};
    out.panel.right = {float(m[0][0]), float(m[1][0]), float(m[2][0])};
    out.panel.up = {float(m[0][1]), float(m[1][1]), float(m[2][1])};
    return out;
}

Garment importPattern(const Json& root, const GarmentCodeImportOptions& options)
{
    if (!root.is_object())
        fail("top level is not a JSON object");
    const Json& pattern = root.contains("pattern") ? root["pattern"] : root;
    if (!pattern.is_object() || !pattern.contains("panels") || !pattern["panels"].is_object())
        fail("no pattern.panels object");

    double unitsInMeter = 100.0;
    if (root.contains("properties") && root["properties"].is_object())
    {
        const Json& props = root["properties"];
        if (props.contains("units_in_meter"))
            unitsInMeter = number(props["units_in_meter"], "units_in_meter");
        // Only relative curvature is defined by the format as written; absolute is a template-only legacy.
        if (props.value("curvature_coords", std::string("relative")) != "relative")
            fail("absolute curvature coordinates are not supported");
    }
    if (unitsInMeter <= 0)
        fail("units_in_meter must be positive");
    const double scale = 1.0 / unitsInMeter;
    const double spacing = std::max(double(options.curveSpacing), 1e-4) * unitsInMeter;

    Garment g;
    std::unordered_map<std::string, int> panelIndex;
    std::vector<std::vector<EdgeRun>> runs;
    for (const auto& [name, spec] : pattern["panels"].items())
    {
        ImportedPanel p = importPanel(name, spec, scale, spacing);
        panelIndex[name] = int(g.panels.size());
        g.panels.push_back(std::move(p.panel));
        runs.push_back(std::move(p.edges));
    }

    if (pattern.contains("stitches"))
    {
        const Json& stitches = pattern["stitches"];
        if (!stitches.is_array())
            fail("pattern.stitches is not an array");
        for (const Json& st : stitches)
        {
            if (!st.is_array() || st.size() < 2)
                fail("stitch needs two {panel, edge} sides");
            SeamSide sides[2];
            bool flipped[2] = {false, false};
            for (int k = 0; k < 2; ++k)
            {
                const Json& side = st[k];
                if (!side.is_object() || !side.contains("panel") || !side.contains("edge") || !side["panel"].is_string()
                    || !side["edge"].is_number_integer())
                    fail("stitch side must be {\"panel\": name, \"edge\": index}");
                const std::string pname = side["panel"].get<std::string>();
                auto pi = panelIndex.find(pname);
                if (pi == panelIndex.end())
                    fail("stitch references unknown panel '" + pname + "'");
                const int e = side["edge"].get<int>();
                const auto& pr = runs[pi->second];
                if (e < 0 || e >= int(pr.size()))
                    fail("stitch edge index out of range on panel '" + pname + "'");
                sides[k] = {pi->second, pr[e].from, pr[e].to};
                flipped[k] = pr[e].flipped;
            }
            // pygarment pairs the first edge's end with the second edge's start unless marked "right_wrong".
            const bool rightWrong = st.size() >= 3 && st[2].is_string() && st[2].get<std::string>() == "right_wrong";
            Seam seam;
            seam.a.push_back(sides[0]);
            seam.b.push_back(sides[1]);
            seam.reversed = (!rightWrong) != (flipped[0] != flipped[1]);
            g.seams.push_back(std::move(seam));
        }
    }
    return g;
}

} // namespace gcimport

bool importGarmentCode(const std::string& jsonText, Garment& out, std::string* error,
                       const GarmentCodeImportOptions& options)
{
    using namespace gcimport;
    const Json root = Json::parse(jsonText, nullptr, false);
    if (root.is_discarded())
    {
        if (error)
            *error = "GarmentCode import: invalid JSON";
        return false;
    }
    try
    {
        out = importPattern(root, options);
        return true;
    }
    catch (const ImportError& e)
    {
        if (error)
            *error = "GarmentCode import: " + e.message;
    }
    catch (const nlohmann::json::exception& e)
    {
        if (error)
            *error = std::string("GarmentCode import: ") + e.what();
    }
    return false;
}

bool importGarmentCodeFile(const std::string& path, Garment& out, std::string* error,
                           const GarmentCodeImportOptions& options)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        if (error)
            *error = "GarmentCode import: cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return importGarmentCode(ss.str(), out, error, options);
}

} // namespace tailor
