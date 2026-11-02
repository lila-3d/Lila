#include "scene/UnrealCheck.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace lila::scene::uecheck
{
UeTransform fromLocal(const glm::dmat4& local, RotateOrder order)
{
    const rigel::ExportTrs trs = rigel::exportTrs(local, order, glm::dvec3(0.0));
    return {.translation = trs.translate,
            .rotation = glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(trs.rotate, order)))),
            .scale = trs.scale};
}

UeTransform compose(const UeTransform& child, const UeTransform& parent)
{
    return {.translation = parent.rotation * (parent.scale * child.translation) + parent.translation,
            .rotation = glm::normalize(parent.rotation * child.rotation),
            .scale = child.scale * parent.scale};
}

glm::dmat4 toMatrix(const UeTransform& t)
{
    glm::dmat4 m(glm::mat3_cast(t.rotation) * glm::dmat3(glm::scale(glm::dmat4(1.0), t.scale)));
    m[3] = glm::dvec4(t.translation, 1.0);
    return m;
}

namespace
{
bool uniform(const glm::dvec3& s)
{
    const glm::dvec3 a = glm::abs(s);
    const double hi = std::max({a.x, a.y, a.z}), lo = std::min({a.x, a.y, a.z});
    return hi - lo <= 1e-6 * std::max(hi, 1e-12);
}

/** One skeleton as exported: its deform joints, parents first, each with its nearest deform ancestor. */
struct Skeleton
{
    std::vector<NodeId> joints;
    std::vector<int> parent;
};

std::vector<Skeleton> skeletonsOf(Graph& g, const std::vector<NodeId>& roots)
{
    std::vector<NodeId> tops;
    auto addTop = [&](NodeId j) {
        while (g.parent(j) && rig::isJoint(*g.node(g.parent(j))))
            j = g.parent(j);
        if (std::find(tops.begin(), tops.end(), j) == tops.end())
            tops.push_back(j);
    };
    if (roots.empty())
    {
        for (NodeId id : g.nodes())
            if (rig::isJoint(*g.node(id)))
                addTop(id);
    }
    else
        for (NodeId r : roots)
            addTop(r);

    std::vector<Skeleton> out;
    for (NodeId top : tops)
    {
        Skeleton s;
        std::map<NodeId, int> index;
        std::vector<NodeId> stack{top};
        while (!stack.empty())
        {
            const NodeId id = stack.back();
            stack.pop_back();
            if (!rig::isJoint(*g.node(id)))
                continue;
            if (rigel::isDeform(*g.node(id)))
            {
                NodeId up = g.parent(id);
                while (up && !index.contains(up))
                    up = g.parent(up);
                index[id] = int(s.joints.size());
                s.joints.push_back(id);
                s.parent.push_back(up ? index[up] : -1);
            }
            const std::vector<NodeId>& kids = g.children(id);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                stack.push_back(*it);
        }
        if (!s.joints.empty())
            out.push_back(std::move(s));
    }
    return out;
}

/** A skin's bind data with each influence's joint in the skeleton (-1 when it has none there). */
struct SkinData
{
    NodeId node = kNoNode;
    MeshPtr input;
    WeightTable weights;
    glm::dmat4 geom{1.0}, geomInv{1.0};
    /** Indexed by influence id. */
    std::vector<glm::dmat4> bindPre;
    std::vector<NodeId> joint;
    std::vector<int> inSkeleton;
    bool linearOut = false;
};

std::vector<SkinData> skinsFor(Graph& g, const Skeleton& s, const std::string& suffix)
{
    std::map<std::string, int> byName;
    for (size_t i = 0; i < s.joints.size(); ++i)
        byName[g.node(s.joints[i])->name()] = int(i);
    std::vector<SkinData> out;
    for (NodeId id : g.nodes())
    {
        const Node& n = *g.node(id);
        if (!n.type().isA("skinCluster"))
            continue;
        SkinData d;
        d.node = id;
        bool any = false;
        for (const skin::Influence& inf : skin::influences(g, id))
        {
            if (inf.joint == kNoNode)
                continue;
            if (inf.index >= d.joint.size())
            {
                d.joint.resize(inf.index + 1, kNoNode);
                d.bindPre.resize(inf.index + 1, glm::dmat4(1.0));
                d.inSkeleton.resize(inf.index + 1, -1);
            }
            const std::string name = g.node(inf.joint)->name();
            int at = -1;
            if (auto it = byName.find(name + suffix); it != byName.end())
                at = it->second;
            else if (auto same = byName.find(name); same != byName.end() && s.joints[size_t(same->second)] == inf.joint)
                at = same->second;
            d.joint[inf.index] = inf.joint;
            d.bindPre[inf.index] = g.node(inf.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
            d.inSkeleton[inf.index] = at;
            any = any || at >= 0;
        }
        if (!any)
            continue;
        d.input = skin::inputMesh(g, id);
        if (!d.input)
            continue;
        d.weights = skin::table(g, id);
        d.geom = n.as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
        d.geomInv = glm::inverse(d.geom);
        d.linearOut = n.as<bool>(SkinClusterNode::kEnabled) && n.as<double>(SkinClusterNode::kEnvelope) == 1.0 &&
                      n.as<int64_t>(SkinClusterNode::kSkinningMethod) == SkinClusterNode::kLinear;
        out.push_back(std::move(d));
    }
    return out;
}

/** The nearest deform ancestor scaling non-uniformly under Unreal's rules. */
NodeId scaledAncestor(const Skeleton& s, const std::vector<UeTransform>& ue, size_t i)
{
    for (int up = s.parent[i]; up >= 0; up = s.parent[size_t(up)])
        if (!uniform(ue[size_t(up)].scale))
            return s.joints[size_t(up)];
    return kNoNode;
}
} // namespace

Report check(Graph& g, const Options& opts)
{
    LILA_ZONE();

    Report r;
    const double fps = opts.fps > 0.0 ? opts.fps : 30.0;
    const long long frames = std::max<long long>(0, std::llround((opts.end - opts.start) * fps));
    r.frames = int(frames + 1);
    std::map<NodeId, Mismatch> worst;
    auto note = [&](NodeId joint, double error, double time, NodeId scaled) {
        r.worst = std::max(r.worst, error);
        if (error <= opts.tolerance)
            return;
        Mismatch& m = worst[joint];
        if (error > m.error)
            m = {.joint = joint, .error = error, .time = time, .scaledAncestor = scaled};
    };

    const double was = g.time();
    for (const Skeleton& s : skeletonsOf(g, opts.roots))
    {
        r.joints += s.joints.size();
        std::vector<SkinData> skins = skinsFor(g, s, opts.suffix);
        r.skins += skins.size();
        std::vector<char> skinned(s.joints.size(), 0);
        for (const SkinData& d : skins)
            for (int at : d.inSkeleton)
                if (at >= 0)
                    skinned[size_t(at)] = 1;

        for (long long f = 0; f <= frames; ++f)
        {
            const double t = opts.start + double(f) / fps;
            g.setTime(t);
            g.evaluate();
            std::vector<glm::dmat4> lila(s.joints.size()), unreal(s.joints.size());
            std::vector<UeTransform> ue(s.joints.size());
            for (size_t i = 0; i < s.joints.size(); ++i)
            {
                const Node& j = *g.node(s.joints[i]);
                lila[i] = j.as<glm::dmat4>(Transform::kWorldMatrix);
                const int up = s.parent[i];
                const glm::dmat4 local = up < 0 ? lila[i] : glm::inverse(lila[size_t(up)]) * lila[i];
                ue[i] = fromLocal(local, RotateOrder(j.as<int64_t>(Transform::kRotateOrder)));
                if (up >= 0)
                    ue[i] = compose(ue[i], ue[size_t(up)]);
                unreal[i] = toMatrix(ue[i]);
            }

            // Joints nothing is skinned to: their axes out to the bone's length.
            for (size_t i = 0; i < s.joints.size(); ++i)
            {
                if (skinned[i])
                    continue;
                const Node& j = *g.node(s.joints[i]);
                double reach = std::max(j.as<double>(Joint::kRadius), 1.0);
                for (NodeId c : g.children(s.joints[i]))
                    if (rig::isJoint(*g.node(c)))
                        reach = std::max(reach, glm::length(glm::dvec3(g.node(c)->as<glm::dmat4>(Transform::kWorldMatrix)[3] - lila[i][3])));
                double e = glm::length(glm::dvec3(unreal[i][3] - lila[i][3]));
                for (int k = 0; k < 3; ++k)
                {
                    const glm::dvec3 axis = glm::dvec3(lila[i][k]);
                    const double unit = glm::length(axis) > 1e-12 ? reach / glm::length(axis) : 0.0;
                    e = std::max(e, glm::length(glm::dvec3(unreal[i][k] - lila[i][k]) * unit + glm::dvec3(unreal[i][3] - lila[i][3])));
                }
                note(s.joints[i], e, t, scaledAncestor(s, ue, i));
            }

            for (SkinData& d : skins)
            {
                const size_t count = d.joint.size();
                std::vector<glm::dmat4> palLila(count), palUe(count);
                for (size_t k = 0; k < count; ++k)
                {
                    if (d.joint[k] == kNoNode)
                        continue;
                    const int at = d.inSkeleton[k];
                    const glm::dmat4 w = at >= 0 ? lila[size_t(at)] : g.node(d.joint[k])->as<glm::dmat4>(Transform::kWorldMatrix);
                    palLila[k] = d.geomInv * w * d.bindPre[k] * d.geom;
                    palUe[k] = at >= 0 ? d.geomInv * unreal[size_t(at)] * d.bindPre[k] * d.geom : palLila[k];
                }
                MeshPtr out = d.linearOut ? g.node(d.node)->as<MeshPtr>(SkinClusterNode::kOutMesh) : nullptr;
                const auto in = d.input->positions();
                const size_t n = std::min(in.size(), d.weights.points.size());
                for (size_t p = 0; p < n; ++p)
                {
                    const glm::dvec4 x(in[p], 1.0);
                    glm::dvec3 viaLila(0.0), viaUe(0.0);
                    double blameError = -1.0;
                    int blame = -1;
                    for (const auto& [k, w] : d.weights.points[p])
                    {
                        if (k >= count || d.joint[k] == kNoNode)
                            continue;
                        const glm::dvec3 a(palLila[k] * x), b(palUe[k] * x);
                        viaLila += double(w) * a;
                        viaUe += double(w) * b;
                        if (const double e = double(w) * glm::length(b - a); d.inSkeleton[k] >= 0 && e > blameError)
                            blameError = e, blame = d.inSkeleton[k];
                    }
                    const glm::dvec3 lilaPoint = out && p < out->positions().size() ? out->positions()[p] : viaLila;
                    if (blame >= 0)
                        note(s.joints[size_t(blame)], glm::length(viaUe - lilaPoint), t, scaledAncestor(s, ue, size_t(blame)));
                }
            }
        }
    }
    g.setTime(was);
    g.evaluate();

    for (const auto& [joint, m] : worst)
        r.mismatches.push_back(m);
    std::sort(r.mismatches.begin(), r.mismatches.end(), [](const Mismatch& a, const Mismatch& b) { return a.error > b.error; });
    return r;
}

void reportIssues(Graph& g, const Report& r)
{
    const double fps = anim::playback().fps > 0.0 ? anim::playback().fps : 30.0;
    for (const Mismatch& m : r.mismatches)
    {
        NodeId top = m.joint;
        while (g.parent(top) && rig::isJoint(*g.node(g.parent(top))))
            top = g.parent(top);
        diagnostics::Issue i;
        i.source = kValidator;
        i.severity = diagnostics::Severity::Error;
        i.target = g.node(m.joint)->path();
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.3g cm at frame %g", m.error, std::round(m.time * fps));
        i.message = std::string("Unreal can't match this joint: its skin is off by up to ") + buf;
        if (m.scaledAncestor != kNoNode)
            i.message += ". It inherits the non-uniform scale of " + g.node(m.scaledAncestor)->path() +
                         " under a rotation, a shear Unreal has no way to hold; bake with scaleSafe, or keep that scale on joints with no deform children";
        i.blocks = {"exportFbx", "exportGltf", "unrealSync"};
        i.fix = "bakeToDeformSkeleton(\"" + g.node(top)->path() + "\", scaleSafe=True)";
        diagnostics::report(std::move(i));
    }
}

namespace
{
Options optionsFrom(Graph& g, const Args& a)
{
    const anim::Playback& pb = anim::playback();
    Options o{.start = a.get<double>("start", pb.minTime), .end = a.get<double>("end", pb.maxTime), .fps = a.get<double>("fps", pb.fps),
              .tolerance = a.get<double>("tolerance", 0.01), .suffix = a.get<std::string>("suffix", "_bake")};
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n || !rig::isJoint(*n))
            throw CommandError("unrealCheck: " + name + " is not a joint");
        o.roots.push_back(n->id());
    }
    if (o.end < o.start)
        throw CommandError("unrealCheck: end is before start");
    return o;
}
} // namespace

