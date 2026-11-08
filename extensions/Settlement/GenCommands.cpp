// Settlement generator commands: settlementCreate's generator flags (resolved here into the generators input, reading
// curves and running Terrain commands by name), and settlementGenerators, settlementEras, settlementWalls (curves and
// Masonry walls) and settlementLevels.

#include "GenCommands.h"
#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <map>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using scene::Node;
using GenArg = ArgType;

/** A flag's value as JSON: text that parses as a JSON object or list is read as one. */
json jsonFlag(const Args& a, const char* name)
{
    json v = a.flags[name];
    if (v.is_string())
    {
        const std::string s = v.get<std::string>();
        if (!s.empty() && (s.front() == '{' || s.front() == '['))
        {
            json parsed = json::parse(s, nullptr, false);
            if (parsed.is_discarded())
                throw CommandError(std::string("settlement: ") + name + " isn't valid JSON");
            return parsed;
        }
    }
    return v;
}

std::string pathOf(const json& made) { return made.is_array() ? made.at(0).get<std::string>() : made.get<std::string>(); }

/** The splines of a curve object in world space, flattened to XZ. */
std::vector<std::vector<Vec2>> curveLines(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": '" + name + "' is not a curve");
    g.evaluate();
    std::vector<std::vector<Vec2>> out;
    for (const curve::Spline& s : curve::splinesOf(n->value(curve::CurveShape::kWorldSpace)))
    {
        std::vector<Vec2> pts;
        if (curve::isValid(s) && s.size() >= 2)
        {
            const curve::Polyline line = curve::tessellate(s);
            for (const glm::dvec3& p : line.points)
                pts.push_back({p.x, p.z});
            if (line.closed && !pts.empty())
                pts.push_back(pts.front());
        }
        out.push_back(std::move(pts));
    }
    return out;
}

/** Streets from a curve terrainImportOsm made: each spline's OSM class from its osmFeatures attribute. */
json osmCurveStreets(Graph& g, const std::string& name, const char* cmd)
{
    const std::vector<std::vector<Vec2>> lines = curveLines(g, name, cmd);
    json info = json::array();
    std::string kind;
    try
    {
        const json v = ops::execute("getAttr", {{"args", {g.find(name)->path() + ".osmFeatures"}}});
        if (v.is_string())
            info = json::parse(v.get<std::string>(), nullptr, false);
    }
    catch (const std::exception&)
    {
    }
    try
    {
        const json v = ops::execute("getAttr", {{"args", {g.find(name)->path() + ".terrainFeature"}}});
        if (v.is_string())
            kind = v.get<std::string>();
    }
    catch (const std::exception&)
    {
    }
    json out = json::array();
    for (size_t k = 0; k < lines.size(); ++k)
    {
        if (lines[k].size() < 2)
            continue;
        std::string cls = kind == "path" ? "path" : "local";
        std::string label;
        if (info.is_array() && k < info.size() && info[k].is_object())
        {
            cls = info[k].value("class", cls);
            label = info[k].value("name", std::string());
        }
        json pts = json::array();
        for (const Vec2& p : lines[k])
            pts.push_back({p.x, p.y});
        out.push_back({{"points", pts}, {"class", cls}, {"name", label}});
    }
    return out;
}

const CommandDef* needCommand(const char* name, const char* extension, const char* cmd)
{
    const CommandDef* def = ops::find(name);
    if (!def)
        throw CommandError(std::string(cmd) + ": " + name + " is not available; load the " + extension + " extension");
    return def;
}

