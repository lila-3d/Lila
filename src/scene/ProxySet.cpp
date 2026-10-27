#include "scene/ProxySet.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/DominantInfluence.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/Rig.h"
#include "scene/SelectAs.h"
#include "scene/Skin.h"
#include "scene/SplitMesh.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <climits>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace lila::scene::proxy
{
namespace
{
using T = ArgType;
using geo::Index;

constexpr uint32_t kStored = AttrInput | AttrStorable;
const std::vector<std::string> kDisplayNames = {"proxy", "real"};
const std::vector<std::string> kKindNames = {"proxy", "true"};
const std::vector<std::string> kPieceDisplayNames = {"follow", "proxy", "real"};

/** Re-throws scene errors as user-facing command errors. */
template <class F> auto guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}

/** A node by id while it lives, by name when read from a file. */
struct Ref
{
    mutable NodeId id = kNoNode;
    mutable std::string name;

    NodeId resolve(const Graph& g) const
    {
        if (id != kNoNode)
            return g.node(id) ? id : kNoNode;
        if (name.empty())
            return kNoNode;
        try
        {
            if (const Node* n = g.find(name))
                id = n->id();
        }
        catch (const std::exception&)
        {
        }
        return id;
    }
    json toJson() const
    {
        const Node* n = id != kNoNode ? Graph::current().node(id) : nullptr;
        return n ? json(n->path()) : id == kNoNode ? json(name) : json(nullptr);
    }
    static Ref of(const Graph& g, NodeId node)
    {
        Ref r;
        if (const Node* n = g.node(node))
        {
            r.id = node;
            r.name = n->path();
        }
        return r;
    }
    static Ref fromJson(const json& j)
    {
        Ref r;
        if (j.is_string())
            r.name = j.get<std::string>();
        return r;
    }
};

struct Members final : Data
{
    struct Entry
    {
        Ref mesh;
        Ref joint;
        Kind kind = Kind::Proxy;
        PieceDisplay display = PieceDisplay::Follow;
    };
    std::vector<Ref> real;
    std::vector<Entry> pieces;

    const char* kind() const override { return kMembersKind; }
    json toJson() const override
    {
        json r = json::array();
        for (const Ref& x : real)
            r.push_back(x.toJson());
        json p = json::array();
        for (const Entry& e : pieces)
            p.push_back({{"mesh", e.mesh.toJson()}, {"joint", e.joint.toJson()}, {"kind", kKindNames[size_t(e.kind)]},
                         {"display", kPieceDisplayNames[size_t(e.display)]}});
        return {{"kind", kMembersKind}, {"real", std::move(r)}, {"pieces", std::move(p)}};
    }
    size_t bytes() const override { return 64 + (real.size() + pieces.size() * 2) * 48; }

    static DataPtr fromJson(const json& j)
    {
        auto out = std::make_shared<Members>();
        if (j.contains("real") && j["real"].is_array())
            for (const json& x : j["real"])
                out->real.push_back(Ref::fromJson(x));
        if (j.contains("pieces") && j["pieces"].is_array())
            for (const json& x : j["pieces"])
            {
                Entry e;
                e.mesh = Ref::fromJson(x.value("mesh", json()));
                e.joint = Ref::fromJson(x.value("joint", json()));
                e.kind = x.value("kind", std::string()) == "true" ? Kind::True : Kind::Proxy;
                const std::string d = x.value("display", std::string());
                e.display = d == "proxy" ? PieceDisplay::Proxy : d == "real" ? PieceDisplay::Real : PieceDisplay::Follow;
                out->pieces.push_back(std::move(e));
            }
        return out;
    }
};

const Members* membersOf(const Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    if (!n || !isSet(*n))
        return nullptr;
    return dynamic_cast<const Members*>(n->as<DataPtr>(ProxySet::kMembers).get());
}

/** A copy of the set's members to edit and store back with store(). */
std::shared_ptr<Members> editable(const Graph& g, NodeId set)
{
    const Members* m = membersOf(g, set);
    return m ? std::make_shared<Members>(*m) : std::make_shared<Members>();
}

void store(Graph& g, NodeId set, std::shared_ptr<Members> m) { g.setValue({set, ProxySet::kMembers, -1}, DataPtr(std::move(m))); }

Node& setArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!isSet(*n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a proxy set");
    return *n;
}

Node& meshArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

NodeId topJoint(const Graph& g, NodeId joint)
{
    NodeId top = joint;
    while (g.parent(top) != kNoNode && rig::isJoint(*g.node(g.parent(top))))
        top = g.parent(top);
    return top;
}

std::vector<NodeId> skinnedMeshesOf(const Graph& g, NodeId set)
{
    std::vector<NodeId> out;
    for (NodeId r : realOf(g, set))
    {
        std::vector<NodeId> under = g.descendants(r);
        under.insert(under.begin(), r);
        for (NodeId id : under)
            if (const Node* n = g.node(id); n && n->type().isA("mesh") && skin::find(const_cast<Graph&>(g), id) != kNoNode)
                out.push_back(id);
    }
    return out;
}

glm::dvec3 worldPos(const Graph& g, NodeId id) { return glm::dvec3(g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix)[3]); }

/** Distance to the bone from a to b, or infinity past either end: points beyond the child belong to it. */
double boneDistance(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 ab = b - a;
    const double len2 = glm::dot(ab, ab);
    const double t = len2 > 0.0 ? glm::dot(p - a, ab) / len2 : -1.0;
    return t >= 0.0 && t <= 1.0 ? glm::length(p - (a + ab * t)) : 1e300;
}

