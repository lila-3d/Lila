#include "scene/FullBodyIk.h"

#include "core/Profile.h"
#include "scene/AutoPose.h"
#include "scene/Ik.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/RigUtil.h"
#include "scene/TransformOps.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <Eigen/Dense>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lila::scene::fbik
{
using namespace rigutil;

namespace
{
/** The largest turn a joint takes in one iteration, radians. */
constexpr double kMaxStep = 0.5;

/** How much more a joint resists than a free one: 1 at stiffness 0, growing without bound toward 1. */
double resistance(double stiffness)
{
    const double s = std::clamp(stiffness, 0.0, 0.99);
    return 1.0 + s / (1.0 - s);
}

/** The rotation vector (axis times angle) of q, the short way round. */
glm::dvec3 rotationVector(glm::dquat q)
{
    if (q.w < 0.0)
        q = -q;
    const glm::dvec3 v(q.x, q.y, q.z);
    const double s = glm::length(v);
    if (s < 1e-12)
        return v * 2.0;
    return v * (2.0 * std::atan2(s, q.w) / s);
}

glm::dquat fromRotationVector(const glm::dvec3& v)
{
    const double angle = glm::length(v);
    if (angle < 1e-12)
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    return glm::angleAxis(angle, v / angle);
}

glm::dquat rotationOf(const glm::dmat4& m) { return glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(m)))); }

/** World frames for one iteration: each bone's matrix, the origin it turns about and the frame R turns in. */
struct Frames
{
    std::vector<glm::dmat4> world;
    std::vector<glm::dvec3> pivot;
    std::vector<glm::dmat3> frame;
    std::vector<glm::dmat3> parent;
};

Frames framesOf(const Problem& p, const std::vector<glm::dquat>& rotation, const std::vector<glm::dvec3>& offset)
{
    const size_t n = p.bones.size();
    Frames f{std::vector<glm::dmat4>(n), std::vector<glm::dvec3>(n), std::vector<glm::dmat3>(n), std::vector<glm::dmat3>(n)};
    for (size_t i = 0; i < n; ++i)
    {
        const Bone& b = p.bones[i];
        const glm::dmat4& parent = b.parent < 0 ? b.parentWorld : f.world[size_t(b.parent)];
        const glm::dmat4 base = parent * glm::translate(glm::dmat4(1.0), offset[i]) * b.before;
        f.world[i] = base * glm::dmat4(glm::mat3_cast(rotation[i])) * b.after;
        f.pivot[i] = glm::dvec3(base[3]);
        f.frame[i] = orthonormal(glm::dmat3(base));
        f.parent[i] = glm::dmat3(parent);
    }
    return f;
}

/**
 * How much of the skeleton each bone carries (the length of everything below it), as a multiple of the average, plus a
 * floor so end joints still keep their pose. Turning a spine joint moves far more than turning a wrist, so it resists
 * more.
 */
std::vector<double> carriedWeights(const Problem& p)
{
    const size_t n = p.bones.size();
    std::vector<double> carried(n, 0.0);
    for (size_t i = n; i-- > 0;)
        if (int parent = p.bones[i].parent; parent >= 0)
            carried[size_t(parent)] += carried[i] + glm::length(glm::dvec3(p.bones[i].before[3]));
    double sum = 0.0;
    for (double& c : carried)
        sum += c += 0.25 * p.lengthScale;
    const double mean = n ? sum / double(n) : 1.0;
    for (double& c : carried)
        c /= mean;
    return carried;
}

glm::dmat3 skew(const glm::dvec3& v) { return {0.0, v.z, -v.y, -v.z, 0.0, v.x, v.y, -v.x, 0.0}; }

void put(Eigen::MatrixXd& m, Eigen::Index row, Eigen::Index col, const glm::dmat3& b)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m(row + r, col + c) = b[c][r];
}
} // namespace

