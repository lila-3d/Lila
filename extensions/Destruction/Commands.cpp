// breakApart: one command that turns meshes into a ready destruction setup, a geometry-nodes group anyone can open and
// edit afterwards (Fracture, Anchor Pieces, Rigid Body Solver), with the few settings that matter exposed in plain terms.

#include "Destruction.h"
#include "PieceTransform.h"
#include "RigidState.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Sequences.h>
#include <lila/Node.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>

using namespace lila;

namespace destruction
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;

json v3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 vecArg(const Args& a, const char* flag, const glm::dvec3& fallback)
{
    if (!a.has(flag))
        return fallback;
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("breakApart: ") + flag + " takes [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
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

/** A name usable as a group name: letters, digits and underscores, starting with a letter. */
std::string identifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Mesh" + out;
    return out;
}

/** The outward normal of the surface nearest a point, in the mesh's space. */
glm::dvec3 surfaceNormalNear(const geo::Mesh& m, const glm::dvec3& p)
{
    geo::MeshBvh bvh;
    bvh.build(m);
    geo::BvhClosest hit;
    if (!bvh.closestPoint(p, hit) || hit.face == geo::kInvalid)
        return glm::dvec3(0, 0, 1);
    return m.faceNormal(hit.face);
}

json buildOne(Node& mesh, const Args& a)
{
    const std::string materialName = a.get<std::string>("material", "concrete");
    const Preset& p = preset(materialName);
    const bool simulate = a.get<bool>("simulate", true);
    const bool anchor = a.get<bool>("anchor", true);

    const auto out = mesh.as<scene::MeshPtr>(mesh.type().attrIndex("outMesh"));
    if (!out || out->numFaces() == 0)
        throw CommandError("breakApart: " + mesh.path() + " has no faces");
    const glm::dmat4 world = mesh.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat4 toLocal = glm::inverse(world);
    geo::Bounds wb;
    for (geo::Index i = 0; i < out->pointCapacity(); ++i)
        if (out->pointValid(i))
            wb.extend(glm::dvec3(world * glm::dvec4(out->position(i), 1.0)));

    const glm::dvec3 impactWorld = vecArg(a, "impact", wb.center());
    const glm::dvec3 impact = glm::dvec3(toLocal * glm::dvec4(impactWorld, 1.0));
    const glm::dmat3 rot = glm::dmat3(world);
    const glm::dvec3 gravity = glm::normalize(glm::inverse(rot) * glm::dvec3(0, -1, 0)) * 981.0;
    // The ground is the world floor (or the mesh's bottom when it sits below it), only while the mesh stands upright.
    const bool upright = glm::length(glm::normalize(rot * glm::dvec3(0, 1, 0)) - glm::dvec3(0, 1, 0)) < 1e-3;
    const double groundWorld = std::min(0.0, wb.min.y);
    const double ground = (toLocal * glm::dvec4(0, groundWorld, 0, 1)).y;
    const double radius = std::max(10.0, glm::length(wb.size()) * 0.35);
    // The blast pushes out from a point just outside the hit surface, so pieces are driven into the mesh, not along it.
    const glm::dvec3 blastOffset = surfaceNormalNear(*out, impact) * (radius * 0.35);
    const int64_t startFrame = a.has("startFrame") ? a.get<int64_t>("startFrame")
                                                   : scene::sim::frameOf(scene::anim::playback().minTime);

    const std::string group = uniqueGroupName(identifier(mesh.name()) + "_Break");
    json inputs = json::array({
        {{"name", "pieces"}, {"type", "int"}, {"default", a.get<int64_t>("pieces", p.pieces)}, {"min", 1}, {"softMax", 500},
         {"description", "Roughly how many pieces."}},
        {{"name", "seed"}, {"type", "int"}, {"default", a.get<int64_t>("seed", 1)}, {"description", "Another seed gives another pattern."}},
        {{"name", "impact"}, {"type", "vector"}, {"default", v3(impact)}, {"description", "Where it's hit (the mesh's own space, cm)."}},
        {{"name", "roughness"}, {"type", "float"}, {"default", p.roughness}, {"min", 0.0}, {"softMax", 10.0}, {"description", "How jagged the cracks are, cm."}},
        {{"name", "strength"}, {"type", "float"}, {"default", p.strength}, {"min", 0.0}, {"softMax", 200.0},
         {"description", "How hard it is to break: N per cm2 the pieces share."}},
    });
    json nodes = json::array({
        {{"name", "Fracture"}, {"type", "Fracture"}, {"props", {{"material", p.material}}}, {"values", {{"impactFocus", p.impactFocus}}}},
        {{"name", "Realize"}, {"type", "RealizeInstances"}},
    });
    json links = json::array({"GroupInput.geometry -> Fracture.mesh", "GroupInput.pieces -> Fracture.pieces", "GroupInput.seed -> Fracture.seed",
                              "GroupInput.impact -> Fracture.impact", "GroupInput.roughness -> Fracture.roughness",
                              "GroupInput.strength -> Fracture.linkStrength", "Realize.geometry -> GroupOutput.geometry"});
    if (simulate)
    {
        inputs.push_back({{"name", "blast"}, {"type", "float"}, {"default", a.get<double>("blast", p.blast)}, {"min", 0.0}, {"softMax", 5000.0},
                          {"description", "How hard the impact hits: speed given to pieces at the impact, cm/s."}});
        inputs.push_back({{"name", "startFrame"}, {"type", "int"}, {"default", startFrame}, {"description", "Frame it breaks on."}});
        nodes.push_back({{"name", "Solver"},
                         {"type", "RigidBodySolver"},
                         {"values",
                          {{"gravity", v3(gravity)}, {"ground", upright}, {"groundHeight", ground}, {"density", p.density},
                           {"friction", p.friction}, {"bounce", p.bounce}, {"blastRadius", radius}}}});
        nodes.push_back({{"name", "Behind"}, {"type", "VectorMath"}, {"props", {{"operation", "add"}}}, {"values", {{"b", v3(blastOffset)}}}});
        links.push_back("GroupInput.impact -> Behind.a");
        links.push_back("Behind.vector -> Solver.blastCenter");
        links.push_back("GroupInput.blast -> Solver.blastSpeed");
        links.push_back("GroupInput.startFrame -> Solver.startFrame");
        links.push_back("GroupInput.startFrame -> Solver.blastFrame");
        links.push_back("Fracture.pieces -> Solver.pieces");
        links.push_back("Solver.pieces -> Realize.geometry");
        if (anchor && upright)
        {
            nodes.push_back({{"name", "Anchor"}, {"type", "AnchorPieces"}, {"values", {{"height", ground}, {"tolerance", 1.0}}}});
            links.push_back("Fracture.pieces -> Anchor.pieces");
            links.push_back("Fracture.links -> Anchor.links");
            links.push_back("GroupInput.strength -> Anchor.strength");
            links.push_back("Anchor.links -> Solver.links");
        }
        else
            links.push_back("Fracture.links -> Solver.links");
    }
    else
        links.push_back("Fracture.pieces -> Realize.geometry");

    json spec = {{"name", group},
                 {"description", std::string("Breaks ") + mesh.name() + " apart like " + p.material + (simulate ? " and simulates it falling." : ".")},
                 {"inputs", inputs},
                 {"nodes", nodes},
                 {"links", links}};
    ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
    return ops::execute("gnApply", {{"args", {mesh.path(), group}}});
}