/** Whether something already drives the mesh's translate, so binding it again would fight that. */
bool alreadyBound(const Graph& g, NodeId mesh)
{
    return g.source({mesh, Transform::kTranslate, -1}).has_value() || skin::find(const_cast<Graph&>(g), mesh) != kNoNode;
}

/** Skins piece with real's influences, bind pose and weights; the piece must sit in real's bind pose, in world space. */
void skinLike(Graph& g, NodeId piece, NodeId real)
{
    LILA_ZONE();

    const NodeId rs = skin::find(g, real);
    if (rs == kNoNode)
        throw std::invalid_argument("proxy: " + g.path(real) + " is not skinned, so a true piece can't copy its weights");
    std::map<NodeId, glm::dmat4> bindPre;
    json args = json::array();
    for (const skin::Influence& inf : skin::influences(g, rs))
        if (inf.joint != kNoNode)
        {
            args.push_back(g.path(inf.joint));
            bindPre[inf.joint] = g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
        }
    args.push_back(g.path(piece));
    ops::execute("skinCluster", {{"args", args},
                                 {"bindTo", "selected"},
                                 {"maxInfluences", g.node(rs)->as<int64_t>(SkinClusterNode::kMaxInfluences)},
                                 {"skinningMethod", g.node(rs)->as<int64_t>(SkinClusterNode::kSkinningMethod) == SkinClusterNode::kDualQuaternion
                                                        ? "dualQuaternion"
                                                        : "linear"}});
    const NodeId ps = skin::find(g, piece);
    for (const skin::Influence& inf : skin::influences(g, ps))
        if (auto it = bindPre.find(inf.joint); it != bindPre.end())
            g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, it->second);
    ops::execute("copySkinWeights", {{"args", {g.path(real), g.path(piece)}}});
}

/** Shown pieces of one set showing proxies: per joint, the kind its override (or proxy) asks for when it has one. */
void hidePieces(const Graph& g, const Members& m, std::unordered_set<NodeId>& out)
{
    struct Region
    {
        PieceDisplay want = PieceDisplay::Follow;
        bool hasProxy = false, hasTrue = false;
    };
    std::map<NodeId, Region> regions;
    std::vector<std::pair<NodeId, const Members::Entry*>> live;
    for (const Members::Entry& e : m.pieces)
    {
        const NodeId mesh = e.mesh.resolve(g);
        if (mesh == kNoNode)
            continue;
        NodeId key = e.joint.resolve(g);
        if (key == kNoNode)
            key = mesh;
        Region& r = regions[key];
        if (r.want == PieceDisplay::Follow)
            r.want = e.display;
        (e.kind == Kind::True ? r.hasTrue : r.hasProxy) = true;
        live.push_back({key, &e});
    }
    for (const auto& [key, e] : live)
    {
        const Region& r = regions[key];
        const Kind want = r.want == PieceDisplay::Real ? Kind::True : Kind::Proxy;
        const bool available = want == Kind::True ? r.hasTrue : r.hasProxy;
        if (available && e->kind != want)
            out.insert(e->mesh.resolve(g));
    }
}

void hideTree(const Graph& g, NodeId id, std::unordered_set<NodeId>& out, const std::unordered_set<NodeId>& keep)
{
    if (keep.contains(id))
        return;
    out.insert(id);
    for (NodeId c : g.children(id))
        hideTree(g, c, out, keep);
}

int lodLevelOf(const Graph& g, NodeId set) { return int(g.node(set)->as<int64_t>(ProxySet::kLodLevel)); }

/** The level the view shows a character at, sized by its real nodes as an LOD group would be. */
int viewLevel(Graph& g, const std::vector<NodeId>& real, const lod::View& view, int levels)
{
    switch (view.mode)
    {
    case lod::Mode::Level: return view.level;
    case lod::Mode::All: return 0;
    default: break;
    }
    geo::Bounds b;
    for (NodeId r : real)
    {
        std::vector<NodeId> under = g.descendants(r);
        under.insert(under.begin(), r);
        for (NodeId id : under)
            if (geo::Bounds wb = worldBounds(g, id); wb.valid())
            {
                b.extend(wb.min);
                b.extend(wb.max);
            }
    }
    if (!b.valid())
        return 0;
    std::vector<double> sizes(size_t(std::max(levels, 1)));
    for (size_t i = 0; i < sizes.size(); ++i)
        sizes[i] = lod::autoScreenSize(int(i), i ? sizes[i - 1] : 2.0);
    return lod::levelForScreenSize(sizes, lod::boundsScreenSize(b.center(), glm::length(b.size()) * 0.5, view));
}

Kind kindArg(const std::string& s, const char* cmd)
{
    if (s == "proxy")
        return Kind::Proxy;
    if (s == "true")
        return Kind::True;
    throw CommandError(std::string(cmd) + ": kind must be proxy or true");
}

std::vector<NodeId> setsFromArgs(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> out;
    auto add = [&](NodeId s) {
        if (std::ranges::find(out, s) == out.end())
            out.push_back(s);
    };
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        for (NodeId s : setsFor(g, n->id()))
            add(s);
    }
    if (names.empty())
        for (NodeId s : all(g))
            add(s);
    return out;
}

NodeId jointNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* j = g.find(name);
    if (!j || !rig::isJoint(*j))
        throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
    return j->id();
}

