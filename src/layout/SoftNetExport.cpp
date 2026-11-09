#include "layout/SoftNetBind.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Node.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"
#include "scene/UnrealCheck.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <Eigen/Dense>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;

struct Range
{
    std::vector<double> times;
    double fps = 30.0;
};

Range rangeOf(const Args& a)
{
    const anim::Playback& pb = anim::playback();
    Range r;
    r.fps = std::max(pb.fps, 1e-6);
    const int64_t start = a.has("start") ? a.get<int>("start") : int64_t(std::llround(pb.minTime * r.fps));
    const int64_t end = a.has("end") ? a.get<int>("end") : int64_t(std::llround(pb.maxTime * r.fps));
    for (int64_t f = start; f <= end; f += std::max(1, a.get<int>("step", 1)))
        r.times.push_back(double(f) / r.fps);
    if (r.times.empty())
        r.times.push_back(Graph::current().time());
    return r;
}

std::vector<glm::dvec3> outPoints(Graph& g, NodeId mesh)
{
    const MeshPtr m = g.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    return m ? std::vector<glm::dvec3>(m->positions().begin(), m->positions().end()) : std::vector<glm::dvec3>{};
}

void writeLocal(Graph& g, NodeId j, const glm::dmat4& local, std::optional<double> time, glm::dvec3& near)
{
    near = eulerNear(orthonormal(glm::dmat3(local)), RotateOrder(g.node(j)->as<int64_t>(Transform::kRotateOrder)), near);
    if (!time)
    {
        g.setValue({j, Transform::kTranslate, -1}, glm::dvec3(local[3]));
        g.setValue({j, Transform::kRotate, -1}, near);
        return;
    }
    for (int c = 0; c < 3; ++c)
        anim::setKey(g, {j, Transform::kTranslate, c}, *time, local[3][c]);
    anim::setRotationKey(g, {j, Transform::kRotate, -1}, *time, near);
}

/** The net's knots and handles as joints Unreal can carry, with each one's world matrix while the controls rest. */
struct NetJoints
{
    std::vector<NodeId> joints;
    std::vector<glm::dmat4> restWorld;
};

/**
 * Joints for a net, keyed over times: per knot a frame joint riding the surface (its frame) with a control joint under
 * it (the knot control), per tangent handle a joint under its knot's control. Frame joints go under the skin influence
 * that weighs most at the knot. Joints made earlier for the net are replaced.
 */