void registerCommands()
{
    if (ops::find("unrealCheck"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "unrealCheck",
        .doc = "Evaluate skeletons (default all) as they export, under Unreal's transform rules (scale handed down per axis, "
               "no shear), and compare their skins with Lila's deformed meshes every frame of the range. Each joint past the "
               "tolerance becomes an error that blocks exporting until the check passes. Returns the worst drift and the "
               "mismatched joints.",
        .positionalDoc = "a joint of each skeleton",
        .flags = {
            {"start", "st", T::Float, nullptr, "Seconds; default the playback start."},
            {"end", "et", T::Float, nullptr, "Seconds; default the playback end."},
            {"fps", "fps", T::Float, nullptr, "Frames per second; default the scene's.", {}, {}, 1.0},
            {"tolerance", "tol", T::Float, 0.01, "cm a skinned point may drift.", {}, {}, 0.0},
            {"suffix", "sfx", T::String, "_bake", "A baked copy's name suffix, to find the skin of its original joints."},
            {"report", "rp", T::Bool, true, "Replace the check's issues with this run's."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = rigutil::graph();
            const Report r = check(g, optionsFrom(g, a));
            if (a.get<bool>("report", true))
            {
                diagnostics::clear(kValidator);
                reportIssues(g, r);
            }
            json joints = json::array();
            for (const Mismatch& m : r.mismatches)
                joints.push_back({{"joint", g.node(m.joint)->path()},
                                  {"error", m.error},
                                  {"time", m.time},
                                  {"scaledAncestor", m.scaledAncestor != kNoNode ? g.node(m.scaledAncestor)->path() : std::string()}});
            return {{"worst", r.worst}, {"frames", r.frames}, {"joints", r.joints}, {"skins", r.skins}, {"mismatches", joints}};
        },
    });
    diagnostics::validators().put(kValidator,
                                  {"Skeletons evaluated under Unreal's transform rules match Lila's deformed meshes over the playback range.",
                                   [] {
                                       Graph& g = Graph::current();
                                       const anim::Playback& pb = anim::playback();
                                       reportIssues(g, check(g, {.start = pb.minTime, .end = pb.maxTime, .fps = pb.fps}));
                                   }},
                                  "lila");
}
} // namespace lila::scene::uecheck
