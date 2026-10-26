// Blades: varies meshes already built, so one modelled shrub gives blades of different thickness and lean. Blade
// extrudes record what built them on the mesh (BladeExtrude.h); the deformers are Lila's nonlinear ones, weighted to the
// blades they bend.

#include "BladeExtrude.h"
#include "Blades.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace lila;
using namespace lila::scene;

namespace blades
{
namespace
{
using FaceMap = std::vector<std::pair<NodeId, std::vector<Index>>>;

Graph& graph() { return Graph::current(); }

json run(const char* name, const json& call) { return ops::execute(name, call); }

Node& meshNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("mesh") || n->type().isA("lattice"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

int upIndex(const std::string& choice)
{
    if (choice == "X" || choice == "x")
        return 0;
    if (choice == "Y" || choice == "y")
        return 1;
    if (choice == "Z" || choice == "z")
        return 2;
    return math::upAxis() == math::UpAxis::Z ? 2 : 1;
}

glm::dmat4 worldOf(Graph& g, NodeId id)
{
    g.evaluate();
    return g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
}

glm::dvec3 xformPoint(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

MeshPtr cageOf(Graph& g, NodeId id)
{
    g.evaluate();
    return modifiers::cage(g, id);
}

/** The base mesh direct edits change, and the plug it lives on. */
Mesh editable(Graph& g, NodeId id, PlugRef& plug)
{
    plug = modifiers::editPlug(g, id);
    g.evaluate();
    const MeshPtr& m = std::get<MeshPtr>(g.storedValue(plug));
    return m ? *m : Mesh();
}

void store(Graph& g, const PlugRef& plug, Mesh&& m) { g.setValue(plug, MeshPtr(std::make_shared<Mesh>(std::move(m)))); }

std::vector<Index> allFaces(const Mesh& m)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

std::vector<Index> indexList(const json& j)
{
    std::vector<Index> out;
    if (!j.is_array())
        return out;
    for (const json& v : j)
    {
        if (v.is_number_integer())
            out.push_back(v.get<Index>());
        else if (v.is_array() && v.size() == 2)
            for (Index i = v[0].get<Index>(); i <= v[1].get<Index>(); ++i)
                out.push_back(i);
    }
    return out;
}

/** Faces per mesh behind the selection: components converted to faces, a whole object as every face it has. */
FaceMap selectedFaces(Graph& g)
{
    FaceMap out;
    std::set<NodeId> seen;
    // Components stay selected underneath object mode, where they are not what the user is pointing at.
    const bool objectMode = ops::execute("selectMode", json::object()) == "object";
    const json comp = objectMode ? json::object() : ops::execute("componentSelection", json::object());
    for (auto& [path, kinds] : comp.items())
    {
        Node* n = g.find(path);
        if (!n || !n->type().isA("mesh"))
            continue;
        MeshPtr m = cageOf(g, n->id());
        if (!m)
            continue;
        std::set<Index> faces;
        for (Index f : indexList(kinds.value("faces", json::array())))
            if (m->faceValid(f))
                faces.insert(f);
        for (Index e : indexList(kinds.value("edges", json::array())))
            if (m->edgeValid(e))
                m->forEachEdgeCorner(e, [&](Index c) { faces.insert(m->cornerFace(c)); });
        for (Index p : indexList(kinds.value("vertices", json::array())))
            if (m->pointValid(p))
                for (Index f : m->pointFaces(p))
                    faces.insert(f);
        if (faces.empty())
            continue;
        out.push_back({n->id(), {faces.begin(), faces.end()}});
        seen.insert(n->id());
    }
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n || seen.count(id) || !n->type().isA("mesh") || n->type().isA("lattice"))
            continue;
        if (MeshPtr m = cageOf(g, id))
        {
            out.push_back({id, allFaces(*m)});
            seen.insert(id);
        }
    }
    return out;
}

/** Meshes the command works on: those named, else the ones behind the selection. */
std::vector<NodeId> targetMeshes(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    std::vector<std::string> named;
    for (const json& v : a.positional)
        if (v.is_string())
            named.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    named.push_back(x.get<std::string>());
    if (!named.empty())
    {
        for (const std::string& name : named)
            out.push_back(meshNamed(g, name, cmd).id());
        return out;
    }
    for (auto& [id, faces] : selectedFaces(g))
        out.push_back(id);
    return out;
}

const char* kNeedsExtrude = "Select a mesh with a blade extrude on it.";

std::string noExtrude(Graph& g, NodeId id) { return g.node(id)->path() + " has no blade extrude. Extrude Blades first."; }

json bladeJson(const Blade& b, const glm::dmat4& world)
{
    json values = json::object(), base = json::object();
    for (int f = 0; f < kFieldCount; ++f)
    {
        values[fieldName(f)] = b.values[f];
        base[fieldName(f)] = b.base[f];
    }
    const glm::dvec3 r = xformPoint(world, b.root);
    json out = {{"blade", b.id}, {"caps", b.caps}, {"walls", b.walls}, {"points", b.points}, {"root", {r.x, r.y, r.z}}, {"base", base}};
    out.update(values);
    return out;
}

// ------------------------------------------------------------------------------------------------ deformers

std::string deformerTypeOf(const Node& n)
{
    const std::string& t = n.type().name;
    const std::string suffix = "Deformer";
    return t.size() > suffix.size() && t.ends_with(suffix) ? t.substr(0, t.size() - suffix.size()) : std::string();
}

/** The spec of a nonlinear deformer node, null for anything else. */
const DeformerSpec* specOf(const Node& n)
{
    if (n.type().attrIndex("handleMatrix") < 0)
        return nullptr;
    const std::string type = deformerTypeOf(n);
    for (const DeformerSpec& s : deformerSpecs())
        if (type == s.type)
            return &s;
    return nullptr;
}

NodeId handleOf(Graph& g, const Node& d)
{
    const int a = d.type().attrIndex("handleMatrix");
    if (a < 0)
        return kNoNode;
    auto src = g.source({d.id(), a, -1});
    return src ? src->node : kNoNode;
}

double numberAttr(const Node& n, const char* name)
{
    const int a = n.type().attrIndex(name);
    return a >= 0 ? n.as<double>(a) : 0.0;
}

void setNumber(Graph& g, const Node& n, const char* name, double v)
{
    const int a = n.type().attrIndex(name);
    if (a >= 0)
        g.setValue({n.id(), a, -1}, v);
}

/** The amount a deformer's spec varies: bend stores its curvature per handle unit as the angle over its bounds. */
void setAmount(Graph& g, const Node& d, const DeformerSpec& spec, double value)
{
    const double span = std::string(spec.kind) == "bend" ? numberAttr(d, "highBound") - numberAttr(d, "lowBound") : 1.0;
    for (const char* attr : spec.attrs)
        setNumber(g, d, attr, value * span);
}

/** Nonlinear deformers of the kind behind the selection or the named nodes: on a mesh, a component or a handle. */
std::vector<NodeId> findDeformers(Graph& g, const Args& a, const DeformerSpec* spec)
{
    std::vector<NodeId> roots;
    std::vector<std::string> named;
    for (const json& v : a.positional)
        if (v.is_string())
            named.push_back(v.get<std::string>());
    if (!named.empty())
        for (const std::string& name : named)
        {
            Node* n = g.find(name);
            if (!n)
                throw CommandError("no node named '" + name + "'");
            roots.push_back(n->id());
        }
    else
    {
        for (auto& [id, faces] : selectedFaces(g))
            roots.push_back(id);
        for (NodeId id : g.selection())
            if (std::find(roots.begin(), roots.end(), id) == roots.end())
                roots.push_back(id);
    }

    std::vector<NodeId> out;
    auto take = [&](NodeId id) {
        Node* n = g.node(id);
        const DeformerSpec* s = n ? specOf(*n) : nullptr;
        if (!s || (spec && s != spec) || std::find(out.begin(), out.end(), id) != out.end())
            return;
        out.push_back(id);
    };
    for (NodeId id : roots)
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        if (specOf(*n))
            take(id);
        else if (n->type().isA("mesh") && !n->type().isA("lattice"))
            for (NodeId d : deformers::list(g, id))
                take(d);
        else
            for (const PlugRef& dst : g.destinations({id, Transform::kWorldMatrix, -1}))
                take(dst.node);
    }
    return out;
}

const DeformerSpec& specArg(const Args& a, const char* cmd)
{
    const std::string kind = a.get<std::string>("kind", "bend");
    const DeformerSpec* s = findSpec(kind);
    if (!s)
        throw CommandError(std::string(cmd) + ": unknown deformer '" + kind + "'");
    return *s;
}

/** The handle's Y along the up axis, sized so the points fill its bounds, spun by angle degrees around it. */
glm::dmat4 handleMatrix(const std::vector<glm::dvec3>& points, int axis, bool fromBase, double angle, const glm::dvec3* root,
                        double fallbackHalf)
{
    glm::dvec3 low(1e300), high(-1e300);
    for (const glm::dvec3& p : points)
    {
        low = glm::min(low, p);
        high = glm::max(high, p);
    }
    glm::dvec3 position = (low + high) * 0.5;
    double half;
    if (fromBase)
    {
        // A blade's own points stop short of the ring it stands on, which is where it should bend from.
        if (root)
            position = *root;
        position[axis] = root ? (*root)[axis] : low[axis];
        half = (high[axis] - position[axis]) * 0.5;
    }
    else
        half = (high[axis] - low[axis]) * 0.5;
    if (half < 1e-4)
        half = std::max(fallbackHalf, 1e-4);

    glm::dvec3 up(0.0);
    up[axis] = 1.0;
    const int s0 = axis == 0 ? 1 : 0;
    const int s1 = axis == 2 ? 1 : 2;
    const double r = glm::radians(angle);
    glm::dvec3 side(0.0);
    side[s0] = std::cos(r);
    side[s1] = std::sin(r);
    const glm::dvec3 third = glm::cross(side, up);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(side * half, 0.0);
    m[1] = glm::dvec4(up * half, 0.0);
    m[2] = glm::dvec4(third * half, 0.0);
    m[3] = glm::dvec4(position, 1.0);
    return m;
}

void setWorld(Graph& g, NodeId id, const glm::dmat4& world)
{
    g.evaluate();
    Transform::setLocalMatrix(g, id, glm::inverse(g.node(id)->as<glm::dmat4>(Transform::kParentMatrix)) * world);
}

/** Random amounts across the deformers, biased up the mesh by where their handles sit. Returns how many changed. */
int randomizeDeformers(Graph& g, const std::vector<NodeId>& ids, double low, double high, double bias, int axis, uint64_t seed, bool spin)
{
    Rng rng(seed);
    std::vector<NodeId> handles;
    std::vector<double> heights;
    for (NodeId id : ids)
    {
        const NodeId h = handleOf(g, *g.node(id));
        handles.push_back(h);
        heights.push_back(h ? worldOf(g, h)[3][axis] : 0.0);
    }
    const std::vector<double> frac = fractions(heights);
    int touched = 0;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        Node& d = *g.node(ids[i]);
        const DeformerSpec* spec = specOf(d);
        if (!spec)
            continue;
        setAmount(g, d, *spec, biased(rng, low, high, frac[i], bias));
        if (spin && handles[i])
        {
            const glm::dmat4 w = worldOf(g, handles[i]);
            glm::dvec3 up(0.0);
            up[axis] = 1.0;
            const glm::dvec3 at(w[3]);
            const glm::dmat4 turn = glm::translate(glm::dmat4(1.0), at) * glm::rotate(glm::dmat4(1.0), glm::radians(rng.uniform(0.0, 360.0)), up) *
                                    glm::translate(glm::dmat4(1.0), -at);
            setWorld(g, handles[i], turn * w);
        }
        ++touched;
    }
    return touched;
}