/** The joint whose region holds most of the faces, by the dominant-influence map, selectAs followed. */
NodeId votedJoint(const rig::DominantInfluenceMap& map, const std::unordered_map<int64_t, NodeId>& jointOf,
                  const std::vector<uint32_t>& faces)
{
    std::map<NodeId, int> votes;
    for (uint32_t f : faces)
        if (f < map.faceInfluence.size())
            if (auto it = jointOf.find(map.faceInfluence[f]); it != jointOf.end())
                ++votes[it->second];
    NodeId best = kNoNode;
    int most = 0;
    for (auto [j, v] : votes)
        if (v > most)
        {
            most = v;
            best = j;
        }
    return best;
}

std::string shortName(const Graph& g, NodeId id)
{
    std::string n = g.node(id)->name();
    if (const size_t colon = n.rfind(':'); colon != std::string::npos)
        n = n.substr(colon + 1);
    return n;
}

geo::Mesh transformed(geo::Mesh m, const glm::dmat4& xf)
{
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(xf * glm::dvec4(m.position(p), 1.0)));
    return m;
}

// Point layer carrying the bind-pose world position through the split, for true pieces.
constexpr const char* kBindAttr = "lilaProxyBind";

/**
 * Deletes the set with its pieces and what binds them: the constraints, blends, skins and their links that feed only
 * the pieces. Joints and anything else the rest of the scene reads stay.
 */
void deleteSet(Graph& g, NodeId set)
{
    LILA_ZONE();

    std::set<NodeId> doomed{set};
    for (NodeId d : g.descendants(set))
        doomed.insert(d);
    std::multimap<NodeId, NodeId> upstream, downstream;
    for (const auto& [dst, src] : g.connections())
        if (src.node != dst.node)
        {
            upstream.insert({dst.node, src.node});
            downstream.insert({src.node, dst.node});
        }
    std::vector<NodeId> work(doomed.begin(), doomed.end());
    while (!work.empty())
    {
        const NodeId id = work.back();
        work.pop_back();
        for (auto [it, end] = upstream.equal_range(id); it != end; ++it)
        {
            const NodeId src = it->second;
            const Node* n = g.node(src);
            if (doomed.contains(src) || !n || n->type().isA("transform"))
                continue;
            bool onlyDoomed = true;
            for (auto [d, dend] = downstream.equal_range(src); d != dend && onlyDoomed; ++d)
                onlyDoomed = doomed.contains(d->second);
            if (onlyDoomed)
            {
                doomed.insert(src);
                work.push_back(src);
            }
        }
    }
    for (NodeId id : doomed)
        if (id != set && g.node(id) && !g.node(id)->type().isA("transform"))
            g.deleteNode(id);
    g.deleteNode(set);
}