/** The simulating geometry-nodes modifier in a mesh's stack (the mesh itself may be given as the modifier). */
scene::NodeId simulationOf(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (scene::sim::isSimulation(*n))
        return n->id();
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && scene::sim::isSimulation(*mod))
            return m;
    throw CommandError(std::string(cmd) + ": " + n->path() + " has no destruction simulation (run breakApart first)");
}

/** The mesh a modifier belongs to, or the node named when it is the mesh. */
scene::NodeId meshFor(Graph& g, scene::NodeId modifier, const std::string& named)
{
    const scene::NodeId m = scene::modifiers::meshOf(g, modifier);
    return m != scene::kNoNode ? m : g.find(named)->id();
}

struct Break
{
    int64_t frame = 0;
    glm::dvec3 position{0.0};
    double impulse = 0.0;
    int a = -1, b = -1;
};

/** Every link broken by `last`, placed where its first piece had carried it on the frame it broke, in world space. */
std::vector<Break> breaksOf(Graph& g, scene::NodeId modifier, scene::NodeId mesh, int64_t last)
{
    const std::vector<uint8_t> end = gn::simulationState(g, modifier, last);
    const StateView view(end);
    std::shared_ptr<const gn::SimInputs> in = gn::simulationInputs(modifier);
    if (!view.valid() || !in)
        throw CommandError("exportBreaks: the simulation has no state to read");
    gn::GeometryPtr links = in->geometry("links"), pieces = in->geometry("pieces");
    if (!links->curves || !pieces->instances)
        return {};
    const gn::CurveSet& c = *links->curves;
    const gn::Instances& inst = *pieces->instances;
    const glm::dmat4 world = g.node(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::span<const int32_t> pa = c.curveAttrs.get<int32_t>("pieceA"), pb = c.curveAttrs.get<int32_t>("pieceB");
    std::vector<Break> out;
    size_t link = 0;
    for (size_t k = 0; k < c.numCurves() && link < view.header().links; ++k)
    {
        if (c.curveSize(k) == 0)
            continue;
        const LinkRecord r = view.link(link++);
        if (r.brokenStep < 0)
            continue;
        Break b;
        b.frame = view.frameOfStep(r.brokenStep);
        b.impulse = r.impulse;
        b.a = k < pa.size() ? pa[k] : -1;
        b.b = k < pb.size() ? pb[k] : -1;
        glm::dvec3 anchor = c.curvePoints(k)[0];
        if (b.a >= 0 && size_t(b.a) < inst.size())
        {
            const std::vector<uint8_t> bytes = gn::simulationState(g, modifier, b.frame);
            const StateView at(bytes);
            if (at.valid() && size_t(b.a) < at.header().bodies)
            {
                const BodyRecord body = at.body(size_t(b.a));
                const Decomposed rest = decompose(inst.transforms[size_t(b.a)]);
                const glm::dvec3 p = glm::dvec3(body.position[0], body.position[1], body.position[2]) / kM;
                const glm::dquat q = glm::normalize(glm::dquat(body.rotation[3], body.rotation[0], body.rotation[1], body.rotation[2]));
                anchor = glm::dvec3(compose(p, q, rest.scale) * glm::inverse(inst.transforms[size_t(b.a)]) * glm::dvec4(anchor, 1.0));
            }
        }
        b.position = glm::dvec3(world * glm::dvec4(anchor, 1.0));
        out.push_back(b);
    }
    std::stable_sort(out.begin(), out.end(), [](const Break& x, const Break& y) { return x.frame < y.frame; });
    return out;
}

int64_t playbackEnd() { return scene::sim::frameOf(scene::anim::playback().maxTime); }
} // namespace

