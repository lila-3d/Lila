// settlementGrade shapes a Terrain under the settlement through the terrain contract's commands (roads as road features,
// lots as pads, squares as region stamps); settlementSendToUnreal sends roads (landscape splines, spline actors or ribbon
// meshes), lots and envelopes as a PCG data asset, and the lane graph as ZoneGraph shapes.

#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/Graph.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>

using namespace lila;

namespace settlement
{
namespace
{
using Arg = ArgType;

// ---------------------------------------------------------------------------------------------------------------------
// Grading through the terrain contract

/** The first of the names the command takes as a flag; empty when it takes none of them. */
std::string flagOf(const CommandDef& def, std::initializer_list<const char*> names)
{
    for (const char* n : names)
        for (const FlagDef& f : def.flags)
            if (f.name == n)
                return n;
    return {};
}

json xzRing(std::span<const Vec2> pts)
{
    json a = json::array();
    for (const Vec2& q : pts)
        a.push_back(json::array({q.x, q.y}));
    return a;
}

json xyz(std::span<const glm::dvec3> pts)
{
    json a = json::array();
    for (const glm::dvec3& q : pts)
        a.push_back(json::array({q.x, q.y, q.z}));
    return a;
}

/** Calls a contract command with the flags it declares; what it does not take is left out. Records failures. */
struct Stamper
{
    const CommandDef* def = nullptr;
    std::string name, terrain;
    int calls = 0;
    json errors = json::array();
    json last;