json animProxyFromMesh(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError("animProxyFromMesh: give a skinned mesh");
    Node& meshNode = meshArg(g, objs.front(), "animProxyFromMesh");
    const NodeId mesh = meshNode.id();
    const NodeId skinId = skin::find(g, mesh);
    if (skinId == kNoNode)
        throw CommandError("animProxyFromMesh: " + meshNode.path() + " is not skinned; bind it first, or split it with splitMesh and "
                                                                     "add the pieces with addToProxySet");
    std::vector<split::Plane> planes = split::planesFromJson(g, a.flags.value("planes", json()));
    if (planes.empty() || a.get<bool>("suggest", false))
        for (const split::Plane& p : split::suggestPlanes(g, mesh))
            planes.push_back(p);
    if (planes.empty())
        throw CommandError("animProxyFromMesh: no planes given and none found at the joint boundaries");
    split::Remesh base;
    std::vector<NodeId> trueJoints;
    try
    {
        base = split::remeshFromJson({{"remesh", a.get<std::string>("remesh", "voxelRemesh")},
                                      {"targetFaces", a.get<int>("targetFaces", 300)},
                                      {"resolution", a.get<int>("resolution", 32)},
                                      {"ratio", a.get<double>("ratio", 0.25)}});
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string("animProxyFromMesh: ") + e.what());
    }
    if (const json tj = a.flags.value("trueJoints", json()); tj.is_array())
        for (const json& j : tj)
            trueJoints.push_back(rig::selectAsTarget(g, jointNamed(g, j.get<std::string>(), "animProxyFromMesh")));

    g.evaluate();
    const MeshPtr& out = meshNode.as<MeshPtr>(MeshNode::kOutMesh);
    const MeshPtr input = skin::inputMesh(g, skinId);
    if (!out || !out->numFaces() || !input)
        throw CommandError("animProxyFromMesh: " + meshNode.path() + " has no faces");
    geo::Mesh world = transformed(*out, meshNode.as<glm::dmat4>(Transform::kWorldMatrix));
    const bool bindKnown = input->pointCapacity() == world.pointCapacity();
    if (bindKnown)
    {
        const glm::dmat4 geom = g.node(skinId)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
        std::span<glm::vec3> bind = world.pointAttrs().getOrAdd<glm::vec3>(kBindAttr);
        for (Index p = 0; p < world.pointCapacity(); ++p)
            if (input->pointValid(p))
                bind[p] = glm::vec3(geom * glm::dvec4(input->position(p), 1.0));
    }
    else
        log::warn("animProxyFromMesh: deformers after the skin change {}'s points, so true pieces take its current shape", meshNode.path());

    split::Result res = split::splitByPlanes(world, planes, {.cap = a.get<bool>("cap", true)});
    for (const std::string& w : res.warnings)
        log::warn("animProxyFromMesh: {}", w);

    std::shared_ptr<const rig::DominantInfluenceMap> map = rig::dominantInfluences(g, skinId);
    std::unordered_map<int64_t, NodeId> jointOf;
    for (const skin::Influence& inf : skin::influences(g, skinId))
        if (inf.joint != kNoNode)
            jointOf[int64_t(inf.index)] = rig::selectAsTarget(g, inf.joint);
    const json pieceSettings = a.flags.value("pieceSettings", json::object());

    struct Made
    {
        geo::Mesh mesh;
        NodeId joint = kNoNode;
        Kind kind = Kind::Proxy;
        split::Remesh remesh;
    };
    std::vector<Made> made;
    std::vector<split::Remesh> settings(res.pieces.size(), base);
    for (size_t i = 0; i < settings.size(); ++i)
        if (pieceSettings.is_object() && pieceSettings.contains(std::to_string(i)))
            try
            {
                settings[i] = split::remeshFromJson(pieceSettings[std::to_string(i)], base);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("animProxyFromMesh: ") + e.what());
            }
    split::applyPieceOverrides(settings, a.get<std::string>("pieceFaces", ""), a.get<std::string>("exactPieces", ""));
    for (size_t i = 0; i < res.pieces.size(); ++i)
    {
        const split::Remesh& r = settings[i];
        const NodeId joint = map ? votedJoint(*map, jointOf, res.sourceFaces[i]) : kNoNode;
        const bool wantsTrue = r.method == split::Remesh::Method::None || std::ranges::find(trueJoints, joint) != trueJoints.end();
        if (r.method != split::Remesh::Method::None)
            made.push_back({res.pieces[i], joint, Kind::Proxy, r});
        if (wantsTrue)
        {
            geo::Mesh t = res.pieces[i];
            if (std::span<const glm::vec3> bind = t.pointAttrs().get<glm::vec3>(kBindAttr); !bind.empty())
                for (Index p = 0; p < t.pointCapacity(); ++p)
                    if (t.pointValid(p))
                        t.setPosition(p, glm::dvec3(bind[p]));
            made.push_back({std::move(t), joint, Kind::True, {}});
        }
    }
    for (Made& m : made)
        m.mesh.pointAttrs().remove(kBindAttr);
    tbb::parallel_for(size_t(0), made.size(), [&](size_t i) {
        if (made[i].kind == Kind::Proxy)
            made[i].mesh = split::remeshPiece(made[i].mesh, made[i].remesh);
    });

    UndoChunk chunk("Anim Proxy");
    NodeId set = kNoNode;
    if (a.has("set"))
        set = setArg(g, a.get<std::string>("set"), "animProxyFromMesh").id();
    else
    {
        const int lodLevel = a.get<int>("lodLevel", -1);
        if (a.get<bool>("replace", true))
            for (NodeId old : setsOfMesh(g, mesh))
                if (lodLevelOf(g, old) == lodLevel)
                    deleteSet(g, old);
        const std::string name = a.has("name") ? a.get<std::string>("name") : shortName(g, mesh) + "_animProxy";
        set = create(g, name, {mesh}, lodLevel);
    }
    const json givenPlanes = a.flags.value("planes", json());
    g.setValue({set, ProxySet::kPlanes, -1}, (givenPlanes.is_array() && !a.get<bool>("suggest", false) ? givenPlanes : split::planesToJson(planes)).dump());
    addReal(g, set, mesh);
    const std::string baseName = shortName(g, mesh);
    for (const Made& m : made)
    {
        const std::string jointName = m.joint != kNoNode ? shortName(g, m.joint) : "piece";
        Node* piece = g.createNode(*types::find("mesh"), std::format("{}_{}_{}", baseName, m.kind == Kind::True ? "true" : "proxy", jointName), set, true);
        g.setValue({piece->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(m.mesh)));
        g.evaluate();
        if (m.kind == Kind::True)
        {
            guarded([&] {
                addPiece(g, set, piece->id(), m.joint, Kind::True, false, false);
                skinLike(g, piece->id(), mesh);
                return 0;
            });
        }
        else
            guarded([&] {
                addPiece(g, set, piece->id(), m.joint, Kind::Proxy, true, false);
                return 0;
            });
    }
    g.select({set});
    return g.path(set);
}

} // namespace

const char* displayName(Display d) { return kDisplayNames[size_t(d)].c_str(); }
const char* kindName(Kind k) { return kKindNames[size_t(k)].c_str(); }
const char* pieceDisplayName(PieceDisplay d) { return kPieceDisplayNames[size_t(d)].c_str(); }

bool isSet(const Node& n) { return n.type().isA(kTypeName); }

std::vector<NodeId> all(const Graph& g)
{
    std::vector<NodeId> ids = g.nodes();
    std::ranges::sort(ids);
    std::vector<NodeId> out;
    for (NodeId id : ids)
        if (isSet(*g.node(id)))
            out.push_back(id);
    return out;
}

std::vector<NodeId> setsOfMesh(const Graph& g, NodeId mesh)
{
    std::vector<NodeId> out;
    for (NodeId s : all(g))
        if (const std::vector<NodeId> real = realOf(g, s); std::ranges::find(real, mesh) != real.end())
            out.push_back(s);
    return out;
}

json planesOf(const Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    if (!n || !isSet(*n))
        return json::array();
    json j = json::parse(n->as<std::string>(ProxySet::kPlanes), nullptr, false);
    return j.is_array() ? j : json::array();
}

std::vector<NodeId> realOf(const Graph& g, NodeId set)
{
    std::vector<NodeId> out;
    if (const Members* m = membersOf(g, set))
        for (const Ref& r : m->real)
            if (NodeId id = r.resolve(g); id != kNoNode)
                out.push_back(id);
    return out;
}