std::vector<glm::dmat4> forward(const Problem& p, const std::vector<glm::dquat>& rotation, const std::vector<glm::dvec3>& offset)
{
    return framesOf(p, rotation, offset).world;
}

Solution solve(const Problem& p)
{
    LILA_ZONE();

    const size_t n = p.bones.size();
    Solution out{std::vector<glm::dquat>(n), std::vector<glm::dvec3>(n, glm::dvec3(0.0))};
    for (size_t i = 0; i < n; ++i)
    {
        out.rotation[i] = p.bones[i].rotation;
        out.offset[i] = p.bones[i].offset;
    }

    // Columns: three per turning bone, three more per moving bone.
    std::vector<Eigen::Index> rotCol(n, -1), moveCol(n, -1);
    Eigen::Index cols = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (p.bones[i].solve)
            rotCol[i] = std::exchange(cols, cols + 3);
        if (p.bones[i].translate)
            moveCol[i] = std::exchange(cols, cols + 3);
    }
    std::vector<std::vector<size_t>> chains;
    Eigen::Index rows = 0;
    for (const Effector& e : p.effectors)
    {
        if (e.bone < 0 || size_t(e.bone) >= n)
            throw std::invalid_argument("fbik: effector bone out of range");
        std::vector<size_t> chain;
        for (int b = e.bone; b >= 0; b = p.bones[size_t(b)].parent)
            chain.push_back(size_t(b));
        chains.push_back(std::move(chain));
        rows += (e.position ? 3 : 0) + (e.rotation ? 3 : 0);
    }

    auto pinError = [&](const Frames& f) {
        double worst = 0.0;
        for (const Effector& e : p.effectors)
            if (e.position && !e.soft)
                worst = std::max(worst, glm::length(*e.position - glm::dvec3(f.world[size_t(e.bone)][3])));
        return worst;
    };

    Frames f = framesOf(p, out.rotation, out.offset);
    out.error = pinError(f);
    if (cols == 0 || rows == 0)
        return out;

    const double L = std::max(p.lengthScale, 1e-6);
    const std::vector<double> carried = carriedWeights(p);
    auto pullOf = [&](size_t i, bool pulling) {
        const Bone& b = p.bones[i];
        return pulling ? p.pull * b.pull * resistance(b.stiffness) * carried[i] : 0.0;
    };
    // The reference residual is the joint's turn away from its reference, seen in the frame R turns in.
    auto turnAway = [&](const Frames& fr, const std::vector<glm::dquat>& rot, size_t i) {
        return fr.frame[i] * rotationVector(rot[i] * glm::conjugate(p.bones[i].reference));
    };
    auto moveAway = [&](const Frames& fr, const std::vector<glm::dvec3>& off, size_t i) {
        return fr.parent[i] * (off[i] - p.bones[i].offset) / L;
    };
    auto cost = [&](const Frames& fr, const std::vector<glm::dquat>& rot, const std::vector<glm::dvec3>& off, bool pulling) {
        double c = 0.0;
        for (const Effector& e : p.effectors)
        {
            if (e.soft && !pulling)
                continue;
            const glm::dmat4& w = fr.world[size_t(e.bone)];
            if (e.position)
                c += glm::length2((*e.position - glm::dvec3(w[3])) * (e.weight / L));
            if (e.rotation)
                c += glm::length2(rotationVector(*e.rotation * glm::conjugate(rotationOf(w))) * e.weight);
        }
        if (pulling)
            for (size_t i = 0; i < n; ++i)
            {
                const double pull = pullOf(i, true);
                if (rotCol[i] >= 0)
                    c += pull * pull * glm::length2(turnAway(fr, rot, i));
                if (moveCol[i] >= 0)
                    c += pull * pull * glm::length2(moveAway(fr, off, i));
            }
        return c;
    };

    Eigen::MatrixXd J(rows, cols), H(cols, cols);
    Eigen::VectorXd r(rows), g(cols);
    std::vector<glm::dquat> trialRot(n);
    std::vector<glm::dvec3> trialOff(n);
    // Levenberg-Marquardt: the damping grows while steps overshoot and shrinks while they land.
    double mu = 1e-3;
    bool pulling = p.iterations > 0;
    double current = cost(f, out.rotation, out.offset, pulling);
    const int total = std::max(p.iterations, 0) + std::max(p.finishIterations, 0);
    for (int it = 0; it < total; ++it)
    {
        if (pulling && it >= p.iterations)
        {
            pulling = false;
            mu = 1e-3;
            current = cost(f, out.rotation, out.offset, false);
        }
        if (!pulling && out.error <= p.tolerance)
            break;

        J.setZero();
        r.setZero();
        Eigen::Index row = 0;
        for (size_t k = 0; k < p.effectors.size(); ++k)
        {
            const Effector& e = p.effectors[k];
            if (e.soft && !pulling)
            {
                row += (e.position ? 3 : 0) + (e.rotation ? 3 : 0);
                continue;
            }
            const glm::dmat4& w = f.world[size_t(e.bone)];
            if (e.position)
            {
                const glm::dvec3 at(w[3]);
                const glm::dvec3 err = (*e.position - at) * (e.weight / L);
                r.segment<3>(row) << err.x, err.y, err.z;
                for (size_t b : chains[k])
                {
                    if (rotCol[b] >= 0)
                        put(J, row, rotCol[b], -skew(at - f.pivot[b]) * (e.weight / L));
                    if (moveCol[b] >= 0)
                        put(J, row, moveCol[b], glm::dmat3(e.weight));
                }
                row += 3;
            }
            if (e.rotation)
            {
                const glm::dvec3 err = rotationVector(*e.rotation * glm::conjugate(rotationOf(w))) * e.weight;
                r.segment<3>(row) << err.x, err.y, err.z;
                for (size_t b : chains[k])
                    if (rotCol[b] >= 0)
                        put(J, row, rotCol[b], glm::dmat3(e.weight));
                row += 3;
            }
        }

        H.noalias() = J.transpose() * J;
        g.noalias() = J.transpose() * r;
        for (size_t i = 0; i < n; ++i)
        {
            const double pull = pullOf(i, pulling);
            const double damp = mu * resistance(p.bones[i].stiffness) * carried[i];
            for (auto [c, d] : {std::pair{rotCol[i], rotCol[i] >= 0 ? turnAway(f, out.rotation, i) : glm::dvec3(0.0)},
                                std::pair{moveCol[i], moveCol[i] >= 0 ? moveAway(f, out.offset, i) : glm::dvec3(0.0)}})
                if (c >= 0)
                    for (int a = 0; a < 3; ++a)
                    {
                        H(c + a, c + a) += pull * pull + damp;
                        g(c + a) -= pull * pull * d[a];
                    }
        }

        const Eigen::VectorXd x = H.ldlt().solve(g);
        if (!x.allFinite())
            break;
        for (size_t i = 0; i < n; ++i)
        {
            trialRot[i] = out.rotation[i];
            trialOff[i] = out.offset[i];
            if (rotCol[i] >= 0)
            {
                glm::dvec3 w(x(rotCol[i]), x(rotCol[i] + 1), x(rotCol[i] + 2));
                if (double a = glm::length(w); a > kMaxStep)
                    w *= kMaxStep / a;
                trialRot[i] = glm::normalize(fromRotationVector(glm::transpose(f.frame[i]) * w) * out.rotation[i]);
                if (p.bones[i].limit.kind != ik::JointLimit::Kind::None)
                    trialRot[i] = glm::normalize(ik::clampRotate(p.bones[i].limit, trialRot[i]));
            }
            if (moveCol[i] >= 0)
                trialOff[i] += glm::inverse(f.parent[i]) * (glm::dvec3(x(moveCol[i]), x(moveCol[i] + 1), x(moveCol[i] + 2)) * L);
        }
        Frames trial = framesOf(p, trialRot, trialOff);
        const double next = cost(trial, trialRot, trialOff, pulling);
        out.iterations = it + 1;
        if (next < current)
        {
            const bool settled = current - next < 1e-12 * std::max(current, 1e-30);
            std::swap(out.rotation, trialRot);
            std::swap(out.offset, trialOff);
            f = std::move(trial);
            out.error = pinError(f);
            current = next;
            mu = std::max(mu / 3.0, 1e-9);
            if (settled && pulling)
                it = std::max(it, p.iterations - 1);
        }
        else if ((mu *= 4.0) > 1e8)
        {
            if (!pulling)
                break;
            it = std::max(it, p.iterations - 1);
        }
    }
    return out;
}