    bool run(json flags)
    {
        json call = json::object();
        if (!terrain.empty())
            call["args"] = json::array({terrain});
        for (auto& [k, v] : flags.items())
            if (!k.empty())
                call[k] = v;
        try
        {
            last = ops::execute(name, call);
            ++calls;
            return true;
        }
        catch (const std::exception& e)
        {
            if (errors.size() < 20)
                errors.push_back(name + ": " + e.what());
            return false;
        }
    }
};

Stamper stamper(const char* name, const std::string& terrain)
{
    Stamper s;
    s.name = name;
    s.terrain = terrain;
    s.def = ops::find(name);
    return s;
}

json grade(const Rebuilt& r, const Args& a, const char* cmd)
{
    LILA_ZONE();

    const std::string terrain = a.get<std::string>("terrain", "");
    const std::string padMode = a.get<std::string>("padMode", "flatten");
    const std::string key = r.mesh.empty() ? r.modifier : r.mesh;
    Stamper road = stamper("terrainAddFeature", terrain), pad = stamper("terrainStampPad", terrain),
            region = stamper("terrainStampRegion", terrain);
    const bool streets = a.get<bool>("streets", true), lots = a.get<bool>("lots", true), plazas = a.get<bool>("plazas", true);
    if ((streets && !road.def) || (lots && !pad.def) || (plazas && !region.def))
        throw CommandError(std::string(cmd) + ": the terrain grading commands (terrainAddFeature, terrainStampPad, "
                                              "terrainStampRegion) are not available; load the Terrain extension");
    json out = json::object();
    if (streets)
    {
        const std::string pts = flagOf(*road.def, {"points", "curve", "centreline", "centerline", "path"});
        const std::string kind = flagOf(*road.def, {"kind", "type", "feature"});
        const std::string width = flagOf(*road.def, {"width"});
        const std::string cls = flagOf(*road.def, {"roadClass", "streetClass", "class"});
        const std::string id = flagOf(*road.def, {"id", "key"});
        if (pts.empty())
            throw CommandError(std::string(cmd) + ": terrainAddFeature takes no points flag this settlement knows");
        int k = 0;
        for (const StreetLine& line : streetLines(r.layout, r.ground))
        {
            if (line.surface)
                continue;
            json f = {{pts, xyz(line.pts)}, {width, line.width}, {cls, className(line.cls)}, {id, key + "/street/" + std::to_string(k++)}};
            if (!kind.empty())
                f[kind] = line.cls == StreetClass::Path ? (line.profile >= 0 && r.layout.roads[size_t(line.profile)].name == "stairs" ? "stairs" : "path") : "street";
            road.run(f);
        }
        out["streets"] = road.calls;
    }
    if (lots)
    {
        const std::string fp = flagOf(*pad.def, {"footprint", "polygon", "outline", "region", "points"});
        const std::string mode = flagOf(*pad.def, {"mode"});
        const std::string id = flagOf(*pad.def, {"id", "key"});
        if (fp.empty())
            throw CommandError(std::string(cmd) + ": terrainStampPad takes no footprint flag this settlement knows");
        std::map<int, const Envelope*> byLot;
        for (const Envelope& e : r.layout.envelopes)
            byLot[e.lot] = &e;
        for (const Lot& lot : r.layout.lots)
        {
            if (lot.use == Use::Open || lot.use == Use::Plaza)
                continue;
            auto it = byLot.find(lot.id);
            const Polygon& shape = it != byLot.end() ? it->second->footprint : lot.outline;
            pad.run({{fp, xzRing(shape.pts)}, {mode, padMode}, {id, key + "/lot/" + std::to_string(lot.id)}});
        }
        out["pads"] = pad.calls;
    }
    if (plazas)
    {
        const std::string rg = flagOf(*region.def, {"region", "polygon", "outline", "footprint", "points"});
        const std::string id = flagOf(*region.def, {"id", "key"});
        if (rg.empty())
            throw CommandError(std::string(cmd) + ": terrainStampRegion takes no region flag this settlement knows");
        for (const Lot& lot : r.layout.lots)
            if (lot.use == Use::Plaza)
                region.run({{rg, xzRing(lot.outline.pts)}, {id, key + "/plaza/" + std::to_string(lot.id)}});
        out["regions"] = region.calls;
    }
    json errors = json::array();
    for (const Stamper* s : {&road, &pad, &region})
        for (const json& e : s->errors)
            errors.push_back(e);
    out["errors"] = errors;
    for (const Stamper* s : {&road, &pad, &region})
        if (s->last.is_object() && s->last.contains("revision"))
            out["revision"] = s->last["revision"];
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Unreal

json ue(const glm::dvec3& p)
{
    const math::Vector v = math::toUnreal(math::Vector(p));
    return json::array({v.x, v.y, v.z});
}

json uePoints(std::span<const glm::dvec3> pts)
{
    json a = json::array();
    for (const glm::dvec3& p : pts)
        a.push_back(ue(p));
    return a;
}

json ueRing(const Polygon& p, double y)
{
    json a = json::array();
    for (const Vec2& q : p.pts)
        a.push_back(ue({q.x, y, q.y}));
    return a;
}

/** A smooth spline through the points (Catmull-Rom tangents), radius r at every point. */
curve::Spline smoothSpline(std::span<const glm::dvec3> pts, double radius)
{
    curve::Spline s;
    s.kind = curve::Kind::Bezier;
    s.points.assign(pts.begin(), pts.end());
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 prev = pts[i > 0 ? i - 1 : i], next = pts[i + 1 < n ? i + 1 : i];
        // A sixth of the neighbour span, shortened at sharp corners so handles never overshoot the next point.
        glm::dvec3 t = (next - prev) / 6.0;
        const double room = std::min(i > 0 ? glm::length(pts[i] - prev) : 1e30, i + 1 < n ? glm::length(next - pts[i]) : 1e30);
        if (glm::length(t) > 0.45 * room)
            t *= 0.45 * room / glm::length(t);
        s.handleL.push_back(pts[i] - t);
        s.handleR.push_back(pts[i] + t);
    }
    s.radius.assign(n, radius);
    return s;
}

std::shared_ptr<link::RpcClient> client(const char* cmd, std::initializer_list<const char*> features)
{
    try
    {
        link::Link::get().ensureConnected();
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": not connected to Unreal: " + e.what());
    }
    std::shared_ptr<link::RpcClient> c = link::Link::get().client();
    if (!c)
        throw CommandError(std::string(cmd) + ": not connected to Unreal");
    const json have = link::Link::get().status().value("features", json::array());
    for (const char* f : features)
        if (!have.is_array() || std::ranges::find(have, json(f)) == have.end())
            throw CommandError(std::string(cmd) + ": the LilaLink plugin in Unreal predates '" + f + "'; rebuild it from Lila's ue/LilaLink");
    return c;
}

json call(link::RpcClient& c, const char* method, const json& params, const char* cmd, int timeoutMs = 300000)
{
    link::RpcResult r = c.call(method, params, timeoutMs);
    if (!r.ok)
        throw CommandError(std::string(cmd) + ": " + method + ": " + r.error);
    return r.result;
}

/** ZoneGraph lanes of a cross-section, left to right looking along the road: sidewalks, driving lanes, median. */
json lanesOf(const RoadProfile& p)
{
    json lanes = json::array();
    auto lane = [&](double w, const char* dir, std::initializer_list<const char*> tags) {
        if (w <= 0.0)
            return;
        json t = json::array();
        for (const char* tag : tags)
            t.push_back(tag);
        lanes.push_back({{"width", w}, {"direction", dir}, {"tags", t}});
    };
    lane(p.sidewalk, "forward", {"Pedestrian"});
    const int back = p.oneWay ? 0 : p.lanes / 2, fwd = p.lanes - back;
    for (int k = 0; k < back; ++k)
        lane(p.laneWidth, "backward", {"Vehicle"});
    lane(p.median, "none", {});
    for (int k = 0; k < fwd; ++k)
        lane(p.laneWidth, "forward", {"Vehicle"});
    lane(p.sidewalk, "forward", {"Pedestrian"});
    if (lanes.empty())
        lane(std::max(p.width(), 100.0), "forward", {"Pedestrian"});
    return lanes;
}

/** The PCG data: lots and envelopes in Unreal space with their attributes, and road centrelines. */
json pcgData(const Rebuilt& r, const RoadNetwork& net)
{
    const Layout& l = r.layout;
    json lots = json::array();
    for (const Lot& lot : l.lots)
    {
        double lo = std::numeric_limits<double>::max();
        for (const Vec2& q : lot.outline.pts)
            lo = std::min(lo, r.ground.height(q));
        lots.push_back({{"id", lot.id},
                        {"block", lot.block},
                        {"outline", ueRing(lot.outline, lo)},
                        {"use", useName(lot.use)},
                        {"area", lot.area},
                        {"frontages", lot.frontages},
                        {"corner", lot.corner},
                        {"frontClass", className(lot.frontClass)},
                        {"seed", int64_t(lot.random * 1e6)}});
    }
    json envelopes = json::array();
    for (const Envelope& e : l.envelopes)
    {
        const Vec2 c = centroid(e.footprint);
        json sides = json::array();
        for (int t : e.footprint.tag)
            sides.push_back(t == 0 ? "street" : t == 1 ? "party" : "open");
        envelopes.push_back({{"lot", e.lot},
                             {"footprint", ueRing(e.footprint, e.ground)},
                             {"sides", sides},
                             {"center", ue({c.x, e.ground, c.y})},
                             {"front", ue({e.front.x, 0.0, e.front.y})},
                             {"base", e.base},
                             {"ground", e.ground},
                             {"top", e.top},
                             {"maxHeight", e.top - e.ground},
                             {"height", e.height},
                             {"floors", e.floors},
                             {"floorHeight", l.profile.floorHeight},
                             {"use", useName(e.use)},
                             {"roof", e.roof},
                             {"era", 0},
                             {"style", l.profile.name},
                             {"seed", int64_t(e.random * 1e6)}});
    }
    json roads = json::array();
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& s = net.lines[k];
        const RoadProfile& p = l.roads[size_t(s.profile)];
        roads.push_back({{"id", int(k)},
                         {"class", className(s.cls)},
                         {"profile", p.name},
                         {"width", s.width},
                         {"lanes", p.lanes},
                         {"surface", s.surface},
                         {"path", s.path},
                         {"points", uePoints(s.pts)}});
    }
    return {{"lots", lots}, {"envelopes", envelopes}, {"roads", roads}};
}

