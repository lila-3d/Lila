#include "scene/SelectAs.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/DominantInfluence.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace lila::scene
{
using namespace rigutil;

namespace
{
/** The id is authoritative while the node lives; the name binds a reference read from a file. */
struct NodeRef final : Data
{
    mutable NodeId id = kNoNode;
    mutable std::string name;

    const char* kind() const override { return rig::kNodeRefKind; }
    json toJson() const override
    {
        const Node* n = id != kNoNode ? Graph::current().node(id) : nullptr;
        return {{"kind", kind()}, {"node", n ? n->path() : id == kNoNode ? name : std::string()}};
    }

    static DataPtr fromJson(const json& j)
    {
        auto out = std::make_shared<NodeRef>();
        if (j.contains("node") && j["node"].is_string())
            out->name = j["node"].get<std::string>();
        return out;
    }
};

const Node* jointNode(const Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    return n && rig::isJoint(*n) ? n : nullptr;
}

NodeId parentJoint(const Graph& g, NodeId id)
{
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        if (jointNode(g, p))
            return p;
    return kNoNode;
}

std::vector<std::string> tokensOf(std::string_view name)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty())
            out.push_back(std::exchange(cur, {}));
    };
    for (size_t i = 0; i < name.size(); ++i)
    {
        const unsigned char c = (unsigned char)name[i];
        if (!std::isalpha(c))
        {
            flush();
            continue;
        }
        if (std::isupper(c) && i > 0 && std::islower((unsigned char)name[i - 1]))
            flush();
        cur.push_back(char(std::tolower(c)));
    }
    flush();
    return out;
}

/** Joints of every skin each joint influences, for the weighting preference. */
std::unordered_map<NodeId, std::set<NodeId>> skinsByJoint(Graph& g)
{
    std::unordered_map<NodeId, std::set<NodeId>> out;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA("skinCluster"))
            for (const skin::Influence& inf : skin::influences(g, id))
                if (inf.joint != kNoNode)
                    out[inf.joint].insert(id);
    return out;
}

bool sharesSkin(const std::unordered_map<NodeId, std::set<NodeId>>& skins, NodeId a, NodeId b)
{
    auto ia = skins.find(a), ib = skins.find(b);
    if (ia == skins.end() || ib == skins.end())
        return false;
    for (NodeId s : ia->second)
        if (ib->second.contains(s))
            return true;
    return false;
}

const SkinWeights* weightsOf(Graph& g, NodeId skin)
{
    return dynamic_cast<const SkinWeights*>(std::get<DataPtr>(g.storedValue({skin, SkinClusterNode::kWeights, -1})).get());
}

const char* roleName(rig::CandidateRole r)
{
    switch (r)
    {
    case rig::CandidateRole::Redirect: return "redirect";
    case rig::CandidateRole::Weighted: return "weighted";
    case rig::CandidateRole::Parent: return "parent";
    case rig::CandidateRole::Child: return "child";
    case rig::CandidateRole::Influence: return "influence";
    }
    return "";
}

std::vector<NodeId> jointsFrom(Graph&, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : a.objects())
    {
        Node& n = nodeNamed(name, cmd);
        if (!rig::isJoint(n))
            throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
        out.push_back(n.id());
    }
    return out;
}
} // namespace

