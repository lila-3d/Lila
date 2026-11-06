// The Terrain Volume node and the commands that write its edits (caves, tunnels, overhangs, arches, stamps), set and
// list its split, check its seams, and send its pieces to files and Unreal.

#include "Stamps.h"
#include "Volume.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
using scene::Graph;
using scene::Node;
using VT = ArgType;
namespace fs = std::filesystem;

constexpr const char* kNodeName = "Volume";
constexpr const char* kVolumeSourcePrefix = "VolumeSource";

bool volumeZUp() { return math::upAxis() == math::UpAxis::Z; }
glm::dvec3 volumeFromGround(glm::dvec2 g, double up = 0.0) { return volumeZUp() ? glm::dvec3(g.x, g.y, up) : glm::dvec3(g.x, up, g.y); }

/** A terrain as these commands see it: its mesh, the modifier making it, its heightfield, and world from its own space. */
struct VolumeTarget
{
    Node* mesh = nullptr;
    Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> local;
    glm::dmat4 world{1.0};
    glm::dmat4 inv{1.0};

    double across() const { return 0.5 * (glm::length(glm::dvec3(world[0])) + glm::length(glm::dvec3(world[2]))); }
    glm::dvec3 toLocal(const glm::dvec3& w) const { return glm::dvec3(inv * glm::dvec4(w, 1.0)); }
    glm::dvec3 toWorld(const glm::dvec3& l) const { return glm::dvec3(world * glm::dvec4(l, 1.0)); }
};

VolumeTarget findVolumeTarget(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    g.evaluate();
    VolumeTarget t;
    if (gn::isGnModifier(*n))
    {
        t.modifier = n;
        t.local = gn::modifierHeightfield(n->id());
        const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
        t.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
    }
    else
    {
        t.mesh = n;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend() && !t.local; ++it)
            if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()))
                {
                    t.modifier = m;
                    t.local = std::move(h);
                }
    }
    if (!t.local || t.local->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    t.world = t.mesh ? t.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    t.inv = glm::inverse(t.world);
    return t;
}

VolumeTarget oneVolumeTarget(const Args& a, const std::string& cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    return findVolumeTarget(Graph::current(), names[0], cmd);
}

std::string volumePathOf(const VolumeTarget& t, const Args& a) { return t.mesh ? t.mesh->path() : a.objects()[0]; }