// Scene skeletons -------------------------------------------------------------------------------------------------

namespace
{
/** Whether something besides an IK handle owns the joint's rotation, like a twist driver. */
bool drivenElsewhere(Graph& g, NodeId j)
{
    const auto src = g.source({j, Joint::kSolvedRotate, -1});
    return src && !g.node(src->node)->type().isA("ikHandle");
}

/**
 * The hips: the joint named pelvis or hips (UE, Mixamo, HumanIK), else the highest joint where the skeleton branches
 * into two or more solved joints. The name comes first because a root above the pelvis branches too, into UE's ik_
 * helpers.
 */
int hipsOf(Graph& g, const Problem& p, const std::vector<NodeId>& index)
{
    for (size_t i = 0; i < index.size(); ++i)
    {
        std::string name = g.node(index[i])->name();
        name = name.substr(name.find_last_of(":|") + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (p.bones[i].solve && (name == "pelvis" || name == "hips" || name.ends_with("_hips") || name.ends_with("_pelvis")))
            return int(i);
    }
    std::vector<int> children(p.bones.size(), 0);
    for (const Bone& b : p.bones)
        if (b.parent >= 0 && b.solve)
            ++children[size_t(b.parent)];
    for (size_t i = 0; i < p.bones.size(); ++i)
        if (children[i] >= 2)
            return int(i);
    return p.bones.empty() ? -1 : 0;
}
} // namespace

Problem problemFor(Graph& g, NodeId root, const PoseOptions& opts, std::vector<NodeId>& index)
{
    Node* top = g.node(root);
    if (!top || !rig::isJoint(*top))
        throw std::invalid_argument("fullBodyIk: the root must be a joint");
    g.evaluate();

    Problem p;
    index.clear();
    // Depth first from the root, parents before children; helper joints that do not deform are left out with
    // everything under them.
    std::vector<std::pair<NodeId, int>> stack{{root, -1}};
    while (!stack.empty())
    {
        auto [id, parent] = stack.back();
        stack.pop_back();
        const Node& j = *g.node(id);
        Bone b;
        b.parent = parent;
        if (parent < 0)
            b.parentWorld = j.as<glm::dmat4>(Transform::kParentMatrix);
        Transform::splitLocal(j, b.before, b.after);
        b.rotation = b.reference = glm::normalize(glm::quat_cast(glm::dmat3(static_cast<const Transform&>(j).rotation())));
        b.solve = !drivenElsewhere(g, id);
        b.limit = ik::limitOf(g, id);
        b.stiffness = b.limit.stiffness;
        const int self = int(p.bones.size());
        p.bones.push_back(b);
        index.push_back(id);
        const auto kids = g.children(id);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            if (const Node* c = g.node(*it); c && rig::isJoint(*c) && rigel::isDeform(*c))
                stack.push_back({*it, self});
    }

    for (const auto& [joint, s] : opts.stiffness)
        if (auto it = std::find(index.begin(), index.end(), joint); it != index.end())
            p.bones[size_t(it - index.begin())].stiffness = s;
    if (opts.moveRoot)
        if (int hips = hipsOf(g, p, index); hips >= 0)
            p.bones[size_t(hips)].translate = true;

    // A limb's length, roughly: a fifth of the skeleton's extent.
    glm::dvec3 lo(1e300), hi(-1e300);
    for (NodeId id : index)
    {
        const glm::dvec3 at = positionOf(*g.node(id));
        lo = glm::min(lo, at);
        hi = glm::max(hi, at);
    }
    p.lengthScale = std::max(glm::length(hi - lo) * 0.2, 1.0);

    for (const Pin& pin : opts.pins)
    {
        auto it = std::find(index.begin(), index.end(), pin.joint);
        if (it == index.end())
            throw std::invalid_argument("fullBodyIk: " + (g.node(pin.joint) ? g.node(pin.joint)->path() : std::string("a pin")) +
                                        " is not a deform joint under " + top->path());
        p.effectors.push_back({.bone = int(it - index.begin()), .position = pin.position, .rotation = pin.rotation, .weight = pin.weight});
    }
    return p;
}

namespace
{
/** How strongly the auto-poser's suggestion pulls, against a pin's 1. */
constexpr double kSuggestionWeight = 0.5;

std::string bareName(const std::string& name) { return name.substr(name.rfind(':') + 1); }

/**
 * Soft effectors toward where the auto-poser puts the points nobody pinned. Needs every point's joint, matched by name
 * (namespaces ignored); returns false without them or the model.
 */
bool addSuggestions(Graph& g, Problem& p, const std::vector<NodeId>& index)
{
    const autopose::Model* model = autopose::shipped();
    if (!model)
        return false;
    std::array<int, autopose::kPointCount> bone;
    bone.fill(-1);
    for (size_t i = 0; i < index.size(); ++i)
        for (int k = 0; k < autopose::kPointCount; ++k)
            if (bareName(g.node(index[i])->name()) == autopose::kPoints[size_t(k)])
                bone[size_t(k)] = int(i);
    autopose::Points current;
    autopose::Pins pins;
    for (int k = 0; k < autopose::kPointCount; ++k)
    {
        if (bone[size_t(k)] < 0)
            return false;
        current[size_t(k)] = positionOf(*g.node(index[size_t(bone[size_t(k)])]));
        for (const Effector& e : p.effectors)
            if (e.bone == bone[size_t(k)] && e.position && !e.soft)
                pins[size_t(k)] = e.position;
    }
    const autopose::Points want = model->predict(current, pins);
    for (int k = 0; k < autopose::kPointCount; ++k)
        if (!pins[size_t(k)])
            p.effectors.push_back({.bone = bone[size_t(k)], .position = want[size_t(k)], .weight = kSuggestionWeight, .soft = true});
    return true;
}
} // namespace

double pose(Graph& g, NodeId root, const PoseOptions& opts)
{
    LILA_ZONE();

    std::vector<NodeId> index;
    Problem p = problemFor(g, root, opts, index);
    if (opts.autoPose)
        addSuggestions(g, p, index);
    const Solution s = solve(p);
    std::vector<std::string> plugs;
    for (size_t i = 0; i < index.size(); ++i)
    {
        const Bone& b = p.bones[i];
        Node& j = *g.node(index[i]);
        if (b.solve)
        {
            const auto order = RotateOrder(j.as<int64_t>(Transform::kRotateOrder));
            const glm::dvec3 now = j.as<glm::dvec3>(Transform::kRotate);
            const glm::dvec3 rotate = xformops::closestEuler(glm::mat3_cast(s.rotation[i]), order, now);
            if (glm::length(rotate - now) > 1e-9)
            {
                g.setValue({index[i], Transform::kRotate}, clean(rotate));
                plugs.push_back(j.path() + ".rotate");
            }
        }
        if (b.translate && glm::length(s.offset[i]) > 1e-9)
        {
            g.setValue({index[i], Transform::kTranslate}, j.as<glm::dvec3>(Transform::kTranslate) + s.offset[i]);
            plugs.push_back(j.path() + ".translate");
        }
    }
    if (opts.key && !plugs.empty())
        ops::execute("setKeyframe", {{"args", plugs}});
    return s.error;
}

// Command ---------------------------------------------------------------------------------------------------------

namespace
{
NodeId topJoint(Graph& g, NodeId j)
{
    while (g.parent(j) != kNoNode && rig::isJoint(*g.node(g.parent(j))))
        j = g.parent(j);
    return j;
}

NodeId jointId(const std::string& name)
{
    Node& n = nodeNamed(name, "fullBodyIk");
    if (!rig::isJoint(n))
        throw CommandError("fullBodyIk: " + name + " is not a joint");
    return n.id();
}

glm::dquat worldEuler(const json& j)
{
    return glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(vec3Of(j), RotateOrder::XYZ))));
}

