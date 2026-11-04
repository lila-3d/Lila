#include "ops/Command.h"
#include "core/Profile.h"
#include "fbx/Fbx.h"
#include "link/UnrealLink.h"
#include "math/MathCore.h"
#include "rigmath/Expressions.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Morphs.h"
#include "scene/RigElements.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <algorithm>
#include <map>
#include <set>

namespace lila::link
{
namespace
{
using namespace scene;

json rows(const glm::dmat3& m)
{
    json out = json::array();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.push_back(m[c][r]);
    return out;
}

/** Unreal axes from Lila's: the ue5 FBX basis, then the mirror Unreal's FBX import applies to its Y axis. */
glm::dmat3 unrealBasis()
{
    glm::dmat3 k = fbx::profileBasis(fbx::AxisProfile::Ue5, math::upAxis());
    for (int c = 0; c < 3; ++c)
        k[c][1] = -k[c][1];
    return k;
}

std::string curveName(const Graph& g, const PlugRef& p)
{
    std::string s = g.node(p.node)->name() + "_" + g.attr(p).name;
    if (p.component >= 0)
        s += "xyz"[p.component];
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    return s;
}

struct RigBuild
{
    Graph& g;
    NodeId root = kNoNode;
    glm::dmat3 basis{1.0};
    std::map<NodeId, std::string> bones;
    std::map<NodeId, glm::dmat4> bindWorld; // skinned joints' world matrices at bind
    json controls = json::array();
    std::set<std::string> controlNames;
    std::set<std::string> curves;
    json warnings = json::array();

    bool under(NodeId id) const
    {
        for (NodeId n = id; n != kNoNode; n = g.parent(n))
            if (n == root)
                return true;
        return false;
    }

    /**
     * A root bone's local matrix at bind, the pose its Unreal reference pose was exported from. LilaLink solves the frame
     * the import put the bone in (its non-bone ancestors, the facing turn) by comparing the two.
     */
    void bindLocal(NodeId joint, json& b) const
    {
        glm::dmat4 local = g.node(joint)->as<glm::dmat4>(Transform::kMatrix);
        if (auto it = bindWorld.find(joint); it != bindWorld.end())
        {
            const NodeId p = g.parent(joint);
            local = (p == kNoNode ? glm::dmat4(1.0) : glm::inverse(g.node(p)->as<glm::dmat4>(Transform::kWorldMatrix))) * it->second;
        }
        b["bindLocal"] = rows(glm::dmat3(local));
        b["bindTranslation"] = {local[3][0], local[3][1], local[3][2]};
    }

    json boneBinding(const PlugRef& p, int channel)
    {
        const Node& n = *g.node(p.node);
        const auto& t = static_cast<const Transform&>(n);
        json b = {{"kind", "bone"}, {"name", bones.at(p.node)}, {"channel", channel}, {"order", n.as<int64_t>(Transform::kRotateOrder)},
                  {"orient", rows(glm::dmat3(t.preRotation()))}, {"axis", rows(glm::dmat3(t.rotateAxisMatrix()))}};
        json rest = json::array();
        for (int attr : {int(Transform::kTranslate), int(Transform::kRotate), int(Transform::kScale)})
        {
            const glm::dvec3 v = n.as<glm::dvec3>(attr);
            rest.insert(rest.end(), {v.x, v.y, v.z});
        }
        b["rest"] = rest;
        if (!bones.count(g.parent(p.node)))
            bindLocal(p.node, b);
        for (int attr : {int(Transform::kRotatePivot), int(Transform::kScalePivot), int(Transform::kRotatePivotTranslate), int(Transform::kScalePivotTranslate)})
            if (glm::length(n.as<glm::dvec3>(attr)) > 1e-9)
            {
                warnings.push_back(n.name() + " has pivots, which Unreal bones don't: its channels read and write as if they were zero");
                break;
            }
        return b;
    }