json volumeVec(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

// ---------------------------------------------------------------------------------------------------- the group

struct VolumeGroup
{
    Node* node = nullptr;
    gn::Group group;
};

VolumeGroup volumeGroup(Graph& g, const VolumeTarget& t, const std::string& cmd)
{
    Node* node = t.modifier ? gn::groupOfModifier(g, *t.modifier) : nullptr;
    if (!node)
        throw CommandError(cmd + ": " + (t.modifier ? t.modifier->path() : std::string("the terrain")) + " has no group");
    try
    {
        return {node, gn::readGroup(*node)};
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void commitVolumeGroup(VolumeGroup& v, const std::string& cmd)
{
    try
    {
        gn::commitGroup(Graph::current(), *v.node, std::move(v.group));
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

gn::GNode* volumeNodeNamed(gn::Group& grp, const std::string& name)
{
    for (gn::GNode& n : grp.nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

/** The Terrain Volume node, added last on the heightfield chain into the group output when missing. */
gn::GNode& ensureVolumeNode(gn::Group& grp, const std::string& cmd)
{
    if (gn::GNode* n = volumeNodeNamed(grp, kNodeName))
    {
        if (n->type != "TerrainVolume")
            throw CommandError(cmd + ": the terrain's group has a " + n->type + " node named " + kNodeName + " already");
        return *n;
    }
    auto linkInto = [&](const std::string& node, const std::string& socket) -> gn::GLink* {
        for (gn::GLink& l : grp.links)
            if (l.toNode == node && l.toSocket == socket)
                return &l;
        return nullptr;
    };
    // Biomes and material layers read the split heights, so the volume goes before them.
    std::string toName = "GroupOutput", toSocket = "geometry";
    gn::GLink* in = linkInto(toName, toSocket);
    for (const gn::GNode* f = in ? volumeNodeNamed(grp, in->fromNode) : nullptr; f && (f->type == "TerrainBiomes" || f->type == "TerrainLayers");
         f = in ? volumeNodeNamed(grp, in->fromNode) : nullptr)
    {
        toName = f->name;
        toSocket = "heightfield";
        in = linkInto(toName, toSocket);
    }
    if (!in)
        throw CommandError(cmd + ": the terrain's group has no heightfield running into its output");
    const gn::GNode* from = volumeNodeNamed(grp, in->fromNode);
    const gn::GNode* to = volumeNodeNamed(grp, toName);
    const glm::dvec2 at = from && to ? (from->position + to->position) * 0.5 + glm::dvec2(0.0, 160.0) : glm::dvec2(0.0);
    const std::string fromNode = in->fromNode, fromSocket = in->fromSocket;
    in->fromNode = kNodeName;
    in->fromSocket = "heightfield";
    grp.links.push_back({fromNode, fromSocket, kNodeName, "heightfield"});
    gn::GNode n;
    n.name = kNodeName;
    n.type = "TerrainVolume";
    n.position = at;
    grp.nodes.push_back(std::move(n));
    return grp.nodes.back();
}

json opsOf(const gn::GNode& n)
{
    auto it = n.values.find("ops");
    if (it == n.values.end() || !it->is_string() || it->get<std::string>().empty())
        return json::array();
    json j = json::parse(it->get<std::string>(), nullptr, false);
    return j.is_array() ? j : json::array();
}

json settingsOf(const gn::GNode& n)
{
    auto it = n.values.find("settings");
    if (it == n.values.end() || !it->is_string() || it->get<std::string>().empty())
        return json::object();
    json j = json::parse(it->get<std::string>(), nullptr, false);
    return j.is_object() ? j : json::object();
}

std::string volumeSource(gn::Group& grp, const std::string& object)
{
    for (const gn::GNode& n : grp.nodes)
        if (n.type == "ObjectInfo" && n.name.starts_with(kVolumeSourcePrefix) && n.values.value("object", std::string()) == object)
            return n.name;
    gn::GNode n;
    n.name = grp.uniqueNodeName(kVolumeSourcePrefix);
    n.type = "ObjectInfo";
    n.position = {-600.0, -400.0 - 120.0 * double(grp.nodes.size())};
    n.values["object"] = object;
    grp.nodes.push_back(n);
    return n.name;
}

void dropUnusedVolumeSources(gn::Group& grp)
{
    std::set<std::string> used;
    for (const gn::GNode& n : grp.nodes)
        if (n.type == "TerrainVolume")
            for (const json& o : opsOf(n))
                if (o.contains("object"))
                    used.insert(o["object"].get<std::string>());
    std::erase_if(grp.nodes, [&](const gn::GNode& n) { return n.type == "ObjectInfo" && n.name.starts_with(kVolumeSourcePrefix) && !used.count(n.name); });
}

// ---------------------------------------------------------------------------------------------------- results as json

json boundsJson(const VolumeTarget& t, const geo::Bounds& b)
{
    if (!b.valid())
        return nullptr;
    glm::dvec3 lo(1e300), hi(-1e300);
    for (int c = 0; c < 8; ++c)
    {
        const glm::dvec3 p((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
        const glm::dvec3 w = t.toWorld(p);
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
    return {{"min", volumeVec(lo)}, {"max", volumeVec(hi)}};
}

json volumeJson(const VolumeTarget& t, const VolumeResult& r)
{
    const double a = t.across();
    json pieces = json::array();
    for (const VolumePiece& p : r.pieces)
    {
        json lods = json::array();
        for (const auto& m : p.lods)
            lods.push_back(int64_t(m->numFaces()));
        pieces.push_back({{"name", p.name},
                          {"region", p.region + 1},
                          {"chunk", p.chunk + 1},
                          {"faces", int64_t(p.lods[0]->numFaces())},
                          {"points", int64_t(p.lods[0]->numPoints())},
                          {"lodFaces", lods},
                          {"bounds", boundsJson(t, p.bounds)},
                          {"center", volumeVec(t.toWorld(p.bounds.center()))},
                          {"holeCells", int64_t(p.holeCells)},
                          {"buriedCells", int64_t(p.buriedCells)},
                          {"materials", p.materials}});
    }
    json ops = json::object();
    for (const auto& [id, o] : r.ops)
    {
        ops[id] = {{"applied", o.applied}, {"bounds", boundsJson(t, o.bounds)}};
        if (!o.message.empty())
            ops[id]["message"] = o.message;
    }
    return {{"split", r.settings.split},
            {"voxelSize", r.voxel * a},
            {"voxelsPerCell", r.voxelsPerCell},
            {"bricks", int64_t(r.bricks)},
            {"voxels", int64_t(r.voxels)},
            {"heightChanged", int64_t(r.heightChanged)},
            {"heightError", r.heightError * a},
            {"openCells", int64_t(r.openCells)},
            {"buriedCells", int64_t(r.buriedCells)},
            {"holeSamples", int64_t(r.holeSamples)},
            {"holeCells", int64_t(r.holeCells)},
            {"unmeshedColumns", int64_t(r.unmeshedColumns)},
            {"seamLength", r.seamLength * a},
            {"seamUncovered", r.seamUncovered * a},
            {"seamGap", r.seamGap * a},
            {"pieceCount", int64_t(r.pieces.size())},
            {"pieces", pieces},
            {"ops", ops},
            {"warnings", r.warnings}};
}

std::shared_ptr<const VolumeResult> lastResult(const VolumeTarget& t)
{
    return t.modifier ? volumeResult(t.modifier->id(), kNodeName) : nullptr;
}

// ---------------------------------------------------------------------------------------------------- input points

/** A curve object's shape as world-space polylines. */
std::vector<std::vector<glm::dvec3>> volumeCurvePolylines(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError(cmd + ": no curve named '" + name + "'");
    const int attr = n->type().attrIndex("worldSpace");
    gn::CurveSet set;
    for (const curve::Spline& s : curve::splinesOf(g.value({n->id(), attr, -1})))
        set.addSpline(s);
    const gn::CurveSet e = set.isPoly() ? set : set.evaluated();
    std::vector<std::vector<glm::dvec3>> out;
    for (size_t c = 0; c < e.numCurves(); ++c)
    {
        const auto pts = e.curvePoints(c);
        out.emplace_back(pts.begin(), pts.end());
        if (e.cyclic[c] && !out.back().empty())
            out.back().push_back(out.back().front());
    }
    return out;
}

/**
 * The path in the heightfield's space: [x, y, z] world points as given, or [x, z] ground points (x, y on a Z-up scene)
 * placed below cm under the ground (ends at endBelow when it is set), or a curve object's first curve.
 */
std::vector<glm::dvec3> localPath(const Args& a, const VolumeTarget& t, const std::string& cmd, double below, double endBelow, const char* flag = "path")
{
    std::vector<glm::dvec3> out;
    if (a.has(flag))
    {
        const json list = a.flags[flag];
        if (!list.is_array())
            throw CommandError(cmd + ": " + flag + " is a list of points");
        for (size_t k = 0; k < list.size(); ++k)
        {
            const json& p = list[k];
            if (!p.is_array() || (p.size() != 2 && p.size() != 3))
                throw CommandError(cmd + ": points are [x, y, z] or ground [x, " + (volumeZUp() ? "y" : "z") + "]");
            if (p.size() == 3)
            {
                out.push_back(t.toLocal({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}));
                continue;
            }
            glm::dvec3 l = t.toLocal(volumeFromGround({p[0].get<double>(), p[1].get<double>()}));
            const bool end = k == 0 || k + 1 == list.size();
            l.y = t.local->heightAt(l.x, l.z) - (end && endBelow >= 0.0 ? endBelow : below) / t.across();
            out.push_back(l);
        }
    }
    else if (a.has("curve"))
    {
        const auto lines = volumeCurvePolylines(Graph::current(), a.get<std::string>("curve", ""), cmd);
        if (!lines.empty())
            for (const glm::dvec3& p : lines[0])
                out.push_back(t.toLocal(p));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- writing ops

/** Adds or replaces an op on the terrain's Terrain Volume node and returns the result of the evaluation after it. */
json writeOp(const Args& a, VolumeOp op, const std::string& cmd)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const VolumeTarget t = oneVolumeTarget(a, cmd);
    VolumeGroup v = volumeGroup(g, t, cmd);
    gn::GNode& node = ensureVolumeNode(v.group, cmd);
    json list = opsOf(node);
    std::string id = a.get<std::string>("id", "");
    if (id.empty())
        for (int k = 1; id.empty(); ++k)
        {
            const std::string candidate = op.kind + std::to_string(k);
            if (std::none_of(list.begin(), list.end(), [&](const json& e) { return e.value("id", std::string()) == candidate; }))
                id = candidate;
        }
    op.id = id;
    if (!op.object.empty())
    {
        Node* src = g.find(op.object);
        if (!src || !src->type().isA("mesh"))
            throw CommandError(cmd + ": object must name a mesh");
        if (t.mesh && src->id() == t.mesh->id())
            throw CommandError(cmd + ": a terrain can't stamp itself");
        op.object = volumeSource(v.group, src->path());
    }
    bool replaced = false;
    for (json& e : list)
        if (e.value("id", std::string()) == id)
        {
            e = op.toJson();
            replaced = true;
        }
    if (!replaced)
        list.push_back(op.toJson());
    node.values["ops"] = list.dump();
    dropUnusedVolumeSources(v.group);
    commitVolumeGroup(v, cmd);

    const VolumeTarget after = findVolumeTarget(g, volumePathOf(t, a), cmd);
    json out = {{"id", id}, {"kind", op.kind}, {"replaced", replaced}};
    if (auto r = lastResult(after))
    {
        out["volume"] = volumeJson(after, *r);
        if (auto it = r->ops.find(id); it != r->ops.end())
        {
            out["applied"] = it->second.applied;
            out["bounds"] = boundsJson(after, it->second.bounds);
            if (!it->second.message.empty())
                out["message"] = it->second.message;
        }
    }
    out["revision"] = revision(*after.local);
    return out;
}

VolumeOp baseOp(const Args& a, const VolumeTarget& t, const std::string& kind, const std::string& material, bool subtract)
{
    VolumeOp op;
    op.kind = kind;
    op.material = a.get<std::string>("material", material);
    op.subtract = a.has("mode") ? a.get<std::string>("mode", "subtract") == "subtract" : subtract;
    op.seed = uint32_t(a.get<int64_t>("seed", 0));
    op.noise = std::max(0.0, a.get<double>("noise", op.noise));
    op.noiseSize = std::max(1.0, a.get<double>("noiseSize", op.noiseSize)) / t.across();
    op.blend = std::max(0.0, a.get<double>("blend", 0.0)) / t.across();
    return op;
}

std::vector<FlagDef> commonFlags(std::vector<FlagDef> flags, const char* material, double noise)
{
    flags.push_back({"id", "id", VT::String, "", "Stable id; the same id again replaces that edit. Empty makes one."});
    flags.push_back({"material", "mat", VT::String, material, "Material name its surfaces get (a face material on the pieces)."});
    flags.push_back({"noise", "nz", VT::Float, noise, "Surface roughness, a fraction of its size.", {}, {}, 0.0, 2.0});
    flags.push_back({"noiseSize", "ns", VT::Float, 400.0, "Size of the bumps, cm.", {}, {}, 1.0});
    flags.push_back({"blend", "bl", VT::Float, 0.0, "Width of a smooth join with what is there, cm.", {}, {}, 0.0});
    flags.push_back({"seed", "sd", VT::Int, int64_t(0), "Another seed gives another shape.", {}, {}, 0.0});
    return flags;
}

std::vector<FlagDef> pathFlags(const char* what)
{
    return {{"path", "p", VT::List, nullptr, std::string(what) + ": [[x, y, z], ...] world points, or [[x, z], ...] on the ground (x, y on a Z-up scene)."},
            nodeFlag({"curve", "cv", VT::String, nullptr, std::string(what) + " from a curve object (its first curve, world space)."}, "curve")};
}

json removeOps(const Args& a, const std::string& cmd)
{
    Graph& g = Graph::current();
    const VolumeTarget t = oneVolumeTarget(a, cmd);
    VolumeGroup v = volumeGroup(g, t, cmd);
    gn::GNode* node = volumeNodeNamed(v.group, kNodeName);
    if (!node)
        return {{"removed", json::array()}, {"revision", revision(*t.local)}};
    json list = opsOf(*node);
    std::set<std::string> ids;
    for (const json& id : a.flags.value("ids", json::array()))
        ids.insert(id.get<std::string>());
    json removed = json::array(), kept = json::array();
    for (const json& e : list)
    {
        const std::string id = e.value("id", std::string());
        if (ids.empty() || ids.count(id))
            removed.push_back(id);
        else
            kept.push_back(e);
    }
    node->values["ops"] = kept.dump();
    dropUnusedVolumeSources(v.group);
    commitVolumeGroup(v, cmd);
    const VolumeTarget after = findVolumeTarget(g, volumePathOf(t, a), cmd);
    json out = {{"removed", removed}, {"revision", revision(*after.local)}};
    if (auto r = lastResult(after))
        out["volume"] = volumeJson(after, *r);
    return out;
}

// ---------------------------------------------------------------------------------------------------- the node

gn::NodeDecl volumeDecl()
{
    gn::NodeDecl d;
    d.type = "TerrainVolume";
    d.label = "Terrain Volume";
    d.category = "Terrain";
    d.doc = "Caves, tunnels, overhangs, arches and mesh stamps added to or carved from the ground as a sparse signed distance "
            "field, then split: what the heightfield can hold goes into its heights, every connected place it can't becomes a "
            "mesh piece (with LODs, triplanar uvs and material ids), with a holes mask under the pieces the ground opens for. "
            "Mesh edges drop below the ground by a skirt and sit exactly on it at the seam. terrainCave, terrainTunnel, "
            "terrainOverhang, terrainArch and terrainVolumeStamp write the list; terrainSplit sets the split. Keep it last.";
    d.props = {{.id = "ops", .defaultValue = "[]", .doc = "The edits, a JSON list in the heightfield's own space (see terrainCave)."},
               {.id = "settings", .defaultValue = "{}", .doc = "Split settings, a JSON object (see terrainSplit)."}};
    d.sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        gn::GeometryPtr g = c.geometry("heightfield");
        const Heightfield* in = heightfieldOf(*g);
        if (!in)
            throw gn::ExecError(c.node.name + ": the geometry has no heightfield (start from Heightfield Grid or Mesh to Heightfield)");
        json list = json::array(), settings = json::object();
        try
        {
            if (const std::string text = c.prop("ops"); !text.empty())
                list = json::parse(text);
            if (const std::string text = c.prop("settings"); !text.empty())
                settings = json::parse(text);
        }
        catch (const std::exception& e)
        {
            throw gn::ExecError(c.node.name + ": " + e.what());
        }
        if (!list.is_array() || list.empty())
        {
            publishVolumeResult(c.env.owner, c.node.name, std::make_shared<const VolumeResult>());
            c.out("heightfield", g);
            return;
        }
        std::vector<VolumeOp> ops;
        std::map<std::string, std::shared_ptr<const geo::Mesh>> meshes;
        for (const json& j : list)
        {
            try
            {
                ops.push_back(VolumeOp::fromJson(j));
            }
            catch (const std::exception& e)
            {
                throw gn::ExecError(c.node.name + ": " + e.what());
            }
            const VolumeOp& op = ops.back();
            if (op.kind != "mesh" || meshes.count(op.object))
                continue;
            auto it = c.env.objects.find(op.object);
            if (it == c.env.objects.end() || !it->second.mesh)
                continue;
            geo::Mesh m = *it->second.mesh;
            const glm::dmat4 toLocal = glm::inverse(c.env.ownerMatrix) * it->second.matrix;
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p))
                    m.setPosition(p, glm::dvec3(toLocal * glm::dvec4(m.position(p), 1.0)));
            meshes[op.object] = std::make_shared<const geo::Mesh>(std::move(m));
        }
        Heightfield h = *in;
        auto r = std::make_shared<VolumeResult>(buildVolume(h, ops, VolumeSettings::fromJson(settings), meshes));
        gn::Geometry out = *g;
        out.heightfield = std::make_shared<const Heightfield>(std::move(h));
        if (r->mesh)
        {
            geo::Mesh joined = out.mesh ? *out.mesh : geo::Mesh();
            joined.append(*r->mesh);
            joined.compact();
            out.mesh = std::make_shared<const geo::Mesh>(std::move(joined));
        }
        publishVolumeResult(c.env.owner, c.node.name, std::move(r));
        c.out("heightfield", gn::makeGeometry(std::move(out)));
    };
    return d;
}

// ---------------------------------------------------------------------------------------------------- commands

/** terrainTunnel fromRoad=: one bore per tunnel span the road reported, along its floor, at its radius. */
json tunnelsFromRoad(const Args& a)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError("terrainTunnel: give one terrain");
    const std::string ref = a.get<std::string>("fromRoad", "");
    const size_t slash = ref.find('/');
    const std::string layer = slash == std::string::npos ? std::string() : ref.substr(0, slash);
    const std::string id = slash == std::string::npos ? ref : ref.substr(slash + 1);
    const json roads = ops::execute("terrainRoads", {{"args", {names[0]}}});
    std::string prefix = a.get<std::string>("id", "");
    if (prefix.empty())
        prefix = id + "_tunnel";
    json made = json::array();
    bool found = false;
    for (const auto& [name, list] : roads["layers"].items())
    {
        if (!layer.empty() && name != layer)
            continue;
        for (const json& road : list)
        {
            if (road.value("id", std::string()) != id)
                continue;
            found = true;
            int k = 0;
            for (const json& tunnel : road.value("tunnels", json::array()))
            {
                json call = {{"args", {names[0]}},
                             {"path", tunnel["floor"]},
                             {"radius", tunnel.value("radius", a.get<double>("radius", 400.0))},
                             {"id", prefix + std::to_string(++k)}};
                made.push_back(ops::execute("terrainTunnel", call));
            }
        }
    }
    if (!found)
        throw CommandError("terrainTunnel: no road " + ref + " (terrainRoads lists them)");
    if (made.empty())
        throw CommandError("terrainTunnel: road " + ref + " has no tunnel spans");
    json out = made.back();
    out["tunnels"] = made;
    return out;
}

void registerVolumeCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainCave",
        .doc = "Carve a cave into a terrain along a path: a tunnel of radius cm that wanders and swells with noise, branches "
               "splitting off it, and chambers at their ends. Ground points ([x, z]) sit below cm under the ground, the ends "
               "only endBelow so the cave opens there; world points ([x, y, z]) are used as they are.\n\n"
               "The terrain's split then keeps what the heightfield can hold in its heights and turns the cave into mesh "
               "pieces: under solid ground the landscape stays on top, where the cave opens the ground gets a hole and the "
               "piece fills it, its edge sitting on the ground. Returns the edit's id and bounds, the split (pieces, holes, seam "
               "figures; see terrainSplit) and the revision. The same id again replaces the cave.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags(
            [] {
                std::vector<FlagDef> f = pathFlags("The cave's line");
                f.push_back({"radius", "r", VT::Float, 300.0, "Tunnel radius, cm.", {}, {}, 1.0});
                f.push_back({"below", "bw", VT::Float, nullptr, "Ground points: depth of the line under the ground, cm. Default three radii."});
                f.push_back({"endBelow", "eb", VT::Float, nullptr, "Ground points: depth of the first and last point, cm. Default a third of the radius, so the cave opens."});
                f.push_back({"branches", "br", VT::Int, int64_t(0), "Side passages.", {}, {}, 0.0, 32.0});
                f.push_back({"branchLength", "bln", VT::Float, 2000.0, "Length of a side passage, cm.", {}, {}, 1.0});
                f.push_back({"chambers", "ch", VT::Int, int64_t(0), "Wider chambers, at the passages' ends first.", {}, {}, 0.0, 32.0});
                f.push_back({"chamberRadius", "chr", VT::Float, 600.0, "Chamber radius, cm.", {}, {}, 1.0});
                return f;
            }(),
            "cave", 0.25),
        .run = [](const Args& a) -> json {
            const VolumeTarget t = oneVolumeTarget(a, "terrainCave");
            VolumeOp op = baseOp(a, t, "cave", "cave", true);
            const double r = std::max(1.0, a.get<double>("radius", 300.0));
            op.radius = r / t.across();
            op.path = localPath(a, t, "terrainCave", a.get<double>("below", 3.0 * r), a.get<double>("endBelow", r / 3.0));
            if (op.path.size() < 2)
                throw CommandError("terrainCave: give path= (two or more points) or curve=");
            op.branches = int(a.get<int64_t>("branches", 0));
            op.branchLength = std::max(1.0, a.get<double>("branchLength", 2000.0)) / t.across();
            op.chambers = int(a.get<int64_t>("chambers", 0));
            op.chamberRadius = std::max(1.0, a.get<double>("chamberRadius", 600.0)) / t.across();
            if (!a.has("blend"))
                op.blend = 0.3 * op.radius;
            return writeOp(a, op, "terrainCave");
        },
        .examples = {"terrainCave('Terrain', path=[[-3000, 0], [0, 500], [2500, 2000]], radius=350, branches=2, chambers=1)['volume']['pieces']",
                     "terrainCave('Terrain', curve='CaveLine', radius=250, noise=0.4, seed=3)"},
    });

    ctx.registerCommand({
        .name = "terrainTunnel",
        .doc = "Bore a road tunnel along a path at road level: a flat floor on the path and a round roof radius cm high at "
               "the middle, opened a radius past each end so the portals are clear. Ground points sit on the ground; world "
               "points are the road surface. fromStamp='layer/id' takes the tunnel spans of a street or tunnel stamp (see "
               "terrainAddFeature) with a radius from its width; fromRoad='id' (or 'layer/id') bores every tunnel span of a road "
               "laid with terrainAddRoad, at its radius. Returns as terrainCave does.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags(
            [] {
                std::vector<FlagDef> f = pathFlags("The road's line");
                f.push_back({"radius", "r", VT::Float, 400.0, "Half the tunnel's width at the springline, cm.", {}, {}, 1.0});
                f.push_back({"fromStamp", "fs", VT::String, nullptr, "A line stamp as layer/id, such as Stamps/street1, whose tunnel spans to bore."});
                f.push_back({"fromRoad", "fr", VT::String, nullptr, "A road's id, or layer/id, whose tunnel spans to bore."});
                return f;
            }(),
            "tunnel", 0.0),
        .run = [](const Args& a) -> json {
            const VolumeTarget t = oneVolumeTarget(a, "terrainTunnel");
            VolumeOp op = baseOp(a, t, "tunnel", "tunnel", true);
            const double r = std::max(1.0, a.get<double>("radius", 400.0));
            op.radius = r / t.across();
            if (a.has("fromRoad"))
                return tunnelsFromRoad(a);
            if (a.has("fromStamp"))
            {
                const std::string ref = a.get<std::string>("fromStamp", "");
                const size_t slash = ref.find('/');
                if (slash == std::string::npos || !t.modifier)
                    throw CommandError("terrainTunnel: fromStamp is layer/id, such as Stamps/street1");
                const auto results = stampResults(t.modifier->id(), ref.substr(0, slash));
                auto it = results.find(ref.substr(slash + 1));
                if (it == results.end())
                    throw CommandError("terrainTunnel: no stamp " + ref);
                const StampResult& sr = it->second;
                json made = json::array();
                int k = 0;
                for (const StampResult::Span& sp : sr.spans)
                {
                    if (sp.kind != "tunnel")
                        continue;
                    VolumeOp span = op;
                    span.path.clear();
                    for (size_t p = sp.from; p <= sp.to && p < sr.centreline.size(); ++p)
                        span.path.push_back(sr.centreline[p]);
                    if (span.path.size() < 2)
                        continue;
                    Args sub = a;
                    sub.flags["id"] = a.get<std::string>("id", ref.substr(slash + 1) + "_tunnel") + std::to_string(++k);
                    made.push_back(writeOp(sub, span, "terrainTunnel"));
                }
                if (made.empty())
                    throw CommandError("terrainTunnel: " + ref + " has no tunnel spans");
                json out = made.back();
                out["tunnels"] = made;
                return out;
            }
            op.path = localPath(a, t, "terrainTunnel", 0.0, -1.0);
            if (op.path.size() < 2)
                throw CommandError("terrainTunnel: give path=, curve=, fromStamp= or fromRoad=");
            return writeOp(a, op, "terrainTunnel");
        },
        .examples = {"terrainTunnel('Terrain', path=[[-4000, 0], [4000, 0]], radius=450)", "terrainTunnel('Terrain', fromStamp='Stamps/street1')"},
    });

    ctx.registerCommand({
        .name = "terrainOverhang",
        .doc = "Undercut a cliff along a line: rock is cut into the hill (whichever side rises) depth cm deep from the ground up "
               "to height cm, deeper in soft strata than in the hard bands between them, roughened by noise, closing toward "
               "its top and ends, so the hill above overhangs. lip adds a shelf jutting out over the cut. Returns as "
               "terrainCave does.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags(
            [] {
                std::vector<FlagDef> f = pathFlags("The cliff's foot");
                f.push_back({"depth", "dp", VT::Float, 400.0, "How far it cuts in, cm.", {}, {}, 1.0});
                f.push_back({"height", "h", VT::Float, 600.0, "How high the cut runs above the ground, cm.", {}, {}, 1.0});
                f.push_back({"strata", "sa", VT::Float, 150.0, "Thickness of the rock bands, cm.", {}, {}, 1.0});
                f.push_back({"hardness", "hd", VT::Float, 0.35, "How deep a hard band cuts, as a fraction of a soft one.", {}, {}, 0.0, 1.0});
                f.push_back({"lip", "lp", VT::Float, 0.0, "A shelf this far out over the cut, cm.", {}, {}, 0.0});
                return f;
            }(),
            "cliff", 0.3),
        .run = [](const Args& a) -> json {
            const VolumeTarget t = oneVolumeTarget(a, "terrainOverhang");
            VolumeOp op = baseOp(a, t, "overhang", "cliff", true);
            op.path = localPath(a, t, "terrainOverhang", 0.0, -1.0);
            if (op.path.size() < 2)
                throw CommandError("terrainOverhang: give path= (two or more points) or curve=");
            const double s = t.across();
            op.depth = std::max(1.0, a.get<double>("depth", 400.0)) / s;
            op.height = std::max(1.0, a.get<double>("height", 600.0)) / s;
            op.strata = std::max(1.0, a.get<double>("strata", 150.0)) / s;
            op.hardness = std::clamp(a.get<double>("hardness", 0.35), 0.0, 1.0);
            op.lip = std::max(0.0, a.get<double>("lip", 0.0)) / s;
            return writeOp(a, op, "terrainOverhang");
        },
        .examples = {"terrainOverhang('Terrain', path=[[-2000, 1500], [2000, 1500]], depth=500, height=800, lip=200)"},
    });

    ctx.registerCommand({
        .name = "terrainArch",
        .doc = "Raise a natural arch between two feet on the ground: a rock band height cm high at its top, thickness cm "
               "thick and width cm wide, thicker at the feet, roughened by noise. carve also opens the rock under it, so an "
               "arch drawn through a rock fin or a ridge gets its hole. Returns as terrainCave does.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags(
            {{"from", "fr", VT::List, nullptr, "First foot: [x, z] on the ground (x, y on a Z-up scene) or [x, y, z]."},
             {"to", "to", VT::List, nullptr, "Second foot."},
             {"height", "h", VT::Float, 1500.0, "Height of the arch's top over its feet, cm.", {}, {}, 1.0},
             {"thickness", "th", VT::Float, 250.0, "Thickness of the band in the arch's plane, cm.", {}, {}, 1.0},
             {"width", "w", VT::Float, 400.0, "Width across the arch, cm.", {}, {}, 1.0},
             {"carve", "cr", VT::Bool, true, "Open the rock under the arch."}},
            "rock", 0.15),
        .run = [](const Args& a) -> json {
            const VolumeTarget t = oneVolumeTarget(a, "terrainArch");
            if (!a.has("from") || !a.has("to"))
                throw CommandError("terrainArch: give from= and to=");
            VolumeOp op = baseOp(a, t, "arch", "rock", false);
            Args feet = a;
            feet.flags["path"] = json::array({a.flags["from"], a.flags["to"]});
            op.path = localPath(feet, t, "terrainArch", 0.0, -1.0);
            const double s = t.across();
            op.height = std::max(1.0, a.get<double>("height", 1500.0)) / s;
            op.thickness = std::max(1.0, a.get<double>("thickness", 250.0)) / s;
            op.width = std::max(1.0, a.get<double>("width", 400.0)) / s;
            op.carve = a.get<bool>("carve", true);
            op.subtract = false;
            return writeOp(a, op, "terrainArch");
        },
        .examples = {"terrainArch('Terrain', **{'from': [-1500, 0], 'to': [1500, 0]}, height=1800, thickness=300)"},
    });

    ctx.registerCommand({
        .name = "terrainVolumeStamp",
        .doc = "Add or carve a shape in a terrain's volume: a closed mesh (live: moving it moves the stamp; a library rock, a "
               "boulder, a cut), or a sphere, box or capsule. mode='add' piles it on, 'subtract' carves it out. Returns as "
               "terrainCave does.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags({nodeFlag({"object", "obj", VT::String, nullptr, "A closed mesh to stamp, followed live."}, "mesh"),
                              {"shape", "sh", VT::String, "mesh", "mesh (object=), sphere, box or capsule (path=).", {"mesh", "sphere", "box", "capsule"}},
                              {"mode", "m", VT::String, "add", "add or subtract.", {"add", "subtract"}},
                              {"center", "c", VT::List, nullptr, "Sphere and box centre: [x, y, z], or [x, z] on the ground."},
                              {"size", "sz", VT::Vec3, json::array({500.0, 500.0, 500.0}), "Box size, cm (x, up, z in the terrain's space)."},
                              {"radius", "r", VT::Float, 300.0, "Sphere and capsule radius, cm.", {}, {}, 1.0},
                              {"path", "p", VT::List, nullptr, "Capsule line: [[x, y, z], ...] or ground [[x, z], ...]."}},
                             "rock", 0.0),
        .run = [](const Args& a) -> json {
            const VolumeTarget t = oneVolumeTarget(a, "terrainVolumeStamp");
            std::string shape = a.get<std::string>("shape", "mesh");
            if (shape == "mesh" && !a.has("object"))
                throw CommandError("terrainVolumeStamp: give object= or shape=");
            VolumeOp op = baseOp(a, t, shape, "rock", a.get<std::string>("mode", "add") == "subtract");
            const double s = t.across();
            op.radius = std::max(1.0, a.get<double>("radius", 300.0)) / s;
            if (shape == "mesh")
                op.object = a.get<std::string>("object", "");
            else if (shape == "capsule")
            {
                op.path = localPath(a, t, "terrainVolumeStamp", 0.0, -1.0);
                if (op.path.size() < 2)
                    throw CommandError("terrainVolumeStamp: a capsule needs path=");
            }
            else
            {
                if (!a.has("center"))
                    throw CommandError("terrainVolumeStamp: give center=");
                Args c = a;
                c.flags["path"] = json::array({a.flags["center"]});
                op.center = localPath(c, t, "terrainVolumeStamp", 0.0, -1.0).at(0);
                const json sz = a.flags.value("size", json::array({500.0, 500.0, 500.0}));
                op.size = glm::dvec3(sz[0].get<double>(), sz[1].get<double>(), sz[2].get<double>()) / s;
            }
            return writeOp(a, op, "terrainVolumeStamp");
        },
        .examples = {"terrainVolumeStamp('Terrain', object='Boulder', mode='add')",
                     "terrainVolumeStamp('Terrain', shape='sphere', center=[0, 0], radius=800, mode='subtract')"},
    });

    ctx.registerCommand({
        .name = "terrainVolumeRemove",
        .doc = "Remove volume edits from a terrain by id (all of them when ids is empty). Returns the removed ids and the split.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"ids", "i", VT::List, json::array(), "Ids to remove; empty removes every edit."}},
        .run = [](const Args& a) -> json { return removeOps(a, "terrainVolumeRemove"); },
        .examples = {"terrainVolumeRemove('Terrain', ids=['cave1'])"},
    });

    ctx.registerCommand({
        .name = "terrainVolume",
        .doc = "A terrain's volume edits (their settings, in its own space) and the split's state, as terrainSplit returns it.",
        .positionalDoc = "terrain mesh or its modifier",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const VolumeTarget t = oneVolumeTarget(a, "terrainVolume");
            VolumeGroup v = volumeGroup(g, t, "terrainVolume");
            json out = {{"ops", json::array()}, {"settings", VolumeSettings().toJson()}};
            if (gn::GNode* n = volumeNodeNamed(v.group, kNodeName))
            {
                out["ops"] = opsOf(*n);
                out["settings"] = VolumeSettings::fromJson(settingsOf(*n)).toJson();
            }
            if (auto r = lastResult(t))
                out["volume"] = volumeJson(t, *r);
            out["revision"] = revision(*t.local);
            return out;
        },
        .examples = {"terrainVolume('Terrain')['volume']['pieceCount']"},
    });

    ctx.registerCommand({
        .name = "terrainSplit",
        .doc = "Set how a terrain's volume is split and list the pieces.\n\n"
               "split on (default, for heightfield engines): what the heightfield can hold goes into its heights; every connected "
               "place it can't (caves, overhangs, arches) becomes a mesh piece. Where the ground still covers a cave from above "
               "(at least minRoof of rock) the landscape stays and the piece lies under it; where the cave opens, the holes mask "
               "takes out those cells (margin samples past the opening) and the piece fills them exactly, its edge on the "
               "ground's heights and normals, and a skirt dropping skirt cm below it. split off keeps the heights as they were "
               "and meshes every changed place.\n\n"
               "Returns voxelSize, bricks, heightChanged, heightError (most the heightfield misses of the volume, cm), openCells, "
               "buriedCells, holeCells, unmeshedColumns (0: every region became a mesh), seamLength, seamUncovered and seamGap "
               "(cracks: 0 is none), and each piece: name, region, chunk, faces, LOD faces, bounds, centre, hole and buried cells "
               "and materials.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"split", "s", VT::Bool, nullptr, "Split for heightfield engines (on), or mesh every changed place (off)."},
                  {"voxelSize", "vs", VT::Float, nullptr, "Voxel edge, cm; 0 picks a third of the smallest feature. Rounded to a whole fraction of the cell.", {}, {}, 0.0},
                  {"minRoof", "mr", VT::Float, nullptr, "Rock over a cave the landscape stays on, cm.", {}, {}, 0.0},
                  {"maxHeightError", "mhe", VT::Float, nullptr, "Mesh places the heightfield's cells miss the shape by more than this, cm; 0 is off.", {}, {}, 0.0},
                  {"margin", "mg", VT::Int, nullptr, "Hole samples past each opening.", {}, {}, 0.0, 16.0},
                  {"skirt", "sk", VT::Float, nullptr, "Mesh edges drop this far below the ground, cm.", {}, {}, 0.0},
                  {"lods", "ld", VT::Int, nullptr, "Levels of detail per piece.", {}, {}, 1.0, 5.0},
                  {"chunkSize", "cs", VT::Float, nullptr, "Pieces wider than this are cut into chunks, cm; 0 keeps them whole.", {}, {}, 0.0},
                  {"uvScale", "uv", VT::Float, nullptr, "Triplanar texture repeat, cm.", {}, {}, 1.0},
                  {"maxVoxels", "mv", VT::Float, nullptr, "Most voxels; past it the voxel grows, with a warning.", {}, {}, 1e5}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const VolumeTarget t = oneVolumeTarget(a, "terrainSplit");
            VolumeGroup v = volumeGroup(g, t, "terrainSplit");
            gn::GNode& node = ensureVolumeNode(v.group, "terrainSplit");
            json s = settingsOf(node);
            const double k = t.across();
            for (const char* key : {"split", "margin", "lods"})
                if (a.has(key))
                    s[key] = a.flags[key];
            for (const char* key : {"voxelSize", "minRoof", "maxHeightError", "skirt", "chunkSize", "uvScale"})
                if (a.has(key))
                    s[key] = a.get<double>(key, 0.0) / k;
            if (a.has("maxVoxels"))
                s["maxVoxels"] = a.get<double>("maxVoxels", 16e6);
            node.values["settings"] = VolumeSettings::fromJson(s).toJson().dump();
            commitVolumeGroup(v, "terrainSplit");
            const VolumeTarget after = findVolumeTarget(g, volumePathOf(t, a), "terrainSplit");
            json out = {{"settings", VolumeSettings::fromJson(s).toJson()}, {"revision", revision(*after.local)}};
            if (auto r = lastResult(after))
                out["volume"] = volumeJson(after, *r);
            return out;
        },
        .examples = {"s = terrainSplit('Terrain', lods=3)\n[(p['name'], p['faces']) for p in s['volume']['pieces']]", "terrainSplit('Terrain', split=False)"},
    });
}
} // namespace