json run(const Args& a)
{
    LILA_ZONE();

    Graph& g = graph();
    g.evaluate();
    PoseOptions opts;
    opts.moveRoot = a.get<bool>("moveRoot", true);
    opts.key = a.get<bool>("key", false);
    opts.autoPose = a.get<bool>("autoPose", false);
    if (a.has("pins"))
        for (const json& pin : a.flags["pins"])
        {
            if (!pin.is_object() || !pin.contains("joint"))
                throw CommandError("fullBodyIk: each pin is {\"joint\": name, \"position\": [x, y, z], \"rotation\": [x, y, z]}");
            Pin p{.joint = jointId(pin["joint"].get<std::string>())};
            if (pin.contains("position"))
                p.position = vec3Of(pin["position"]);
            if (pin.contains("rotation"))
                p.rotation = worldEuler(pin["rotation"]);
            p.weight = pin.value("weight", 1.0);
            opts.pins.push_back(p);
        }
    if (a.has("hold"))
        for (const json& name : a.flags["hold"])
        {
            const NodeId id = jointId(name.get<std::string>());
            const glm::dmat4 w = worldOf(*g.node(id));
            opts.pins.push_back({.joint = id, .position = glm::dvec3(w[3]), .rotation = rotationOf(w)});
        }
    if (opts.pins.empty())
        throw CommandError("fullBodyIk: give pins or hold");
    if (a.has("stiffness"))
        for (const auto& [name, value] : a.flags["stiffness"].items())
            opts.stiffness.push_back({jointId(name), value.get<double>()});

    const NodeId root = a.has("root") ? jointId(a.get<std::string>("root")) : topJoint(g, opts.pins.front().joint);
    const double error = guarded([&] { return pose(g, root, opts); });
    return {{"error", error}};
}
} // namespace

