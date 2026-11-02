#include "retarget/Retarget.h"

#include "scene/FullBodyIk.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace lila::retarget
{
namespace
{
glm::dquat eulerQuat(const glm::dvec3& degrees) { return glm::dquat(glm::radians(degrees)); }
double smooth01(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
glm::dvec3 flatten(const glm::dvec3& v, const glm::dvec3& up) { return v - glm::dot(v, up) * up; }

/** Fraction of the chain's length at each joint; by index when the chain has no length. */
std::vector<double> paramsOf(const std::vector<int>& joints, const std::vector<glm::dmat4>& world)
{
    std::vector<double> t(joints.size(), 0.0);
    double total = 0.0;
    for (size_t i = 1; i < joints.size(); ++i)
    {
        total += glm::length(positionOf(world[size_t(joints[i])]) - positionOf(world[size_t(joints[i - 1])]));
        t[i] = total;
    }
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = total > 1e-9 ? t[i] / total : joints.size() > 1 ? double(i) / double(joints.size() - 1) : 0.0;
    return t;
}

/** Index k and blend f so the fraction t lies between joints k and k + 1. */
std::pair<size_t, double> segmentAt(const std::vector<double>& params, double t)
{
    if (params.size() < 2 || t <= params.front())
        return {0, 0.0};
    for (size_t k = 0; k + 1 < params.size(); ++k)
        if (t <= params[k + 1])
        {
            const double span = params[k + 1] - params[k];
            return {k, span > 1e-12 ? (t - params[k]) / span : 1.0};
        }
    return {params.size() - 1, 0.0};
}

glm::dvec3 pointAt(const std::vector<int>& joints, const std::vector<double>& params, const std::vector<glm::dmat4>& world, double t)
{
    const auto [k, f] = segmentAt(params, t);
    const glm::dvec3 a = positionOf(world[size_t(joints[k])]);
    if (k + 1 >= joints.size())
        return a;
    return glm::mix(a, positionOf(world[size_t(joints[k + 1])]), f);
}

/** Rotation taking direction a onto b by the shortest arc. */
glm::dquat arc(glm::dvec3 a, glm::dvec3 b)
{
    if (glm::length(a) < 1e-12 || glm::length(b) < 1e-12)
        return {1.0, 0.0, 0.0, 0.0};
    a = glm::normalize(a);
    b = glm::normalize(b);
    const double c = glm::dot(a, b);
    if (c < -1.0 + 1e-9)
    {
        glm::dvec3 axis = glm::cross(a, glm::dvec3(1, 0, 0));
        if (glm::length(axis) < 1e-6)
            axis = glm::cross(a, glm::dvec3(0, 1, 0));
        return glm::angleAxis(glm::pi<double>(), glm::normalize(axis));
    }
    return glm::rotation(a, b);
}

/** The direction a chain bends: the middle joint's offset from the start-to-end line, unit or zero. */
glm::dvec3 bendOf(const std::vector<int>& joints, const std::vector<glm::dmat4>& world, glm::dvec3& axis)
{
    const glm::dvec3 a = positionOf(world[size_t(joints.front())]);
    const glm::dvec3 b = positionOf(world[size_t(joints.back())]);
    const glm::dvec3 m = positionOf(world[size_t(joints[joints.size() / 2])]);
    axis = glm::length(b - a) > 1e-9 ? glm::normalize(b - a) : glm::dvec3(0.0);
    glm::dvec3 off = m - a;
    off -= glm::dot(off, axis) * axis;
    return glm::length(off) > 1e-9 ? glm::normalize(off) : glm::dvec3(0.0);
}

/** Signed angle about axis from a to b (both roughly perpendicular to axis). */
double angleAbout(const glm::dvec3& a, glm::dvec3 b, const glm::dvec3& axis)
{
    b -= glm::dot(b, axis) * axis;
    if (glm::length(a) < 1e-9 || glm::length(b) < 1e-9)
        return 0.0;
    b = glm::normalize(b);
    return std::atan2(glm::dot(glm::cross(a, b), axis), glm::dot(a, b));
}

struct Mapped
{
    const ChainMap* map = nullptr;
    const Chain* sourceChain = nullptr;
    const Chain* targetChain = nullptr;
    std::vector<int> src, tgt;
    std::vector<double> srcParam, tgtParam;
    int srcGoal = -1, tgtGoal = -1; // joint indices
    std::vector<int> ikJoints;      // target joints from the chain start to the goal
    double srcLen0 = 0.0;
};

/** What the FK pass asks of each target joint; assemble turns it into a pose. */
struct Wants
{
    std::vector<std::optional<glm::dquat>> rotation; // world
    std::vector<std::optional<glm::dvec3>> position; // world
    std::vector<std::optional<glm::dvec3>> offset;   // world offset from the parent's origin
    std::vector<double> stretch;                     // local translate multiplier
    std::vector<double> translationAlpha;

    explicit Wants(size_t n) : rotation(n), position(n), offset(n), stretch(n, 1.0), translationAlpha(n, 1.0) {}
};

struct Goal
{
    size_t chain = 0; // index into Impl::mapped
    glm::dvec3 position{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 source{0.0}; // the source goal, world
};

struct Stage
{
    Pose pose;
    std::vector<glm::dmat4> world;
    std::vector<Goal> goals;
};
} // namespace

double solveChainIk(const Skeleton& s, Pose& pose, std::vector<glm::dmat4>& world, const std::vector<int>& joints, const glm::dvec3& position,
                    const glm::dquat& rotation)
{
    if (joints.size() < 2)
        return 0.0;
    scene::fbik::Problem p;
    for (size_t k = 0; k < joints.size(); ++k)
    {
        const size_t j = size_t(joints[k]);
        scene::fbik::Bone b;
        b.parent = int(k) - 1;
        if (k == 0)
            b.parentWorld = parentWorldOf(s, world, int(j));
        b.before = glm::translate(glm::dmat4(1.0), pose.translate[j]) * s.pre[j];
        b.after = s.after[j];
        b.rotation = b.reference = pose.rotation[j];
        p.bones.push_back(b);
    }
    p.lengthScale = std::max(chainLength(s, joints, world), 1.0);
    p.effectors.push_back({.bone = int(joints.size()) - 1, .position = position, .rotation = rotation});
    const scene::fbik::Solution sol = scene::fbik::solve(p);
    for (size_t k = 0; k < joints.size(); ++k)
        pose.rotation[size_t(joints[k])] = sol.rotation[k];
    world = worldOf(s, pose);
    return sol.error;
}

// Settings ---------------------------------------------------------------------------------------------------------

ChainMap* Settings::chain(std::string_view target)
{
    for (ChainMap& c : chains)
        if (c.target == target)
            return &c;
    return nullptr;
}

const ChainMap* Settings::chain(std::string_view target) const
{
    for (const ChainMap& c : chains)
        if (c.target == target)
            return &c;
    return nullptr;
}

namespace
{
constexpr const char* kFkRotation[] = {"interpolated", "oneToOne", "oneToOneReversed", "matchChain", "none"};
constexpr const char* kFkTranslation[] = {"none", "globallyScaled", "absolute", "stretchUniform", "stretchNonUniform"};

template <size_t N> int enumIndex(const char* const (&names)[N], const std::string& value, const char* what)
{
    for (size_t i = 0; i < N; ++i)
        if (value == names[i])
            return int(i);
    throw std::invalid_argument(std::string("unknown ") + what + " '" + value + "'");
}

json v3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 v3(const json& j, const glm::dvec3& fallback)
{
    if (j.is_null())
        return fallback;
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("expected [x, y, z], got " + j.dump());
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}
json at(const json& j, const char* key) { return j.is_object() && j.contains(key) ? j[key] : json(); }
template <class T> T get(const json& j, const char* key, T fallback) { return j.is_object() && j.contains(key) ? j[key].get<T>() : fallback; }
} // namespace

json Settings::toJson() const
{
    json cs = json::array();
    for (const ChainMap& c : chains)
        cs.push_back({{"target", c.target},
                      {"source", c.source},
                      {"fk",
                       {{"enabled", c.fk.enabled},
                        {"rotation", kFkRotation[size_t(c.fk.rotation)]},
                        {"rotationAlpha", c.fk.rotationAlpha},
                        {"translation", kFkTranslation[size_t(c.fk.translation)]},
                        {"translationAlpha", c.fk.translationAlpha},
                        {"poleMatching", c.fk.poleMatching},
                        {"poleMaintainOffset", c.fk.poleMaintainOffset},
                        {"poleOffset", c.fk.poleOffset}}},
                      {"ik",
                       {{"enabled", c.ik.enabled},
                        {"blendToSource", c.ik.blendToSource},
                        {"blendToSourceWeights", v3(c.ik.blendToSourceWeights)},
                        {"staticOffset", v3(c.ik.staticOffset)},
                        {"staticLocalOffset", v3(c.ik.staticLocalOffset)},
                        {"staticRotationOffset", v3(c.ik.staticRotationOffset)},
                        {"scaleVertical", c.ik.scaleVertical},
                        {"extension", c.ik.extension},
                        {"affectedByWarping", c.ik.affectedByWarping}}},
                      {"plant",
                       {{"enabled", c.plant.enabled},
                        {"speedThreshold", c.plant.speedThreshold},
                        {"heightTolerance", c.plant.heightTolerance},
                        {"blendFrames", c.plant.blendFrames},
                        {"minFrames", c.plant.minFrames}}}});
    json pinsJ = json::array();
    for (const PinBone& p : pins)
        pinsJ.push_back({{"bone", p.bone}, {"to", p.to}, {"translate", p.translate}, {"rotate", p.rotate}, {"maintainOffset", p.maintainOffset}});
    json contactsJ = json::array();
    for (const Contact& c : contacts)
        contactsJ.push_back({{"effector", c.effector}, {"body", c.body}, {"bodyParam", c.bodyParam}, {"distance", c.distance}});
    json curvesJ = json::array();
    for (const CurveMap& c : curves)
        curvesJ.push_back({{"source", c.source}, {"target", c.target}});
    return {{"format", "lilaRetargeter"},
            {"version", 1},
            {"enableRoot", enableRoot},
            {"enableFk", enableFk},
            {"enableIk", enableIk},
            {"scale", scale},
            {"fullBody", fullBody},
            {"copyCurves", copyCurves},
            {"root",
             {{"enabled", root.enabled},
              {"rotationAlpha", root.rotationAlpha},
              {"translationAlpha", root.translationAlpha},
              {"blendToSource", root.blendToSource},
              {"blendToSourceWeights", v3(root.blendToSourceWeights)},
              {"scaleHorizontal", root.scaleHorizontal},
              {"scaleVertical", root.scaleVertical},
              {"translationOffset", v3(root.translationOffset)},
              {"rotationOffset", v3(root.rotationOffset)},
              {"affectIkHorizontal", root.affectIkHorizontal},
              {"affectIkVertical", root.affectIkVertical}}},
            {"warp",
             {{"enabled", warp.enabled}, {"useVelocity", warp.useVelocity}, {"warpForwards", warp.warpForwards}, {"sidewaysOffset", warp.sidewaysOffset}}},
            {"chains", cs},
            {"pins", pinsJ},
            {"contacts", contactsJ},
            {"curves", curvesJ}};
}

Settings Settings::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("retarget settings are a JSON object");
    Settings s;
    s.enableRoot = get(j, "enableRoot", true);
    s.enableFk = get(j, "enableFk", true);
    s.enableIk = get(j, "enableIk", true);
    s.scale = get(j, "scale", 0.0);
    s.copyCurves = get(j, "copyCurves", true);
    s.fullBody = get(j, "fullBody", true);
    const json r = at(j, "root");
    s.root.enabled = get(r, "enabled", true);
    s.root.rotationAlpha = get(r, "rotationAlpha", 1.0);
    s.root.translationAlpha = get(r, "translationAlpha", 1.0);
    s.root.blendToSource = get(r, "blendToSource", 0.0);
    s.root.blendToSourceWeights = v3(at(r, "blendToSourceWeights"), glm::dvec3(1.0));
    s.root.scaleHorizontal = get(r, "scaleHorizontal", 1.0);
    s.root.scaleVertical = get(r, "scaleVertical", 1.0);
    s.root.translationOffset = v3(at(r, "translationOffset"), glm::dvec3(0.0));
    s.root.rotationOffset = v3(at(r, "rotationOffset"), glm::dvec3(0.0));
    s.root.affectIkHorizontal = get(r, "affectIkHorizontal", true);
    s.root.affectIkVertical = get(r, "affectIkVertical", false);
    const json w = at(j, "warp");
    s.warp.enabled = get(w, "enabled", false);
    s.warp.useVelocity = get(w, "useVelocity", true);
    s.warp.warpForwards = get(w, "warpForwards", 1.0);
    s.warp.sidewaysOffset = get(w, "sidewaysOffset", 0.0);
    for (const json& c : at(j, "chains").is_array() ? j["chains"] : json::array())
    {
        ChainMap m;
        m.target = c.at("target").get<std::string>();
        m.source = get(c, "source", std::string());
        const json fk = at(c, "fk"), ik = at(c, "ik"), pl = at(c, "plant");
        m.fk.enabled = get(fk, "enabled", true);
        m.fk.rotation = FkRotation(enumIndex(kFkRotation, get(fk, "rotation", std::string("interpolated")), "FK rotation mode"));
        m.fk.rotationAlpha = get(fk, "rotationAlpha", 1.0);
        m.fk.translation = FkTranslation(enumIndex(kFkTranslation, get(fk, "translation", std::string("none")), "FK translation mode"));
        m.fk.translationAlpha = get(fk, "translationAlpha", 1.0);
        m.fk.poleMatching = get(fk, "poleMatching", 0.0);
        m.fk.poleMaintainOffset = get(fk, "poleMaintainOffset", false);
        m.fk.poleOffset = get(fk, "poleOffset", 0.0);
        m.ik.enabled = get(ik, "enabled", true);
        m.ik.blendToSource = get(ik, "blendToSource", 0.0);
        m.ik.blendToSourceWeights = v3(at(ik, "blendToSourceWeights"), glm::dvec3(1.0));
        m.ik.staticOffset = v3(at(ik, "staticOffset"), glm::dvec3(0.0));
        m.ik.staticLocalOffset = v3(at(ik, "staticLocalOffset"), glm::dvec3(0.0));
        m.ik.staticRotationOffset = v3(at(ik, "staticRotationOffset"), glm::dvec3(0.0));
        m.ik.scaleVertical = get(ik, "scaleVertical", 1.0);
        m.ik.extension = get(ik, "extension", 1.0);
        m.ik.affectedByWarping = get(ik, "affectedByWarping", true);
        m.plant.enabled = get(pl, "enabled", true);
        m.plant.speedThreshold = get(pl, "speedThreshold", 15.0);
        m.plant.heightTolerance = get(pl, "heightTolerance", 4.0);
        m.plant.blendFrames = get(pl, "blendFrames", 3);
        m.plant.minFrames = get(pl, "minFrames", 2);
        s.chains.push_back(std::move(m));
    }
    for (const json& p : at(j, "pins").is_array() ? j["pins"] : json::array())
        s.pins.push_back({p.at("bone").get<std::string>(), p.at("to").get<std::string>(), get(p, "translate", true), get(p, "rotate", true),
                          get(p, "maintainOffset", true)});
    for (const json& c : at(j, "contacts").is_array() ? j["contacts"] : json::array())
        s.contacts.push_back({c.at("effector").get<std::string>(), c.at("body").get<std::string>(), get(c, "bodyParam", 1.0), get(c, "distance", 8.0)});
    for (const json& c : at(j, "curves").is_array() ? j["curves"] : json::array())
        s.curves.push_back({c.at("source").get<std::string>(), c.at("target").get<std::string>()});
    return s;
}

Settings autoMap(const Rig& source, const Rig& target)
{
    Settings s;
    for (const Chain& t : target.chains)
    {
        ChainMap m;
        m.target = t.name;
        const Chain* match = source.chain(t.name);
        if (match && match->role != t.role)
            match = nullptr;
        if (!match)
            match = source.chainFor(t.role, t.side, t.index);
        if (!match)
            for (const Chain& c : source.chains)
                if (c.role == t.role && c.side == t.side)
                {
                    match = &c;
                    break;
                }
        if (match)
            m.source = match->name;
        if (t.role == Role::Root)
            m.fk.translation = FkTranslation::GloballyScaled;
        m.ik.enabled = match && !t.goal.empty() && !match->goal.empty();
        m.plant.enabled = t.role == Role::Leg;
        s.chains.push_back(std::move(m));
    }
    return s;
}

// The retargeter -----------------------------------------------------------------------------------------------------

struct Retargeter::Impl
{
    Skeleton src, tgt;
    Rig srcRig, tgtRig;
    Settings set;
    Pose tgtPose0;
    std::vector<glm::dmat4> S0, T0;
    int sp = -1, tp = -1;
    double scale = 1.0;
    glm::dvec3 up{0.0, 1.0, 0.0};
    std::vector<Mapped> mapped;
    std::vector<std::string> notes;

    /** The source's change of world rotation from its retarget pose, at fraction t of a mapped chain. */
    glm::dquat sourceDelta(const Mapped& m, size_t i, double t, const std::vector<glm::dmat4>& S) const
    {
        auto delta = [&](size_t k) {
            const int j = m.src[k];
            return rotationOf(S[size_t(j)]) * glm::inverse(rotationOf(S0[size_t(j)]));
        };
        const size_t ns = m.src.size(), nt = m.tgt.size();
        switch (m.map->fk.rotation)
        {
        case FkRotation::OneToOne:
            return delta(std::min(i, ns - 1));
        case FkRotation::OneToOneReversed:
            return delta(ns - 1 >= nt - 1 - i ? ns - 1 - (nt - 1 - i) : 0);
        case FkRotation::MatchChain:
        {
            size_t best = 0;
            for (size_t k = 1; k < ns; ++k)
                if (std::abs(m.srcParam[k] - t) < std::abs(m.srcParam[best] - t))
                    best = k;
            return delta(best);
        }
        default:
        {
            const auto [k, f] = segmentAt(m.srcParam, t);
            if (k + 1 >= ns)
                return delta(k);
            return glm::slerp(delta(k), delta(k + 1), f);
        }
        }
    }

    /** The source joint nearest fraction t of a mapped chain. */
    int sourceJointAt(const Mapped& m, double t) const
    {
        size_t best = 0;
        for (size_t k = 1; k < m.src.size(); ++k)
            if (std::abs(m.srcParam[k] - t) < std::abs(m.srcParam[best] - t))
                best = k;
        return m.src[best];
    }

    void assemble(const Wants& w, Pose& pose, std::vector<glm::dmat4>& world) const
    {
        pose = tgtPose0;
        world.assign(tgt.size(), glm::dmat4(1.0));
        for (size_t i = 0; i < tgt.size(); ++i)
        {
            const glm::dmat4 parentW = parentWorldOf(tgt, world, int(i));
            if (w.rotation[i])
                pose.rotation[i] = localRotationFor(tgt, int(i), parentW, *w.rotation[i]);
            const glm::dvec3 rest = tgtPose0.translate[i];
            glm::dvec3 t = rest;
            if (w.position[i])
                t = translateFor(parentW, *w.position[i]);
            else if (w.offset[i])
                t = translateFor(parentW, (tgt.parent[i] < 0 ? glm::dvec3(0.0) : positionOf(parentW)) + *w.offset[i]);
            else if (w.stretch[i] != 1.0)
                t = rest * w.stretch[i];
            pose.translate[i] = glm::mix(rest, t, w.translationAlpha[i]);
            world[i] = parentW * localOf(tgt, pose, int(i));
        }
    }

    /** Root and FK (with pole matching) for one frame, plus where the IK goals go. */
    Stage fk(const std::vector<glm::dmat4>& S, const glm::dvec3& origin) const
    {
        Wants w(tgt.size());
        const glm::dvec3 srcPelvis = positionOf(S[size_t(sp)]);
        // Ground motion counts from `origin` (where the source's pelvis starts the clip), so a clip plays from where
        // the target stands whatever frame the rigs were made at.
        const glm::dvec3 shift = flatten(origin - positionOf(S0[size_t(sp)]), up);
        if (set.enableRoot && set.root.enabled)
        {
            const RootSettings& r = set.root;
            const glm::dquat d = rotationOf(S[size_t(sp)]) * glm::inverse(rotationOf(S0[size_t(sp)]));
            const glm::dquat rest = rotationOf(T0[size_t(tp)]);
            w.rotation[size_t(tp)] = glm::slerp(rest, d * rest, r.rotationAlpha) * eulerQuat(r.rotationOffset);
            const glm::dvec3 moved = srcPelvis - positionOf(S0[size_t(sp)]) - shift;
            const glm::dvec3 vertical = glm::dot(moved, up) * up;
            const glm::dvec3 restPos = positionOf(T0[size_t(tp)]);
            glm::dvec3 p = restPos + (moved - vertical) * scale * r.scaleHorizontal + vertical * scale * r.scaleVertical;
            p = glm::mix(p, srcPelvis, r.blendToSource * r.blendToSourceWeights);
            p += r.translationOffset;
            w.position[size_t(tp)] = glm::mix(restPos, p, r.translationAlpha);
        }
        if (set.enableFk)
            for (const Mapped& m : mapped)
            {
                const FkSettings& f = m.map->fk;
                if (!f.enabled)
                    continue;
                const double srcLen = f.translation == FkTranslation::StretchUniform ? chainLength(src, m.src, S) : 0.0;
                for (size_t k = 0; k < m.tgt.size(); ++k)
                {
                    const size_t ti = size_t(m.tgt[k]);
                    if (int(ti) == tp)
                        continue;
                    const double t = m.tgtParam[k];
                    if (f.rotation != FkRotation::None)
                    {
                        const glm::dquat rest = rotationOf(T0[ti]);
                        w.rotation[ti] = glm::slerp(rest, sourceDelta(m, k, t, S) * rest, f.rotationAlpha);
                    }
                    if (f.translation == FkTranslation::None)
                        continue;
                    const int sj = sourceJointAt(m, t);
                    const int sParent = src.parent[size_t(sj)];
                    switch (f.translation)
                    {
                    case FkTranslation::GloballyScaled:
                    case FkTranslation::Absolute:
                    {
                        const glm::dvec3 off = positionOf(S[size_t(sj)]) - (sParent < 0 ? glm::dvec3(0.0) : positionOf(S[size_t(sParent)]));
                        w.offset[ti] = off * (f.translation == FkTranslation::GloballyScaled ? scale : 1.0);
                        break;
                    }
                    case FkTranslation::StretchUniform:
                        w.stretch[ti] = m.srcLen0 > 1e-9 ? srcLen / m.srcLen0 : 1.0;
                        break;
                    case FkTranslation::StretchNonUniform:
                        if (sParent >= 0)
                        {
                            const double now = glm::length(positionOf(S[size_t(sj)]) - positionOf(S[size_t(sParent)]));
                            const double rest = glm::length(positionOf(S0[size_t(sj)]) - positionOf(S0[size_t(sParent)]));
                            w.stretch[ti] = rest > 1e-9 ? now / rest : 1.0;
                        }
                        break;
                    default:
                        break;
                    }
                    w.translationAlpha[ti] = f.translationAlpha;
                }
            }

        Stage st;
        assemble(w, st.pose, st.world);

        bool poled = false;
        for (const Mapped& m : mapped)
        {
            const FkSettings& f = m.map->fk;
            if (!set.enableFk || !f.enabled || f.poleMatching <= 0.0 || m.src.size() < 3 || m.tgt.size() < 3)
                continue;
            glm::dvec3 sAxis, tAxis;
            const glm::dvec3 sBend = bendOf(m.src, S, sAxis), tBend = bendOf(m.tgt, st.world, tAxis);
            if (glm::length(tAxis) < 0.5)
                continue;
            double angle = angleAbout(tBend, sBend, tAxis);
            if (f.poleMaintainOffset)
            {
                glm::dvec3 a0, b0;
                const glm::dvec3 s0 = bendOf(m.src, S0, a0), t0 = bendOf(m.tgt, T0, b0);
                angle -= angleAbout(t0, s0, b0);
            }
            angle = angle * f.poleMatching + glm::radians(f.poleOffset);
            const glm::dquat turn = glm::angleAxis(angle, tAxis);
            for (int j : m.tgt)
                w.rotation[size_t(j)] = turn * rotationOf(st.world[size_t(j)]);
            poled = true;
        }
        if (poled)
            assemble(w, st.pose, st.world);

        if (set.enableIk)
            for (size_t c = 0; c < mapped.size(); ++c)
            {
                const Mapped& m = mapped[c];
                const IkSettings& ik = m.map->ik;
                if (!ik.enabled || m.srcGoal < 0 || m.tgtGoal < 0 || m.ikJoints.size() < 2)
                    continue;
                const glm::dvec3 srcGoal = positionOf(S[size_t(m.srcGoal)]);
                const glm::dvec3 srcGoal0 = positionOf(S0[size_t(m.srcGoal)]);
                const glm::dvec3 tgtGoal0 = positionOf(T0[size_t(m.tgtGoal)]);
                const glm::dvec3 tgtPelvis = positionOf(st.world[size_t(tp)]);
                // The goal's offset from the pelvis changes as the source's does, measured in the pelvis's frame so it
                // turns with the body; the target keeps its own stance and foot height.
                const glm::dquat turn = rotationOf(S[size_t(sp)]) * glm::inverse(rotationOf(S0[size_t(sp)]));
                const glm::dvec3 change = glm::inverse(turn) * (srcGoal - srcPelvis) - (srcGoal0 - positionOf(S0[size_t(sp)]));
                const glm::dvec3 offset = turn * (tgtGoal0 - positionOf(T0[size_t(tp)]) + change * scale);
                glm::dvec3 horizontal = set.root.affectIkHorizontal ? flatten(tgtPelvis + offset, up) : flatten(tgtGoal0 + (srcGoal - srcGoal0 - shift) * scale, up);
                double height = set.root.affectIkVertical ? glm::dot(tgtPelvis + offset, up)
                                                          : glm::dot(tgtGoal0, up) + glm::dot(srcGoal - srcGoal0, up) * scale * ik.scaleVertical;
                Goal g;
                g.chain = c;
                g.source = srcGoal;
                g.rotation = rotationOf(st.world[size_t(m.tgtGoal)]) * eulerQuat(ik.staticRotationOffset);
                g.position = horizontal + height * up;
                g.position += ik.staticOffset + glm::dvec3(glm::mat3_cast(g.rotation) * ik.staticLocalOffset);
                g.position = glm::mix(g.position, srcGoal, ik.blendToSource * ik.blendToSourceWeights);
                const glm::dvec3 start = positionOf(st.world[size_t(m.ikJoints.front())]);
                g.position = start + (g.position - start) * ik.extension;
                st.goals.push_back(g);
            }
        return st;
    }

    static constexpr double kFullBodyTolerance = 0.5; // cm

    /** Every goal pinned at once over the joints between them and the pelvis, the pelvis free to move. */
    void fullBody(Stage& st) const
    {
        scene::fbik::Problem p;
        std::vector<bool> carries(tgt.size(), false);
        for (const Goal& g : st.goals)
            for (int j = mapped[g.chain].ikJoints.back(); j >= 0; j = tgt.parent[size_t(j)])
                carries[size_t(j)] = true;
        for (size_t i = 0; i < tgt.size(); ++i)
        {
            scene::fbik::Bone b;
            b.parent = tgt.parent[i];
            if (b.parent < 0)
                b.parentWorld = tgt.parentWorld[i];
            b.before = glm::translate(glm::dmat4(1.0), st.pose.translate[i]) * tgt.pre[i];
            b.after = tgt.after[i];
            b.rotation = b.reference = st.pose.rotation[i];
            // Only the joints between the pelvis and the goals turn; above the pelvis (the root) nothing moves.
            b.solve = carries[i] && tgt.isBelow(int(i), tp);
            b.translate = int(i) == tp;
            b.stiffness = int(i) == tp ? 0.6 : 0.0;
            p.bones.push_back(b);
        }
        for (const Mapped& m : mapped)
            if (m.targetChain->role == Role::Spine || m.targetChain->role == Role::Neck)
                for (int j : m.tgt)
                    p.bones[size_t(j)].stiffness = 0.5;
        for (const Goal& g : st.goals)
            p.effectors.push_back({.bone = mapped[g.chain].ikJoints.back(), .position = g.position, .rotation = g.rotation});
        p.lengthScale = std::max(scale * 50.0, 1.0);
        const scene::fbik::Solution sol = scene::fbik::solve(p);
        for (size_t i = 0; i < tgt.size(); ++i)
        {
            if (!p.bones[i].solve)
                continue;
            st.pose.rotation[i] = sol.rotation[i];
            if (p.bones[i].translate)
                st.pose.translate[i] += sol.offset[i];
        }
        st.world = worldOf(tgt, st.pose);
    }

    /**
     * Solves each goal's chain on its own (the rest of the body stays as FK put it), then the pinned bones. errors gets
     * each goal's miss, in goal order.
     */
    double finish(Stage& st, std::vector<double>* errors = nullptr) const
    {
        double worst = 0.0;
        std::vector<double> miss;
        for (const Goal& g : st.goals)
        {
            const double e = solveChainIk(tgt, st.pose, st.world, mapped[g.chain].ikJoints, g.position, g.rotation);
            worst = std::max(worst, e);
            miss.push_back(e);
        }
        if (set.fullBody && worst > kFullBodyTolerance)
        {
            fullBody(st);
            worst = 0.0;
            for (size_t k = 0; k < st.goals.size(); ++k)
            {
                miss[k] = glm::length(positionOf(st.world[size_t(mapped[st.goals[k].chain].ikJoints.back())]) - st.goals[k].position);
                worst = std::max(worst, miss[k]);
            }
        }
        if (errors)
            *errors = miss;
        for (const PinBone& pin : set.pins)
        {
            const int b = tgt.find(pin.bone), to = tgt.find(pin.to);
            if (b < 0 || to < 0)
                continue;
            const glm::dmat4 want = pin.maintainOffset ? st.world[size_t(to)] * glm::inverse(T0[size_t(to)]) * T0[size_t(b)] : st.world[size_t(to)];
            const glm::dmat4 parentW = parentWorldOf(tgt, st.world, b);
            if (pin.rotate)
                st.pose.rotation[size_t(b)] = localRotationFor(tgt, b, parentW, rotationOf(want));
            if (pin.translate)
                st.pose.translate[size_t(b)] = translateFor(parentW, positionOf(want));
            st.world = worldOf(tgt, st.pose);
        }
        return worst;
    }
};

Retargeter::Retargeter(const Skeleton& source, const Rig& sourceRig, const Pose& sourceCurrent, const Skeleton& target, const Rig& targetRig,
                       const Pose& targetCurrent, Settings settings)
    : mImpl(new Impl)
{
    Impl& d = *mImpl;
    d.src = source;
    d.tgt = target;
    d.srcRig = sourceRig;
    d.tgtRig = targetRig;
    d.set = std::move(settings);
    d.up = glm::length(targetRig.up) > 1e-9 ? glm::normalize(targetRig.up) : glm::dvec3(0, 1, 0);
    d.S0 = worldOf(source, sourceRig.retargetPose(source, sourceCurrent));
    d.tgtPose0 = targetRig.retargetPose(target, targetCurrent);
    d.T0 = worldOf(target, d.tgtPose0);
    d.sp = source.find(sourceRig.pelvis);
    d.tp = target.find(targetRig.pelvis);
    if (d.sp < 0 || d.tp < 0)
    {
        delete mImpl;
        throw std::invalid_argument(d.sp < 0 ? "the source rig has no pelvis on its skeleton" : "the target rig has no pelvis on its skeleton");
    }

    for (const ChainMap& cm : d.set.chains)
    {
        const Chain* tc = targetRig.chain(cm.target);
        if (!tc)
        {
            d.notes.push_back("no target chain " + cm.target);
            continue;
        }
        if (cm.source.empty())
            continue;
        const Chain* sc = sourceRig.chain(cm.source);
        if (!sc)
        {
            d.notes.push_back("no source chain " + cm.source + " for " + cm.target);
            continue;
        }
        Mapped m;
        m.map = &cm;
        m.sourceChain = sc;
        m.targetChain = tc;
        m.src = chainJoints(source, *sc);
        m.tgt = chainJoints(target, *tc);
        if (m.src.empty() || m.tgt.empty())
        {
            d.notes.push_back("chain " + (m.src.empty() ? sc->name : tc->name) + " does not run from its start down to its end");
            continue;
        }
        m.srcParam = paramsOf(m.src, d.S0);
        m.tgtParam = paramsOf(m.tgt, d.T0);
        m.srcLen0 = chainLength(source, m.src, d.S0);
        m.srcGoal = sc->goal.empty() ? -1 : source.find(sc->goal);
        m.tgtGoal = tc->goal.empty() ? -1 : target.find(tc->goal);
        if (m.tgtGoal >= 0)
            m.ikJoints = target.path(m.tgt.front(), m.tgtGoal);
        d.mapped.push_back(std::move(m));
    }

    if (d.set.scale > 0.0)
        d.scale = d.set.scale;
    else
    {
        // Leg length says how big a body is better than height: a crouched retarget pose still measures right.
        double s = 0.0, t = 0.0;
        for (const Mapped& m : d.mapped)
            if (m.targetChain->role == Role::Leg && m.src.size() >= 2 && m.tgt.size() >= 2)
            {
                s += m.srcLen0;
                t += chainLength(target, m.tgt, d.T0);
            }
        if (s > 1e-6 && t > 1e-6)
            d.scale = t / s;
        else
        {
            const double hs = glm::dot(positionOf(d.S0[size_t(d.sp)]), d.up) - sourceRig.ground;
            const double ht = glm::dot(positionOf(d.T0[size_t(d.tp)]), d.up) - targetRig.ground;
            d.scale = hs > 1e-6 && ht > 1e-6 ? ht / hs : 1.0;
        }
    }
}

Retargeter::~Retargeter() { delete mImpl; }

double Retargeter::scale() const { return mImpl->scale; }
const std::vector<std::string>& Retargeter::notes() const { return mImpl->notes; }

Pose Retargeter::frame(const std::vector<glm::dmat4>& sourceWorld) const
{
    if (sourceWorld.size() != mImpl->src.size())
        throw std::invalid_argument("the source frame has the wrong number of joints");
    Stage st = mImpl->fk(sourceWorld, positionOf(mImpl->S0[size_t(mImpl->sp)]));
    mImpl->finish(st);
    return st.pose;
}

Result Retargeter::clip(const Frames& frames) const
{
    const Impl& d = *mImpl;
    for (const auto& f : frames.world)
        if (f.size() != d.src.size())
            throw std::invalid_argument("a source frame has the wrong number of joints");
    const size_t n = frames.world.size();
    Result out;
    out.scale = d.scale;
    out.notes = d.notes;
    std::vector<Stage> st(n);
    if (n == 0)
        return out;
    const glm::dvec3 origin = positionOf(frames.world.front()[size_t(d.sp)]);
    tbb::parallel_for(size_t(0), n, [&](size_t f) { st[f] = d.fk(frames.world[f], origin); });
    const double dt = 1.0 / (frames.fps > 0.0 ? frames.fps : 30.0);
    const glm::dvec3 up = d.up;

    // Goals are stored per frame in the same order, one per IK chain.
    const size_t goals = st.front().goals.size();
    auto goalAt = [&](size_t f, size_t g) -> Goal& { return st[f].goals[g]; };

    if (d.set.warp.enabled)
        for (size_t f = 0; f < n; ++f)
        {
            const glm::dvec3 pelvis = positionOf(st[f].world[size_t(d.tp)]);
            glm::dvec3 dir(0.0);
            if (d.set.warp.useVelocity && n > 1)
            {
                const size_t a = f > 0 ? f - 1 : f, b = f + 1 < n ? f + 1 : f;
                dir = flatten(positionOf(st[b].world[size_t(d.tp)]) - positionOf(st[a].world[size_t(d.tp)]), up);
            }
            if (glm::length(dir) < 1e-3)
                dir = flatten(glm::mat3_cast(rotationOf(st[f].world[size_t(d.tp)]) * glm::inverse(rotationOf(d.T0[size_t(d.tp)]))) * d.tgtRig.forward, up);
            if (glm::length(dir) < 1e-9)
                continue;
            dir = glm::normalize(dir);
            const glm::dvec3 side = glm::normalize(glm::cross(up, dir));
            for (size_t g = 0; g < goals; ++g)
            {
                Goal& goal = goalAt(f, g);
                const Mapped& m = d.mapped[goal.chain];
                if (!m.map->ik.affectedByWarping)
                    continue;
                const glm::dvec3 rel = goal.position - pelvis;
                const double along = glm::dot(rel, dir), across = glm::dot(rel, side), height = glm::dot(rel, up);
                const double sign = m.targetChain->side == Side::Left ? 1.0 : m.targetChain->side == Side::Right ? -1.0 : 0.0;
                goal.position = pelvis + dir * along * d.set.warp.warpForwards + side * (across + sign * d.set.warp.sidewaysOffset) + up * height;
            }
        }

    for (const Contact& c : d.set.contacts)
    {
        size_t eg = goals;
        for (size_t g = 0; g < goals; ++g)
            if (d.mapped[goalAt(0, g).chain].targetChain->name == c.effector)
                eg = g;
        const Mapped* body = nullptr;
        for (const Mapped& m : d.mapped)
            if (m.targetChain->name == c.body)
                body = &m;
        if (eg == goals || !body)
        {
            out.notes.push_back("contact " + c.effector + " on " + c.body + ": both need mapped chains, the effector with IK");
            continue;
        }
        for (size_t f = 0; f < n; ++f)
        {
            Goal& goal = goalAt(f, eg);
            const std::vector<glm::dmat4>& S = frames.world[f];
            const glm::dvec3 sp = pointAt(body->src, body->srcParam, S, c.bodyParam);
            const double dist = glm::length(goal.source - sp);
            const double w = 1.0 - smooth01((dist - c.distance) / std::max(c.distance * 0.5, 1e-6));
            if (w <= 0.0)
                continue;
            const glm::dquat sFrame = rotationOf(S[size_t(d.sourceJointAt(*body, c.bodyParam))]);
            const glm::dvec3 local = glm::inverse(sFrame) * (goal.source - sp);
            size_t tk = 0;
            for (size_t k = 1; k < body->tgt.size(); ++k)
                if (std::abs(body->tgtParam[k] - c.bodyParam) < std::abs(body->tgtParam[tk] - c.bodyParam))
                    tk = k;
            const glm::dvec3 tp = pointAt(body->tgt, body->tgtParam, st[f].world, c.bodyParam);
            const glm::dvec3 want = tp + rotationOf(st[f].world[size_t(body->tgt[tk])]) * (local * d.scale);
            goal.position = glm::mix(goal.position, want, w);
        }
    }

    for (size_t g = 0; g < goals; ++g)
    {
        const Mapped& m = d.mapped[goalAt(0, g).chain];
        const PlantSettings& ps = m.map->plant;
        std::vector<bool> planted(n, false);
        const auto intent = frames.channels.find("plant" + m.sourceChain->name);
        if (ps.enabled && intent != frames.channels.end() && intent->second.size() == n)
            for (size_t f = 0; f < n; ++f)
                planted[f] = intent->second[f] >= 0.5;
        else if (ps.enabled && n > 2)
        {
            double lowest = 1e300;
            for (size_t f = 0; f < n; ++f)
                lowest = std::min(lowest, glm::dot(goalAt(f, g).source, up));
            for (size_t f = 0; f < n; ++f)
            {
                const size_t a = f > 0 ? f - 1 : f, b = f + 1 < n ? f + 1 : f;
                const double speed = glm::length(goalAt(b, g).source - goalAt(a, g).source) / (double(b - a) * dt) * d.scale;
                planted[f] = speed < ps.speedThreshold && glm::dot(goalAt(f, g).source, up) < lowest + ps.heightTolerance;
            }
            // Spans too short to be a step are noise.
            for (size_t f = 0; f < n;)
            {
                size_t e = f;
                while (e < n && planted[e] == planted[f])
                    ++e;
                if (planted[f] && int(e - f) < ps.minFrames)
                    std::fill(planted.begin() + std::ptrdiff_t(f), planted.begin() + std::ptrdiff_t(e), false);
                f = e;
            }
        }
        if (ps.enabled)
        {
            for (size_t f = 0; f < n;)
            {
                if (!planted[f])
                {
                    ++f;
                    continue;
                }
                size_t e = f;
                while (e < n && planted[e])
                    ++e;
                const glm::dvec3 anchor = goalAt(f, g).position;
                for (size_t k = f; k < e; ++k)
                    goalAt(k, g).position = anchor;
                for (int k = 1; k <= ps.blendFrames; ++k)
                {
                    const double w = smooth01(1.0 - double(k) / double(ps.blendFrames + 1));
                    if (f >= size_t(k) && !planted[f - size_t(k)])
                        goalAt(f - size_t(k), g).position = glm::mix(goalAt(f - size_t(k), g).position, anchor, w);
                    if (e - 1 + size_t(k) < n && !planted[e - 1 + size_t(k)])
                        goalAt(e - 1 + size_t(k), g).position = glm::mix(goalAt(e - 1 + size_t(k), g).position, anchor, w);
                }
                f = e;
            }
        }
        out.planted.push_back({m.targetChain->name, std::move(planted)});
    }

    std::vector<std::vector<double>> err(n);
    tbb::parallel_for(size_t(0), n, [&](size_t f) { d.finish(st[f], &err[f]); });
    for (size_t g = 0; g < goals; ++g)
        out.chainError.push_back({d.mapped[goalAt(0, g).chain].targetChain->name, 0.0});
    out.poses.reserve(n);
    for (size_t f = 0; f < n; ++f)
    {
        for (size_t g = 0; g < err[f].size() && g < goals; ++g)
        {
            out.chainError[g].second = std::max(out.chainError[g].second, err[f][g]);
            out.ikError = std::max(out.ikError, err[f][g]);
        }
        out.poses.push_back(std::move(st[f].pose));
    }
    for (const auto& [chain, miss] : out.chainError)
        if (miss > 1.0)
            out.notes.push_back(chain + " misses its goal by up to " + std::to_string(int(std::lround(miss))) +
                                " cm: the target can't reach where the source goes (try IK extension or blend to source)");
    return out;
}

Pose alignRetargetPose(const Skeleton& source, const Rig& sourceRig, const Pose& sourceCurrent, const Skeleton& target, const Rig& targetRig,
                       const Pose& targetCurrent, const Settings& settings)
{
    const std::vector<glm::dmat4> S = worldOf(source, sourceRig.retargetPose(source, sourceCurrent));
    Pose pose = targetRig.retargetPose(target, targetCurrent);
    for (const ChainMap& cm : settings.chains)
    {
        const Chain* tc = targetRig.chain(cm.target);
        const Chain* sc = cm.source.empty() ? nullptr : sourceRig.chain(cm.source);
        if (!tc || !sc || tc->role == Role::Root)
            continue;
        const std::vector<int> sj = chainJoints(source, *sc), tj = chainJoints(target, *tc);
        if (sj.size() < 2 || tj.size() < 2)
            continue;
        const std::vector<double> sp = paramsOf(sj, S);
        std::vector<glm::dmat4> T = worldOf(target, pose);
        const std::vector<double> tp = paramsOf(tj, T);
        for (size_t k = 0; k + 1 < tj.size(); ++k)
        {
            const glm::dvec3 want = pointAt(sj, sp, S, tp[k + 1]) - pointAt(sj, sp, S, tp[k]);
            const glm::dvec3 have = positionOf(T[size_t(tj[k + 1])]) - positionOf(T[size_t(tj[k])]);
            const size_t j = size_t(tj[k]);
            const glm::dmat4 parentW = parentWorldOf(target, T, int(j));
            pose.rotation[j] = localRotationFor(target, int(j), parentW, arc(have, want) * rotationOf(T[j]));
            T = worldOf(target, pose);
        }
    }
    return pose;
}
} // namespace lila::retarget