// ---------------------------------------------------------------------------------------------------- shared with terrain commands

json volumeIssues(uint64_t modifier, const glm::dmat4& world, double tolerance)
{
    LILA_ZONE();

    json issues = json::array();
    const auto h = gn::modifierHeightfield(modifier);
    if (!h)
        return issues;
    const double across = 0.5 * (glm::length(glm::dvec3(world[0])) + glm::length(glm::dvec3(world[2])));
    auto at = [&](const glm::dvec3& l) { return volumeVec(glm::dvec3(world * glm::dvec4(l, 1.0))); };
    for (const auto& [node, r] : volumeResults(modifier))
    {
        if (!r)
            continue;
        const std::string about = node;
        for (const auto& [id, o] : r->ops)
            if (!o.applied || !o.message.empty())
                issues.push_back({{"type", "volumeEdit"}, {"about", about + "/" + id}, {"position", o.bounds.valid() ? at(o.bounds.center()) : json()},
                                  {"message", o.message.empty() ? std::string("changes nothing") : o.message}});
        if (r->unmeshedColumns > 0)
            issues.push_back({{"type", "volumeUnmeshed"}, {"about", about}, {"position", nullptr},
                              {"message", std::format("{} columns the heightfield can't hold have no mesh", r->unmeshedColumns)}});
        std::vector<std::shared_ptr<const geo::Mesh>> lod0;
        for (const VolumePiece& p : r->pieces)
            lod0.push_back(p.lods[0]);
        if (lod0.empty() && r->holeCells == 0)
            continue;
        const SeamReport seams = measureSeams(*h, lod0);
        if (seams.uncovered > 0.0)
            issues.push_back({{"type", "volumeSeam"}, {"about", about}, {"position", nullptr},
                              {"message", std::format("{:.0f} cm of hole outline has no mesh over it", seams.uncovered * across)}});
        if (seams.maxGap * across > tolerance)
            issues.push_back({{"type", "volumeSeam"}, {"about", about}, {"position", at(seams.worst)},
                              {"message", std::format("mesh and ground part by {:.2f} cm at the seam, over {:.2f}", seams.maxGap * across, tolerance)}});
        const FootprintReport fp = measureFootprint(*h, lod0, std::max(tolerance / across, 2.0 * r->voxel));
        if (fp.uncoveredInHole > 0 || fp.meshOutsideHole > 0)
            issues.push_back({{"type", "volumeHoles"}, {"about", about}, {"position", nullptr},
                              {"message", std::format("{} points in holes have no mesh, {} points outside holes have mesh above the ground", fp.uncoveredInHole,
                                                      fp.meshOutsideHole)}});
    }
    return issues;
}