struct Group
{
    NodeId mesh;
    std::vector<Index> points;
    bool hasRoot = false;
    glm::dvec3 root{0.0};
};

/** The selected faces as point groups: one per blade when asked for, falling back to connected faces. */
std::vector<Group> bladeGroups(Graph& g, NodeId mesh, const std::vector<Index>& faces, bool perBlade)
{
    MeshPtr m = cageOf(g, mesh);
    if (!m)
        return {};
    const std::vector<Index> points = facePoints(*m, faces);
    if (!perBlade)
        return {{mesh, points}};
    std::vector<Group> out;
    const std::set<Index> chosen(points.begin(), points.end());
    for (const Blade& b : blades(*m))
    {
        Group grp{mesh, {}, true, b.root};
        for (Index p : b.points)
            if (chosen.count(p))
                grp.points.push_back(p);
        if (!grp.points.empty())
            out.push_back(std::move(grp));
    }
    if (!out.empty())
        return out;
    for (const std::vector<Index>& region : faceRegions(*m, faces))
        out.push_back({mesh, facePoints(*m, region)});
    return out;
}
} // namespace

const std::vector<DeformerSpec>& deformerSpecs()
{
    static const std::vector<DeformerSpec> specs = {
        {"bend", "Bend", "Curvature", "bend", {"angle"}, 10.0, 70.0, true},
        {"flare", "Flare", "End Flare", "taper", {"endScale"}, 0.2, 1.0, true},
        {"twist", "Twist", "End Angle", "twist", {"endAngle"}, -60.0, 60.0, true},
        {"sine", "Sine", "Amplitude", "sine", {"amplitude"}, 0.0, 0.3, true},
        {"squash", "Squash", "Factor", "squash", {"factor"}, -0.4, 0.4, true},
        {"wave", "Wave", "Amplitude", "wave", {"amplitude"}, 0.0, 0.3, false},
    };
    return specs;
}