/** ZoneGraph shapes: a spline per road with its lane profile, a polygon per junction joining the road ends. */
json zoneData(const Rebuilt& r, const RoadNetwork& net)
{
    const Layout& l = r.layout;
    json profiles = json::object();
    json roads = json::array();
    for (const StreetLine& s : net.lines)
    {
        const RoadProfile& p = l.roads[size_t(s.profile)];
        profiles[p.name] = {{"lanes", lanesOf(p)}};
        json tags = json::array({s.cls == StreetClass::Path ? "Pedestrian" : "Vehicle"});
        if (s.surface)
            tags.push_back("Structure");
        roads.push_back({{"points", uePoints(s.pts)}, {"profile", p.name}, {"tags", tags}});
    }
    json junctions = json::array();
    for (const Junction& j : net.junctions)
    {
        json ends = json::array();
        for (const RoadEnd& e : j.ends)
        {
            const StreetLine& s = net.lines[size_t(e.line)];
            // ZoneGraph's lane profile points face into the shape, so the polygon's points point at the junction.
            ends.push_back({{"position", ue(e.p)}, {"forward", ue(-e.out)}, {"profile", l.roads[size_t(s.profile)].name}});
        }
        if (ends.size() >= 2)
            junctions.push_back({{"center", ue(j.p)}, {"points", ends}, {"tags", json::array({"Vehicle", "Crossing"})}});
    }
    return {{"profiles", profiles}, {"roads", roads}, {"junctions", junctions}};
}

