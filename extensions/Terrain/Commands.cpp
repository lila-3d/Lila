// Terrain commands. A terrain is a mesh whose geometry-nodes modifier makes a heightfield; the commands build that group,
// read what it made (in world space) and bake it to a file.

#include "Erosion.h"
#include "Export.h"
#include "Inputs.h"
#include "Terrain.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

using namespace lila;

namespace terrain
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;

/** A terrain as the commands see it: its mesh, the modifier making it, and its heightfield in world space. */
struct Found
{
    Node* mesh = nullptr;
    Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> local;
    Heightfield world;
};

Found findTerrain(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    g.evaluate();
    Found f;
    if (gn::isGnModifier(*n))
    {
        f.modifier = n;
        f.local = gn::modifierHeightfield(n->id());
        const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
        f.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
    }
    else
    {
        f.mesh = n;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend() && !f.local; ++it)
            if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()))
                {
                    f.modifier = m;
                    f.local = std::move(h);
                }
    }
    if (!f.local || f.local->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    gn::Geometry geom;
    geom.heightfield = f.local;
    const glm::dmat4 xf = f.mesh ? f.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    gn::Geometry w = gn::transformed(geom, xf);
    if (!w.heightfield)
        throw CommandError(cmd + ": " + n->path() + " is rotated; terrain commands need it upright and unrotated");
    f.world = *w.heightfield;
    return f;
}

std::string uniqueGroupName(const std::string& base)
{
    const json existing = ops::execute("gnGroups", json::object());
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
        out = "Terrain" + out;
    return out;
}

json vec2(glm::dvec2 v) { return json::array({v.x, v.y}); }

/** What a terrain is, for agents: grid, area, height range and masks. */
json describe(const Found& f)
{
    const Heightfield& h = f.world;
    const glm::dvec2 r = h.range(), e = h.extent();
    json masks = json::object();
    for (const auto& [name, values] : h.masks)
    {
        if (values.empty())
            continue;
        auto [lo, hi] = std::minmax_element(values.begin(), values.end());
        double sum = 0.0;
        for (float v : values)
            sum += v;
        masks[name] = {{"min", *lo}, {"max", *hi}, {"mean", sum / double(values.size())}};
    }
    return {{"mesh", f.mesh ? f.mesh->path() : std::string()},
            {"modifier", f.modifier ? f.modifier->path() : std::string()},
            {"resolution", json::array({h.nx, h.nz})},
            {"cellSize", h.cell},
            {"size", vec2(e)},
            {"min", vec2(h.origin)},
            {"max", vec2(h.origin + e)},
            {"center", vec2(h.origin + e * 0.5)},
            {"heightRange", vec2(r)},
            {"samples", int64_t(h.size())},
            {"masks", masks}};
}

json sampleAt(const Heightfield& h, double x, double z, const std::vector<std::string>& masks)
{
    const glm::dvec3 n = h.normalAt(x, z);
    json s = {{"x", x},
              {"z", z},
              {"height", h.heightAt(x, z)},
              {"normal", json::array({n.x, n.y, n.z})},
              {"slope", std::acos(std::clamp(n.y, -1.0, 1.0)) * 57.29577951308232},
              {"inside", h.contains(x, z)}};
    for (const std::string& m : masks)
        s[m] = h.maskAt(m, x, z);
    return s;
}

glm::dvec2 xzOf(const json& p, const std::string& cmd)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(cmd + ": points are [x, z] or [x, y, z]");
    return p.size() == 2 ? glm::dvec2(p[0].get<double>(), p[1].get<double>()) : glm::dvec2(p[0].get<double>(), p[2].get<double>());
}

fs::path bakeDir()
{
    const fs::path& scene = io::currentPath();
    if (!scene.empty())
        return scene.parent_path() / (scene.stem().string() + "_terrain");
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain";
    return fs::temp_directory_path() / "lila_terrain";
}

/** Exposed inputs of a new terrain's group, in attribute editor order: name, type, panel, min, soft max, description. */
struct Exposed
{
    const char* name;
    const char* type;
    const char* panel;
    double min;
    double softMax;
    const char* node;
    const char* socket;
    const char* description;
};

