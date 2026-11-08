// seamFind, seamInfo, seamCheck, seamUpdate and seamTag: make and read the seam node.

#include "Seamwork.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
using T = ArgType;

namespace
{
bool userBool(const Node& n, const char* attr)
{
    const int a = n.type().attrIndex(attr);
    if (a < 0)
        return false;
    const PlugValue& v = n.value(a);
    if (const bool* b = std::get_if<bool>(&v))
        return *b;
    if (const int64_t* i = std::get_if<int64_t>(&v))
        return *i != 0;
    return false;
}

int64_t userInt(const Node& n, const char* attr)
{
    const int a = n.type().attrIndex(attr);
    if (a < 0)
        return 0;
    if (const int64_t* i = std::get_if<int64_t>(&n.value(a)))
        return *i;
    return 0;
}

bool isMesh(const Node& n) { return n.type().isA("mesh"); }

/** True for meshes Seamwork made (strips, skirts, points), which never take part in joins. */
bool isSeamworkOutput(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n)
        return true;
    if (auto src = g.source({id, MeshNode::kInMesh, -1}))
        if (Node* s = g.node(src->node))
        {
            const std::string& t = s->type().name;
            if (t == kSeamType || t == kStripType || t == kSkirtType)
                return true;
        }
    return userBool(*n, "seamworkOutput");
}

/** The terrain mesh a name means, or the first mesh in the scene whose gn modifier makes a heightfield. */
Node* findTerrainMesh(Graph& g, const std::string& name, const std::string& cmd)
{
    auto makesHeightfield = [&](NodeId mesh) {
        for (NodeId mid : modifiers::stack(g, mesh))
            if (Node* m = g.node(mid); m && gn::isGnModifier(*m) && gn::modifierHeightfield(mid))
                return true;
        return false;
    };
    if (!name.empty())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(cmd + ": no node named '" + name + "'");
        if (modifiers::isModifier(*n))
            n = g.node(modifiers::meshOf(g, n->id()));
        if (!n || !isMesh(*n))
            throw CommandError(cmd + ": " + name + " is not a terrain mesh");
        return n;
    }
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && isMesh(*n) && makesHeightfield(id))
            return n;
    return nullptr;
}

/** Every visible mesh that isn't the terrain or a Seamwork output. */
std::vector<NodeId> sceneMeshes(Graph& g, NodeId terrain)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !isMesh(*n) || id == terrain || isSeamworkOutput(g, id))
            continue;
        if (!effectivelyVisible(g, id, inheritVisibility()))
            continue;
        out.push_back(id);
    }
    return out;
}

void connectObject(Graph& g, NodeId seam, int index, NodeId mesh, int64_t flags)
{
    Node* s = g.node(seam);
    Node* m = g.node(mesh);
    g.connect({mesh, MeshNode::kOutMesh, -1}, {seam, s->type().attrIndex("objects"), -1, index}, true);
    g.connect({mesh, Transform::kWorldMatrix, -1}, {seam, s->type().attrIndex("matrices"), -1, index}, true);
    g.setValue({seam, s->type().attrIndex("names"), -1, index}, m->path());
    g.setValue({seam, s->type().attrIndex("flags"), -1, index}, flags);
    g.setValue({seam, s->type().attrIndex("groups"), -1, index}, userInt(*m, "seamGroup"));
}

void setAttr(Graph& g, NodeId id, const char* attr, const PlugValue& v)
{
    Node* n = g.node(id);
    const int a = n->type().attrIndex(attr);
    if (a < 0)
        throw CommandError(std::string("seamwork: no attr ") + attr);
    g.setValue({id, a, -1}, v);
}

