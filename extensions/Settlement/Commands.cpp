// settlementCreate builds a settlement as a geometry-nodes group anyone can open (a Settlement node fed by a mesh holding
// the outline, fixed streets and attractors, and optionally a ground mesh); settlementInfo measures and checks it;
// settlementExport writes its streets, lots and envelopes as JSON; settlementProfiles lists the styles.

#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using scene::Node;
using T = ArgType;

/** Exposed group inputs, in the order the attribute editor shows them: name, type, panel, min, soft max, description. */
struct Exposed
{
    const char* name;
    const char* type;
    const char* panel;
    double min;
    double softMax;
    const char* description;
};

constexpr Exposed kInputs[] = {
    {"profile", "string", "Style", 0, 0, "hamlet, streetVillage, medievalTown, usTown, manhattan, europeanCity, modernCity or scifiMegacity."},
    {"pattern", "string", "Style", 0, 0, "Empty for the profile's; else organic, grid, radial, linear or organicGrid."},
    {"overrides", "string", "Style", 0, 0, "JSON object of profile fields to change, such as {\"maxFloors\": 6}."},
    {"growth", "float", "Layout", 0, 1, "How far the settlement has grown, 0 to 1."},
    {"seed", "int", "Layout", 0, 0, "Another seed gives another settlement on the same site."},
    {"size", "float", "Layout", 100, 500000, "Side of the square used when the site has no outline, cm."},
    {"heightScale", "float", "Scale", 0, 4, "Multiplies every building's floors."},
    {"widthScale", "float", "Scale", 0.05, 4, "Multiplies every street width."},
    {"lotScale", "float", "Scale", 0.05, 4, "Multiplies lot frontage, depth and building depth."},
    {"showStreets", "bool", "Display", 0, 0, "Show street ribbons."},
    {"showLots", "bool", "Display", 0, 0, "Show lot outlines."},
    {"showBuildings", "bool", "Display", 0, 0, "Show building envelopes."},
};

std::vector<FlagDef> layoutFlags()
{
    const Options o;
    return {
        {"profile", "pf", T::String, "medievalTown", "Style.", profileNames()},
        {"pattern", "pt", T::String, "", "Override the profile's pattern: organic, grid, radial, linear, organicGrid."},
        {"overrides", "ov", T::String, "", "JSON object of profile fields to change."},
        {"growth", "gr", T::Float, o.growth, "How far the settlement has grown, 0 to 1."},
        {"seed", "s", T::Int, int64_t(o.seed), "Random seed."},
        {"size", "sz", T::Float, 40000.0, "Side of the square used without an outline, cm."},
        {"heightScale", "hs", T::Float, o.heightScale, "Multiplies building floors."},
        {"widthScale", "ws", T::Float, o.widthScale, "Multiplies street widths."},
        {"lotScale", "ls", T::Float, o.lotScale, "Multiplies lot sizes."},
        {"showStreets", "ss", T::Bool, true, "Show street ribbons."},
        {"showLots", "sl", T::Bool, true, "Show lot outlines."},
        {"showBuildings", "sb", T::Bool, true, "Show building envelopes."},
    };
}

Options optionsFromJson(const json& values)
{
    auto num = [&](const char* id) { return values.at(id).get<double>(); };
    auto integer = [&](const char* id) { return int64_t(std::llround(values.at(id).get<double>())); };
    auto text = [&](const char* id) { return values.at(id).is_string() ? values.at(id).get<std::string>() : std::string(); };
    try
    {
        return optionsFrom(num, integer, text);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("settlement: ") + e.what());
    }
}

std::string uniqueGroupName(const std::string& base)
{
    json existing = ops::execute("gnGroups", json::object());
    auto taken = [&](const std::string& n) {
        for (const json& e : existing)
            if ((e.is_string() && e.get<std::string>() == n) || (e.is_object() && e.value("name", std::string()) == n))
                return true;
        return false;
    };
    std::string name = base;
    for (int k = 2; taken(name); ++k)
        name = base + std::to_string(k);
    return name;
}

std::string identifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Settlement" + out;
    return out;
}