constexpr Exposed kInputs[] = {
    {"sizeX", "float", "Grid", 100, 1000000, "Grid", "sizeX", "Width along X, cm."},
    {"sizeZ", "float", "Grid", 100, 1000000, "Grid", "sizeZ", "Depth along Z, cm."},
    {"resolution", "int", "Grid", 2, 4097, "Grid", "resolution", "Samples along the longer side."},
    {"amplitude", "float", "Shape", 0, 100000, "Noise", "amplitude", "Height of the tallest features, cm."},
    {"featureSize", "float", "Shape", 100, 200000, "Noise", "featureSize", "Size of the largest features, cm."},
    {"octaves", "int", "Shape", 1, 12, "Noise", "octaves", "Layers of detail."},
    {"roughness", "float", "Shape", 0, 1, "Noise", "roughness", "How strong each smaller layer is."},
    {"ridged", "float", "Shape", 0, 1, "Noise", "ridged", "0 rolling hills, 1 sharp ridges."},
    {"warp", "float", "Shape", 0, 2, "Noise", "warp", "Bends shapes so they don't line up."},
    {"seed", "int", "Shape", 0, 0, "Noise", "seed", "Another seed gives other shapes."},
    {"erosion", "int", "Erosion", 0, 300, "Erode", "iterations", "Erosion steps: rivers carve valleys; 0 is off, more is older."},
    {"erosionStrength", "float", "Erosion", 0, 0.005, "Erode", "strength", "How fast rivers cut."},
    {"uplift", "float", "Erosion", 0, 100, "Erode", "uplift", "Rise per step, cm, building ranges as they wear."},
    {"talusAngle", "float", "Erosion", 5, 60, "Erode", "talusAngle", "Slopes steeper than this shed scree, degrees."},
};
} // namespace