const DeformerSpec* findSpec(const std::string& kindOrLabel)
{
    for (const DeformerSpec& s : deformerSpecs())
        if (kindOrLabel == s.kind || kindOrLabel == s.label)
            return &s;
    return nullptr;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;

    ctx.registerCommand({
        .name = "bladeExtrude",
        .doc = "Extrude faces into blades that remember how they were built, so randomizeBladeExtrude can vary each one later. "
               "Faces default to the face selection (a whole selected mesh means every face). Returns {mesh: [blade ids]}.",
        .positionalDoc = "mesh (with faces=)",
        .flags = {{"faces", "f", T::List, nullptr, "Face ids of the one mesh given."},
                  {"thickness", "t", T::Float, 1.0, "How far the blades grow along their face normal, cm."},
                  {"offset", "o", T::Float, 0.0, "How much the blades narrow as they grow: the cap border moves this far inward, cm."},
                  {"localTranslateZ", "ltz", T::Float, 0.0, "Extra growth along the face normal, cm."},
                  {"divisions", "d", T::Int, 1, "Segments up each blade."},
                  {"keepFacesTogether", "kft", T::Bool, false, "Connected faces grow as one blade; off, every face is its own."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            FaceMap targets;
            if (a.has("faces"))
            {
                std::vector<NodeId> meshes = targetMeshes(g, a, "bladeExtrude");
                if (meshes.size() != 1)
                    throw CommandError("bladeExtrude: faces= needs exactly one mesh");
                targets.push_back({meshes[0], indexList(a.flags["faces"])});
            }
            else if (!a.positional.empty())
                for (NodeId id : targetMeshes(g, a, "bladeExtrude"))
                    if (MeshPtr m = cageOf(g, id))
                        targets.push_back({id, allFaces(*m)});
            if (targets.empty())
                targets = selectedFaces(g);
            if (targets.empty())
                throw CommandError("bladeExtrude: select the faces to grow blades from");
            ExtrudeOptions o;
            o.values[kThickness] = a.get<double>("thickness", 1.0);
            o.values[kOffset] = a.get<double>("offset", 0.0);
            o.values[kTranslateZ] = a.get<double>("localTranslateZ", 0.0);
            o.divisions = std::clamp(a.get<int>("divisions", 1), 1, 64);
            o.keepFacesTogether = a.get<bool>("keepFacesTogether", false);
            json out = json::object();
            for (auto& [id, faces] : targets)
            {
                PlugRef plug;
                Mesh m = editable(g, id, plug);
                std::vector<int> ids = extrude(m, faces, o);
                if (ids.empty())
                    continue;
                store(g, plug, std::move(m));
                out[g.node(id)->path()] = ids;
            }
            if (out.empty())
                throw CommandError("bladeExtrude: none of those faces exist");
            return out;
        },
    });

    ctx.registerCommand({
        .name = "randomizeBladeExtrude",
        .doc = "Give every blade of the meshes random extrude values in the ranges given. Each blade is rebuilt from its root, so "
               "running it again rerolls rather than stacking. bias 1 gives the high end of a range to the blades nearest the top, "
               "-1 to the bottom ones, 0 is pure random. seed 0 differs every run. Returns {meshes, blades}.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"fields", "", T::Any, nullptr, "{thickness: [low, high], offset: [low, high], localTranslateZ: [low, high]}; leave a field out to keep it."},
                  {"bias", "b", T::Float, 0.0, "Bias to top, -1 to 1."},
                  {"up", "", T::String, "Auto", "Up axis for the bias: Auto follows the scene.", {"Auto", "X", "Y", "Z"}},
                  {"seed", "s", T::Int, 0, "0 differs every run; any other number repeats."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = targetMeshes(g, a, "randomizeBladeExtrude");
            if (meshes.empty())
                throw CommandError(kNeedsExtrude);
            std::vector<std::pair<int, glm::dvec2>> fields;
            if (a.has("fields") && a.flags["fields"].is_object())
                for (int f = 0; f < kFieldCount; ++f)
                    if (const json& r = a.flags["fields"].value(fieldName(f), json()); r.is_array() && r.size() == 2)
                        fields.push_back({f, {r[0].get<double>(), r[1].get<double>()}});
            if (fields.empty())
                throw CommandError("Turn on something to randomize.");
            const int axis = upIndex(a.get<std::string>("up", "Auto"));
            const double bias = std::clamp(a.get<double>("bias", 0.0), -1.0, 1.0);
            Rng rng(uint64_t(std::max<int64_t>(a.get<int64_t>("seed", 0), 0)));
            int touched = 0;
            for (NodeId id : meshes)
            {
                PlugRef plug;
                Mesh m = editable(g, id, plug);
                std::vector<Blade> bs = blades(m);
                if (bs.empty())
                    throw CommandError(noExtrude(g, id));
                const glm::dmat4 world = worldOf(g, id);
                std::vector<double> heights;
                for (const Blade& b : bs)
                    heights.push_back(xformPoint(world, b.root)[axis]);
                const std::vector<double> frac = fractions(heights);
                for (size_t i = 0; i < bs.size(); ++i)
                {
                    BladeValues v = bs[i].values;
                    for (auto& [f, range] : fields)
                        v[f] = biased(rng, range.x, range.y, frac[i], bias);
                    setValues(m, bs[i].id, v);
                }
                store(g, plug, std::move(m));
                touched += int(bs.size());
            }
            return {{"meshes", meshes.size()}, {"blades", touched}};
        },
    });

    ctx.registerCommand({
        .name = "setBladeExtrude",
        .doc = "Set the extrude values of blades of one mesh directly, every blade by default. Fields not given keep their value.",
        .positionalDoc = "mesh",
        .flags = {{"blades", "", T::List, nullptr, "Blade ids; default every blade."},
                  {"thickness", "t", T::Float, nullptr, "cm."},
                  {"offset", "o", T::Float, nullptr, "cm."},
                  {"localTranslateZ", "ltz", T::Float, nullptr, "cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = targetMeshes(g, a, "setBladeExtrude");
            if (meshes.size() != 1)
                throw CommandError("setBladeExtrude: give one mesh");
            PlugRef plug;
            Mesh m = editable(g, meshes[0], plug);
            std::vector<Blade> bs = blades(m);
            if (bs.empty())
                throw CommandError(noExtrude(g, meshes[0]));
            std::set<int> only;
            if (a.has("blades"))
                for (const json& v : a.flags["blades"])
                    only.insert(v.get<int>());
            int n = 0;
            for (const Blade& b : bs)
            {
                if (!only.empty() && !only.count(b.id))
                    continue;
                BladeValues v = b.values;
                for (int f = 0; f < kFieldCount; ++f)
                    if (a.has(fieldName(f)))
                        v[f] = a.get<double>(fieldName(f));
                setValues(m, b.id, v);
                ++n;
            }
            store(g, plug, std::move(m));
            return n;
        },
    });

    ctx.registerCommand({
        .name = "resetBladeExtrude",
        .doc = "Put every blade of the meshes back to the values it was extruded with. Returns the blade count.",
        .positionalDoc = "meshes (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = targetMeshes(g, a, "resetBladeExtrude");
            if (meshes.empty())
                throw CommandError(kNeedsExtrude);
            int n = 0;
            for (NodeId id : meshes)
            {
                PlugRef plug;
                Mesh m = editable(g, id, plug);
                std::vector<Blade> bs = blades(m);
                if (bs.empty())
                    throw CommandError(noExtrude(g, id));
                for (const Blade& b : bs)
                    setValues(m, b.id, b.base);
                store(g, plug, std::move(m));
                n += int(bs.size());
            }
            return n;
        },
    });

    ctx.registerCommand({
        .name = "bladeExtrudeInfo",
        .doc = "The blades of a mesh: [{blade, caps, walls, points, root (world), thickness, offset, localTranslateZ, base: {...}}].",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = targetMeshes(g, a, "bladeExtrudeInfo");
            if (meshes.empty())
                throw CommandError("bladeExtrudeInfo: give a mesh");
            MeshPtr m = cageOf(g, meshes[0]);
            json out = json::array();
            if (m)
            {
                const glm::dmat4 world = worldOf(g, meshes[0]);
                for (const Blade& b : blades(*m))
                    out.push_back(bladeJson(b, world));
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "bladeExtrudeRanges",
        .doc = "A range around the extrude values the meshes' blades use now: their mean, give or take spread. Returns "
               "{thickness: [low, high], offset: [...], localTranslateZ: [...]}.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"spread", "", T::Float, 0.3, "Fraction either side of the mean."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = targetMeshes(g, a, "bladeExtrudeRanges");
            if (meshes.empty())
                throw CommandError(kNeedsExtrude);
            const double spread = a.get<double>("spread", 0.3);
            double sum[kFieldCount] = {};
            size_t count = 0;
            for (NodeId id : meshes)
            {
                MeshPtr m = cageOf(g, id);
                std::vector<Blade> bs = m ? blades(*m) : std::vector<Blade>{};
                if (bs.empty())
                    throw CommandError(noExtrude(g, id));
                for (const Blade& b : bs)
                    for (int f = 0; f < kFieldCount; ++f)
                        sum[f] += b.values[f];
                count += bs.size();
            }
            json out = json::object();
            for (int f = 0; f < kFieldCount; ++f)
            {
                const double mid = sum[f] / double(count);
                out[fieldName(f)] = {mid * (1.0 - spread), mid * (1.0 + spread)};
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "selectBladeFaces",
        .doc = "Select the faces the blades grew from, which are their caps now, or with stems=True every face a blade made, caps "
               "and walls, down to the ring it stands on. Worked out from the mesh each time. Returns the face count.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"stems", "", T::Bool, false, "Whole stems rather than caps."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes = targetMeshes(g, a, "selectBladeFaces");
            if (meshes.empty())
                throw CommandError(kNeedsExtrude);
            const bool stems = a.get<bool>("stems", false);
            std::vector<std::pair<NodeId, std::vector<Index>>> picks;
            size_t total = 0;
            for (NodeId id : meshes)
            {
                MeshPtr m = cageOf(g, id);
                if (!m || !hasBlades(*m))
                    throw CommandError(noExtrude(g, id));
                picks.push_back({id, stems ? stemFaces(*m) : capFaces(*m)});
                total += picks.back().second.size();
            }
            if (total == 0)
                throw CommandError("Those blade extrudes made no faces to select.");
            bool first = true;
            for (auto& [id, faces] : picks)
            {
                if (faces.empty())
                    continue;
                run("selectComponents", {{"args", {g.node(id)->path()}}, {"faces", faces}, {"add", !first}});
                first = false;
            }
            return total;
        },
    });

    ctx.registerCommand({
        .name = "addBladeDeformer",
        .doc = "Add a nonlinear deformer to the selected faces (a whole mesh when no components are selected), one per blade or one "
               "for the whole selection, weighted so only the blades' own points move and their roots stay planted. Then randomizes "
               "the amount in low..high. Returns the deformers.",
        .flags = {{"kind", "k", T::String, "bend", "bend | flare | twist | sine | squash | wave",
                   {"bend", "flare", "twist", "sine", "squash", "wave"}},
                  {"perBlade", "pb", T::Bool, true, "A deformer per blade; off, one for the whole selection."},
                  {"fromBase", "fb", T::Bool, true, "Anchor the handle at each blade's root so it deforms root to tip; off centres it."},
                  {"spin", "", T::Bool, true, "Spin each handle around the up axis, so blades lean different ways."},
                  {"up", "", T::String, "Auto", "Up axis: Auto follows the scene.", {"Auto", "X", "Y", "Z"}},
                  {"seed", "s", T::Int, 0, "0 differs every run; any other number repeats."},
                  {"low", "", T::Float, nullptr, "Lowest amount; the kind's default range when not given."},
                  {"high", "", T::Float, nullptr, "Highest amount."},
                  {"bias", "b", T::Float, 0.0, "Bias to top, -1 to 1, over the amount range."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const DeformerSpec& spec = specArg(a, "addBladeDeformer");
            FaceMap selection = selectedFaces(g);
            if (selection.empty())
                throw CommandError("Select a mesh, or the faces to deform.");
            const bool perBlade = a.get<bool>("perBlade", true), fromBase = a.get<bool>("fromBase", true), spin = a.get<bool>("spin", true);
            const int axis = upIndex(a.get<std::string>("up", "Auto"));
            const uint64_t seed = uint64_t(std::max<int64_t>(a.get<int64_t>("seed", 0), 0));
            std::vector<Group> groups;
            for (auto& [id, faces] : selection)
                for (Group& grp : bladeGroups(g, id, faces, perBlade))
                    groups.push_back(std::move(grp));

            const std::vector<NodeId> restore = g.selection();
            Rng rng(seed);
            std::vector<NodeId> made;
            for (const Group& grp : groups)
            {
                json params = json::object();
                if (spec.bounded)
                {
                    params["lowBound"] = fromBase ? 0.0 : -1.0;
                    params["highBound"] = fromBase ? 2.0 : 1.0;
                }
                // The default wavelength puts zero crossings on the ends and middle of the bounds, right where a low
                // division blade has its points.
                if (std::string(spec.type) == "sine" || std::string(spec.type) == "wave")
                    params["wavelength"] = 4.0;
                const std::string meshPath = g.node(grp.mesh)->path();
                json r = run("nonlinear", {{"args", {meshPath}}, {"type", spec.type}, {"params", params}});
                Node* handle = g.find(r.at(0).get<std::string>());
                Node* d = g.find(r.at(1).get<std::string>());
                if (!handle || !d)
                    throw CommandError("addBladeDeformer: the deformer was not made");

                g.evaluate();
                const MeshPtr in = std::get<MeshPtr>(g.value({d->id(), DeformerNode::kInMesh, -1}));
                json weights = json::array();
                std::vector<float> w(in ? in->pointCapacity() : 0, 0.0f);
                for (Index p : grp.points)
                    if (p < w.size())
                        w[p] = 1.0f;
                for (float v : w)
                    weights.push_back(v);
                run("setDeformerWeights", {{"args", {d->path()}}, {"weights", weights}});

                const glm::dmat4 world = worldOf(g, grp.mesh);
                MeshPtr cage = cageOf(g, grp.mesh);
                std::vector<glm::dvec3> pts;
                for (Index p : grp.points)
                    pts.push_back(xformPoint(world, cage->position(p)));
                const glm::dvec3 root = xformPoint(world, grp.root);
                geo::Bounds whole = cage->bounds();
                double fallback = 0.0;
                {
                    glm::dvec3 lo(1e300), hi(-1e300);
                    for (glm::dvec3 c : {whole.min, whole.max})
                    {
                        lo = glm::min(lo, xformPoint(world, c));
                        hi = glm::max(hi, xformPoint(world, c));
                    }
                    fallback = (hi[axis] - lo[axis]) * 0.5;
                }
                const double angle = spin ? rng.uniform(0.0, 360.0) : 0.0;
                setWorld(g, handle->id(), handleMatrix(pts, axis, fromBase, angle, grp.hasRoot ? &root : nullptr, fallback));
                made.push_back(d->id());
            }
            g.select(restore);

            const double low = a.get<double>("low", spec.low), high = a.get<double>("high", spec.high);
            randomizeDeformers(g, made, low, high, std::clamp(a.get<double>("bias", 0.0), -1.0, 1.0), axis, seed, false);
            json out = json::array();
            for (NodeId id : made)
                out.push_back(g.node(id)->path());
            return out;
        },
    });

    ctx.registerCommand({
        .name = "randomizeBladeDeformers",
        .doc = "Re-roll the amount of the deformers of a kind on the selection (found from a mesh, a component, a handle or the "
               "deformer), biased up the mesh by where their handles sit. Returns how many changed.",
        .positionalDoc = "nodes (default the selection)",
        .flags = {{"kind", "k", T::String, "bend", "bend | flare | twist | sine | squash | wave",
                   {"bend", "flare", "twist", "sine", "squash", "wave"}},
                  {"low", "", T::Float, nullptr, "Lowest amount; the kind's default range when not given."},
                  {"high", "", T::Float, nullptr, "Highest amount."},
                  {"bias", "b", T::Float, 0.0, "Bias to top, -1 to 1."},
                  {"up", "", T::String, "Auto", "Up axis: Auto follows the scene.", {"Auto", "X", "Y", "Z"}},
                  {"seed", "s", T::Int, 0, "0 differs every run; any other number repeats."},
                  {"spin", "", T::Bool, false, "Also spin each handle around the up axis."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const DeformerSpec& spec = specArg(a, "randomizeBladeDeformers");
            std::vector<NodeId> ids = findDeformers(g, a, &spec);
            if (ids.empty())
                throw CommandError("Select a mesh or handle carrying the deformers to randomize.");
            return randomizeDeformers(g, ids, a.get<double>("low", spec.low), a.get<double>("high", spec.high),
                                      std::clamp(a.get<double>("bias", 0.0), -1.0, 1.0), upIndex(a.get<std::string>("up", "Auto")),
                                      uint64_t(std::max<int64_t>(a.get<int64_t>("seed", 0), 0)), a.get<bool>("spin", false));
        },
    });

    ctx.registerCommand({
        .name = "bladeDeformers",
        .doc = "The nonlinear deformers of a kind on the selection or the named nodes (meshes, components, handles).",
        .positionalDoc = "nodes (default the selection)",
        .flags = {{"kind", "k", T::String, "bend", "bend | flare | twist | sine | squash | wave",
                   {"bend", "flare", "twist", "sine", "squash", "wave"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId id : findDeformers(g, a, &specArg(a, "bladeDeformers")))
                out.push_back(g.node(id)->path());
            return out;
        },
    });

    ctx.registerCommand({
        .name = "deleteBladeDeformers",
        .doc = "Delete the deformers of a kind on the selection, and their handles. Returns how many went.",
        .positionalDoc = "nodes (default the selection)",
        .flags = {{"kind", "k", T::String, "bend", "bend | flare | twist | sine | squash | wave",
                   {"bend", "flare", "twist", "sine", "squash", "wave"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const DeformerSpec& spec = specArg(a, "deleteBladeDeformers");
            std::vector<NodeId> ids = findDeformers(g, a, &spec);
            if (ids.empty())
                throw CommandError(std::string("No ") + spec.kind + " deformers on the selection.");
            std::vector<NodeId> handles;
            for (NodeId id : ids)
                if (NodeId h = handleOf(g, *g.node(id)))
                    handles.push_back(h);
            json names = json::array();
            for (NodeId id : ids)
                names.push_back(g.node(id)->path());
            run("removeModifier", {{"args", names}});
            json doomed = json::array();
            for (NodeId h : handles)
                if (g.node(h) && g.destinations({h, Transform::kWorldMatrix, -1}).empty())
                    doomed.push_back(g.node(h)->path());
            if (!doomed.empty())
                run("delete", {{"args", doomed}});
            return ids.size();
        },
    });
}
} // namespace blades