namespace
{
/** A piece's LODs as an FBX in world space about centre: an LOD group when there are several. */
void writePiece(const VolumePiece& p, const glm::dmat4& world, const glm::dvec3& centre, const fs::path& file, const std::string& name)
{
    fbx::ExportScene scene;
    auto placed = [&](const geo::Mesh& m) {
        geo::Mesh out = m;
        for (geo::Index v = 0; v < out.pointCapacity(); ++v)
            if (out.pointValid(v))
                out.setPosition(v, glm::dvec3(world * glm::dvec4(out.position(v), 1.0)) - centre);
        const glm::dmat3 n = glm::transpose(glm::inverse(glm::dmat3(world)));
        std::span<glm::vec3> nrm = out.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        for (glm::vec3& v : nrm)
            v = glm::vec3(glm::normalize(n * glm::dvec3(v)));
        if (glm::determinant(glm::dmat3(world)) < 0.0)
            for (geo::Index f = 0; f < out.faceCapacity(); ++f)
                if (out.faceValid(f))
                    out.flipFace(f);
        return std::make_shared<const geo::Mesh>(std::move(out));
    };
    if (p.lods.size() == 1)
    {
        fbx::ExportNode node;
        node.name = name;
        node.mesh = placed(*p.lods[0]);
        scene.nodes.push_back(std::move(node));
    }
    else
    {
        fbx::ExportNode group;
        group.name = name;
        group.lodGroup = true;
        scene.nodes.push_back(std::move(group));
        for (size_t l = 0; l < p.lods.size(); ++l)
        {
            fbx::ExportNode node;
            node.name = name + "_LOD" + std::to_string(l);
            node.parent = 0;
            node.mesh = placed(*p.lods[l]);
            scene.nodes.push_back(std::move(node));
        }
    }
    std::string err;
    if (!fbx::writeFbx(scene, file, fbx::ExportOptions{}, &err))
        throw std::runtime_error("writing " + file.string() + ": " + err);
}

std::string assetName(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out;
}
} // namespace