std::vector<Piece> pieces(const Graph& g, NodeId set)
{
    std::vector<Piece> out;
    if (const Members* m = membersOf(g, set))
        for (const Members::Entry& e : m->pieces)
            if (NodeId mesh = e.mesh.resolve(g); mesh != kNoNode)
                out.push_back({mesh, e.joint.resolve(g), e.kind, e.display});
    return out;
}

Display display(const Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    return n && isSet(*n) && n->as<int64_t>(ProxySet::kDisplay) == 1 ? Display::Real : Display::Proxy;
}

NodeId setOfPiece(const Graph& g, NodeId mesh)
{
    for (NodeId s : all(g))
        if (const Members* m = membersOf(g, s))
            for (const Members::Entry& e : m->pieces)
                if (e.mesh.resolve(g) == mesh)
                    return s;
    return kNoNode;
}

std::vector<NodeId> setsFor(const Graph& g, NodeId node)
{
    std::vector<NodeId> out;
    const Node* n = g.node(node);
    if (!n)
        return out;
    const NodeId skeleton = rig::isJoint(*n) ? topJoint(g, node) : kNoNode;
    for (NodeId s : all(g))
    {
        bool hit = s == node;
        for (NodeId r : realOf(g, s))
            for (NodeId p = node; !hit && p != kNoNode; p = g.parent(p))
                hit = p == r;
        for (const Piece& p : pieces(g, s))
            hit = hit || p.mesh == node || (skeleton != kNoNode && p.joint != kNoNode && topJoint(g, p.joint) == skeleton);
        if (hit)
            out.push_back(s);
    }
    return out;
}

bool excludedFromExport(const Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    if (!n)
        return false;
    if (isSet(*n))
        return true;
    return n->type().isA("mesh") && setOfPiece(g, id) != kNoNode;
}

NodeId clickJoint(const Graph& g, NodeId mesh)
{
    for (NodeId s : all(g))
        if (const Members* m = membersOf(g, s))
            for (const Members::Entry& e : m->pieces)
                if (e.mesh.resolve(g) == mesh)
                    return e.kind == Kind::True && skin::find(const_cast<Graph&>(g), mesh) != kNoNode ? kNoNode : e.joint.resolve(g);
    return kNoNode;
}

NodeId shownSet(Graph& g, const std::vector<NodeId>& sets, const lod::View& view)
{
    if (sets.empty())
        return kNoNode;
    if (sets.size() == 1)
        return sets[0];
    int levels = 1;
    for (NodeId s : sets)
        levels = std::max(levels, lodLevelOf(g, s) + 1);
    const int level = viewLevel(g, realOf(g, sets[0]), view, levels);
    NodeId best = kNoNode, any = kNoNode, lowest = kNoNode;
    int bestLevel = -1, lowestLevel = INT32_MAX;
    for (NodeId s : sets)
    {
        const int l = lodLevelOf(g, s);
        if (l < 0)
        {
            if (any == kNoNode)
                any = s;
            continue;
        }
        if (l <= level && l > bestLevel)
        {
            best = s;
            bestLevel = l;
        }
        if (l < lowestLevel)
        {
            lowest = s;
            lowestLevel = l;
        }
    }
    return best != kNoNode ? best : any != kNoNode ? any : lowest;
}

std::unordered_set<NodeId> hidden(Graph& g, const lod::View& view)
{
    LILA_ZONE();

    std::unordered_set<NodeId> out;
    const std::vector<NodeId> sets = all(g);
    if (sets.empty())
        return out;
    // Sets standing in for the same real node show one at a time, by LOD.
    std::map<NodeId, std::vector<NodeId>> byCharacter;
    for (NodeId s : sets)
    {
        const std::vector<NodeId> real = realOf(g, s);
        byCharacter[real.empty() ? s : real.front()].push_back(s);
    }
    std::unordered_set<NodeId> pieceIds;
    for (NodeId s : sets)
    {
        pieceIds.insert(s);
        for (const Piece& p : pieces(g, s))
            pieceIds.insert(p.mesh);
    }
    for (const auto& [character, group] : byCharacter)
    {
        const NodeId shown = shownSet(g, group, view);
        for (NodeId s : group)
        {
            const Members* m = membersOf(g, s);
            if (!m)
                continue;
            if (s != shown || display(g, s) == Display::Real)
            {
                for (const Piece& p : pieces(g, s))
                    out.insert(p.mesh);
                continue;
            }
            for (NodeId r : realOf(g, s))
                hideTree(g, r, out, pieceIds);
            hidePieces(g, *m, out);
        }
    }
    out.erase(kNoNode);
    return out;
}

NodeId create(Graph& g, std::string_view name, const std::vector<NodeId>& real, int lodLevel)
{
    Node* made = g.createNode(*types::find(kTypeName), name.empty() ? "animProxy" : name, kNoNode, true);
    auto m = std::make_shared<Members>();
    for (NodeId r : real)
        if (g.node(r))
            m->real.push_back(Ref::of(g, r));
    store(g, made->id(), std::move(m));
    g.setValue({made->id(), ProxySet::kLodLevel, -1}, int64_t(std::clamp(lodLevel, -1, lod::kMaxLevels - 1)));
    return made->id();
}

NodeId nearestJoint(Graph& g, NodeId set, const glm::dvec3& point)
{
    std::set<NodeId> joints;
    for (NodeId mesh : skinnedMeshesOf(g, set))
        for (const skin::Influence& inf : skin::influences(g, skin::find(g, mesh)))
            if (inf.joint != kNoNode)
                joints.insert(inf.joint);
    if (joints.empty())
        for (NodeId id : g.nodes())
            if (rig::isJoint(*g.node(id)))
                joints.insert(id);
    g.evaluate();
    NodeId best = kNoNode;
    double bestDist = 1e300;
    for (NodeId j : joints)
    {
        const glm::dvec3 a = worldPos(g, j);
        double d = glm::length(point - a);
        for (NodeId c : g.children(j))
            if (rig::isJoint(*g.node(c)))
                d = std::min(d, boneDistance(point, a, worldPos(g, c)));
        if (d < bestDist)
        {
            bestDist = d;
            best = j;
        }
    }
    return best != kNoNode ? rig::selectAsTarget(g, best) : kNoNode;
}