NetJoints makeJoints(Graph& g, NodeId hub, NodeId skin, const std::vector<double>& times)
{
    LILA_ZONE();
    const Node& h = *g.node(hub);
    const NetBind bind = NetBind::fromJson(h.as<std::string>(h.type().attrIndex("binding")));
    const glm::dmat4 meshWorld = h.as<glm::dmat4>(h.type().attrIndex("meshWorld"));
    const NodeType* knotType = types::find(kSoftNetKnotType);
    std::vector<NodeId> knots(bind.knots.size(), kNoNode);
    for (NodeId c : g.children(hub))
        if (const Node* n = g.node(c); n && n->type().name == kSoftNetKnotType)
            if (const int64_t i = n->as<int64_t>(knotType->attrIndex("index")); i >= 0 && size_t(i) < knots.size())
                knots[size_t(i)] = c;

    // The influence weighing most at each knot's surface point carries its frame joint.
    std::vector<NodeId> parentOf(knots.size(), kNoNode);
    if (skin != kNoNode)
    {
        const WeightTable t = skin::table(g, skin);
        std::map<uint32_t, NodeId> jointOf;
        for (const skin::Influence& i : skin::influences(g, skin))
            jointOf[i.index] = i.joint;
        for (size_t k = 0; k < knots.size(); ++k)
        {
            const glm::dvec3& w = bind.knots[k].at.bary;
            const int corner = w.x >= w.y && w.x >= w.z ? 0 : w.y >= w.z ? 1 : 2;
            const uint32_t p = bind.knots[k].at.points[size_t(corner)];
            float best = -1.0f;
            if (p < t.points.size())
                for (const auto& [inf, wt] : t.points[p])
                    if (wt > best && jointOf.contains(inf))
                    {
                        best = wt;
                        parentOf[k] = jointOf[inf];
                    }
        }
    }

    // What each joint follows: a frame joint the knot's frame (its parentMatrix), a control joint the knot's local
    // matrix, a handle joint the handle's rest offset times its local matrix.
    enum class Kind
    {
        Frame,
        Control,
        Handle
    };
    struct Made
    {
        NodeId joint;
        NodeId source;
        Kind kind;
    };
    std::vector<Made> made;
    NetJoints out;
    const NodeType& jointType = *types::find("joint");
    auto replace = [&](const std::string& name) {
        if (Node* old = g.find(name); old && rig::isJoint(*old))
            g.deleteNode(old->id());
    };
    for (size_t k = 0; k < knots.size(); ++k)
    {
        if (knots[k] == kNoNode)
            continue;
        const std::string name = g.node(knots[k])->name();
        replace(name + "_frame");
        replace(name + "_jnt");
        Node* frame = g.createNode(jointType, name + "_frame", parentOf[k]);
        Node* control = g.createNode(jointType, name + "_jnt", frame->id());
        glm::dmat4 rest(bind.knots[k].rest);
        rest[3] = glm::dvec4(bind.knots[k].origin, 1.0);
        rest = meshWorld * rest;
        made.push_back({frame->id(), knots[k], Kind::Frame});
        out.restWorld.push_back(rest);
        made.push_back({control->id(), knots[k], Kind::Control});
        out.restWorld.push_back(rest);
        for (NodeId t : std::vector<NodeId>(g.children(knots[k])))
            if (const Node* tn = g.node(t); tn && tn->type().name == kSoftNetTangentType)
            {
                replace(tn->name() + "_jnt");
                made.push_back({g.createNode(jointType, tn->name() + "_jnt", control->id())->id(), t, Kind::Handle});
                out.restWorld.push_back(rest * tn->as<glm::dmat4>(Transform::kOffsetParentMatrix));
            }
    }

    std::vector<std::vector<glm::dmat4>> local(made.size());
    const double saved = g.time();
    for (double t : times)
    {
        g.setTime(t);
        g.evaluate();
        for (size_t i = 0; i < made.size(); ++i)
        {
            const Node& src = *g.node(made[i].source);
            switch (made[i].kind)
            {
            case Kind::Frame:
            {
                const NodeId p = g.parent(made[i].joint);
                local[i].push_back(glm::inverse(p != kNoNode ? worldOf(*g.node(p)) : glm::dmat4(1.0)) * src.as<glm::dmat4>(Transform::kParentMatrix));
                break;
            }
            case Kind::Control: local[i].push_back(src.as<glm::dmat4>(Transform::kMatrix)); break;
            case Kind::Handle: local[i].push_back(src.as<glm::dmat4>(Transform::kOffsetParentMatrix) * src.as<glm::dmat4>(Transform::kMatrix)); break;
            }
        }
    }
    g.setTime(saved);
    g.evaluate();
    for (size_t i = 0; i < made.size(); ++i)
    {
        bool animated = false;
        for (size_t f = 1; f < local[i].size() && !animated; ++f)
            for (int c = 0; c < 4; ++c)
                animated = animated || glm::length(local[i][f][c] - local[i][0][c]) > 1e-9;
        glm::dvec3 near(0.0);
        for (size_t f = 0; f < times.size(); ++f)
        {
            writeLocal(g, made[i].joint, local[i][f], animated ? std::optional<double>(times[f]) : std::nullopt, near);
            if (!animated)
                break;
        }
        out.joints.push_back(made[i].joint);
    }
    return out;
}

/** Each point's linear-skinning matrix (rest to posed, object space) from the skin's weights and influence matrices. */
std::vector<glm::dmat3> blendedLinear(Graph& g, NodeId skin, size_t points)
{
    const std::vector<glm::dmat4> m = skin::objectMatrices(g, skin);
    const WeightTable t = skin::table(g, skin);
    std::vector<glm::dmat3> out(points, glm::dmat3(1.0));
    for (size_t p = 0; p < points && p < t.points.size(); ++p)
    {
        glm::dmat3 b(0.0);
        double sum = 0.0;
        for (const auto& [i, w] : t.points[p])
            if (i < m.size())
            {
                b += double(w) * glm::dmat3(m[i]);
                sum += w;
            }
        if (sum > 0.0)
            out[p] = b;
    }
    return out;
}

struct Errors
{
    double rms = 0.0, max = 0.0;
};