json xyz(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

json pointsJson(std::span<const glm::dvec3> pts)
{
    json out = json::array();
    for (const glm::dvec3& p : pts)
        out.push_back(xyz(p));
    return out;
}

/** The closed wall line draped on the ground, densified so it follows the slope. */
std::vector<glm::dvec3> drapedRing(const std::vector<Vec2>& ring, const Ground& g, double step)
{
    std::vector<glm::dvec3> out;
    for (size_t k = 0; k < ring.size(); ++k)
    {
        const Vec2 a = ring[k], b = ring[(k + 1) % ring.size()];
        const int n = std::max(1, int(std::ceil(glm::length(b - a) / step)));
        for (int s = 0; s < n; ++s)
        {
            const Vec2 q = a + (b - a) * (double(s) / n);
            out.push_back({q.x, g.height(q), q.y});
        }
    }
    return out;
}

json eraJson(const Rebuilt& r)
{
    const Layout& l = r.layout;
    json eras = json::array();
    if (!l.gen)
        return eras;
    const GenLayout& gen = *l.gen;
    for (size_t k = 0; k < gen.eras.size(); ++k)
    {
        double len = 0.0;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
            if (l.graph.isStreet(int(e)) && l.graph.edges[e].era == int(k))
                len += l.graph.length(int(e));
        const auto lots = std::count_if(l.lots.begin(), l.lots.end(), [&](const Lot& x) { return x.era == int(k); });
        const auto envs = std::count_if(l.envelopes.begin(), l.envelopes.end(), [&](const Envelope& x) { return x.era == int(k); });
        int walls = 0;
        for (const WallLine& w : gen.walls)
            walls += w.era == int(k) ? 1 : 0;
        eras.push_back({{"era", int(k)},
                        {"name", gen.eras[k].name},
                        {"profile", gen.eras[k].profile.name},
                        {"pattern", patternName(gen.eras[k].profile.pattern)},
                        {"roof", gen.eras[k].profile.roof},
                        {"streetKm", len / 1.0e5},
                        {"lots", int64_t(lots)},
                        {"envelopes", int64_t(envs)},
                        {"walls", walls}});
    }
    return eras;
}

json genSummary(const Rebuilt& r)
{
    const Layout& l = r.layout;
    json out = {{"generators", r.options.generators}, {"targets", targetReport(l)}};
    if (!l.gen)
        return out;
    const GenLayout& gen = *l.gen;
    int standing = 0;
    for (const WallLine& w : gen.walls)
        standing += w.demolished < 0 ? 1 : 0;
    out["eras"] = eraJson(r);
    out["districts"] = gen.districts;
    out["walls"] = {{"standing", standing}, {"demolished", int(gen.walls.size()) - standing}, {"gates", gen.gates.size()}, {"towers", gen.towers.size()}};
    out["imported"] = gen.imported;
    out["demolishedStreets"] = gen.demolishedStreets;
    out["levels"] = gen.levels.size();
    out["ramps"] = gen.ramps.size();
    out["airLanes"] = gen.air.links.size();
    out["megablocks"] = gen.megablocks.size();
    out["warnings"] = gen.warnings;
    out["statistics"] = toJson(streetStats(l.graph, l.blocks));
    return out;
}
} // namespace

std::vector<FlagDef> generatorFlags()
{
    return {
        {"tensor", "tn", GenArg::Any, nullptr, "Tensor field for pattern tensor: {fields: [{kind: grid|radial|boundary|height, angle, center, "
                                          "weight, decay, lines, follow: outline|water}], paint: [{points, radius, angle, weight}], "
                                          "mask, majorSpacing, minorSpacing}."},
        {"eras", "er", GenArg::Any, nullptr, "Eras in order, each {name, profile, pattern, overrides, growth, inner, outer, polygon, walls, "
                                        "demolishWalls, ring, axes, highwayRing, highwaySpurs}; or a preset: walledTown, "
                                        "medievalToModern, gridToSprawl."},
        {"walls", "wl", GenArg::Any, nullptr, "Walls round the core: True, or {radius, polygon, contour, thickness, height, towerSpacing, "
                                         "starFort, bastions, moat, moatGap}."},
        {"districts", "di", GenArg::List, nullptr, "Districts, each {name, polygon, profile, pattern, overrides, growth, era, seed, eras}: "
                                              "their own generator inside the polygon."},
        {"osm", "os", GenArg::List, nullptr, "Imported streets: curves made by terrainImportOsm (by name), or {points, class} with an OSM "
                                        "highway value or a street class."},
        {"osmFile", "of", GenArg::String, nullptr, "OSM file (.osm, .json, .osm.pbf) read through terrainImportOsm onto the ground."},
        {"osmOrigin", "oo", GenArg::List, nullptr, "[latitude, longitude] of the ground's centre, for osmFile on a terrain not from real data."},
        {"example", "eg", GenArg::Any, nullptr, "Grow like another street network: a settlement by name, or {orientation, degree, "
                                           "blockAreaMedianM2, meanSegment} (settlementGenerators statistics), or {streets}."},
        {"contourStreets", "cst", GenArg::Bool, false, "Streets along the ground's contours from terrainContourStreets (the ground must be a Terrain)."},
        {"contourSpacing", "csp", GenArg::Float, 3000.0, "Between contour streets across the slope, cm.", {}, {}, 100.0},
        {"levels", "lv", GenArg::Any, nullptr, "Levels above the ground: {decks: [{name, polygon, height, pattern, ramps}], elevated: "
                                          "[{points, height, class, ramps}], skybridgeSpan, skybridgeHeight, airHeights, airSpacing, megablock}."},
    };
}