void addPiece(Graph& g, NodeId set, NodeId mesh, NodeId joint, Kind kind, bool bind, bool reparent)
{
    LILA_ZONE();

    const Node* s = g.node(set);
    const Node* n = g.node(mesh);
    if (!s || !isSet(*s))
        throw std::invalid_argument("proxy: not a proxy set");
    if (!n || !n->type().isA("mesh"))
        throw std::invalid_argument("proxy: a piece must be a mesh");
    if (const std::vector<NodeId> real = realOf(g, set); std::ranges::find(real, mesh) != real.end())
        throw std::invalid_argument("proxy: " + g.path(mesh) + " is one of the meshes the set stands in for");
    if (const NodeId other = setOfPiece(g, mesh); other != kNoNode && other != set)
        throw std::invalid_argument("proxy: " + g.path(mesh) + " is already a piece of " + g.path(other));
    g.evaluate();
    if (joint == kNoNode)
    {
        const geo::Bounds b = worldBounds(g, mesh);
        joint = nearestJoint(g, set, b.valid() ? b.center() : worldPos(g, mesh));
    }
    if (reparent && g.parent(mesh) != set)
        g.setParent(mesh, set, true);
    auto m = editable(g, set);
    bool found = false;
    for (Members::Entry& e : m->pieces)
        if (e.mesh.resolve(g) == mesh)
        {
            e.joint = Ref::of(g, joint);
            e.kind = kind;
            found = true;
        }
    if (!found)
        m->pieces.push_back({Ref::of(g, mesh), Ref::of(g, joint), kind, PieceDisplay::Follow});
    store(g, set, std::move(m));
    if (!bind || alreadyBound(g, mesh))
        return;
    if (kind == Kind::True)
    {
        const std::vector<NodeId> skinned = skinnedMeshesOf(g, set);
        if (skinned.empty())
            throw std::invalid_argument("proxy: a true piece needs a skinned real mesh to copy weights from");
        skinLike(g, mesh, skinned.front());
    }
    else if (joint != kNoNode)
        ops::execute("parentConstraint", {{"args", {g.path(joint), g.path(mesh)}}, {"maintainOffset", true}});
}

void removePiece(Graph& g, NodeId set, NodeId mesh)
{
    auto m = editable(g, set);
    std::erase_if(m->pieces, [&](const Members::Entry& e) { return e.mesh.resolve(g) == mesh; });
    store(g, set, std::move(m));
}

void addReal(Graph& g, NodeId set, NodeId node)
{
    if (!g.node(node))
        throw std::invalid_argument("proxy: no such node");
    if (setOfPiece(g, node) != kNoNode)
        throw std::invalid_argument("proxy: " + g.path(node) + " is a proxy piece");
    auto m = editable(g, set);
    for (const Ref& r : m->real)
        if (r.resolve(g) == node)
            return;
    m->real.push_back(Ref::of(g, node));
    store(g, set, std::move(m));
}

void setDisplay(Graph& g, NodeId set, Display d)
{
    if (display(g, set) != d)
        g.setValue({set, ProxySet::kDisplay, -1}, int64_t(d));
}

void setPieceDisplay(Graph& g, NodeId set, NodeId mesh, PieceDisplay d)
{
    auto m = editable(g, set);
    const NodeId joint = [&] {
        for (const Members::Entry& e : m->pieces)
            if (e.mesh.resolve(g) == mesh)
                return e.joint.resolve(g);
        throw std::invalid_argument("proxy: " + g.path(mesh) + " is not a piece of " + g.path(set));
    }();
    // The override belongs to the joint's region, so every piece of the joint takes it.
    for (Members::Entry& e : m->pieces)
    {
        const NodeId pm = e.mesh.resolve(g);
        if (pm == mesh || (joint != kNoNode && e.joint.resolve(g) == joint))
            e.display = d;
    }
    store(g, set, std::move(m));
}

json info(const Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    json real = json::array();
    for (NodeId r : realOf(g, set))
        real.push_back(g.path(r));
    json list = json::array();
    for (const Piece& p : pieces(g, set))
        list.push_back({{"mesh", g.path(p.mesh)},
                        {"joint", p.joint != kNoNode ? json(g.path(p.joint)) : json(nullptr)},
                        {"kind", kindName(p.kind)},
                        {"display", pieceDisplayName(p.display)}});
    return {{"name", n->path()}, {"display", displayName(display(g, set))}, {"lodLevel", lodLevelOf(g, set)}, {"real", real}, {"pieces", list}};
}