Errors errorsAgainst(Graph& g, NodeId mesh, const std::vector<double>& times, const std::vector<std::vector<glm::dvec3>>& targets, std::vector<double>* perFrame = nullptr)
{
    Errors e;
    double sum = 0.0;
    size_t n = 0;
    const double saved = g.time();
    for (size_t f = 0; f < times.size(); ++f)
    {
        g.setTime(times[f]);
        g.evaluate();
        const std::vector<glm::dvec3> pts = outPoints(g, mesh);
        double frameMax = 0.0;
        for (size_t p = 0; p < pts.size() && p < targets[f].size(); ++p)
        {
            const double d = glm::length(pts[p] - targets[f][p]);
            sum += d * d;
            ++n;
            frameMax = std::max(frameMax, d);
        }
        e.max = std::max(e.max, frameMax);
        if (perFrame)
            perFrame->push_back(frameMax);
    }
    g.setTime(saved);
    g.evaluate();
    e.rms = n ? std::sqrt(sum / double(n)) : 0.0;
    return e;
}

/**
 * Morph correctives for what linear skinning leaves: the residual per frame taken back through the skin into rest
 * space, its principal components as morph targets (up to cap, until tolerance), their weights keyed per frame.
 */
json bakeCorrectives(Graph& g, NodeId mesh, NodeId skin, const std::vector<double>& times, const std::vector<std::vector<glm::dvec3>>& targets, int cap,
                     double tolerance)
{
    LILA_ZONE();
    if (cap <= 0 || times.empty())
        return json::array();
    const size_t frames = times.size();
    const size_t points = targets.front().size();
    Eigen::MatrixXd r(Eigen::Index(frames), Eigen::Index(points * 3));
    const double saved = g.time();
    for (size_t f = 0; f < frames; ++f)
    {
        g.setTime(times[f]);
        g.evaluate();
        const std::vector<glm::dvec3> pts = outPoints(g, mesh);
        const std::vector<glm::dmat3> b = blendedLinear(g, skin, points);
        for (size_t p = 0; p < points; ++p)
        {
            const glm::dvec3 miss = p < pts.size() ? targets[f][p] - pts[p] : glm::dvec3(0.0);
            const double det = glm::determinant(b[p]);
            const glm::dvec3 rest = std::abs(det) > 1e-12 ? glm::inverse(b[p]) * miss : miss;
            for (int c = 0; c < 3; ++c)
                r(Eigen::Index(f), Eigen::Index(p * 3 + size_t(c))) = rest[c];
        }
    }
    g.setTime(saved);
    g.evaluate();
    const Eigen::MatrixXd gram = r * r.transpose();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(gram);
    const Eigen::VectorXd values = eig.eigenvalues();
    const Eigen::MatrixXd vectors = eig.eigenvectors();
    double remaining = values.sum();
    const double total = double(frames * points);
    NodeId morph = morphs::find(g, mesh);
    if (morph == kNoNode)
        morph = morphs::create(g, mesh, "SoftNetCorrectives");
    json made = json::array();
    for (Eigen::Index k = values.size() - 1; k >= 0 && int(made.size()) < cap; --k)
    {
        if (values[k] <= 1e-12 || std::sqrt(std::max(remaining, 0.0) / total) <= tolerance)
            break;
        const Eigen::VectorXd v = vectors.col(k);
        Eigen::VectorXd shape = r.transpose() * v; // = sigma * unit shape
        Eigen::VectorXd weights = v;              // per frame
        // Scaled so the largest keyed weight is 1.
        const double peak = weights.cwiseAbs().maxCoeff();
        if (!(peak > 0.0))
            break;
        shape *= peak;
        weights /= peak;
        auto deltas = std::make_shared<MorphDeltas>();
        for (size_t p = 0; p < points; ++p)
        {
            const glm::dvec3 d(shape[Eigen::Index(p * 3)], shape[Eigen::Index(p * 3 + 1)], shape[Eigen::Index(p * 3 + 2)]);
            if (glm::length(d) > 1e-7)
            {
                deltas->indices.push_back(uint32_t(p));
                deltas->deltas.push_back(d);
            }
        }
        const std::string name = "softNetFix" + std::to_string(made.size() + 1);
        const NodeId target = morphs::addTarget(g, morph, name, deltas, 0.0);
        for (size_t f = 0; f < frames; ++f)
            anim::setKey(g, {target, morphs::kWeight, -1}, times[f], weights[Eigen::Index(f)]);
        made.push_back(g.node(target)->path());
        remaining -= values[k];
    }
    return made;
}