void registerCommands(ext::Context& ctx)
{
    const NoiseParams np;
    ctx.registerCommand({
        .name = "terrainCreate",
        .doc = "Make a terrain: a mesh whose geometry-nodes modifier builds a heightfield, starting as fractal noise (flat=True for "
               "level ground).\n\n"
               "The size, resolution and noise settings become keyable inputs on the modifier, so change the terrain later with "
               "setAttr on '<modifier>.<flag>' and read it back with terrainInfo or terrainSample. Open the group to add Terrain "
               "nodes. Lengths are cm; Y is up. Returns the mesh, modifier, group and terrainInfo's stats.",
        .flags = {{"size", "sz", T::List, json::array({100000.0, 100000.0}), "[width along X, depth along Z], cm."},
                  {"resolution", "r", T::Int, int64_t(513), "Samples along the longer side.", {}, {}, 2.0, 16385.0},
                  {"center", "c", T::Vec3, json::array({0.0, 0.0, 0.0}), "Centre; Y is the base height."},
                  {"flat", "f", T::Bool, false, "Level ground, no noise."},
                  {"amplitude", "a", T::Float, np.amplitude, "Height of the tallest features, cm.", {}, {}, 0.0},
                  {"featureSize", "fs", T::Float, np.featureSize, "Size of the largest features, cm.", {}, {}, 0.0},
                  {"octaves", "o", T::Int, int64_t(np.octaves), "Layers of detail.", {}, {}, 1.0, 16.0},
                  {"roughness", "ro", T::Float, np.roughness, "0 to 1: how strong each smaller layer is.", {}, {}, 0.0, 1.0},
                  {"ridged", "ri", T::Float, np.ridged, "0 rolling hills, 1 sharp ridges.", {}, {}, 0.0, 1.0},
                  {"warp", "w", T::Float, np.warp, "Domain warp, a fraction of featureSize.", {}, {}, 0.0},
                  {"seed", "s", T::Int, int64_t(0), "Random seed."},
                  {"erosion", "e", T::Int, int64_t(0), "Erosion steps: rivers carve valleys; 0 is off, 50 to 150 looks natural.", {}, {}, 0.0, 10000.0},
                  {"erosionStrength", "es", T::Float, ErosionParams().strength, "How fast rivers cut.", {}, {}, 0.0},
                  {"uplift", "u", T::Float, 0.0, "Rise per erosion step, cm."},
                  {"talusAngle", "ta", T::Float, ErosionParams().talusAngle, "Slopes steeper than this shed scree, degrees.", {}, {}, 1.0, 89.0},
                  {"name", "n", T::String, "Terrain", "Name of the mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const json size = a.flags["size"];
            if (!size.is_array() || size.size() != 2)
                throw CommandError("terrainCreate: size is [width, depth]");
            json values = {{"sizeX", size[0]},
                           {"sizeZ", size[1]},
                           {"resolution", a.get<int64_t>("resolution", 513)},
                           {"amplitude", a.flags["amplitude"]},
                           {"featureSize", a.flags["featureSize"]},
                           {"octaves", a.flags["octaves"]},
                           {"roughness", a.flags["roughness"]},
                           {"ridged", a.flags["ridged"]},
                           {"warp", a.flags["warp"]},
                           {"seed", a.flags["seed"]},
                           {"erosion", a.flags["erosion"]},
                           {"erosionStrength", a.flags["erosionStrength"]},
                           {"uplift", a.flags["uplift"]},
                           {"talusAngle", a.flags["talusAngle"]}};
            const bool flat = a.get<bool>("flat", false);
            if (flat)
                values["amplitude"] = 0.0;

            json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", a.get<std::string>("name", "Terrain")}});
            const std::string host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Terrain");

            json inputs = json::array();
            json links = json::array({"Grid.heightfield -> Noise.heightfield", "Noise.heightfield -> Erode.heightfield",
                                      "Erode.heightfield -> GroupOutput.geometry"});
            for (const Exposed& e : kInputs)
            {
                json in = {{"name", e.name}, {"type", e.type}, {"default", values[e.name]}, {"panel", e.panel}, {"description", e.description}, {"min", e.min}};
                if (e.softMax > e.min)
                    in["softMax"] = e.softMax;
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> " + e.node + "." + e.socket);
            }
            json spec = {{"name", group},
                         {"description", "A terrain: a heightfield grid shaped by fractal noise. Add Terrain nodes between Noise and the output."},
                         {"inputs", inputs},
                         {"nodes", json::array({{{"name", "Grid"}, {"type", "HeightfieldGrid"}, {"values", {{"center", a.flags["center"]}}}},
                                                {{"name", "Noise"}, {"type", "TerrainNoise"}},
                                                {{"name", "Erode"}, {"type", "ErodeTerrain"}}})},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            json mod = ops::execute("gnApply", {{"args", {host, group}}});

            json out = describe(findTerrain(g, host, "terrainCreate"));
            out["group"] = group;
            return out;
        },
        .examples = {"t = terrainCreate(size=[200000, 200000], resolution=1025, amplitude=40000, ridged=0.8)",
                     "terrainCreate(flat=True, size=[50000, 50000], name='Plain')"},
    });

    ctx.registerCommand({
        .name = "terrainInfo",
        .doc = "What a terrain is: resolution, cell size, the area it covers in world X and Z (min, max, center, size), its height "
               "range and each mask's min, max and mean. Lengths are cm.",
        .positionalDoc = "terrain mesh or its modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainInfo: give one terrain");
            return describe(findTerrain(Graph::current(), names[0], "terrainInfo"));
        },
        .examples = {"terrainInfo('Terrain')['heightRange']"},
    });

    ctx.registerCommand({
        .name = "terrainSample",
        .doc = "Heights under points, or a profile along a path.\n\n"
               "points= reads each point; path= walks the polyline every step cm and adds the distance along it and the grade "
               "(rise over run, percent) from the previous sample. Each sample has x, z, height, normal, slope (degrees), inside, "
               "and the masks named by masks=. World space, cm.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"points", "p", T::List, nullptr, "[[x, z] or [x, y, z], ...]."},
                  {"path", "pa", T::List, nullptr, "Polyline [[x, z] or [x, y, z], ...] to profile."},
                  {"step", "st", T::Float, 0.0, "Spacing along path, cm; 0 uses the terrain's cell size.", {}, {}, 0.0},
                  {"masks", "m", T::List, json::array(), "Mask names to read."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSample: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainSample");
            std::vector<std::string> masks;
            for (const json& m : a.flags.value("masks", json::array()))
                masks.push_back(m.get<std::string>());
            json out = json::array();
            if (a.has("points"))
                for (const json& p : a.flags["points"])
                {
                    const glm::dvec2 q = xzOf(p, "terrainSample");
                    out.push_back(sampleAt(f.world, q.x, q.y, masks));
                }
            else if (a.has("path"))
            {
                std::vector<glm::dvec2> pts;
                for (const json& p : a.flags["path"])
                    pts.push_back(xzOf(p, "terrainSample"));
                if (pts.size() < 2)
                    throw CommandError("terrainSample: path takes at least two points");
                const double step = a.get<double>("step", 0.0) > 0.0 ? a.get<double>("step", 0.0) : f.world.cell;
                std::vector<double> at(pts.size(), 0.0);
                for (size_t k = 1; k < pts.size(); ++k)
                    at[k] = at[k - 1] + glm::length(pts[k] - pts[k - 1]);
                const double total = at.back();
                const size_t count = size_t(std::ceil(total / step - 1e-9)) + 1;
                if (count > 1000000)
                    throw CommandError("terrainSample: over a million samples; use a larger step");
                double prevHeight = 0.0, prevAlong = 0.0;
                size_t seg = 1;
                for (size_t i = 0; i < count; ++i)
                {
                    const double d = std::min(total, double(i) * step);
                    while (seg + 1 < pts.size() && at[seg] < d)
                        ++seg;
                    const double len = at[seg] - at[seg - 1];
                    const glm::dvec2 q = len > 1e-12 ? pts[seg - 1] + (pts[seg] - pts[seg - 1]) * ((d - at[seg - 1]) / len) : pts[seg];
                    json smp = sampleAt(f.world, q.x, q.y, masks);
                    const double h = smp["height"].get<double>();
                    smp["distance"] = d;
                    smp["grade"] = i == 0 || d - prevAlong < 1e-9 ? 0.0 : (h - prevHeight) / (d - prevAlong) * 100.0;
                    prevHeight = h;
                    prevAlong = d;
                    out.push_back(std::move(smp));
                }
            }
            else
                throw CommandError("terrainSample: give points= or path=");
            return out;
        },
        .examples = {"terrainSample('Terrain', points=[[0, 0], [5000, -2000]])",
                     "profile = terrainSample('Terrain', path=[[-40000, 0], [40000, 0]], step=500)\nmax(abs(s['grade']) for s in profile)"},
    });

    ctx.registerCommand({
        .name = "terrainToMesh",
        .doc = "A plain mesh copy of the terrain as it is now, in world space: quads facing up, every step-th sample, uv set 0 over "
               "the whole terrain, masks as point attributes. The terrain is left alone. Returns the new mesh, its point and face counts.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"step", "st", T::Int, int64_t(1), "Keep every step-th sample.", {}, {}, 1.0, 1024.0}, {"name", "n", T::String, "", "Name; defaults to <terrain>_mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainToMesh: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainToMesh");
            geo::HeightfieldMeshOptions o;
            o.step = int(std::clamp<int64_t>(a.get<int64_t>("step", 1), 1, 1024));
            geo::Mesh m = geo::heightfieldToMesh(f.world, o);
            const size_t points = m.numPoints(), faces = m.numFaces();
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = (f.mesh ? f.mesh->name() : std::string("Terrain")) + "_mesh";
            json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
            const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            Node* mesh = g.find(path);
            g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            return {{"mesh", path}, {"points", int64_t(points)}, {"faces", int64_t(faces)}};
        },
        .examples = {"terrainToMesh('Terrain', step=4)"},
    });

    ctx.registerCommand({
        .name = "terrainBake",
        .doc = "Collapse a terrain's node graph into a file: the heightfield, masks and placement are written to a .lilahf next to "
               "the scene (<scene>_terrain/, else the workspace's terrain/ folder), and the terrain's group is replaced by a Read "
               "Heightfield node loading it. Edits made afterwards start from the baked result. Returns the file and terrainInfo's stats.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"file", "f", T::String, "", "Where to write; defaults as above."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainBake: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainBake");
            Node* groupNode = gn::groupOfModifier(g, *f.modifier);
            if (!groupNode)
                throw CommandError("terrainBake: " + f.modifier->path() + " has no group");
            const std::string group = gn::readGroup(*groupNode).name;
            const std::string modifier = f.modifier->path();
            fs::path file = a.get<std::string>("file", "");
            if (file.empty())
                file = bakeDir() / (identifier(f.mesh ? f.mesh->name() : group) + ".lilahf");
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            try
            {
                writeHeightfield(file, *f.local);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainBake: ") + e.what());
            }
            json spec = {{"name", group},
                         {"description", "A baked terrain, read from a file. Add Terrain nodes between Read and the output."},
                         {"inputs", json::array({{{"name", "file"}, {"type", "string"}, {"default", file.generic_string()}, {"description", "The baked heightfield."}}})},
                         {"nodes", json::array({{{"name", "Read"}, {"type", "ReadHeightfield"}}})},
                         {"links", json::array({"GroupInput.file -> Read.file", "Read.heightfield -> GroupOutput.geometry"})}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", true}});
            ops::execute("setAttr", {{"args", {modifier + ".file", file.generic_string()}}});
            json out = describe(findTerrain(g, names[0], "terrainBake"));
            out["file"] = file.generic_string();
            return out;
        },
        .examples = {"terrainBake('Terrain')['file']"},
    });
    ctx.registerCommand({
        .name = "terrainExport",
        .doc = "Write a terrain to files for an engine or another tool.\n\n"
               "target='unreal' resamples to a landscape size Unreal accepts and writes <name>.r16 (16-bit heights laid out for "
               "Landscape import), <name>.png, one 8-bit <name>_<mask>.raw weightmap per mask and <name>.json with the section "
               "size, sections per component, component counts, actor location and scale (Unreal axes, cm) to import it with. "
               "target='files' writes <name>.exr (heights in cm), <name>.png and <name>.r16 (16-bit over the height range), a "
               "PNG per mask and <name>.json. Returns the settings and files.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"target", "t", T::String, "unreal", "unreal or files.", {"unreal", "files"}},
                  {"folder", "f", T::String, "", "Output folder; defaults to the bake folder's export/."},
                  {"name", "n", T::String, "", "File name stem; defaults to the terrain's name."},
                  {"pad", "p", T::Bool, false, "unreal: keep the cell size and grow the landscape past the terrain, instead of resampling."},
                  {"minScaleZ", "sz", T::Float, 1.0, "unreal: smallest Z scale (100 is Unreal's default).", {}, {}, 0.01}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainExport: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainExport");
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = identifier(f.mesh ? f.mesh->name() : std::string("Terrain"));
            fs::path folder = a.get<std::string>("folder", "");
            if (folder.empty())
                folder = bakeDir() / "export";
            try
            {
                if (a.get<std::string>("target", "unreal") == "files")
                    return exportFiles(f.world, folder, name);
                UnrealOptions o;
                o.pad = a.get<bool>("pad", false);
                o.minScaleZ = std::max(0.01, a.get<double>("minScaleZ", 1.0));
                return exportUnreal(f.world, folder, name, o);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainExport: ") + e.what());
            }
        },
        .examples = {"s = terrainExport('Terrain', target='unreal')\ns['resolution'], s['scale'], s['files']['height']"},
    });

    ctx.registerCommand({
        .name = "terrainSendToUnreal",
        .doc = "Send a terrain to the connected Unreal editor as a Landscape, or update the one it sent before.\n\n"
               "Writes the terrain as terrainExport(target='unreal') does, into the link's cache, then has LilaLink build a landscape "
               "of that size and layout at the matching place and scale. Lila's heights go to an edit layer named Lila, so sculpting "
               "and splines on other layers in Unreal survive later sends; a terrain whose size or layout changed is rebuilt. "
               "gridSize sets the World Partition grid in a partitioned level. Returns the landscape and the export settings.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"material", "m", T::String, "", "Landscape material asset path, such as /Game/Terrain/M_Landscape."},
                  {"gridSize", "g", T::Int, int64_t(0), "World Partition grid size in components; 0 leaves it.", {}, {}, 0.0},
                  {"pad", "p", T::Bool, false, "Keep the cell size and grow the landscape past the terrain, instead of resampling."},
                  {"timeout", "to", T::Float, 300.0, "Seconds to wait for Unreal.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSendToUnreal: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainSendToUnreal");
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendToUnreal: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            if (!client || !features.is_array() || std::find(features.begin(), features.end(), json("landscape")) == features.end())
                throw CommandError("terrainSendToUnreal: the LilaLink plugin in Unreal predates landscapes; rebuild it from Lila's ue/LilaLink");

            const std::string name = identifier(f.mesh ? f.mesh->name() : std::string("Terrain"));
            UnrealOptions o;
            o.pad = a.get<bool>("pad", false);
            json settings;
            try
            {
                settings = exportUnreal(f.world, link::cacheDir() / "terrain", name, o);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendToUnreal: ") + e.what());
            }
            json params = {{"id", f.mesh ? f.mesh->path() : names[0]},
                           {"label", name},
                           {"heightmap", settings["files"]["height"]},
                           {"resolution", settings["resolution"]},
                           {"quadsPerSection", settings["quadsPerSection"]},
                           {"sectionsPerComponent", settings["sectionsPerComponent"]},
                           {"location", settings["location"]},
                           {"scale", settings["scale"]},
                           {"gridSize", a.get<int64_t>("gridSize", 0)}};
            if (const std::string m = a.get<std::string>("material", ""); !m.empty())
                params["material"] = m;
            const int timeoutMs = int(std::clamp(a.get<double>("timeout", 300.0), 1.0, 3600.0) * 1000.0);
            link::RpcResult r = client->call("setLandscape", params, timeoutMs);
            if (!r.ok)
                throw CommandError("terrainSendToUnreal: " + r.error);
            json out = r.result;
            out["settings"] = settings;
            return out;
        },
        .examples = {"terrainSendToUnreal('Terrain', material='/Game/Terrain/M_Landscape')"},
    });
    ctx.registerCommand({
        .name = "terrainErode",
        .doc = "Erode a terrain made by terrainCreate: rivers carve drainage-consistent valleys, slopes over the talus angle shed "
               "scree, and the masks flow, wear, deposit and lakes appear for materials and scatter. Sets the terrain's erosion "
               "inputs (iterations is age: 50 to 150 looks natural) and returns terrainInfo's stats.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"iterations", "i", T::Int, int64_t(80), "Erosion steps; 0 turns it off.", {}, {}, 0.0, 10000.0},
                  {"strength", "s", T::Float, nullptr, "How fast rivers cut.", {}, {}, 0.0},
                  {"uplift", "u", T::Float, nullptr, "Rise per step, cm."},
                  {"talusAngle", "ta", T::Float, nullptr, "Slopes steeper than this shed scree, degrees.", {}, {}, 1.0, 89.0}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainErode: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainErode");
            if (f.modifier->type().attrIndex("erosion") < 0)
                throw CommandError("terrainErode: " + f.modifier->path() + " has no erosion inputs; add an Erode Terrain node to its group");
            const std::string mod = f.modifier->path();
            ops::execute("setAttr", {{"args", {mod + ".erosion", a.get<int64_t>("iterations", 80)}}});
            if (a.has("strength"))
                ops::execute("setAttr", {{"args", {mod + ".erosionStrength", a.get<double>("strength", 0.0)}}});
            if (a.has("uplift"))
                ops::execute("setAttr", {{"args", {mod + ".uplift", a.get<double>("uplift", 0.0)}}});
            if (a.has("talusAngle"))
                ops::execute("setAttr", {{"args", {mod + ".talusAngle", a.get<double>("talusAngle", 0.0)}}});
            return describe(findTerrain(g, names[0], "terrainErode"));
        },
        .examples = {"terrainErode('Terrain', iterations=120)['masks']['flow']"},
    });

    ctx.registerCommand({
        .name = "terrainImport",
        .doc = "Make a terrain from a file. Elevation data (GeoTIFF DEM tiles, SRTM .hgt, .asc, .xyz, Terrarium .png with "
               "mode='elevation') is read at its real size, heights in cm from sea level, north toward -Z. A painted image is "
               "read by mode: guide (default) keeps its big shapes and erodes in natural detail, height is literal, uplift grows "
               "ranges where it is bright, mask puts it in a mask on flat ground; it spans size along X.\n\n"
               "The file and erosion settings become inputs on the modifier, like terrainCreate's, so terrainErode and setAttr "
               "work on it. Returns the mesh, modifier, group and terrainInfo's stats.",
        .positionalDoc = "file; relative paths are under the workspace",
        .flags = {{"mode", "m", T::String, "", "elevation, guide, height, uplift or mask; empty picks elevation for DEM files, else guide."},
                  {"size", "sz", T::Float, 100000.0, "Images: width along X, cm.", {}, {}, 0.0},
                  {"low", "lo", T::Float, 0.0, "Images: height of black, cm."},
                  {"high", "hi", T::Float, 50000.0, "Images: height of white, cm."},
                  {"center", "c", T::Vec3, json::array({0.0, 0.0, 0.0}), "Centre."},
                  {"cellSize", "cs", T::Float, 0.0, "Elevation: grid spacing, cm; 0 keeps the data's own.", {}, {}, 0.0},
                  {"exaggeration", "ex", T::Float, 1.0, "Elevation: multiplies heights.", {}, {}, 0.0},
                  {"fromLowest", "fl", T::Bool, false, "Elevation: lowest point at 0 instead of sea level."},
                  {"format", "f", T::String, "auto", "Elevation: file type (auto, geotiff, hgt, asc, xyz, terrarium, terrainrgb)."},
                  {"erosion", "e", T::Float, 0.5, "Guide and uplift: 0 to 1, how far erosion may move the painting.", {}, {}, 0.0, 1.0},
                  {"name", "n", T::String, "Terrain", "Name of the mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::vector<std::string> pos = a.objects();
            if (pos.size() != 1)
                throw CommandError("terrainImport: give one file");
            fs::path file = pos[0];
            if (file.is_relative())
                if (const fs::path& root = Workspace::get().root(); !root.empty())
                    file = root / file;
            if (!fs::exists(file))
                throw CommandError("terrainImport: no file " + file.string());
            std::string mode = a.get<std::string>("mode", "");
            if (mode.empty())
                mode = looksLikeElevation(file) ? "elevation" : "guide";
            const bool elevation = mode == "elevation";
            ImageMode imageMode = ImageMode::Guide;
            if (!elevation && !parseImageMode(mode, &imageMode))
                throw CommandError("terrainImport: unknown mode '" + mode + "' (elevation, guide, height, uplift, mask)");

            Graph& g = Graph::current();
            json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", a.get<std::string>("name", "Terrain")}});
            const std::string host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Terrain");

            json source;
            if (elevation)
                source = {{"name", "Source"},
                          {"type", "ReadElevationData"},
                          {"props", {{"format", a.get<std::string>("format", "auto")}}},
                          {"values",
                           {{"center", a.flags["center"]},
                            {"cellSize", a.flags["cellSize"]},
                            {"exaggeration", a.flags["exaggeration"]},
                            {"fromLowest", a.flags["fromLowest"]}}}};
            else
                source = {{"name", "Source"},
                          {"type", "ImportTerrainImage"},
                          {"props", {{"mode", mode}}},
                          {"values",
                           {{"center", a.flags["center"]},
                            {"width", a.flags["size"]},
                            {"low", a.flags["low"]},
                            {"high", a.flags["high"]},
                            {"erosion", a.flags["erosion"]}}}};
            json inputs = json::array({{{"name", "file"}, {"type", "string"}, {"default", file.generic_string()}, {"panel", "Source"},
                                        {"description", elevation ? "Elevation file." : "Painted height image."}}});
            json links = json::array({"GroupInput.file -> Source.file", "Source.heightfield -> Erode.heightfield", "Erode.heightfield -> GroupOutput.geometry"});
            const json defaults = {{"erosion", 0}, {"erosionStrength", ErosionParams().strength}, {"uplift", 0.0}, {"talusAngle", ErosionParams().talusAngle}};
            for (const Exposed& e : kInputs)
            {
                if (std::string_view(e.node) != "Erode")
                    continue;
                json in = {{"name", e.name}, {"type", e.type}, {"default", defaults[e.name]}, {"panel", e.panel}, {"description", e.description}, {"min", e.min}};
                if (e.softMax > e.min)
                    in["softMax"] = e.softMax;
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> " + e.node + "." + e.socket);
            }
            json spec = {{"name", group},
                         {"description", elevation ? "A terrain from elevation data." : "A terrain from a painted image."},
                         {"inputs", inputs},
                         {"nodes", json::array({source, {{"name", "Erode"}, {"type", "ErodeTerrain"}}})},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            ops::execute("gnApply", {{"args", {host, group}}});

            json out = describe(findTerrain(g, host, "terrainImport"));
            out["group"] = group;
            out["mode"] = mode;
            return out;
        },
        .examples = {"terrainImport('dem/N46E007.hgt', cellSize=3000)", "terrainImport('paint/island.png', size=400000, high=60000)",
                     "terrainImport('paint/ranges.psd', mode='uplift', erosion=0.8)"},
    });
}
} // namespace terrain