std::string assetName(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out.empty() ? std::string("Settlement") : out;
}

/** Road ribbons cut into square cells of side cell, each recentred on its cell's middle. */
std::vector<std::pair<glm::dvec3, geo::Mesh>> ribbonChunks(const geo::Mesh& m, double cell)
{
    std::map<std::pair<int64_t, int64_t>, std::vector<geo::Index>> byCell;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        glm::dvec3 c(0.0);
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (geo::Index p : pts)
            c += m.position(p);
        c /= double(pts.size());
        byCell[{int64_t(std::floor(c.x / cell)), int64_t(std::floor(c.z / cell))}].push_back(f);
    }
    std::vector<std::pair<glm::dvec3, geo::Mesh>> out;
    for (const auto& [key, faces] : byCell)
    {
        const glm::dvec3 centre((double(key.first) + 0.5) * cell, 0.0, (double(key.second) + 0.5) * cell);
        geo::Mesh chunk;
        std::map<geo::Index, geo::Index> remap;
        for (geo::Index f : faces)
        {
            std::vector<geo::Index> ids;
            for (geo::Index p : m.facePoints(f))
            {
                auto [it, fresh] = remap.try_emplace(p, 0);
                if (fresh)
                    it->second = chunk.addPoint(m.position(p) - centre);
                ids.push_back(it->second);
            }
            chunk.addFace(ids);
        }
        out.push_back({centre, std::move(chunk)});
    }
    return out;
}