NodeId meshArg(Graph& g, const Args& a, const char* cmd)
{
    if (a.objects().empty())
        throw CommandError(std::string(cmd) + ": give a mesh with a Soft Net");
    Node& n = nodeNamed(a.objects().front(), cmd);
    NodeId mesh = n.id();
    if (!n.type().isA("mesh"))
    {
        const NodeId hub = netOf(g, n.id());
        const std::vector<NodeId> ms = hub != kNoNode ? netMeshes(g, hub) : std::vector<NodeId>{};
        if (ms.empty())
            throw CommandError(std::string(cmd) + ": " + n.path() + " is not a mesh with a Soft Net");
        mesh = ms.front();
    }
    if (netOf(g, mesh) == kNoNode)
        throw CommandError(std::string(cmd) + ": " + g.node(mesh)->path() + " has no Soft Net");
    return mesh;
}

NodeId deformerOn(Graph& g, NodeId mesh)
{
    for (NodeId d : modifiers::stack(g, mesh))
        if (g.node(d)->type().name == kSoftNetType)
            return d;
    return kNoNode;
}

json bake(Graph& g, NodeId mesh, const Args& a)
{
    LILA_ZONE();
    const NodeId skin = skin::find(g, mesh);
    if (skin == kNoNode)
        throw CommandError("bakeSoftNet: " + g.node(mesh)->path() + " has no skin to bake into; skin it first, or send it to Unreal as a geometry cache");
    const NodeId hub = netOf(g, mesh);
    const Range range = rangeOf(a);

    // What the stack makes now, the net included, is what the bake must reach.
    std::vector<std::vector<glm::dvec3>> targets;
    const double saved = g.time();
    for (double t : range.times)
    {
        g.setTime(t);
        g.evaluate();
        targets.push_back(outPoints(g, mesh));
    }
    g.setTime(saved);
    g.evaluate();

    // Knot joints as fixed bones: added to the skin with their rest as bind.
    const NetJoints joints = makeJoints(g, hub, skin, range.times);
    const std::vector<NodeId>& made = joints.joints;
    for (size_t i = 0; i < made.size(); ++i)
    {
        skin::addInfluence(g, skin, made[i]);
        for (const skin::Influence& inf : skin::influences(g, skin))
            if (inf.joint == made[i])
                g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, glm::inverse(joints.restWorld[i]));
    }

    json fit = ops::execute("bakeSkinWeights", {{"args", {g.node(mesh)->path()}},
                                                {"source", "animation"},
                                                {"start", int(std::llround(range.times.front() * range.fps))},
                                                {"end", int(std::llround(range.times.back() * range.fps))},
                                                {"step", std::max(1, a.get<int>("step", 1))},
                                                {"helpers", std::max(0, a.get<int>("helpers"))},
                                                {"maxInfluences", a.get<int>("maxInfluences")},
                                                {"iterations", a.get<int>("iterations")}});
    const Errors skinned = errorsAgainst(g, mesh, range.times, targets);
    const json correctives = bakeCorrectives(g, mesh, skin, range.times, targets, a.get<int>("correctives"), a.get<double>("tolerance"));
    std::vector<double> perFrame;
    const Errors final = errorsAgainst(g, mesh, range.times, targets, &perFrame);

    // U1: the skeleton under Unreal's transform rules over the baked frames.
    const uecheck::ExportCheck c = uecheck::checkExport(g, {mesh}, false, range.times.front(), range.times.back(), range.fps, "bakeSoftNet");
    json mismatches = json::array();
    for (const uecheck::Mismatch& m : c.report.mismatches)
        mismatches.push_back({{"joint", g.node(m.joint)->path()}, {"error", m.error}, {"time", m.time}});
    json names = json::array();
    for (NodeId j : made)
        names.push_back(g.node(j)->path());
    log::write(LogLevel::Info, std::format("bakeSoftNet: {} joints, {} correctives, {:.4f} cm RMS ({:.4f} cm skinned only), worst {:.4f} cm", made.size(),
                                           correctives.size(), final.rms, skinned.rms, final.max));
    return {{"joints", names},
            {"helpers", fit.value("helpers", json::array())},
            {"correctives", correctives},
            {"skinnedRmse", skinned.rms},
            {"rmse", final.rms},
            {"maxError", final.max},
            {"frameError", perFrame},
            {"frames", range.times.size()},
            {"disabled", fit.value("disabled", json::array())},
            {"unrealCheck", {{"worst", c.report.worst}, {"frames", c.report.frames}, {"mismatches", mismatches}, {"error", c.error}}}};
}
} // namespace