void registerTypes()
{
    if (types::find(kTypeName))
        return;
    data::registerKind(kMembersKind, &Members::fromJson);
    NodeType t;
    t.name = kTypeName;
    t.icon = "puzzle";
    t.base = types::find("transform");
    t.dag = true;
    t.doc = "Anim proxy: stand-in pieces for a character's meshes, each bound to a joint, shown in place of the real meshes or not. "
            "Several sets can stand in for one character, one per LOD level. Pieces never export.";
    t.ownAttrs = {
        {.name = "display", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored, .enumNames = kDisplayNames,
         .doc = "proxy shows the pieces in place of the real meshes; real shows the real meshes."},
        {.name = "lodLevel", .type = PlugType::Int, .defaultValue = int64_t(-1), .flags = kStored,
         .doc = "The LOD level this set stands in at, among the sets for one character; -1 for any level.", .niceName = "LOD Level",
         .minValue = -1.0, .maxValue = double(lod::kMaxLevels - 1)},
        {.name = "planes", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | AttrHidden,
         .doc = "The split planes the pieces were cut with, as animProxyFromMesh takes them, for editing and regenerating."},
        {.name = "members", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kStored | AttrHidden,
         .doc = "The real meshes and the pieces with their joints."},
    };
    t.create = [] { return std::make_unique<ProxySet>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    if (ops::find("createProxySet"))
        return;
    split::registerCommands();

    ops::registerCommand({
        .name = "createProxySet",
        .doc = "Make an empty proxy set standing in for the given meshes (or groups), to add pieces to with addToProxySet. Returns "
               "the set.",
        .positionalDoc = "real meshes",
        .flags = {{"name", "n", T::String, nullptr, "Name; <mesh>_animProxy by default."},
                  {"lodLevel", "lod", T::Int, -1, "The LOD level the set stands in at; -1 for any."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> real;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("transform"))
                    throw CommandError("createProxySet: " + name + " is not an object");
                if (isSet(*n) || setOfPiece(g, n->id()) != kNoNode)
                    throw CommandError("createProxySet: " + name + " is part of a proxy set already");
                real.push_back(n->id());
            }
            if (real.empty())
                throw CommandError("createProxySet: select the meshes the proxies stand in for");
            UndoChunk chunk("Create Proxy Set");
            const std::string name = a.has("name") ? a.get<std::string>("name") : shortName(g, real[0]) + "_animProxy";
            const NodeId set = create(g, name, real, a.get<int>("lodLevel", -1));
            g.select({set});
            return g.path(set);
        },
        .examples = {"createProxySet(\"Body\", lodLevel=0)"},
    });

    ops::registerCommand({
        .name = "addToProxySet",
        .doc = "Make meshes pieces of a proxy set: each bound to a joint (the nearest bone by default) and moved under the set. A "
               "proxy piece is parent constrained to its joint; a true piece is skinned with the real mesh's weights.",
        .positionalDoc = "set, meshes...",
        .flags = {{"joint", "j", T::String, nullptr, "The joint the pieces follow; the nearest bone when not given."},
                  {"kind", "k", T::String, "proxy", "proxy (rigid stand-in) or true (the real surface, skinned).", {"proxy", "true"}},
                  {"bind", "b", T::Bool, true, "Bind the pieces to their joint; off leaves them as they are."},
                  {"parent", "p", T::Bool, true, "Move the pieces under the set."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("addToProxySet: give the set and the meshes");
            // The set can be named anywhere among the objects, as a selection lists it.
            NodeId set = kNoNode;
            std::vector<NodeId> meshes;
            for (const std::string& name : objs)
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("addToProxySet: no node named '" + name + "'");
                if (isSet(*n))
                    set = set == kNoNode ? n->id() : set;
                else
                    meshes.push_back(meshArg(g, name, "addToProxySet").id());
            }
            if (set == kNoNode || meshes.empty())
                throw CommandError("addToProxySet: give one proxy set and the meshes to add");
            const NodeId joint = a.has("joint") ? jointNamed(g, a.get<std::string>("joint"), "addToProxySet") : kNoNode;
            const Kind kind = kindArg(a.get<std::string>("kind", "proxy"), "addToProxySet");
            UndoChunk chunk("Add to Proxy Set");
            json out = json::array();
            for (NodeId m : meshes)
            {
                guarded([&] {
                    addPiece(g, set, m, joint, kind, a.get<bool>("bind", true), a.get<bool>("parent", true));
                    return 0;
                });
                out.push_back(g.path(m));
            }
            return out;
        },
        .examples = {"addToProxySet(\"Body_animProxy\", \"MyHeadProxy\", joint=\"head\")"},
    });

    ops::registerCommand({
        .name = "removeFromProxySet",
        .doc = "Stop meshes being pieces of their proxy set. They keep their binding and stay where they are.",
        .positionalDoc = "meshes...",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            UndoChunk chunk("Remove from Proxy Set");
            int removed = 0;
            for (const std::string& name : a.objects())
            {
                Node& n = meshArg(g, name, "removeFromProxySet");
                if (const NodeId s = setOfPiece(g, n.id()); s != kNoNode)
                {
                    removePiece(g, s, n.id());
                    ++removed;
                }
            }
            return removed;
        },
    });

    ops::registerCommand({
        .name = "setProxyDisplay",
        .doc = "Show anim proxies or the real meshes. Targets are proxy sets, or anything they stand in for: a real mesh, a piece, "
               "or a joint of the character (every set of its skeleton); every set when none are given or selected. Returns the "
               "display set.\n\n"
               "With piece, only that piece's joint changes: pieceDisplay real shows its true piece (the real head for facial "
               "animation, say) while the rest stays proxy.",
        .positionalDoc = "targets",
        .flags = {{"display", "d", T::String, "toggle", "proxy, real or toggle.", {"proxy", "real", "toggle"}},
                  {"piece", "pc", T::String, nullptr, "A piece, or a joint whose pieces to override."},
                  {"pieceDisplay", "pd", T::String, "real", "follow, proxy or real.", {"follow", "proxy", "real"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            UndoChunk chunk("Proxy Display");
            if (a.has("piece"))
            {
                Node* p = g.find(a.get<std::string>("piece"));
                if (!p)
                    throw CommandError("setProxyDisplay: no node named '" + a.get<std::string>("piece") + "'");
                const std::string pd = a.get<std::string>("pieceDisplay", "real");
                const PieceDisplay d = pd == "follow" ? PieceDisplay::Follow : pd == "proxy" ? PieceDisplay::Proxy : PieceDisplay::Real;
                if (pd != "follow" && pd != "proxy" && pd != "real")
                    throw CommandError("setProxyDisplay: pieceDisplay must be follow, proxy or real");
                int changed = 0;
                for (NodeId s : setsFor(g, p->id()))
                    for (const Piece& pc : pieces(g, s))
                        if (pc.mesh == p->id() || pc.joint == p->id())
                        {
                            guarded([&] {
                                setPieceDisplay(g, s, pc.mesh, d);
                                return 0;
                            });
                            ++changed;
                            break;
                        }
                if (!changed)
                    throw CommandError("setProxyDisplay: " + p->path() + " is neither a piece nor a joint with pieces");
                return pieceDisplayName(d);
            }
            const std::vector<NodeId> sets = setsFromArgs(g, a.objects(), "setProxyDisplay");
            if (sets.empty())
                throw CommandError("setProxyDisplay: no proxy sets found");
            const std::string want = a.get<std::string>("display", "toggle");
            Display d = Display::Proxy;
            if (want == "real")
                d = Display::Real;
            else if (want == "toggle")
                d = display(g, sets[0]) == Display::Proxy ? Display::Real : Display::Proxy;
            else if (want != "proxy")
                throw CommandError("setProxyDisplay: display must be proxy, real or toggle");
            for (NodeId s : sets)
                setDisplay(g, s, d);
            return displayName(d);
        },
        .examples = {"setProxyDisplay(\"Body_animProxy\", display=\"real\")", "setProxyDisplay(piece=\"head\", pieceDisplay=\"real\")"},
    });

    ops::registerCommand({
        .name = "proxySetInfo",
        .doc = "The proxy sets of the targets (every set when none): [{name, display, lodLevel, real, pieces: [{mesh, joint, kind, "
               "display}]}].",
        .positionalDoc = "targets",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId s : setsFromArgs(g, a.positional.empty() ? std::vector<std::string>{} : a.objects(), "proxySetInfo"))
                out.push_back(info(g, s));
            return out;
        },
    });

    ops::registerCommand({
        .name = "animProxyFromMesh",
        .doc = "Make an anim proxy of a skinned mesh in one go: split it along planes (at its joint boundaries when none are given), "
               "remesh each piece, bind each to the joint that dominates it and register them in a new proxy set. Returns the set.\n\n"
               "Pieces with remesh none, and those of trueJoints, become true pieces: the real surface, skinned like the mesh, for "
               "facial animation on the real head while the body stays proxy (setProxyDisplay piece=).",
        .positionalDoc = "mesh",
        .flags = {
            {"planes", "pl", T::Any, nullptr, "World planes as splitMesh takes them; the joint boundaries when not given."},
            {"suggest", "sg", T::Bool, false, "Add the joint-boundary planes to the given ones."},
            {"cap", "c", T::Bool, true, "Close the holes the cuts leave."},
            {"remesh", "rm", T::String, "voxelRemesh", "How each proxy piece is rebuilt.", {"none", "quadRemesh", "voxelRemesh", "decimate"}},
            {"targetFaces", "tf", T::Int, 300, "Quad remesh: faces per piece."},
            {"resolution", "res", T::Int, 32, "Voxel remesh: voxels along a piece's longest side."},
            {"ratio", "r", T::Float, 0.25, "Decimate: share of triangles kept."},
            {"pieceSettings", "ps", T::Any, nullptr, "Per piece index: {remesh, targetFaces, resolution, ratio}."},
            {"pieceFaces", "pf", T::String, "", "Face count per piece, as index:count pairs (0:150 3:80)."},
            {"exactPieces", "ep", T::String, "", "Pieces kept exact (true pieces, skinned like the mesh), by index (1 4)."},
            {"replace", "rp", T::Bool, true, "Replace the mesh's set at the same LOD level instead of adding another."},
            {"trueJoints", "tj", T::List, nullptr, "Joints whose pieces also get a true piece, shown by setProxyDisplay piece=."},
            {"name", "n", T::String, nullptr, "Name of the new set; <mesh>_animProxy by default."},
            {"set", "s", T::String, nullptr, "Add the pieces to this set instead of making one."},
            {"lodLevel", "lod", T::Int, -1, "The new set's LOD level; -1 for any."},
        },
        .run = animProxyFromMesh,
        .adjustable = true,
        .examples = {"animProxyFromMesh(\"Body\")", "animProxyFromMesh(\"Body\", remesh=\"decimate\", ratio=0.1, trueJoints=[\"head\"])",
                     "animProxyFromMesh(\"Body\", lodLevel=1, resolution=16)"},
    });

    // The adjust popup tunes the result: remesh, counts and per-piece overrides, never the planes.
    ops::options::Override o;
    o.order = {"remesh", "targetFaces", "resolution", "ratio", "pieceFaces", "exactPieces", "cap"};
    o.hidden = {"planes", "pieceSettings", "trueJoints", "set", "name", "suggest", "replace", "lodLevel"};
    o.adjust = {"pieceFaces", "exactPieces"};
    o.popupHidden = {"suggest", "replace", "lodLevel"};
    ops::options::setOverride("animProxyFromMesh", std::move(o));
}
} // namespace lila::scene::proxy