/** Splices a Contact Mask node fed by the seam node into the terrain's group, before its layers. Returns the node's name. */
std::string addContactMask(Graph& g, Node& terrain, const std::string& seamName, double width, const std::string& cmd)
{
    for (NodeId mid : modifiers::stack(g, terrain.id()))
    {
        Node* mod = g.node(mid);
        if (!mod || !gn::isGnModifier(*mod) || !gn::modifierHeightfield(mid))
            continue;
        Node* groupNode = gn::groupOfModifier(g, *mod);
        if (!groupNode)
            continue;
        gn::Group grp = gn::readGroup(*groupNode);
        for (const gn::GNode& n : grp.nodes)
            if (n.type == kContactMaskNode && n.values.value("source", std::string()) == seamName)
                return n.name;
        auto linkInto = [&](const std::string& node, const std::string& socket) -> gn::GLink* {
            for (gn::GLink& l : grp.links)
                if (l.toNode == node && l.toSocket == socket)
                    return &l;
            return nullptr;
        };
        auto typeOf = [&](const std::string& node) -> std::string {
            for (const gn::GNode& n : grp.nodes)
                if (n.name == node)
                    return n.type;
            return {};
        };
        // Layer rules read masks made before them, so go in ahead of the layer, biome and ground cover nodes.
        std::string to = "GroupOutput", socket = "geometry";
        for (gn::GLink* l = linkInto(to, socket); l; l = linkInto(to, socket))
        {
            const std::string t = typeOf(l->fromNode);
            if (t != "TerrainLayers" && t != "TerrainBiomes" && t != "TerrainGroundCover")
                break;
            to = l->fromNode;
            socket = "heightfield";
        }
        gn::GLink* l = linkInto(to, socket);
        if (!l)
            throw CommandError(cmd + ": the terrain's group has no heightfield running into " + to);
        gn::GNode n;
        n.name = grp.uniqueNodeName("ContactMask");
        n.type = kContactMaskNode;
        n.values = {{"source", seamName}, {"width", width}};
        const std::string fromNode = l->fromNode, fromSocket = l->fromSocket;
        for (const gn::GNode& x : grp.nodes)
            if (x.name == fromNode)
                n.position = x.position + glm::dvec2(200.0, 150.0);
        l->fromNode = n.name;
        l->fromSocket = "heightfield";
        grp.links.push_back({fromNode, fromSocket, n.name, "heightfield"});
        grp.nodes.push_back(n);
        const std::string name = n.name;
        gn::commitGroup(g, *groupNode, std::move(grp));
        return name;
    }
    throw CommandError(cmd + ": " + terrain.path() + " has no terrain group to add the contact mask to");
}

json curveJson(const SeamData& d, const Curve& c, size_t index, bool withPoints)
{
    auto range = [](const std::vector<double>& v) {
        if (v.empty())
            return json::array({0.0, 0.0});
        auto [lo, hi] = std::minmax_element(v.begin(), v.end());
        return json::array({*lo, *hi});
    };
    double exposure = 0.0;
    for (double e : c.exposure)
        exposure += e;
    json j = {{"index", index},
              {"kind", kindName(c.kind)},
              {"a", objectName(d, c.a)},
              {"b", objectName(d, c.b)},
              {"near", c.nearContact},
              {"closed", c.closed},
              {"points", c.points.size()},
              {"length", c.length()},
              {"angle", range(c.angle)},
              {"depth", range(c.depth)},
              {"gap", range(c.gap)},
              {"exposure", c.exposure.empty() ? 1.0 : exposure / double(c.exposure.size())}};
    if (withPoints)
    {
        json pts = json::array();
        for (size_t k = 0; k < c.points.size(); ++k)
            pts.push_back({{"position", v3(c.points[k])},
                           {"normalA", v3(c.normalA[k])},
                           {"normalB", v3(c.normalB[k])},
                           {"angle", c.angle[k]},
                           {"depth", c.depth[k]},
                           {"gap", c.gap[k]},
                           {"upFacing", c.upFacing[k]},
                           {"exposure", c.exposure[k]},
                           {"layer", c.layer[k] >= 0 && size_t(c.layer[k]) < d.result->layers.size() ? json(d.result->layers[size_t(c.layer[k])]) : json()}});
        j["pointData"] = pts;
    }
    return j;
}