namespace rig
{
DataPtr makeNodeRef(const Graph& g, NodeId node)
{
    auto out = std::make_shared<NodeRef>();
    if (const Node* n = g.node(node))
    {
        out->id = node;
        out->name = n->path();
    }
    return out;
}

NodeId resolveNodeRef(const Graph& g, const DataPtr& ref, const std::string& ownerNamespace)
{
    const auto* r = dynamic_cast<const NodeRef*>(ref.get());
    if (!r)
        return kNoNode;
    if (r->id != kNoNode)
        return g.node(r->id) ? r->id : kNoNode; // a deleted target stays unbound
    if (r->name.empty())
        return kNoNode;
    const Node* n = nullptr;
    try
    {
        if (!ownerNamespace.empty())
            n = g.find(ownerNamespace + ":" + r->name);
        if (!n)
            n = g.find(r->name);
    }
    catch (const std::exception&)
    {
        return kNoNode; // ambiguous: unbound until the name is made unique
    }
    if (!n)
        return kNoNode;
    r->id = n->id();
    r->name = n->path();
    return r->id;
}

void registerNodeRefKind() { data::registerKind(kNodeRefKind, &NodeRef::fromJson); }

NodeId selectAsOf(const Graph& g, NodeId joint)
{
    const Node* n = jointNode(g, joint);
    if (!n)
        return kNoNode;
    return resolveNodeRef(g, n->as<DataPtr>(Joint::kSelectAs), n->nameSpace());
}

void setSelectAs(Graph& g, NodeId joint, NodeId target)
{
    const Node* n = jointNode(g, joint);
    if (!n)
        throw std::invalid_argument("selectAs: not a joint");
    if (target == joint)
        throw std::invalid_argument("selectAs: " + n->path() + " can't select as itself");
    if (target != kNoNode && !g.node(target))
        throw std::invalid_argument("selectAs: no such target");
    if (selectAsOf(g, joint) == target)
        return;
    g.setValue({joint, Joint::kSelectAs, -1}, makeNodeRef(g, target));
}

NodeId selectAsTarget(const Graph& g, NodeId node)
{
    NodeId cur = node;
    std::set<NodeId> seen{node};
    for (NodeId next = selectAsOf(g, cur); next != kNoNode && seen.insert(next).second; next = selectAsOf(g, cur))
        cur = next;
    return cur;
}

const std::vector<std::string>& defaultHelperTokens()
{
    static const std::vector<std::string> t = {"twist", "roll", "cor", "corrective", "helper", "fix", "volume", "muscle",
                                               "bicep", "tricep", "latissimus", "pec", "scap", "knee", "inner", "outer",
                                               "in", "out", "fwd", "bck", "lwr", "upr"};
    return t;
}

bool isHelperName(std::string_view name, const std::vector<std::string>& tokens)
{
    // Namespaces belong to the reference, not the joint.
    if (size_t colon = name.rfind(':'); colon != std::string_view::npos)
        name = name.substr(colon + 1);
    for (const std::string& t : tokensOf(name))
        if (std::find(tokens.begin(), tokens.end(), t) != tokens.end())
            return true;
    return false;
}

std::vector<std::pair<NodeId, NodeId>> autoSelectAsTargets(Graph& g, const std::vector<NodeId>& joints, const AutoSelectAsOptions& opts)
{
    LILA_ZONE();

    std::unordered_map<NodeId, bool> memo;
    // A leaf under a helper is one too (a correctiveRoot's targets); a helper inside the main chain keeps its children.
    auto helper = [&](auto&& self, NodeId id) -> bool {
        if (auto it = memo.find(id); it != memo.end())
            return it->second;
        const Node* n = jointNode(g, id);
        bool h = n && isHelperName(n->name(), opts.tokens);
        if (n && !h)
        {
            bool leaf = std::none_of(g.children(id).begin(), g.children(id).end(), [&](NodeId c) { return jointNode(g, c) != nullptr; });
            NodeId p = parentJoint(g, id);
            h = leaf && p != kNoNode && self(self, p);
        }
        memo[id] = h;
        return h;
    };
    const auto skins = skinsByJoint(g);
    std::vector<std::pair<NodeId, NodeId>> out;
    for (NodeId j : joints)
    {
        if (!jointNode(g, j) || !helper(helper, j))
            continue;
        NodeId nearest = kNoNode, weighted = kNoNode;
        for (NodeId p = parentJoint(g, j); p != kNoNode && weighted == kNoNode; p = parentJoint(g, p))
        {
            if (helper(helper, p))
                continue;
            if (nearest == kNoNode)
                nearest = p;
            if (!skins.contains(j) || sharesSkin(skins, j, p))
                weighted = p;
        }
        if (NodeId t = weighted != kNoNode ? weighted : nearest; t != kNoNode)
            out.emplace_back(j, t);
    }
    return out;
}

int applyAutoSelectAs(Graph& g, const std::vector<NodeId>& roots, const AutoSelectAsOptions& opts)
{
    std::vector<NodeId> joints;
    std::set<NodeId> seen;
    auto walk = [&](auto&& self, NodeId id) -> void {
        if (!seen.insert(id).second)
            return;
        if (jointNode(g, id))
            joints.push_back(id);
        for (NodeId c : g.children(id))
            self(self, c);
    };
    if (roots.empty())
    {
        for (NodeId id : g.nodes())
            if (jointNode(g, id))
                joints.push_back(id);
    }
    else
        for (NodeId r : roots)
            walk(walk, r);
    int count = 0;
    for (auto [j, t] : autoSelectAsTargets(g, joints, opts))
    {
        if (!opts.overwrite && selectAsOf(g, j) != kNoNode)
            continue;
        if (selectAsOf(g, j) == t)
            continue;
        setSelectAs(g, j, t);
        ++count;
    }
    return count;
}

std::vector<SelectCandidate> selectionCandidates(Graph& g, NodeId mesh, uint32_t face, std::optional<glm::dvec3> hit, bool redirect)
{
    LILA_ZONE();

    std::vector<SelectCandidate> out;
    const NodeId dominant = dominantJoint(g, mesh, face);
    if (dominant == kNoNode)
        return out;
    auto push = [&](NodeId j, CandidateRole role, float w) {
        if (j == kNoNode || std::any_of(out.begin(), out.end(), [&](const SelectCandidate& c) { return c.joint == j; }))
            return;
        out.push_back({j, role, w});
    };

    // Each influence's share of the face's corner weights.
    std::map<NodeId, float> shares;
    const NodeId skin = skin::find(g, mesh);
    MeshPtr in = skin != kNoNode ? skin::inputMesh(g, skin) : nullptr;
    const SkinWeights* w = skin != kNoNode ? weightsOf(g, skin) : nullptr;
    if (in && w && face < in->faceCapacity() && in->faceValid(face))
    {
        std::unordered_map<uint32_t, float> sums;
        float total = 0.0f;
        in->forEachFaceCorner(face, [&](geo::Index c) {
            geo::Index p = in->cornerPoint(c);
            if (p >= w->pointCount())
                return;
            for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
            {
                sums[w->influences[k]] += w->weights[k];
                total += w->weights[k];
            }
        });
        for (const skin::Influence& inf : skin::influences(g, skin))
            if (auto it = sums.find(inf.index); it != sums.end() && inf.joint != kNoNode && total > 0.0f)
                shares[inf.joint] += it->second / total;
    }
    auto share = [&](NodeId j) { return shares.contains(j) ? shares[j] : 0.0f; };

    const NodeId primary = redirect ? selectAsTarget(g, dominant) : dominant;
    if (primary != dominant)
        push(primary, CandidateRole::Redirect, share(primary));
    push(dominant, CandidateRole::Weighted, share(dominant));
    push(parentJoint(g, primary), CandidateRole::Parent, 0.0f);

    NodeId child = kNoNode;
    double best = 1e300;
    for (NodeId c : g.children(primary))
    {
        const Node* n = jointNode(g, c);
        // Helpers that click back to this joint aren't where the limb goes next.
        if (!n || selectAsTarget(g, c) == primary)
            continue;
        double d = hit ? glm::length(positionOf(*n) - *hit) : 0.0;
        if (child == kNoNode || d < best)
        {
            child = c;
            best = d;
        }
    }
    push(child, CandidateRole::Child, 0.0f);

    std::vector<std::pair<float, NodeId>> rest;
    for (auto [j, s] : shares)
        if (s >= kSignificantInfluence)
            rest.emplace_back(s, j);
    std::stable_sort(rest.begin(), rest.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto [s, j] : rest)
        push(j, CandidateRole::Influence, s);
    return out;
}
} // namespace rig

void registerSelectAsCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "selectAs",
        .doc = "Set or clear where a mesh click on joints lands instead of them (a twist bone selecting its limb). "
               "With neither target nor clear, returns {joint: target or None} for the joints.",
        .positionalDoc = "joints",
        .flags = {
            nodeFlag({"target", "t", T::String, nullptr, "Joint (or any node) a click on these selects."}, "transform"),
            {"clear", "c", T::Bool, false, "Remove the redirect."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> joints = jointsFrom(g, a, "selectAs");
            if (a.has("target") || a.get<bool>("clear"))
            {
                NodeId target = a.get<bool>("clear") ? kNoNode : nodeNamed(a.get<std::string>("target"), "selectAs").id();
                for (NodeId j : joints)
                    guarded([&] { rig::setSelectAs(g, j, target); });
                return int(joints.size());
            }
            json out = json::object();
            for (NodeId j : joints)
            {
                NodeId t = rig::selectAsOf(g, j);
                out[g.node(j)->path()] = t != kNoNode ? json(g.node(t)->path()) : json(nullptr);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "autoSelectAs",
        .doc = "Point helper joints (twist, corrective, muscle bones by name) at their nearest non-helper ancestor, "
               "preferring one weighted on the same skin. Works on the joints and everything under them, every joint "
               "with none given or selected. Returns {joint: target} for what was (or with query, would be) set.",
        .positionalDoc = "roots",
        .flags = {
            {"overwrite", "ow", T::Bool, false, "Replace redirects already set."},
            {"tokens", "tk", T::List, nullptr, "Name tokens that mark a helper instead of the defaults."},
            {"query", "q", T::Bool, false, "Only report."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> roots;
            for (const std::string& name : a.objects())
                roots.push_back(nodeNamed(name, "autoSelectAs").id());
            rig::AutoSelectAsOptions opts;
            opts.overwrite = a.get<bool>("overwrite");
            if (a.has("tokens"))
            {
                opts.tokens.clear();
                for (const json& t : a.flags["tokens"])
                    for (const std::string& tok : tokensOf(t.get<std::string>()))
                        opts.tokens.push_back(tok);
            }
            std::vector<NodeId> joints;
            std::set<NodeId> seen;
            auto walk = [&](auto&& self, NodeId id) -> void {
                if (!seen.insert(id).second)
                    return;
                if (jointNode(g, id))
                    joints.push_back(id);
                for (NodeId c : g.children(id))
                    self(self, c);
            };
            if (roots.empty())
                for (NodeId id : g.nodes())
                    walk(walk, id);
            else
                for (NodeId r : roots)
                    walk(walk, r);
            json out = json::object();
            for (auto [j, t] : rig::autoSelectAsTargets(g, joints, opts))
            {
                if (!opts.overwrite && rig::selectAsOf(g, j) != kNoNode)
                    continue;
                if (!a.get<bool>("query"))
                    rig::setSelectAs(g, j, t);
                out[g.node(j)->path()] = g.node(t)->path();
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "selectionCandidates",
        .doc = "The joints a click on a skinned mesh's face could mean: [{joint, role, weight}] with role redirect, "
               "weighted, parent, child or influence, in that order.",
        .positionalDoc = "mesh",
        .flags = {
            {"face", "f", T::Int, nullptr, "Face id.", {}, {}, 0.0},
            {"position", "p", T::Vec3, nullptr, "World hit point, to pick the child nearest it."},
            {"redirect", "r", T::Bool, true, "Follow selectAs; off treats the dominant joint as the click."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = a.objects();
            if (objs.size() != 1 || !a.has("face"))
                throw CommandError("selectionCandidates: give one mesh and a face");
            NodeId mesh = nodeNamed(objs[0], "selectionCandidates").id();
            g.evaluate();
            std::optional<glm::dvec3> hit;
            if (a.has("position"))
                hit = vec3Of(a.flags["position"]);
            json out = json::array();
            for (const rig::SelectCandidate& c :
                 rig::selectionCandidates(g, mesh, uint32_t(a.get<int64_t>("face")), hit, a.get<bool>("redirect", true)))
                out.push_back({{"joint", g.node(c.joint)->path()}, {"role", roleName(c.role)}, {"weight", c.weight}});
            return out;
        },
    });
}
} // namespace lila::scene