glm::dvec3 point3(const json& p, const char* what)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(std::string("settlementCreate: each point in ") + what + " is [x, z] or [x, y, z]");
    if (p.size() == 2)
        return {p[0].get<double>(), 0.0, p[1].get<double>()};
    return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
}

/** The host mesh: the outline as a loop, fixed streets as lines, the centre and attractors as loose points. */
geo::Mesh siteMesh(const json& outline, const json& streets, const json& points)
{
    geo::Mesh m;
    auto addLine = [&](const json& pts, bool closed, const char* what) {
        std::vector<geo::Index> ids;
        for (const json& p : pts)
            ids.push_back(m.addPoint(point3(p, what)));
        for (size_t k = 0; k + 1 < ids.size(); ++k)
            m.addEdge(ids[k], ids[k + 1]);
        if (closed && ids.size() > 2)
            m.addEdge(ids.back(), ids.front());
    };
    if (outline.is_array() && outline.size() >= 3)
        addLine(outline, true, "outline");
    if (streets.is_array())
        for (const json& s : streets)
            if (s.is_array() && s.size() >= 2)
                addLine(s, false, "streets");
    if (points.is_array())
        for (const json& p : points)
            m.addPoint(point3(p, "attractors"));
    return m;
}

/** The Settlement modifier in a mesh's stack (the modifier itself may be named). */
Node* settlementModifier(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    auto isSettlement = [](const Node& m) { return m.type().attrIndex("lotScale") >= 0 && m.type().attrIndex("profile") >= 0; };
    if (isSettlement(*n))
        return n;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && isSettlement(*mod))
            return mod;
    throw CommandError(std::string(cmd) + ": " + n->path() + " has no settlement (make one with settlementCreate)");
}

json modifierValues(const Node& mod)
{
    json values = json::object();
    for (const Exposed& e : kInputs)
        values[e.name] = ops::execute("getAttr", {{"args", {mod.path() + "." + e.name}}});
    values["ground"] = ops::execute("getAttr", {{"args", {mod.path() + ".ground"}}});
    return values;
}

/** A mesh object's evaluated mesh in world space. */
Ground groundOf(Graph& g, const std::string& name, double planeY, const char* cmd)
{
    if (name.empty())
        return Ground::plane(planeY);
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": ground '" + name + "' is not a mesh");
    const scene::PlugValue mv = g.value({n->id(), scene::MeshNode::kOutMesh, -1});
    const scene::PlugValue xv = g.value({n->id(), scene::Transform::kWorldMatrix, -1});
    const scene::MeshPtr* mesh = std::get_if<scene::MeshPtr>(&mv);
    if (!mesh || !*mesh || (*mesh)->numFaces() == 0)
        throw CommandError(std::string(cmd) + ": ground '" + name + "' has no faces");
    geo::Mesh world = **mesh;
    if (const glm::dmat4* xf = std::get_if<glm::dmat4>(&xv))
        gn::transformMesh(world, *xf);
    if (world.hasDeleted())
        world.compact();
    return Ground::fromMesh(world, 200.0);
}

/** Rebuilds the layout a settlement modifier shows, for measuring and export. */
struct Rebuilt
{
    Layout layout;
    Ground ground;
    Options options;
    json values;
    std::string mesh, modifier;
};