void registerCommands()
{
    if (ops::find("fullBodyIk"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "fullBodyIk",
        .doc = "Pose a whole skeleton so each pinned joint reaches its target. The motion spreads over the joints between "
               "the pins and the hips by stiffness; everything else keeps its pose. Writes the joints' rotate (and the "
               "hips' translate) as FK. Returns {'error': the largest pin miss in cm}.",
        .flags = {
            {"pins", "p", T::List, nullptr,
             "[{'joint': name, 'position': [x, y, z], 'rotation': [x, y, z], 'weight': 1.0}], world space; rotation is "
             "XYZ Euler degrees. position and rotation are each optional."},
            {"hold", "h", T::List, nullptr, "Joints pinned where they are now (position and rotation)."},
            {"root", "r", T::String, nullptr, "Top joint of the skeleton to solve; default the first pin's top joint."},
            {"stiffness", "st", T::Any, nullptr, "{joint: 0..1}: how much each joint resists turning, so its neighbours take the motion."},
            {"moveRoot", "mr", T::Bool, true, "Let the hips move when turning joints is not enough."},
            {"key", "k", T::Bool, false, "Key the posed joints."},
            {"autoPose", "ap", T::Bool, false,
             "Let the auto-poser place the unpinned body, as a person would stand to reach the pins (UE5 Manny joint names)."},
        },
        .run = run,
    });
}
} // namespace lila::scene::fbik