    /** Where a plug lives in Unreal, or null with why set. */
    json binding(const PlugRef& p, bool target, std::string& why)
    {
        const Node& n = *g.node(p.node);
        if (bones.count(p.node) && p.component >= 0 &&
            (p.attr == Transform::kTranslate || p.attr == Transform::kRotate || p.attr == Transform::kScale))
            return boneBinding(p, (p.attr == Transform::kTranslate ? 0 : p.attr == Transform::kRotate ? 3 : 6) + p.component);
        if (n.type().isA("morphTarget") && p.attr == morphs::kWeight)
        {
            const std::string name = n.as<std::string>(morphs::kMorphName);
            if (target)
                curves.insert(name);
            return {{"kind", "curve"}, {"name", name}};
        }
        if (target)
        {
            if (!under(p.node))
            {
                why = n.name() + " isn't under " + g.node(root)->name();
                return nullptr;
            }
            const std::string name = curveName(g, p);
            curves.insert(name);
            return {{"kind", "curve"}, {"name", name}};
        }
        const std::string name = curveName(g, p);
        if (curves.count(name))
            return {{"kind", "curve"}, {"name", name}};
        if (controlNames.insert(name).second)
        {
            const AttrDef& d = g.attr(p);
            const PlugValue v = g.value(p);
            const double* x = std::get_if<double>(&v);
            const int64_t* i = std::get_if<int64_t>(&v);
            const bool* bv = std::get_if<bool>(&v);
            json c = {{"name", name}, {"plug", g.plugName(p)}, {"value", x ? *x : i ? double(*i) : bv ? double(*bv) : 0.0}};
            if (d.minValue)
                c["min"] = *d.minValue;
            if (d.maxValue)
                c["max"] = *d.maxValue;
            controls.push_back(c);
        }
        return {{"kind", "control"}, {"name", name}};
    }
};

NodeId skeletalRoot(Graph& g, const Args& a, json& mapping)
{
    if (!a.positional.empty())
    {
        Node* n = g.find(a.positional[0].get<std::string>());
        if (!n)
            throw CommandError("unrealExpressionRig: no node named '" + a.positional[0].get<std::string>() + "'");
        mapping = mappingOf(n->path());
        return n->id();
    }
    NodeId found = kNoNode;
    for (const json& m : mappings())
        if (m.value("kind", "") == "skeletal")
        {
            if (found != kNoNode)
                throw CommandError("unrealExpressionRig: several skeletal mappings; give the node");
            Node* n = g.find(m.value("node", ""));
            if (n)
            {
                found = n->id();
                mapping = m;
            }
        }
    if (found == kNoNode)
        throw CommandError("unrealExpressionRig: give the character's root node, or map it with unrealMap kind=skeletal");
    return found;
}

/** Expressions in an order where each runs after the ones whose targets it reads. */
std::vector<size_t> dependencyOrder(const std::vector<json>& exprs)
{
    auto key = [](const json& b) { return b.value("kind", "") + ":" + b.value("name", "") + ":" + std::to_string(b.value("channel", -1)); };
    std::map<std::string, size_t> writer;
    for (size_t i = 0; i < exprs.size(); ++i)
        writer[key(exprs[i]["bindings"]["target"])] = i;
    std::vector<size_t> order;
    std::vector<int> state(exprs.size(), 0);
    std::function<void(size_t)> visit = [&](size_t i) {
        if (state[i])
            return;
        state[i] = 1;
        for (const json& in : exprs[i]["bindings"]["inputs"])
            if (auto it = writer.find(key(in)); it != writer.end() && it->second != i)
                visit(it->second);
        state[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < exprs.size(); ++i)
        visit(i);
    return order;
}
} // namespace

void registerExpressionRigCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "unrealExpressionRig",
        .doc = "Build a Control Rig in Unreal that runs the character's expressions live: one Lila Expression node each, "
               "evaluating exactly as Lila does. Targets are bone channels, morph target weights (as curves) or attributes under "
               "the root (as curves named node_attr); inputs that aren't bones, morph weights or time become float controls. "
               "Expressions that run as Python stay in Lila. Returns {rig, animBlueprint, expressions, skipped, controls, curves, "
               "warnings, unreal}.",
        .positionalDoc = "root",
        .flags = {{"skeletalMesh", "sm", T::String, nullptr, "The skeletal mesh asset. Default: the root's skeletal mapping."},
                  {"rig", "r", T::String, nullptr, "The Control Rig asset to create or update. Default: CR_<mesh>_Expressions beside the mesh."},
                  {"postProcess", "pp", T::Bool, true,
                   "Run the rig from the mesh's post-process Anim Blueprint (made as ABP_<name>_Expressions) so the expressions play "
                   "wherever the mesh does, and make it the mesh's default animating rig. A post-process Anim Blueprint you made is "
                   "left alone, with a warning to add a Control Rig node to it."},
                  {"dryRun", "dry", T::Bool, false, "Return what would be sent without connecting to Unreal."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            g.evaluate();
            json mapping;
            const NodeId root = skeletalRoot(g, a, mapping);
            std::string mesh = a.get<std::string>("skeletalMesh", "");
            if (mesh.empty() && mapping.is_object())
                mesh = mapping.value("contentPath", "");
            if (mesh.empty())
                throw CommandError("unrealExpressionRig: " + g.node(root)->name() + " has no skeletal mapping; give skeletalMesh");
            std::string rigPath = a.get<std::string>("rig", "");
            if (rigPath.empty())
            {
                const size_t slash = mesh.rfind('/');
                std::string name = mesh.substr(slash + 1);
                if (name.rfind("SK_", 0) == 0)
                    name = name.substr(3);
                rigPath = mesh.substr(0, slash + 1) + "CR_" + name + "_Expressions";
            }

            RigBuild rig{g};
            rig.root = root;
            rig.basis = unrealBasis();
            for (NodeId id : g.descendants(root))
                if (Node* n = g.node(id); n && n->type().isA("joint") && rigel::isDeform(*n))
                    rig.bones[id] = n->name();
            if (Node* n = g.node(root); n->type().isA("joint") && rigel::isDeform(*n))
                rig.bones[root] = n->name();
            for (NodeId id : g.nodes())
                if (Node* n = g.node(id); n && n->type().isA("skinCluster"))
                    for (const skin::Influence& inf : skin::influences(g, id))
                        if (inf.joint != kNoNode && rig.bones.count(inf.joint) && !rig.bindWorld.count(inf.joint))
                            rig.bindWorld[inf.joint] = glm::inverse(g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix));

            json skipped = json::array();
            std::vector<json> exprs;
            // Targets first, so inputs that another expression writes read its curve rather than a control.
            std::vector<std::pair<PlugRef, json>> targets;
            for (const PlugRef& t : expressions::targets(g))
            {
                std::string why;
                json target = rig.binding(t, true, why);
                if (target.is_null())
                {
                    if (rig.under(t.node))
                        skipped.push_back({{"plug", g.plugName(t)}, {"why", why}});
                    continue;
                }
                targets.emplace_back(t, std::move(target));
            }
            for (auto& [t, target] : targets)
            {
                const expressions::ProgramText p = expressions::program(g, t);
                if (p.text.empty())
                {
                    skipped.push_back({{"plug", g.plugName(t)}, {"why", p.why}});
                    continue;
                }
                json inputs = json::array();
                std::string why;
                for (const PlugRef& in : p.inputs)
                    inputs.push_back(rig.binding(in, false, why));
                json bindings = {{"basis", rows(rig.basis)}, {"unitScale", 1.0}, {"inputs", inputs}, {"target", target}};
                const AttrDef& d = g.attr(t);
                if (d.minValue)
                    bindings["min"] = *d.minValue;
                if (d.maxValue)
                    bindings["max"] = *d.maxValue;
                json q = ops::execute("expression", {{"args", {g.plugName(t)}}, {"query", true}});
                exprs.push_back({{"name", curveName(g, t)}, {"plug", g.plugName(t)}, {"text", q.value("text", "")}, {"program", p.text},
                                 {"bindings", bindings}});
            }
            json ordered = json::array();
            for (size_t i : dependencyOrder(exprs))
                ordered.push_back(exprs[i]);

            json payload = {{"skeletalMesh", mesh},
                            {"rig", rigPath},
                            {"expressions", ordered},
                            {"controls", rig.controls},
                            {"curves", json(std::vector<std::string>(rig.curves.begin(), rig.curves.end()))}};
            json plugs = json::array();
            for (const json& e : ordered)
                plugs.push_back(e["plug"]);
            json out = {{"rig", rigPath},         {"skeletalMesh", mesh}, {"expressions", plugs}, {"skipped", skipped},
                        {"controls", rig.controls}, {"curves", payload["curves"]}, {"warnings", rig.warnings}};
            if (a.get<bool>("dryRun"))
            {
                out["payload"] = payload;
                return out;
            }
            if (ordered.empty())
                throw CommandError("unrealExpressionRig: no expressions under " + g.node(root)->name() + " reach Unreal" +
                                   (skipped.empty() ? "" : " (" + skipped[0]["plug"].get<std::string>() + ": " + skipped[0]["why"].get<std::string>() + ")"));

            std::shared_ptr<RpcClient> client;
            try
            {
                Link::get().ensureConnected();
                client = Link::get().client();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("unrealExpressionRig: not connected to Unreal: ") + e.what());
            }
            if (!client)
                throw CommandError("unrealExpressionRig: not connected to Unreal");
            if (const json features = Link::get().status().value("features", json::array()); std::ranges::find(features, json("expressionRig")) == features.end())
                throw CommandError("unrealExpressionRig: the LilaLink plugin in Unreal predates expression rigs; rebuild it from Lila's ue/LilaLink");
            // Reloaded so a LilaLink update takes effect without restarting the editor.
            const std::string code = "import importlib, lila_link_expressions\nimportlib.reload(lila_link_expressions)\n"
                                     "lila_link_expressions.build_and_print(" +
                                     json(payload.dump()).dump() + ")\n";
            RpcResult r = client->call("runPython", {{"code", code}}, CallOptions{.timeoutMs = 180000});
            if (!r.ok)
                throw CommandError("unrealExpressionRig: " + r.error);
            json result;
            std::string log;
            for (const json& line : r.result.value("log", json::array()))
            {
                const std::string text = line.value("text", "");
                if (text.rfind("LILA_RESULT:", 0) == 0)
                    result = json::parse(text.substr(12), nullptr, false);
                else
                    log += text + "\n";
            }
            if (!result.is_object())
                throw CommandError("unrealExpressionRig: Unreal didn't build the rig" + (log.empty() ? std::string() : ":\n" + log));
            if (const json errors = result.value("errors", json::array()); !errors.empty())
                throw CommandError("unrealExpressionRig: " + errors[0].get<std::string>() + (log.empty() ? std::string() : "\n" + log));
            out["unreal"] = result;
            if (a.get<bool>("postProcess"))
            {
                RpcResult pp = client->call("expressionPostProcess", {{"skeletalMesh", mesh}, {"rig", result.value("rig", rigPath)}}, 120000);
                if (!pp.ok)
                    throw CommandError("unrealExpressionRig: the rig is built, but " + pp.error);
                out["animBlueprint"] = pp.result.value("animBlueprint", json());
                for (const json& w : pp.result.value("warnings", json::array()))
                    out["warnings"].push_back(w);
            }
            return out;
        },
    });
}
} // namespace lila::link