json exportVolume(uint64_t modifier, const glm::dmat4& world, const fs::path& folder, const std::string& name)
{
    LILA_ZONE();

    json pieces = json::array();
    std::vector<std::string> warnings;
    for (const auto& [node, r] : volumeResults(modifier))
    {
        if (!r)
            continue;
        for (const VolumePiece& p : r->pieces)
        {
            const std::string stem = name + "_" + p.name;
            const glm::dvec3 centre = glm::dvec3(world * glm::dvec4(p.bounds.center(), 1.0));
            const fs::path file = folder / (stem + ".fbx");
            fs::create_directories(folder);
            writePiece(p, world, centre, file, stem);
            json lods = json::array();
            for (const auto& m : p.lods)
                lods.push_back(int64_t(m->numFaces()));
            pieces.push_back({{"name", p.name},
                              {"file", file.generic_string()},
                              {"center", volumeVec(centre)},
                              {"lodFaces", lods},
                              {"holeCells", int64_t(p.holeCells)},
                              {"materials", p.materials}});
        }
        warnings.insert(warnings.end(), r->warnings.begin(), r->warnings.end());
    }
    if (pieces.empty())
        return nullptr;
    json out = {{"pieces", pieces}, {"warnings", warnings}, {"space", "world, each piece about its centre"}};
    std::ofstream(folder / (name + "_volume.json")) << out.dump(2);
    out["settings"] = (folder / (name + "_volume.json")).generic_string();
    return out;
}