/** Seam nodes named, or every seam node when none are. */
std::vector<NodeId> seamNodesNamed(Graph& g, const std::vector<std::string>& names, const std::string& cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(cmd + ": no node named '" + name + "'");
        const NodeId id = seamNodeOf(g, n->id());
        if (id != kNoNode && std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    }
    if (names.empty())
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().name == kSeamType)
                out.push_back(id);
    if (out.empty())
        throw CommandError(cmd + ": no seam node (make one with seamFind)");
    return out;
}

/** What covers each curve: a strip, skirt or scatter fed by the seam node, or a field modifier on its object. */
std::vector<std::string> coverOf(Graph& g, NodeId seam, const SeamData& d, const Curve& c)
{
    std::vector<std::string> by;
    for (const PlugRef& dst : g.destinations({seam, g.node(seam)->type().attrIndex("outSeams"), -1}))
        if (Node* n = g.node(dst.node))
        {
            const std::string& t = n->type().name;
            if (t == kStripType || t == kSkirtType)
            {
                const int kinds = n->type().attrIndex("kinds");
                const int64_t mask = kinds >= 0 ? n->as<int64_t>(kinds) : 7;
                if (mask & (int64_t(1) << int(c.kind)))
                    by.push_back(t == kStripType ? "strip" : "skirt");
            }
        }
    for (const PlugRef& dst : g.destinations({seam, g.node(seam)->type().attrIndex("outPoints"), -1}))
        if (Node* n = g.node(dst.node); n && userBool(*n, "seamScatter"))
            by.push_back("scatter");
    for (int obj : {c.a, c.b})
        if (obj >= 0 && size_t(obj) < d.objects.size())
            if (Node* mesh = g.find(d.objects[size_t(obj)].name))
                for (NodeId mid : modifiers::stack(g, mesh->id()))
                    if (Node* m = g.node(mid); m && m->type().name == kFieldModifier)
                        by.push_back("field");
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && userBool(*n, "seamScatter"))
            if (const int i = n->type().attrIndex("seamSource"); i >= 0)
                if (const auto* s = std::get_if<std::string>(&n->value(i)); s && g.find(*s) && g.find(*s)->id() == seam)
                    by.push_back("scatter");
    std::sort(by.begin(), by.end());
    by.erase(std::unique(by.begin(), by.end()), by.end());
    return by;
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "seamFind",
        .doc = "Find where objects meet the ground and each other, live: a seam node with contact curves, points along them, distance "
               "fields on the meshes and a contact mask on the terrain.\n\n"
               "Exact joins come from where surfaces cross; near contacts (a wall standing on the ground) from surfaces within gap of "
               "each other; creases (rock stacks, wall corners) from short-range occlusion when crease is on. Each curve point carries "
               "both objects, both normals, the wedge angle, penetration depth, gap, how much it faces up, sky exposure and the "
               "ground layer. Objects tagged ignoreSeams (seamTag) are left out; pieces sharing a seamGroup never join each other. "
               "Returns the nodes made and stats: curve count, total length (cm) and per-object pairs.",
        .positionalDoc = "meshes (default the selection, or every visible mesh when nothing is selected)",
        .flags = {nodeFlag({"terrain", "t", T::String, nullptr, "Terrain mesh; default the first terrain in the scene."}, "mesh"),
                  nodeFlag({"ground", "gr", T::List, nullptr, "Meshes that act as ground (floors, slabs), besides the terrain."}, "mesh"),
                  {"findGround", "fg", T::Bool, true, "Joins with the ground."},
                  {"findMesh", "fm", T::Bool, true, "Joins between objects."},
                  {"nearContact", "nc", T::Bool, true, "Also surfaces within gap that don't cross."},
                  {"gap", "gap", T::Float, 2.0, "Largest gap that still counts as a contact, cm.", {}, {}, 0.0},
                  {"crease", "cr", T::Bool, false, "Tight creases from short-range occlusion."},
                  {"creaseRange", "crr", T::Float, 20.0, "How far crease rays look, cm.", {}, {}, 0.1},
                  {"width", "w", T::Float, 30.0, "Band width either side of a join, cm.", {}, {}, 0.0},
                  {"falloff", "fo", T::String, "smooth", "How the band fades.", falloffNames()},
                  {"noise", "no", T::Float, 0.0, "Width varies by up to this fraction with world noise.", {}, {}, 0.0},
                  {"noiseScale", "ns", T::Float, 150.0, "Size of the width noise, cm.", {}, {}, 1.0},
                  {"spacing", "sp", T::Float, 25.0, "Points every this many cm along the joins.", {}, {}, 0.1},
                  {"field", "fi", T::String, "color", "Where each mesh's distance field goes: color, uv or none.", {"color", "uv", "none"}},
                  {"channel", "ch", T::String, "r", "Vertex colour channel for field=color.", {"r", "g", "b", "a"}},
                  {"uvSet", "uv", T::Int, int64_t(2), "UV set for field=uv.", {}, {}, 1.0, 7.0},
                  {"terrainMask", "tm", T::Bool, true, "Add a contact mask to the terrain for layer rules."},
                  {"terrainWidth", "tw", T::Float, 100.0, "How far the terrain mask reaches, cm.", {}, {}, 0.0},
                  {"live", "lv", T::Bool, true, "Keep fields and the terrain mask up to date as objects move."},
                  {"name", "n", T::String, "contacts", "Name of the seam node."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            g.evaluate();
            const std::string cmd = "seamFind";
            Node* terrain = findTerrainMesh(g, a.get<std::string>("terrain", ""), cmd);
            const NodeId terrainId = terrain ? terrain->id() : kNoNode;
            std::vector<NodeId> meshes;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError(cmd + ": no node named '" + name + "'");
                if (n->id() == terrainId || !isMesh(*n) || isSeamworkOutput(g, n->id()))
                    continue;
                meshes.push_back(n->id());
            }
            if (meshes.empty())
                meshes = sceneMeshes(g, terrainId);
            std::set<NodeId> groundSet;
            if (a.has("ground"))
                for (const json& j : a.flags["ground"])
                {
                    Node* n = g.find(j.get<std::string>());
                    if (!n || !isMesh(*n))
                        throw CommandError(cmd + ": ground " + j.get<std::string>() + " is not a mesh");
                    groundSet.insert(n->id());
                    if (std::find(meshes.begin(), meshes.end(), n->id()) == meshes.end())
                        meshes.push_back(n->id());
                }
            if (meshes.empty())
                throw CommandError(cmd + ": no meshes to join");

            Node* seam = g.createNode(*types::find(kSeamType), a.get<std::string>("name", "contacts"));
            const NodeId sid = seam->id();
            setAttr(g, sid, "up", sceneUp());
            setAttr(g, sid, "findGround", a.get<bool>("findGround", true));
            setAttr(g, sid, "findMesh", a.get<bool>("findMesh", true));
            setAttr(g, sid, "nearContact", a.get<bool>("nearContact", true));
            setAttr(g, sid, "gap", a.get<double>("gap", 2.0));
            setAttr(g, sid, "crease", a.get<bool>("crease", false));
            setAttr(g, sid, "creaseRange", a.get<double>("creaseRange", 20.0));
            setAttr(g, sid, "width", a.get<double>("width", 30.0));
            const std::string fall = a.get<std::string>("falloff", "smooth");
            const auto fi = std::find(falloffNames().begin(), falloffNames().end(), fall);
            setAttr(g, sid, "falloff", int64_t(fi == falloffNames().end() ? 1 : fi - falloffNames().begin()));
            setAttr(g, sid, "noise", a.get<double>("noise", 0.0));
            setAttr(g, sid, "noiseScale", a.get<double>("noiseScale", 150.0));
            setAttr(g, sid, "spacing", a.get<double>("spacing", 25.0));
            setAttr(g, sid, "live", a.get<bool>("live", true));
            setAttr(g, sid, "terrainMask", a.get<bool>("terrainMask", true));

            json ignored = json::array();
            int index = 0;
            for (NodeId id : meshes)
            {
                Node* n = g.node(id);
                if (userBool(*n, "ignoreSeams"))
                {
                    ignored.push_back(n->path());
                    continue;
                }
                const int64_t flags = (groundSet.contains(id) || userBool(*n, "seamGround")) ? kFlagGround : 0;
                connectObject(g, sid, index++, id, flags);
            }
            if (terrain)
            {
                g.connect({terrainId, MeshNode::kOutMesh, -1}, {sid, seam->type().attrIndex("terrain"), -1}, true);
                g.connect({terrainId, Transform::kWorldMatrix, -1}, {sid, seam->type().attrIndex("terrainMatrix"), -1}, true);
                setAttr(g, sid, "terrainName", terrain->path());
            }

            // Curves and points as ordinary scene objects under one group.
            const std::string base = g.node(sid)->name();
            Node* group = g.createNode(*types::find("transform"), base + "Display");
            Node* curves = g.createNode(*types::find("curve"), base + "Curves", group->id());
            g.connect({sid, seam->type().attrIndex("outCurves"), -1}, {curves->id(), curve::CurveShape::kCreate, -1}, true);
            Node* points = g.createNode(*types::find("mesh"), base + "Points", group->id());
            g.connect({sid, seam->type().attrIndex("outPoints"), -1}, {points->id(), MeshNode::kInMesh, -1}, true);

            json fields = json::array();
            json skipped = json::array();
            const std::string field = a.get<std::string>("field", "color");
            if (field != "none")
                for (NodeId id : meshes)
                {
                    Node* n = g.node(id);
                    if (groundSet.contains(id) || userBool(*n, "ignoreSeams") || userBool(*n, "seamGround"))
                        continue;
                    if (instancing::members(g, id).size() > 1)
                    {
                        skipped.push_back({{"node", n->path()}, {"reason", "instances share one stack; make it unique for its own field"}});
                        continue;
                    }
                    bool has = false;
                    for (NodeId mid : modifiers::stack(g, id))
                        has |= g.node(mid)->type().name == kFieldModifier;
                    if (has)
                        continue;
                    const std::string channel = a.get<std::string>("channel", "r");
                    const int64_t ch = std::string("rgba").find(channel.empty() ? 'r' : channel[0]);
                    const std::string mod = ops::execute(
                        "addModifier", {{"args", {n->path()}},
                                        {"kind", "seamField"},
                                        {"params", {{"target", field == "uv" ? 1 : 0}, {"channel", ch < 0 ? 0 : ch}, {"uvSet", a.get<int64_t>("uvSet", 2)}}}});
                    fields.push_back(mod);
                }

            json mask = nullptr;
            if (terrain && a.get<bool>("terrainMask", true) && a.get<bool>("findGround", true))
                mask = addContactMask(g, *terrain, g.node(sid)->name(), a.get<double>("terrainWidth", 100.0), cmd);

            g.evaluate();
            const json applied = applySeam(g, sid, true);
            const SeamData* d = seamDataOf(*g.node(sid));
            json out = {{"seam", g.node(sid)->path()},
                        {"curvesNode", curves->path()},
                        {"pointsNode", points->path()},
                        {"group", group->path()},
                        {"fieldModifiers", fields},
                        {"fieldsSkipped", skipped},
                        {"terrainMaskNode", mask},
                        {"ignored", ignored},
                        {"applied", applied}};
            if (d)
                out.update(d->stats);
            const std::string warning = g.node(sid)->as<std::string>(g.node(sid)->type().attrIndex("warning"));
            if (!warning.empty())
                out["warning"] = warning;
            return out;
        },
        .examples = {"r = seamFind(['Wall', 'Rock_01'], gap=2)\nprint(r['curves'], r['length'], r['pairs'])",
                     "seamFind(terrain='Terrain', crease=True, field='uv', uvSet=2)"},
    });

    ctx.registerCommand({
        .name = "seamInfo",
        .doc = "What a seam node found: stats, and each curve's kind, objects, length and value ranges (angle, depth, gap, mean "
               "exposure). points=True adds every curve point's values.",
        .positionalDoc = "seam node (or its curves or points object)",
        .flags = {{"points", "p", T::Bool, false, "Include each point's values."},
                  {"curve", "c", T::Int, nullptr, "Only this curve index."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const auto names = a.objects();
            if (names.size() != 1)
                throw CommandError("seamInfo: give one seam node");
            NodeId id = kNoNode;
            const auto d = seamDataNamed(g, names[0], "seamInfo", &id);
            json curves = json::array();
            const bool pts = a.get<bool>("points", false);
            for (size_t i = 0; i < d->result->curves.size(); ++i)
                if (!a.has("curve") || int64_t(i) == a.get<int64_t>("curve", -1))
                    curves.push_back(curveJson(*d, d->result->curves[i], i, pts));
            json out = d->stats;
            out["seam"] = g.node(id)->path();
            out["curveList"] = curves;
            out["revision"] = std::to_string(d->revision);
            out["band"] = {{"width", d->band.width}, {"falloff", falloffNames()[size_t(d->band.falloff)]}, {"noise", d->band.noise}};
            return out;
        },
        .examples = {"info = seamInfo('contacts')\nfor c in info['curveList']: print(c['kind'], c['a'], c['b'], c['length'])"},
    });

    ctx.registerCommand({
        .name = "seamCheck",
        .doc = "Check joins for problems: floating (an object with nothing under it within reach and no contact, its gap in cm) and "
               "open (a join no strip, skirt, scatter or field covers). Returns issues and ok.",
        .positionalDoc = "seam nodes (default every one)",
        .flags = {{"tolerance", "tol", T::Float, 2.0, "Gaps over this many cm count as floating.", {}, {}, 0.0},
                  {"reach", "r", T::Float, 500.0, "Only look this far below an object for what it floats over, cm.", {}, {}, 0.0},
                  {"open", "op", T::Bool, true, "Report joins nothing covers."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const double tol = a.get<double>("tolerance", 2.0), reach = a.get<double>("reach", 500.0);
            json issues = json::array();
            for (NodeId sid : seamNodesNamed(g, a.positional.get<std::vector<std::string>>(), "seamCheck"))
            {
                const auto d = seamDataNamed(g, g.node(sid)->path(), "seamCheck");
                const std::string seamPath = g.node(sid)->path();
                std::vector<uint8_t> touches(d->objects.size(), 0);
                for (const Curve& c : d->result->curves)
                {
                    if (c.a >= 0)
                        touches[size_t(c.a)] = 1;
                    if (c.b >= 0)
                        touches[size_t(c.b)] = 1;
                }
                const glm::dvec3 up = glm::length(d->up) > 0 ? glm::normalize(d->up) : glm::dvec3(0, 1, 0);
                for (size_t i = 0; i < d->objects.size(); ++i)
                {
                    const Object& o = d->objects[i];
                    if (o.ground || o.ignore || touches[i])
                        continue;
                    // The lowest point along up, and what lies below it.
                    double best = 1e300;
                    glm::dvec3 low(0.0);
                    for (geo::Index p = 0; p < o.mesh->pointCapacity(); ++p)
                        if (o.mesh->pointValid(p))
                        {
                            const glm::dvec3 w(o.world * glm::dvec4(o.mesh->position(p), 1.0));
                            if (glm::dot(w, up) < best)
                            {
                                best = glm::dot(w, up);
                                low = w;
                            }
                        }
                    double gap = 1e300;
                    for (size_t j = 0; j < d->objects.size(); ++j)
                    {
                        if (j == i || d->objects[j].ignore)
                            continue;
                        const Object& b = d->objects[j];
                        const glm::dmat4 inv = glm::inverse(b.world);
                        geo::BvhHit hit;
                        if (b.bvh->raycast(glm::dvec3(inv * glm::dvec4(low, 1.0)), glm::dmat3(inv) * -up, hit, reach))
                            gap = std::min(gap, hit.t);
                    }
                    if (d->ground)
                    {
                        const glm::dmat4 inv = glm::inverse(d->ground->world);
                        const glm::dvec3 l(inv * glm::dvec4(low, 1.0));
                        if (d->ground->field->contains(l.x, l.z))
                        {
                            const double below = l.y - d->ground->field->heightAt(l.x, l.z);
                            if (below >= 0.0 && below <= reach)
                                gap = std::min(gap, below);
                        }
                    }
                    if (gap < 1e299 && gap > tol)
                        issues.push_back({{"kind", "floating"}, {"seam", seamPath}, {"node", o.name}, {"gap", gap}, {"at", v3(low)}});
                }
                if (a.get<bool>("open", true))
                    for (size_t ci = 0; ci < d->result->curves.size(); ++ci)
                    {
                        const Curve& c = d->result->curves[ci];
                        if (coverOf(g, sid, *d, c).empty())
                            issues.push_back({{"kind", "open"}, {"seam", seamPath}, {"curve", ci}, {"a", objectName(*d, c.a)}, {"b", objectName(*d, c.b)},
                                              {"length", c.length()}, {"joinKind", kindName(c.kind)}});
                    }
            }
            return {{"issues", issues}, {"ok", issues.empty()}};
        },
        .examples = {"r = seamCheck()\nfor i in r['issues']: print(i['kind'], i.get('node', i.get('a')))"},
    });

    ctx.registerCommand({
        .name = "seamUpdate",
        .doc = "Write every seam node's fields into its meshes and its contact mask into the terrain now. The app does this by itself "
               "as things move; scripts and batch runs call it after moving objects.",
        .positionalDoc = "seam nodes (default every one)",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId id : seamNodesNamed(g, a.positional.get<std::vector<std::string>>(), "seamUpdate"))
            {
                json r = applySeam(g, id, true);
                r["seam"] = g.node(id)->path();
                out.push_back(r);
            }
            return out;
        },
        .examples = {"move(0, 0, 100, 'Rock_01', relative=True)\nseamUpdate()"},
    });

    ctx.registerCommand({
        .name = "seamTag",
        .doc = "Tag meshes for joins: ignore (never joined, for intended overlaps), ground (acts as ground, like a floor slab) or "
               "group (pieces sharing a non-zero group never join each other). Takes effect the next time seamFind runs.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"ignore", "ig", T::Bool, nullptr, "Never join these."},
                  {"ground", "gr", T::Bool, nullptr, "These act as ground."},
                  {"group", "g", T::Int, nullptr, "Join group; 0 is the mesh's own."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("seamTag: no node named '" + name + "'");
                auto put = [&](const char* attr, PlugType type, const PlugValue& v) {
                    int i = n->type().attrIndex(attr);
                    if (i < 0)
                        i = g.addAttr(n->id(), {.name = attr, .type = type, .defaultValue = type == PlugType::Bool ? PlugValue(false) : PlugValue(int64_t(0)),
                                                .flags = AttrInput | AttrStorable | AttrUser | AttrChannelBox, .doc = "Seamwork tag."});
                    g.setValue({n->id(), i, -1}, v);
                };
                if (a.has("ignore"))
                    put("ignoreSeams", PlugType::Bool, a.get<bool>("ignore", false));
                if (a.has("ground"))
                    put("seamGround", PlugType::Bool, a.get<bool>("ground", false));
                if (a.has("group"))
                    put("seamGroup", PlugType::Int, a.get<int64_t>("group", 0));
                out.push_back({{"node", n->path()}, {"ignore", userBool(*n, "ignoreSeams")}, {"ground", userBool(*n, "seamGround")}, {"group", userInt(*n, "seamGroup")}});
            }
            return out;
        },
        .examples = {"seamTag(['Rock_Cluster_01', 'Rock_Cluster_02'], group=3)", "seamTag('Floor', ground=True)"},
    });
}
} // namespace seamwork