const Preset& preset(const std::string& material)
{
    static const Preset concrete{"concrete", 40, 1.5, 20.0, 1500.0, 2400.0, 0.7, 0.05, 0.5};
    static const Preset glass{"glass", 40, 0.0, 6.0, 1200.0, 2500.0, 0.4, 0.15, 0.6};
    static const Preset wood{"wood", 24, 0.5, 30.0, 1000.0, 700.0, 0.6, 0.1, 0.3};
    if (material == "glass")
        return glass;
    if (material == "wood")
        return wood;
    if (material != "concrete")
        throw CommandError("breakApart: material must be concrete, glass or wood");
    return concrete;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "breakApart",
        .doc = "Break meshes apart like concrete, glass or wood, and (by default) simulate them falling.\n\n"
               "Adds a geometry-nodes modifier to each mesh whose group (Fracture, Anchor Pieces, Rigid Body Solver) can be opened "
               "and edited in the Node Editor. The settings that matter (pieces, seed, impact, roughness, strength, blast, "
               "start frame) show on the modifier and can be keyed. The pieces touching the ground are anchored, so a wall "
               "stands until the impact. Returns the modifiers.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"material", "m", T::String, "concrete", "How it cracks.", {"concrete", "glass", "wood"}},
                  {"impact", "i", T::Vec3, nullptr, "Where it's hit, world space cm. Default: the centre of the mesh."},
                  {"pieces", "p", T::Int, nullptr, "Roughly how many pieces. Default: the material's."},
                  {"seed", "", T::Int, 1, "Another seed gives another pattern."},
                  {"blast", "b", T::Float, nullptr, "How hard the impact hits, cm/s. Default: the material's."},
                  {"startFrame", "", T::Int, nullptr, "Frame it breaks on. Default: the start of the playback range."},
                  {"simulate", "", T::Bool, true, "Simulate the pieces; False only fractures."},
                  {"anchor", "", T::Bool, true, "Tie the pieces touching the ground to it."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.empty())
                throw CommandError("breakApart: select a mesh");
            json made = json::array();
            for (const std::string& name : names)
            {
                Node* n = g.find(name);
                if (!n || n->type().attrIndex("outMesh") < 0)
                    throw CommandError("breakApart: " + name + " is not a mesh");
                g.evaluate();
                made.push_back(buildOne(*n, a));
            }
            return made;
        },
        .examples = {"breakApart('Wall', material='concrete', impact=[0, 120, 15])", "breakApart(material='glass', pieces=60, blast=1500)"},
    });

    ctx.registerCommand({
        .name = "exportBreaks",
        .doc = "Write where and when each piece of a destruction broke off, to time sounds and effects in Unreal.\n\n"
               "One row per broken link, by frame: Frame, Time (seconds), X, Y, Z (cm, in the axis profile's space), Impulse "
               "(N s) and the two pieces (PieceB -1 for the ground). A .csv imports into Unreal as a Data Table (the first "
               "column is the row name); a .json is a list of objects. Returns {path, breaks}.",
        .positionalDoc = "mesh or its destruction modifier (default the selection)",
        .flags = {{"path", "p", T::String, nullptr, "The .csv or .json file."},
                  {"end", "e", T::Int, nullptr, "Last frame. Default: the end of the playback range."},
                  {"profile", "ax", T::String, "ue5", "Axis profile of the positions: ue5, ue6 or generic (Lila's own).", {"ue5", "ue6", "generic"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("exportBreaks: give one mesh");
            if (!a.has("path"))
                throw CommandError("exportBreaks: path is required");
            const scene::NodeId mod = simulationOf(g, names[0], "exportBreaks");
            const scene::NodeId mesh = meshFor(g, mod, names[0]);
            const std::string profile = a.get<std::string>("profile", "ue5");
            const glm::dmat3 basis = profile == "generic" ? glm::dmat3(1.0) : lila::seq::unrealBasis(profile);
            const std::vector<Break> breaks = breaksOf(g, mod, mesh, a.has("end") ? a.get<int64_t>("end") : playbackEnd());
            const std::string file = a.get<std::string>("path");
            const std::filesystem::path path(std::u8string(file.begin(), file.end()));
            std::ofstream out(path, std::ios::binary);
            if (!out)
                throw CommandError("exportBreaks: can't write " + file);
            const double fps = scene::sim::framesPerSecond();
            const bool csv = path.extension() != ".json";
            json rows = json::array();
            if (csv)
                out << "---,Frame,Time,X,Y,Z,Impulse,PieceA,PieceB\n";
            for (size_t k = 0; k < breaks.size(); ++k)
            {
                const Break& b = breaks[k];
                const glm::dvec3 p = basis * b.position;
                if (csv)
                    out << std::format("Break_{:04},{},{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{},{}\n", k, b.frame, double(b.frame) / fps, p.x, p.y, p.z,
                                       b.impulse, b.a, b.b);
                else
                    rows.push_back({{"frame", b.frame}, {"time", double(b.frame) / fps}, {"position", json::array({p.x, p.y, p.z})},
                                    {"impulse", b.impulse}, {"pieceA", b.a}, {"pieceB", b.b}});
            }
            if (!csv)
                out << rows.dump(2);
            return {{"path", file}, {"breaks", int64_t(breaks.size())}};
        },
        .examples = {"exportBreaks('Wall', path='Export/Wall_breaks.csv')"},
    });

    ctx.registerCommand({
        .name = "wedgeDestruction",
        .doc = "Bake several variations of a destruction, one per seed, as rigid vertex animation.\n\n"
               "Sets the modifier's seed to seed, seed + 1, ... in turn, bakes each to <folder>/<mesh>_seed<n>, and puts the seed "
               "back. Needs a seed input on the modifier (breakApart makes one). Returns the bakes' file lists.",
        .positionalDoc = "mesh (default the selection)",
        .flags = {{"count", "c", T::Int, 4, "How many variations."},
                  {"seed", "s", T::Int, nullptr, "First seed. Default: the one after the modifier's own."},
                  {"folder", "fo", T::String, "destruction", "Output folder; relative paths are under the workspace."},
                  {"start", "st", T::Float, nullptr, "First frame. Default: the playback range."},
                  {"end", "e", T::Float, nullptr, "Last frame."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("wedgeDestruction: give one mesh");
            const scene::NodeId mod = simulationOf(g, names[0], "wedgeDestruction");
            Node* m = g.node(mod);
            const int seedAttr = m->type().attrIndex("seed");
            if (seedAttr < 0)
                throw CommandError("wedgeDestruction: " + m->path() + " has no seed input");
            Node* mesh = g.node(meshFor(g, mod, names[0]));
            const int64_t original = m->as<int64_t>(seedAttr);
            const int64_t first = a.has("seed") ? a.get<int64_t>("seed") : original + 1;
            const int64_t count = std::clamp<int64_t>(a.get<int64_t>("count", 4), 1, 1000);
            const std::string plug = m->path() + ".seed";
            json bakes = json::array();
            auto restore = [&] { ops::execute("setAttr", {{"args", {plug, original}}}); };
            try
            {
                for (int64_t k = 0; k < count; ++k)
                {
                    ops::execute("setAttr", {{"args", {plug, first + k}}});
                    json call = {{"args", {mesh->path()}}, {"folder", a.get<std::string>("folder", "destruction")}, {"mode", "rigid"},
                                 {"name", std::format("{}_seed{}", mesh->name(), first + k)}};
                    for (const char* f : {"start", "end"})
                        if (a.has(f))
                            call[f] = a.flags[f];
                    bakes.push_back(ops::execute("exportVertexAnimation", call)["files"]);
                }
            }
            catch (...)
            {
                restore();
                throw;
            }
            restore();
            return bakes;
        },
        .examples = {"wedgeDestruction('Wall', count=8, folder='Export/WallVariants')"},
    });
}
} // namespace destruction