void registerSoftNetExportCommands()
{
    using T = ArgType;
    const std::vector<FlagDef> rangeFlags = {
        {"start", "", T::Int, nullptr, "First frame (the playback start by default)."},
        {"end", "", T::Int, nullptr, "Last frame (the playback end by default)."},
        {"step", "", T::Int, 1, "Frames between samples.", {}, {}, 1.0},
    };
    std::vector<FlagDef> bakeFlags = rangeFlags;
    bakeFlags.insert(bakeFlags.end(), {
        {"helpers", "", T::Int, 4, "Helper joints solved with the weights for what knots and skin can't follow (capped per character).", {}, {}, 0.0, 16.0},
        {"correctives", "", T::Int, 8, "Most morph correctives for the residual.", {}, {}, 0.0, 64.0},
        {"tolerance", "", T::Float, 0.001, "cm of RMS residual at which correctives stop.", {}, {}, 0.0},
        {"maxInfluences", "mi", T::Int, 8, "Influences per point.", {}, {}, 1.0, 12.0},
        {"iterations", "", T::Int, 3, "Fit passes.", {}, {}, 1.0},
    });
    ops::registerCommand({
        .name = "bakeSoftNet",
        .doc = "Bake a skinned mesh's Soft Net for Unreal: each knot becomes a frame joint riding the surface with a control joint "
               "under it, each tangent handle a joint under its knot, keyed over the range and added to the skin as fixed bones; "
               "weights are fitted to the net's result (with up to helpers helper joints), and morph correctives keyed per frame "
               "take the residual. The net is turned off so the viewport shows what Unreal gets. Returns {joints, helpers, "
               "correctives, skinnedRmse, rmse, maxError, frameError, frames, unrealCheck}: errors in cm against the net, per frame "
               "worst, and the Unreal round-trip check.",
        .positionalDoc = "mesh (or its net)",
        .flags = bakeFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return bake(g, meshArg(g, a, "bakeSoftNet"), a);
        },
        .examples = {"bakeSoftNet(\"Mochi\", helpers=4, correctives=8)"},
    });

    std::vector<FlagDef> routeFlags = bakeFlags;
    routeFlags.insert(routeFlags.begin(), {"route", "", T::String, "bake", "How the net reaches Unreal.", {"bake", "live", "cache"}});
    routeFlags.push_back({"cacheFormat", "", T::String, "alembic", "cache: Alembic or vertex animation (VAT).", {"alembic", "vat"}});
    routeFlags.push_back({"path", "", T::String, nullptr, "cache: the file to write; default beside the scene."});
    ops::registerCommand({
        .name = "softNetUnreal",
        .doc = "Send a Soft Net to Unreal one of three ways. bake (default): knot joints, fitted weights and morph correctives on "
               "the skin (bakeSoftNet), so any Unreal export carries it. live: the net runs in Unreal as a Deformer Graph over "
               "the skin, matching Lila (unrealSoftNet; knot joints are baked to drive it). cache: the deformed mesh as an Alembic "
               "or vertex animation cache, exact but fixed, for cinematics. Returns that route's result.",
        .positionalDoc = "mesh (or its net)",
        .flags = routeFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const NodeId mesh = meshArg(g, a, "softNetUnreal");
            const std::string route = a.get<std::string>("route", "bake");
            if (route == "bake")
                return bake(g, mesh, a);
            if (route == "live")
            {
                json call = a.flags;
                call["args"] = {g.node(mesh)->path()};
                for (const char* drop : {"route", "cacheFormat", "path", "helpers", "correctives", "tolerance", "maxInfluences", "iterations"})
                    call.erase(drop);
                return ops::execute("unrealSoftNet", call);
            }
            const Range range = rangeOf(a);
            json call = {{"args", {g.node(mesh)->path()}}, {"start", range.times.front() * range.fps}, {"end", range.times.back() * range.fps}};
            if (a.has("path"))
                call["path"] = a.get<std::string>("path");
            return ops::execute(a.get<std::string>("cacheFormat", "alembic") == "vat" ? "exportVertexAnimation" : "exportAlembic", call);
        },
        .examples = {"softNetUnreal(\"Mochi\")", "softNetUnreal(\"Mochi\", route=\"cache\", cacheFormat=\"alembic\")"},
    });
    ops::options::setOverride("softNetUnreal", {.editors = {{"route", "segmented"}, {"cacheFormat", "segmented"}}});
}
} // namespace lila::layout