json sendRoads(link::RpcClient& c, const Rebuilt& r, const Args& a, const std::string& key, const std::string& label,
               const char* cmd)
{
    LILA_ZONE();

    const std::string mode = a.get<std::string>("roads", "landscape");
    json out = json::object();
    if (mode == "none")
        return out;
    if (mode == "meshes")
    {
        const MeshParts parts = toMeshes(r.layout, r.ground);
        const std::string folder = a.get<std::string>("folder", "/Game/Lila/Settlement") + "/" + assetName(label);
        const std::filesystem::path dir = link::cacheDir() / "settlement" / assetName(label);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        int k = 0;
        json groups = json::array();
        for (const auto& [centre, mesh] : ribbonChunks(parts.streets, std::max(1000.0, a.get<double>("cell", 51200.0))))
        {
            const std::string name = "SM_" + assetName(label) + "_Roads_" + std::to_string(k++);
            const std::filesystem::path file = dir / (name + ".fbx");
            fbx::ExportScene scene;
            fbx::ExportNode node;
            node.name = name;
            node.mesh = std::make_shared<const geo::Mesh>(mesh);
            scene.nodes.push_back(std::move(node));
            std::string err;
            if (!fbx::writeFbx(scene, file, fbx::ExportOptions{}, &err))
                throw CommandError(std::string(cmd) + ": writing " + file.string() + ": " + err);
            const json imported = call(c, "importAsset", link::importParams(file, {{"contentPath", folder + "/" + name}, {"kind", "static"}}), cmd);
            std::string asset = folder + "/" + name + "." + name;
            if (const json made = imported.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
                asset = made[0].get<std::string>();
            const std::string group = key + "/roads/" + std::to_string(k - 1);
            call(c, "placeInstances",
                 {{"group", group},
                  {"label", name},
                  {"mesh", asset},
                  {"mode", "actors"},
                  {"placements", json::array({{{"id", group}, {"name", name}, {"location", ue(centre)}, {"rotation", json::array({0, 0, 0, 1})}, {"scale", json::array({1, 1, 1})}}})}},
                 cmd);
            groups.push_back(group);
        }
        out["meshes"] = groups;
        return out;
    }
    // Splines: ground roads together (on the landscape, or as one actor), surface paths as their own actor.
    curve::Splines ground, surface;
    for (const StreetLine& s : streetLines(r.layout, r.ground))
        (s.surface ? surface : ground).push_back(smoothSpline(s.pts, 0.5 * s.width / 100.0));
    const bool landscape = mode == "landscape";
    if (!ground.empty())
    {
        json params = {{"id", key + "/roads"},
                       {"label", label + "_Roads"},
                       {"mode", landscape ? "landscape" : "actor"},
                       {"location", json::array({0, 0, 0})},
                       {"splines", link::splinePoints(ground, glm::dvec3(0.0))},
                       {"width", 100.0},
                       {"join", true},
                       {"deformTerrain", a.get<bool>("deformTerrain", true)},
                       {"folder", "Lila/Settlement"}};
        if (a.has("landscape"))
            params["landscape"] = a.get<std::string>("landscape");
        if (a.has("roadMesh"))
            params["mesh"] = {{"asset", a.get<std::string>("roadMesh")}, {"forwardAxis", "x"}};
        out["roads"] = call(c, "setSpline", params, cmd);
    }
    if (!surface.empty())
    {
        json params = {{"id", key + "/paths"},
                       {"label", label + "_Paths"},
                       {"mode", "actor"},
                       {"location", json::array({0, 0, 0})},
                       {"splines", link::splinePoints(surface, glm::dvec3(0.0))},
                       {"folder", "Lila/Settlement"}};
        if (a.has("roadMesh"))
            params["mesh"] = {{"asset", a.get<std::string>("roadMesh")}, {"forwardAxis", "x"}};
        out["paths"] = call(c, "setSpline", params, cmd);
    }
    return out;
}
} // namespace

void registerUnrealCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "settlementGrade",
        .doc = "Shapes a Terrain under the settlement through the terrain contract: streets as road features (graded, banked, "
               "bridged by the terrain's road solver), lots as building pads (padMode flatten, plinth, terraced or stilts) and "
               "squares as region stamps, each keyed by the settlement and the part, so grading again updates them. Paths on "
               "the walkable mesh are left alone. Needs the Terrain extension. Returns the calls made per kind, the terrain "
               "revision and any errors.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"terrain", "tr", Arg::String, nullptr, "The terrain to shape (default: the one the terrain commands pick)."},
                  {"streets", "st", Arg::Bool, true, "Streets as road features."},
                  {"lots", "lt", Arg::Bool, true, "Lots as building pads (the envelope's footprint where there is one)."},
                  {"plazas", "pz", Arg::Bool, true, "Squares as region stamps."},
                  {"padMode", "pm", Arg::String, "flatten", "How pads meet the slope.", {"flatten", "plinth", "terraced", "stilts"}}},
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementGrade";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(scene::Graph::current(), names[0], cmd);
            return grade(r, a, cmd);
        },
        .examples = {"settlementGrade('Settlement', terrain='Terrain', padMode='terraced')"},
    });

    ctx.registerCommand({
        .name = "settlementSendToUnreal",
        .doc = "Sends a settlement to the connected Unreal editor, updating what it sent before. Roads go as landscape splines "
               "(roads='landscape', joined at junctions, shaping the landscape), as spline actors (roads='splines', with spline "
               "meshes of roadMesh along them), as ribbon meshes cut into cells (roads='meshes') or not at all; paths on the "
               "walkable mesh always go as a spline actor. pcg=True writes a PCG data asset (Load PCG Data Asset reads it) with "
               "pins Lots (polygons, attributes id, use, area, frontages, corner, frontClass, seed), Envelopes (footprint "
               "polygons with MaxHeight, Floors, FloorHeight, Base, Ground, Use, Roof, Era, Style, Seed), EnvelopePoints (one "
               "point per envelope, bounds the footprint and height, the same attributes) and Roads (splines with class, "
               "profile, width, lanes). zoneGraph=True writes ZoneGraph shapes: a spline per road with a lane profile made from "
               "its cross-section (tags Vehicle, Pedestrian), a polygon per junction, then rebuilds the ZoneGraph. Returns what "
               "Unreal made.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"roads", "rd", Arg::String, "landscape", "How roads go.", {"landscape", "splines", "meshes", "none"}},
                  {"roadMesh", "rm", Arg::String, nullptr, "Static mesh asset laid along road splines, such as /Game/Roads/SM_Road."},
                  {"landscape", "ls", Arg::String, nullptr, "roads='landscape': the landscape's label (default the first one)."},
                  {"deformTerrain", "dt", Arg::Bool, true, "roads='landscape': the splines shape the landscape."},
                  {"cell", "ce", Arg::Float, 51200.0, "roads='meshes': side of the cells ribbons are cut into, cm.", {}, {}, 1000.0},
                  {"pcg", "pc", Arg::Bool, true, "Lots, envelopes and roads as a PCG data asset."},
                  {"zoneGraph", "zg", Arg::Bool, true, "Lanes as ZoneGraph shapes, for Mass traffic and crowds."},
                  {"folder", "f", Arg::String, "/Game/Lila/Settlement", "Content folder for assets."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementSendToUnreal";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(scene::Graph::current(), names[0], cmd);
            const bool pcg = a.get<bool>("pcg", true), zone = a.get<bool>("zoneGraph", true);
            std::shared_ptr<link::RpcClient> c =
                pcg || zone ? client(cmd, {link::kSplineFeature, "settlement"}) : client(cmd, {link::kSplineFeature});
            const std::string key = r.mesh.empty() ? r.modifier : r.mesh;
            std::string label = key;
            if (const size_t slash = label.find_last_of("|/"); slash != std::string::npos)
                label = label.substr(slash + 1);
            const RoadNetwork net = roadNetwork(r.layout, r.ground);
            json out = sendRoads(*c, r, a, key, label, cmd);
            const std::string folder = a.get<std::string>("folder", "/Game/Lila/Settlement");
            if (pcg)
            {
                json params = pcgData(r, net);
                params["id"] = key;
                params["asset"] = folder + "/PCG_" + assetName(label);
                params["label"] = label;
                out["pcg"] = call(*c, "setSettlementData", params, cmd);
            }
            if (zone)
            {
                json params = zoneData(r, net);
                params["id"] = key;
                params["label"] = label + "_Lanes";
                out["zoneGraph"] = call(*c, "setZoneGraph", params, cmd);
            }
            return out;
        },
        .examples = {"settlementSendToUnreal('Settlement', roads='landscape', pcg=True, zoneGraph=True)"},
    });
}
} // namespace settlement