json sendVolumeToUnreal(uint64_t modifier, const glm::dmat4& world, const json& settings, const std::string& id, const std::string& label,
                        const std::string& material)
{
    LILA_ZONE();

    std::vector<const VolumePiece*> pieces;
    size_t holes = 0;
    const auto results = volumeResults(modifier);
    for (const auto& [node, r] : results)
        if (r)
        {
            for (const VolumePiece& p : r->pieces)
                pieces.push_back(&p);
            holes += r->holeSamples;
        }
    const std::string visibility = settings.contains("files") ? settings["files"].value("visibility", std::string()) : std::string();
    if (pieces.empty() && holes == 0)
        return nullptr;
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    if (!client)
        throw std::runtime_error("not connected to Unreal");
    auto call = [&](const char* method, const json& params, link::CallOptions o = {}) {
        link::RpcResult r = client->call(method, params, o);
        if (!r.ok)
            throw std::runtime_error(std::string(method) + ": " + r.error);
        return r.result;
    };
    json out = {{"pieces", json::array()}, {"warnings", json::array()}};
    const std::string base = assetName(label);
    const std::string folder = "/Game/LilaLink/Landscapes/" + base + "/Volume";
    const fs::path dir = link::cacheDir() / "terrain" / (base + "_volume");
    fs::create_directories(dir);
    json meshes = json::array(), groups = json::array();
    for (const VolumePiece* p : pieces)
    {
        const std::string name = "SM_" + base + "_" + p->name;
        const glm::dvec3 centre = glm::dvec3(world * glm::dvec4(p->bounds.center(), 1.0));
        const fs::path file = dir / (name + ".fbx");
        writePiece(*p, world, centre, file, name);
        const json imported = call("importAsset", link::importParams(file, {{"contentPath", folder + "/" + name}, {"kind", "static"}}), link::importCallOptions());
        std::string asset = folder + "/" + name + "." + name;
        if (const json made = imported.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
            asset = made[0].get<std::string>();
        const std::string group = id + "/volume/" + p->name;
        const math::Vector u = math::toUnreal(math::Vector(centre.x, centre.y, centre.z));
        call("placeInstances", {{"group", group},
                                {"label", name},
                                {"mesh", asset},
                                {"mode", "actors"},
                                {"placements", json::array({{{"id", group}, {"name", name}, {"location", json::array({u.x, u.y, u.z})},
                                                             {"rotation", json::array({0, 0, 0, 1})}, {"scale", json::array({1, 1, 1})}}})}});
        meshes.push_back(asset);
        groups.push_back(group);
        out["pieces"].push_back({{"name", p->name}, {"asset", asset}, {"group", group}});
    }
    call("removePlacements", {{"prefixes", json::array({id + "/volume/"})}, {"keep", groups}});

    const json features = link::Link::get().status().value("features", json::array());
    if (!features.is_array() || std::find(features.begin(), features.end(), json("landscapeVolume")) == features.end())
    {
        const std::string w = "the LilaLink plugin in Unreal predates terrain volumes: the landscape has no holes and the pieces keep their imported "
                              "materials; rebuild LilaLink from Lila's ue/LilaLink";
        log::warn("terrainSendToUnreal: {}", w);
        out["warnings"].push_back(w);
        return out;
    }
    json params = {{"id", id}, {"label", label}, {"meshes", meshes}, {"resolution", settings.value("resolution", json::array())}};
    if (!visibility.empty())
        params["visibility"] = visibility;
    else if (holes > 0)
        out["warnings"].push_back("the export wrote no visibility layer, so the landscape has no holes");
    if (!material.empty())
        params["material"] = material;
    out["landscape"] = call("setLandscapeVolume", params);
    return out;
}

bool registerVolume(ext::Context& ctx)
{
    if (!gn::registerDecl(ctx, volumeDecl()))
        return false;
    registerVolumeCommands(ctx);
    return true;
}
} // namespace terrain