Rebuilt rebuild(Graph& g, const std::string& name, const char* cmd)
{
    Node* mod = settlementModifier(g, name, cmd);
    const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id());
    Node* mesh = meshId != scene::kNoNode ? g.node(meshId) : g.find(name);
    Rebuilt r;
    r.values = modifierValues(*mod);
    r.options = optionsFromJson(r.values);
    double planeY = 0.0;
    Site site;
    if (auto base = mesh ? scene::modifiers::cage(g, mesh->id()) : nullptr)
    {
        gn::Geometry geom;
        geom.mesh = base;
        site = siteFrom(geom, &planeY);
    }
    site.size = std::max(100.0, r.values.at("size").get<double>());
    r.ground = groundOf(g, r.values.at("ground").is_string() ? r.values.at("ground").get<std::string>() : std::string(), planeY, cmd);
    try
    {
        r.layout = build(site, r.ground, r.options);
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    r.mesh = mesh ? mesh->path() : std::string();
    r.modifier = mod->path();
    return r;
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    std::vector<FlagDef> flags = {
        {"outline", "o", T::List, nullptr, "Closed outline, [[x, z], ...] or [[x, y, z], ...] in cm. Omit for a square of size."},
        {"streets", "st", T::List, nullptr, "Streets to keep as they are: a list of polylines."},
        {"attractors", "at", T::List, nullptr, "Points: the first is the centre, the rest are places main roads run to (bridge, harbour, gate)."},
        {"ground", "gd", T::String, "", "Mesh the settlement stands on (world space). Empty for flat ground."},
        {"name", "n", T::String, "Settlement", "Name of the mesh made to hold the site."},
    };
    for (FlagDef& f : layoutFlags())
        flags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "settlementCreate",
        .doc = "Lay out a settlement: arterial roads routed over the ground from the centre to gates and attractors, streets "
               "grown, gridded or ringed by the profile, blocks between them, lots along the streets and a building envelope "
               "on each lot (footprint, floors, height, roof: the space a building may fill; buildings are made elsewhere).\n\n"
               "Give outline= (and optionally streets=, attractors=), or a mesh whose loose-edge loop is the outline, open "
               "loose-edge lines fixed streets and loose points the centre and attractors. The settlement is a geometry-nodes "
               "modifier around a Settlement node; every layout flag becomes a keyable input on it, so change it later with "
               "setAttr on '<modifier>.<flag>' and check it with settlementInfo. Returns the mesh, modifier, group, stats, "
               "metrics and checks. Lengths are cm, Y up.",
        .positionalDoc = "mesh holding the site (optional)",
        .flags = flags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            json values = json::object();
            for (const Exposed& e : kInputs)
                values[e.name] = a.flags[e.name];
            const Options o = optionsFromJson(values);
            const std::string groundName = a.get<std::string>("ground", "");
            if (!groundName.empty())
                groundOf(g, groundName, 0.0, "settlementCreate");

            std::string host;
            if (!a.positional.empty())
            {
                Node* mesh = g.find(a.positional[0].get<std::string>());
                if (!mesh || !mesh->type().isA("mesh"))
                    throw CommandError("settlementCreate: " + a.positional[0].dump() + " is not a mesh");
                host = mesh->path();
            }
            else
            {
                json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()},
                                                        {"name", a.get<std::string>("name", "Settlement")}});
                host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                Node* mesh = g.find(host);
                g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1},
                           scene::MeshPtr(std::make_shared<geo::Mesh>(siteMesh(a.flags.value("outline", json()), a.flags.value("streets", json()),
                                                                               a.flags.value("attractors", json())))));
            }

            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Settlement");
            json inputs = json::array();
            json nodes = json::array({{{"name", "Settle"}, {"type", "Settlement"}}});
            json links = json::array({"GroupInput.geometry -> Settle.site", "Settle.geometry -> GroupOutput.geometry"});
            for (const Exposed& e : kInputs)
            {
                json in = {{"name", e.name}, {"type", e.type}, {"default", values[e.name]}, {"panel", e.panel}, {"description", e.description}};
                if (std::string_view(e.type) == "float" || std::string_view(e.type) == "int")
                {
                    in["min"] = e.min;
                    if (e.softMax > e.min)
                        in["softMax"] = e.softMax;
                }
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> Settle." + e.name);
            }
            // The ground's name rides along as an input of its own, so settlementInfo measures on the same ground.
            inputs.push_back({{"name", "ground"}, {"type", "string"}, {"default", groundName}, {"panel", "Layout"},
                              {"description", "Mesh the settlement stands on, set by settlementCreate."}});
            if (!groundName.empty())
            {
                nodes.push_back({{"name", "Ground"}, {"type", "ObjectInfo"}, {"props", {{"object", groundName}, {"transformSpace", "world"}}}});
                links.push_back("Ground.geometry -> Settle.ground");
            }
            json spec = {{"name", group},
                         {"description", "Lays out streets, lots and building envelopes inside the mesh's outline."},
                         {"inputs", inputs},
                         {"nodes", nodes},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            json mod = ops::execute("gnApply", {{"args", {host, group}}});

            Rebuilt r = rebuild(g, host, "settlementCreate");
            json out = stats(r.layout);
            out["metrics"] = metrics(r.layout);
            out["checks"] = checks(r.layout, r.ground, r.options);
            out["mesh"] = host;
            out["modifier"] = mod;
            out["group"] = group;
            return out;
        },
        .examples = {"settlementCreate(profile='medievalTown', size=60000)",
                     "town = settlementCreate(outline=[[-40000, -30000], [45000, -25000], [38000, 35000], [-35000, 30000]],\n"
                     "                        attractors=[[0, 0], [40000, 0]], ground='Terrain', profile='europeanCity')\n"
                     "setAttr(town['modifier'] + '.overrides', '{\"maxFloors\": 9}')\n"
                     "settlementInfo(town['mesh'])['checks']"},
    });

    ctx.registerCommand({
        .name = "settlementInfo",
        .doc = "Measures and checks a settlement.\n\n"
               "Returns stats (street metres by class, blocks, lots by use, buildings, floors), metrics comparable with real "
               "cities (street length and density, orientation entropy and order after Boeing 2019, intersections, dead-end "
               "ratio, degree histogram, block areas), checks (components: separate street networks, crossings: streets that "
               "cross without a junction, steepStreets, sharpJunctions, landlockedLots, badEnvelopes, ok), the current "
               "settings and the resolved profile.",
        .positionalDoc = "mesh or its settlement modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("settlementInfo: give one mesh");
            Rebuilt r = rebuild(g, names[0], "settlementInfo");
            json out = stats(r.layout);
            out["metrics"] = metrics(r.layout);
            out["checks"] = checks(r.layout, r.ground, r.options);
            out["settings"] = r.values;
            out["resolvedProfile"] = toJson(r.layout.profile);
            out["mesh"] = r.mesh;
            out["modifier"] = r.modifier;
            return out;
        },
        .examples = {"settlementInfo('Settlement')['metrics']['orientationOrder']"},
    });

    ctx.registerCommand({
        .name = "settlementExport",
        .doc = "Writes a settlement's layout as JSON (format lila.settlement): street nodes and edges with class, width and "
               "era; blocks; lots with outline, use, frontages and the street class each side faces; building envelopes with "
               "footprint, side kinds (street, party, open), floors, floor height, base, ground and top heights, roof and "
               "facing. 2D points are [x, z] in cm, heights world Y. This is what building generators (Unreal PCG, Masonry, "
               "library placement) read. Returns the path and counts.",
        .positionalDoc = "mesh or its settlement modifier (default the selection)",
        .flags = {{"path", "p", T::String, "", "File to write (.json)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("settlementExport: give one mesh");
            const std::string path = a.get<std::string>("path", "");
            if (path.empty())
                throw CommandError("settlementExport: path= required");
            Rebuilt r = rebuild(g, names[0], "settlementExport");
            const json data = toJson(r.layout);
            std::error_code ec;
            if (auto dir = std::filesystem::path(path).parent_path(); !dir.empty())
                std::filesystem::create_directories(dir, ec);
            std::ofstream f(std::filesystem::path(path), std::ios::binary);
            if (!f)
                throw CommandError("settlementExport: cannot write " + path);
            f << data.dump(1);
            return {{"path", path},
                    {"streets", int64_t(data["streets"].size())},
                    {"lots", int64_t(data["lots"].size())},
                    {"envelopes", int64_t(data["envelopes"].size())}};
        },
        .examples = {"settlementExport('Settlement', path='C:/work/town.json')"},
    });

    ctx.registerCommand({
        .name = "settlementProfiles",
        .doc = "The built-in styles with every field, as {name: profile}. Any field can be changed per settlement through "
               "the overrides input (a JSON object of the fields to change).",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::object();
            for (const std::string& n : profileNames())
            {
                Profile p;
                builtinProfile(n, &p);
                out[n] = toJson(p);
            }
            return out;
        },
        .examples = {"settlementProfiles()['manhattan']['maxFloors']"},
    });
}
} // namespace settlement