void resolveGeneratorFlags(Graph& g, const Args& a, json& values, std::span<const Vec2> region, const std::string& ground, const char* cmd)
{
    LILA_ZONE();

    json gen = json::object();
    if (values.contains("generators") && values["generators"].is_string() && !values["generators"].get<std::string>().empty())
    {
        gen = json::parse(values["generators"].get<std::string>(), nullptr, false);
        if (gen.is_discarded() || !gen.is_object())
            throw CommandError(std::string(cmd) + ": generators isn't a JSON object");
    }
    std::string pattern;
    for (const char* key : {"tensor", "eras", "walls", "districts", "levels"})
        if (a.has(key))
            gen[key] = jsonFlag(a, key);
    if (a.has("tensor"))
        pattern = "tensor";
    if (a.has("osm") || a.has("osmFile"))
    {
        json streets = json::array();
        for (const json& item : a.has("osm") ? a.flags["osm"] : json::array())
        {
            if (item.is_string())
                for (const json& s : osmCurveStreets(g, item.get<std::string>(), cmd))
                    streets.push_back(s);
            else
                streets.push_back(item);
        }
        if (a.has("osmFile"))
        {
            needCommand("terrainImportOsm", "Terrain", cmd);
            if (ground.empty())
                throw CommandError(std::string(cmd) + ": osmFile needs ground= (the terrain the OSM data lies on)");
            json call = {{"args", {a.get<std::string>("osmFile")}}, {"terrain", ground}, {"kinds", {"road", "path"}}, {"landcover", false}};
            if (a.has("osmOrigin"))
                call["origin"] = a.flags["osmOrigin"];
            const json made = ops::execute("terrainImportOsm", call);
            const json curves = made.value("curves", json::object());
            for (const char* kind : {"road", "path"})
                if (curves.contains(kind) && curves[kind].is_string())
                    for (const json& s : osmCurveStreets(g, curves[kind].get<std::string>(), cmd))
                        streets.push_back(s);
            if (streets.empty())
                throw CommandError(std::string(cmd) + ": " + a.get<std::string>("osmFile") + " has no roads or paths on the ground");
        }
        gen["osm"] = streets;
        if (pattern.empty())
            pattern = "osm";
    }
    if (a.has("example"))
    {
        const json ex = jsonFlag(a, "example");
        if (ex.is_string())
        {
            const Rebuilt other = rebuildSettlement(g, ex.get<std::string>(), cmd);
            gen["example"] = toJson(streetStats(other.layout.graph, other.layout.blocks));
        }
        else
            gen["example"] = ex;
        if (pattern.empty())
            pattern = "example";
    }
    if (a.get<bool>("contourStreets", false))
    {
        needCommand("terrainContourStreets", "Terrain", cmd);
        if (ground.empty())
            throw CommandError(std::string(cmd) + ": contourStreets needs ground= (a Terrain)");
        json ring = json::array();
        for (const Vec2& q : region)
            ring.push_back({q.x, 0.0, q.y});
        const json made = ops::execute("terrainContourStreets", {{"args", {ground}},
                                                                 {"region", ring},
                                                                 {"spacing", a.get<double>("contourSpacing", 3000.0)},
                                                                 {"maxGrade", 100.0 * std::max(0.01, 0.12)}});
        json streets = json::array();
        for (const json& s : made.value("streets", json::array()))
            streets.push_back({{"points", s.at("points")}, {"kind", s.value("kind", std::string("contour"))}});
        gen["contour"] = streets;
        if (pattern.empty())
            pattern = "contour";
    }
    try
    {
        parseGenerators(gen);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    values["generators"] = gen.empty() ? std::string() : gen.dump();
    if (!pattern.empty() && (!values.contains("pattern") || !values["pattern"].is_string() || values["pattern"].get<std::string>().empty()))
        values["pattern"] = pattern;
}

void registerGeneratorCommands(ext::Context& ctx)
{
    std::vector<FlagDef> genFlags = generatorFlags();
    genFlags.push_back({"clear", "cl", GenArg::Bool, false, "Remove every generator setting first."});
    genFlags.push_back({"pattern", "pt", GenArg::String, nullptr, "Set the pattern too (organic, grid, radial, linear, organicGrid, none, tensor, contour, osm, example)."});

    ctx.registerCommand({
        .name = "settlementGenerators",
        .doc = "Reads or changes a settlement's generator settings: tensor fields, eras, districts, walls, imported OSM streets, "
               "example statistics, contour streets and levels (the same flags as settlementCreate), stored in its generators "
               "input. Only the flags given change; clear=True starts over. Returns the generator settings, eras (name, profile, "
               "pattern, street km, lots, envelopes, walls), districts, walls (standing, demolished, gates, towers), levels, "
               "ramps, air lanes, megablocks, the street statistics (orientation histogram, degrees, block size: what example= "
               "reads), the profile's metric targets and whether they are met, and warnings for fallbacks taken.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = genFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementGenerators";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            Rebuilt r = rebuildSettlement(g, names[0], cmd);
            bool changed = a.get<bool>("clear", false) || a.has("pattern");
            for (const FlagDef& f : generatorFlags())
                changed = changed || (a.has(f.name) && f.name != "contourSpacing" && f.name != "osmOrigin" && !(f.name == "contourStreets" && !a.get<bool>(f.name)));
            if (changed)
            {
                json values = {{"generators", a.get<bool>("clear", false) ? json(std::string()) : r.values.value("generators", json(std::string()))},
                               {"pattern", a.has("pattern") ? a.flags["pattern"] : r.values.value("pattern", json(std::string()))}};
                resolveGeneratorFlags(g, a, values, r.layout.outline.pts, r.values.value("ground", std::string()), cmd);
                ops::execute("setAttr", {{"args", {r.modifier + ".generators", values["generators"]}}});
                ops::execute("setAttr", {{"args", {r.modifier + ".pattern", values["pattern"]}}});
                r = rebuildSettlement(g, names[0], cmd);
            }
            json out = genSummary(r);
            out["mesh"] = r.mesh;
            out["modifier"] = r.modifier;
            return out;
        },
        .examples = {"settlementGenerators('Settlement', eras='medievalToModern')['eras']",
                     "settlementGenerators('Settlement', pattern='tensor', tensor={'fields': [{'kind': 'grid', 'angle': 30}, "
                     "{'kind': 'radial', 'center': [0, 0], 'decay': 30000}]})",
                     "settlementGenerators('Settlement')['targets']['ok']"},
    });

    ctx.registerCommand({
        .name = "settlementEras",
        .doc = "A settlement's eras in order: name, profile, pattern, roof, street km, lots and envelopes laid in each era, walls "
               "built in it; and its districts. Every street, lot and envelope carries its era (settlementExport writes it on "
               "streets). Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementEras";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(Graph::current(), names[0], cmd);
            return {{"eras", eraJson(r)}, {"districts", r.layout.gen ? json(r.layout.gen->districts) : json::array()}};
        },
        .examples = {"[e['name'] for e in settlementEras('Settlement')['eras']]"},
    });

    ctx.registerCommand({
        .name = "settlementWalls",
        .doc = "A settlement's fortifications: each wall line (points [x, y, z] on the ground, era, the era it was demolished in or "
               "-1, thickness, height, star fort), its gates (point, distance along the wall, road width and class) and towers "
               "(point, gate or bastion). curves=True makes a closed curve per standing wall; masonry=True builds each standing "
               "wall with Masonry's masonryWall (gates cut as arched openings, towers as round walls with towers=True). Returns "
               "the walls and what was made.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"curves", "cv", GenArg::Bool, false, "Make a closed curve per standing wall."},
                  {"masonry", "ms", GenArg::Bool, false, "Build the standing walls with masonryWall (needs the Masonry extension)."},
                  {"towers", "tw", GenArg::Bool, false, "With masonry: a round wall per tower."},
                  {"height", "h", GenArg::Float, nullptr, "Wall height for Masonry, cm; default the wall's.", {}, {}, 10.0},
                  {"thickness", "th", GenArg::Float, nullptr, "Wall thickness for Masonry, cm; default the wall's.", {}, {}, 10.0},
                  {"gateHeight", "gh", GenArg::Float, 500.0, "Height of the gate openings, cm.", {}, {}, 50.0},
                  {"masonryFlags", "mf", GenArg::Any, nullptr, "More masonryWall flags for every wall and tower, such as {'stoneLength': 60, 'bond': 'flemish'}."},
                  {"name", "n", GenArg::String, "Wall", "Name of the curves and walls made."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementWalls";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(g, names[0], cmd);
            const Layout& l = r.layout;
            const bool masonry = a.get<bool>("masonry", false);
            if (masonry)
                needCommand("masonryWall", "Masonry", cmd);
            const std::string base = a.get<std::string>("name", "Wall");
            json walls = json::array(), made = json::array();
            if (!l.gen)
                return {{"walls", walls}};
            const GenLayout& gen = *l.gen;
            for (size_t wi = 0; wi < gen.walls.size(); ++wi)
            {
                const WallLine& w = gen.walls[wi];
                const std::vector<glm::dvec3> ring = drapedRing(w.pts, r.ground, 500.0);
                json gates = json::array(), towers = json::array();
                for (const Gate& gt : gen.gates)
                    if (gt.wall == int(wi))
                        gates.push_back({{"point", xyz({gt.p.x, r.ground.height(gt.p), gt.p.y})},
                                         {"at", gt.at},
                                         {"width", gt.width},
                                         {"class", className(gt.cls)}});
                for (const Tower& t : gen.towers)
                    if (t.wall == int(wi))
                        towers.push_back({{"point", xyz({t.p.x, r.ground.height(t.p), t.p.y})}, {"gate", t.gate}, {"bastion", t.bastion}});
                json entry = {{"points", pointsJson(ring)}, {"era", w.era},   {"demolished", w.demolished}, {"thickness", w.thickness},
                              {"height", w.height},         {"starFort", w.starFort}, {"gates", gates},     {"towers", towers}};
                if (w.demolished < 0 && a.get<bool>("curves", false))
                {
                    const json c = ops::execute("curve", {{"splines", json::array({{{"type", "poly"}, {"closed", true}, {"points", pointsJson(ring)}}})},
                                                          {"name", base + std::to_string(wi)}});
                    entry["curve"] = pathOf(c);
                    made.push_back(entry["curve"]);
                }
                if (w.demolished < 0 && masonry)
                {
                    const double height = a.get<double>("height", w.height), thickness = a.get<double>("thickness", w.thickness);
                    json openings = json::array();
                    for (const json& gt : gates)
                        openings.push_back({{"at", gt["at"]}, {"width", gt["width"].get<double>() + 100.0},
                                            {"height", std::min(a.get<double>("gateHeight", 500.0), 0.9 * height)}, {"arch", "round"}});
                    json call = {{"points", pointsJson(ring)}, {"closed", true}, {"height", height}, {"thickness", thickness},
                                 {"name", base + "Masonry" + std::to_string(wi)}};
                    if (!openings.empty())
                        call["openings"] = openings.dump();
                    const json extra = a.has("masonryFlags") ? jsonFlag(a, "masonryFlags") : json();
                    if (extra.is_object())
                        for (auto& [k, v] : extra.items())
                            call[k] = v;
                    const json m = ops::execute("masonryWall", call);
                    entry["masonry"] = m.value("mesh", json());
                    made.push_back(entry["masonry"]);
                    if (a.get<bool>("towers", false))
                        for (const Tower& t : gen.towers)
                        {
                            if (t.wall != int(wi))
                                continue;
                            const double rad = std::max(w.thickness * 1.5, 300.0);
                            json pts = json::array();
                            for (int s = 0; s < 16; ++s)
                            {
                                const Vec2 q = t.p + Vec2(std::cos(2.0 * kPi * s / 16), std::sin(2.0 * kPi * s / 16)) * rad;
                                pts.push_back({q.x, r.ground.height(q), q.y});
                            }
                            json tc = {{"points", pts}, {"closed", true}, {"height", height * 1.3}, {"thickness", thickness}, {"name", base + "Tower"}};
                            if (extra.is_object())
                                for (auto& [k, v] : extra.items())
                                    tc[k] = v;
                            const json tm = ops::execute("masonryWall", tc);
                            made.push_back(tm.value("mesh", json()));
                        }
                }
                walls.push_back(std::move(entry));
            }
            return {{"walls", walls}, {"made", made}};
        },
        .examples = {"settlementCreate(profile='medievalTown', size=80000, walls={'moat': 1200, 'towerSpacing': 3000})\n"
                     "w = settlementWalls('Settlement', curves=True)['walls'][0]\nlen(w['gates'])",
                     "settlementWalls('Settlement', masonry=True, height=1000, thickness=250)"},
    });

    ctx.registerCommand({
        .name = "settlementLevels",
        .doc = "A settlement's networks above the ground: each level (deck, elevated road, skybridges) with its layer, height, "
               "street count and junctions from its own road network; ramps between levels and the ground; air-traffic lanes "
               "(altitudes, nodes, one-way links, whether every node reaches every other); megablock footprints. Streets on "
               "different layers never meet except through ramps. curves=True makes a curve per kind (deck, elevated, ramp, "
               "skybridge, air) with every line at its height.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"curves", "cv", GenArg::Bool, false, "Make curves for the lines above the ground."},
                  {"name", "n", GenArg::String, "Levels", "Prefix of the curves made."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementLevels";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(g, names[0], cmd);
            const Layout& l = r.layout;
            json levels = json::array(), ramps = json::array(), megas = json::array(), curves = json::object();
            if (!l.gen)
                return {{"levels", levels}};
            const GenLayout& gen = *l.gen;
            for (size_t k = 0; k < gen.levels.size(); ++k)
            {
                const Level& lv = gen.levels[k];
                int streets = 0;
                for (size_t e = 0; e < lv.graph.edges.size(); ++e)
                    streets += lv.graph.isStreet(int(e)) ? 1 : 0;
                const RoadNetwork net = levelNetwork(l, int(k));
                levels.push_back({{"name", lv.name}, {"kind", lv.kind}, {"layer", int(k) + 1}, {"height", lv.height},
                                  {"streets", streets}, {"junctions", net.junctions.size()}, {"lines", lv.lines3.size()}});
            }
            for (const Ramp& rp : gen.ramps)
                ramps.push_back({{"points", pointsJson(rp.pts)}, {"from", rp.from}, {"to", rp.to}, {"class", className(rp.cls)}});
            for (const Megablock& m : gen.megablocks)
                megas.push_back({{"block", m.block}, {"area", area(m.footprint) / kM2}, {"base", m.base}, {"top", m.top}});
            // Every air node reaching every other: forward and backward search from the first.
            bool connected = true;
            if (!gen.air.nodes.empty())
            {
                for (int dir = 0; dir < 2 && connected; ++dir)
                {
                    std::vector<std::vector<int>> adj(gen.air.nodes.size());
                    for (const auto& [x, y] : gen.air.links)
                        adj[size_t(dir ? y : x)].push_back(dir ? x : y);
                    std::vector<char> seen(adj.size(), 0);
                    std::vector<int> stack = {0};
                    seen[0] = 1;
                    while (!stack.empty())
                    {
                        const int n = stack.back();
                        stack.pop_back();
                        for (int m : adj[size_t(n)])
                            if (!seen[size_t(m)])
                            {
                                seen[size_t(m)] = 1;
                                stack.push_back(m);
                            }
                    }
                    connected = std::all_of(seen.begin(), seen.end(), [](char c) { return c != 0; });
                }
            }
            if (a.get<bool>("curves", false))
            {
                std::map<std::string, json> byKind;
                for (const LevelLine& line : levelLines(l, r.ground))
                    byKind[line.kind].push_back({{"type", "poly"}, {"closed", false}, {"points", pointsJson(line.pts)}});
                for (auto& [kind, splines] : byKind)
                    curves[kind] = pathOf(ops::execute("curve", {{"splines", splines}, {"name", a.get<std::string>("name", "Levels") + "_" + kind}}));
            }
            return {{"levels", levels},
                    {"ramps", ramps},
                    {"air", {{"heights", gen.air.heights}, {"nodes", gen.air.nodes.size()}, {"links", gen.air.links.size()}, {"connected", connected}}},
                    {"megablocks", megas},
                    {"curves", curves}};
        },
        .examples = {"settlementCreate(profile='scifiMegacity', size=200000, levels={'decks': [{'polygon': [[-30000, -30000], [30000, -30000], "
                     "[30000, 30000], [-30000, 30000]], 'height': 4000}], 'airHeights': [20000, 40000], 'megablock': 20000})\n"
                     "settlementLevels('Settlement')['air']['connected']"},
    });
}
} // namespace settlement
