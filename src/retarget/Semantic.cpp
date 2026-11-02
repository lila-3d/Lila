#include "retarget/Semantic.h"

#include <glm/gtx/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>

namespace lila::retarget::semantic
{
namespace
{
constexpr const char* kGaitNames[] = {"auto", "walk", "trot", "pace", "canter", "gallop", "tripod", "tetrapod", "wave"};

glm::dvec3 flat(const glm::dvec3& v, const glm::dvec3& up) { return v - glm::dot(v, up) * up; }

/** A frame facing `forward` (levelled) with up kept: columns forward, left, up. */
glm::dquat bodyFrame(glm::dvec3 forward, const glm::dvec3& up)
{
    forward = flat(forward, up);
    if (glm::length(forward) < 1e-9)
        return {1.0, 0.0, 0.0, 0.0};
    forward = glm::normalize(forward);
    const glm::dvec3 left = glm::normalize(glm::cross(up, forward));
    return glm::normalize(glm::quat_cast(glm::dmat3(forward, left, up)));
}

double legLength(const Skeleton& s, const Rig& rig, const std::vector<glm::dmat4>& world)
{
    double total = 0.0;
    int n = 0;
    for (const Chain& c : rig.chains)
        if (c.role == Role::Leg)
            if (const std::vector<int> j = chainJoints(s, c); j.size() >= 2)
                total += chainLength(s, j, world), ++n;
    if (n > 0 && total > 1e-6)
        return total / n;
    const int p = s.find(rig.pelvis);
    const double h = p >= 0 ? glm::dot(positionOf(world[size_t(p)]), rig.up) - rig.ground : 0.0;
    return h > 1e-6 ? h : 1.0;
}

std::vector<double> params(const std::vector<int>& joints, const std::vector<glm::dmat4>& world)
{
    std::vector<double> t(joints.size(), 0.0);
    double total = 0.0;
    for (size_t i = 1; i < joints.size(); ++i)
        t[i] = (total += glm::length(positionOf(world[size_t(joints[i])]) - positionOf(world[size_t(joints[i - 1])])));
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = total > 1e-9 ? t[i] / total : joints.size() > 1 ? double(i) / double(joints.size() - 1) : 0.0;
    return t;
}

/** The chain's world rotation change at fraction t, blended between its joints. */
glm::dquat deltaAt(const std::vector<int>& joints, const std::vector<double>& t, const std::vector<glm::dmat4>& now,
                   const std::vector<glm::dmat4>& rest, double at)
{
    auto d = [&](size_t k) { return rotationOf(now[size_t(joints[k])]) * glm::inverse(rotationOf(rest[size_t(joints[k])])); };
    for (size_t k = 0; k + 1 < joints.size(); ++k)
        if (at <= t[k + 1])
        {
            const double span = t[k + 1] - t[k];
            return glm::slerp(d(k), d(k + 1), span > 1e-12 ? std::clamp((at - t[k]) / span, 0.0, 1.0) : 1.0);
        }
    return d(joints.size() - 1);
}

/** The three bend samples blended at fraction t. */
glm::dquat bendAt(const std::array<glm::dquat, 3>& b, double t)
{
    return t <= 0.5 ? glm::slerp(b[0], b[1], t * 2.0) : glm::slerp(b[1], b[2], (t - 0.5) * 2.0);
}

/** Planted flags from a goal's world path: slow and low, minus blips. */
std::vector<bool> contacts(const std::vector<glm::dvec3>& path, const glm::dvec3& up, double dt, double speedLimit, double heightTolerance, int minFrames)
{
    const size_t n = path.size();
    std::vector<bool> on(n, false);
    if (n < 3)
        return on;
    double lowest = 1e300;
    for (const glm::dvec3& p : path)
        lowest = std::min(lowest, glm::dot(p, up));
    for (size_t f = 0; f < n; ++f)
    {
        const size_t a = f > 0 ? f - 1 : f, b = f + 1 < n ? f + 1 : f;
        const double speed = glm::length(path[b] - path[a]) / (double(b - a) * dt);
        on[f] = speed < speedLimit && glm::dot(path[f], up) < lowest + heightTolerance;
    }
    for (size_t f = 0; f < n;)
    {
        size_t e = f;
        while (e < n && on[e] == on[f])
            ++e;
        if (on[f] && int(e - f) < minFrames)
            std::fill(on.begin() + std::ptrdiff_t(f), on.begin() + std::ptrdiff_t(e), false);
        f = e;
    }
    return on;
}

json quatJson(const glm::dquat& q) { return json::array({q.x, q.y, q.z, q.w}); }
glm::dquat quatFrom(const json& j) { return glm::normalize(glm::dquat(j.at(3).get<double>(), j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>())); }
json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 vecFrom(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
} // namespace

const char* gaitName(Gait g) { return kGaitNames[size_t(g)]; }

Gait gaitFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kGaitNames); ++i)
        if (name == kGaitNames[i])
            return Gait(i);
    throw std::invalid_argument("unknown gait '" + std::string(name) + "'");
}

json Clip::toJson() const
{
    json limbsJ = json::array(), shapesJ = json::array();
    for (const LimbTrack& l : limbs)
    {
        json goal = json::array(), rot = json::array(), con = json::array();
        for (size_t f = 0; f < l.goal.size(); ++f)
        {
            goal.push_back(vecJson(l.goal[f]));
            rot.push_back(quatJson(l.rotation[f]));
            con.push_back(bool(l.contact[f]));
        }
        limbsJ.push_back({{"role", roleName(l.role)}, {"side", sideName(l.side)}, {"index", l.index}, {"goal", goal}, {"rotation", rot}, {"contact", con}});
    }
    for (const ShapeTrack& s : shapes)
    {
        json bend = json::array();
        for (const auto& b : s.bend)
            bend.push_back(json::array({quatJson(b[0]), quatJson(b[1]), quatJson(b[2])}));
        shapesJ.push_back({{"role", roleName(s.role)}, {"side", sideName(s.side)}, {"index", s.index}, {"bend", bend}});
    }
    json vel = json::array(), turnJ = json::array(), tiltJ = json::array();
    for (int f = 0; f < frames; ++f)
    {
        vel.push_back(vecJson(velocity[size_t(f)]));
        turnJ.push_back(quatJson(turn[size_t(f)]));
        tiltJ.push_back(quatJson(tilt[size_t(f)]));
    }
    return {{"format", "lilaSemanticClip"}, {"version", 1}, {"fps", fps},  {"frames", frames},  {"velocity", vel},
            {"turn", turnJ},                {"height", height}, {"tilt", tiltJ}, {"limbs", limbsJ}, {"shapes", shapesJ}};
}

Clip Clip::fromJson(const json& j)
{
    try
    {
        Clip c;
        c.fps = j.at("fps").get<double>();
        c.frames = j.at("frames").get<int>();
        for (const json& v : j.at("velocity"))
            c.velocity.push_back(vecFrom(v));
        for (const json& q : j.at("turn"))
            c.turn.push_back(quatFrom(q));
        c.height = j.at("height").get<std::vector<double>>();
        for (const json& q : j.at("tilt"))
            c.tilt.push_back(quatFrom(q));
        for (const json& l : j.at("limbs"))
        {
            LimbTrack t;
            t.role = roleFromName(l.at("role").get<std::string>());
            t.side = sideFromName(l.at("side").get<std::string>());
            t.index = l.at("index").get<int>();
            for (const json& v : l.at("goal"))
                t.goal.push_back(vecFrom(v));
            for (const json& q : l.at("rotation"))
                t.rotation.push_back(quatFrom(q));
            for (const json& b : l.at("contact"))
                t.contact.push_back(b.get<bool>());
            c.limbs.push_back(std::move(t));
        }
        for (const json& s : j.at("shapes"))
        {
            ShapeTrack t;
            t.role = roleFromName(s.at("role").get<std::string>());
            t.side = sideFromName(s.at("side").get<std::string>());
            t.index = s.at("index").get<int>();
            for (const json& b : s.at("bend"))
                t.bend.push_back({quatFrom(b.at(0)), quatFrom(b.at(1)), quatFrom(b.at(2))});
            c.shapes.push_back(std::move(t));
        }
        const size_t n = size_t(c.frames);
        bool ok = c.velocity.size() == n && c.turn.size() == n && c.height.size() == n && c.tilt.size() == n;
        for (const LimbTrack& t : c.limbs)
            ok = ok && t.goal.size() == n && t.rotation.size() == n && t.contact.size() == n;
        for (const ShapeTrack& t : c.shapes)
            ok = ok && t.bend.size() == n;
        if (!ok)
            throw std::invalid_argument("tracks do not all have 'frames' samples");
        return c;
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(std::string("bad semantic clip: ") + e.what());
    }
}

Clip encode(const Skeleton& s, const Rig& rig, const Pose& current, const Frames& frames, const PlantSettings& plant)
{
    Clip c;
    c.fps = frames.fps > 0.0 ? frames.fps : 30.0;
    c.frames = int(frames.world.size());
    const double dt = 1.0 / c.fps;
    const glm::dvec3 up = glm::normalize(rig.up);
    const std::vector<glm::dmat4> rest = worldOf(s, rig.retargetPose(s, current));
    const int p = s.find(rig.pelvis);
    if (p < 0)
        throw std::invalid_argument("the rig has no pelvis on its skeleton");
    const double L = legLength(s, rig, rest);
    const glm::dquat restBody = bodyFrame(rig.forward, up);
    const double restHeight = glm::dot(positionOf(rest[size_t(p)]), up);
    const size_t n = frames.world.size();

    // The body frame: under the pelvis, facing where the pelvis has turned the rest pose's forward.
    std::vector<glm::dquat> body(n);
    std::vector<glm::dvec3> origin(n);
    for (size_t f = 0; f < n; ++f)
    {
        const std::vector<glm::dmat4>& W = frames.world[f];
        const glm::dquat pelvisTurn = rotationOf(W[size_t(p)]) * glm::inverse(rotationOf(rest[size_t(p)]));
        glm::dvec3 fwd = flat(pelvisTurn * rig.forward, up);
        body[f] = glm::length(fwd) > 1e-6 ? bodyFrame(fwd, up) : f > 0 ? body[f - 1] : restBody;
        origin[f] = flat(positionOf(W[size_t(p)]), up);
    }
    for (size_t f = 0; f < n; ++f)
    {
        const std::vector<glm::dmat4>& W = frames.world[f];
        const size_t a = f > 0 ? f - 1 : f, b = f + 1 < n ? f + 1 : f;
        const glm::dvec3 v = b > a ? (origin[b] - origin[a]) / (double(b - a) * dt) : glm::dvec3(0.0);
        c.velocity.push_back(glm::inverse(body[f]) * v / L);
        c.turn.push_back(glm::normalize(body[f] * glm::inverse(body[0])));
        c.height.push_back((glm::dot(positionOf(W[size_t(p)]), up) - restHeight) / L);
        const glm::dquat bodyTurn = body[f] * glm::inverse(restBody);
        c.tilt.push_back(glm::normalize(glm::inverse(bodyTurn) * rotationOf(W[size_t(p)]) * glm::inverse(rotationOf(rest[size_t(p)]))));
    }

    for (const Chain& ch : rig.chains)
    {
        const std::vector<int> j = chainJoints(s, ch);
        if (j.empty())
            continue;
        if ((ch.role == Role::Leg || ch.role == Role::Arm) && !ch.goal.empty() && j.size() >= 2)
        {
            const int g = s.find(ch.goal);
            const double reach = ch.role == Role::Leg ? L : std::max(chainLength(s, s.path(j.front(), g), rest), 1e-6);
            const glm::dvec3 restRel = glm::inverse(restBody) * (positionOf(rest[size_t(g)]) - positionOf(rest[size_t(j.front())]));
            LimbTrack t{.role = ch.role, .side = ch.side, .index = ch.index};
            std::vector<glm::dvec3> path;
            for (size_t f = 0; f < n; ++f)
            {
                const std::vector<glm::dmat4>& W = frames.world[f];
                const glm::dquat bodyTurn = body[f] * glm::inverse(restBody);
                t.goal.push_back((glm::inverse(body[f]) * (positionOf(W[size_t(g)]) - positionOf(W[size_t(j.front())])) - restRel) / reach);
                t.rotation.push_back(glm::normalize(glm::inverse(bodyTurn) * rotationOf(W[size_t(g)]) * glm::inverse(rotationOf(rest[size_t(g)]))));
                path.push_back(positionOf(W[size_t(g)]));
            }
            // Thresholds in leg lengths, so a mouse and a giant plant alike.
            t.contact = ch.role == Role::Leg && plant.enabled
                            ? contacts(path, up, dt, plant.speedThreshold / 90.0 * L, plant.heightTolerance / 90.0 * L, plant.minFrames)
                            : std::vector<bool>(n, false);
            c.limbs.push_back(std::move(t));
        }
        else if (ch.role == Role::Spine || ch.role == Role::Neck || ch.role == Role::Head || ch.role == Role::Tail)
        {
            const std::vector<double> t = params(j, rest);
            ShapeTrack sh{.role = ch.role, .side = ch.side, .index = ch.index};
            for (size_t f = 0; f < n; ++f)
            {
                const glm::dquat inv = glm::inverse(body[f] * glm::inverse(restBody));
                std::array<glm::dquat, 3> b;
                for (int k = 0; k < 3; ++k)
                    b[size_t(k)] = glm::normalize(inv * deltaAt(j, t, frames.world[f], rest, 0.5 * k));
                sh.bend.push_back(b);
            }
            c.shapes.push_back(std::move(sh));
        }
    }
    return c;
}

std::vector<double> gaitPhases(Gait gait, const std::vector<std::pair<Side, int>>& legs)
{
    int perSide = 0;
    for (const auto& [side, index] : legs)
        perSide = std::max(perSide, index + 1);
    if (gait == Gait::Auto)
        gait = perSide <= 2 ? Gait::Walk : perSide == 3 ? Gait::Tripod : perSide == 4 ? Gait::Tetrapod : Gait::Wave;
    std::vector<double> out;
    for (const auto& [side, index] : legs)
    {
        const bool right = side == Side::Right;
        const double half = right ? 0.5 : 0.0;
        const bool front = index == 0;
        double phase = 0.0;
        if (perSide <= 1)
            phase = half;
        else if (perSide == 2 && gait != Gait::Tripod && gait != Gait::Tetrapod && gait != Gait::Wave)
            switch (gait)
            {
            case Gait::Trot: phase = (front == right) ? 0.5 : 0.0; break; // left fore with right hind
            case Gait::Pace: phase = half; break;
            case Gait::Canter: phase = !right && !front ? 0.0 : right && front ? 2.0 / 3.0 : 1.0 / 3.0; break;
            case Gait::Gallop: phase = (front ? 0.5 : 0.0) + (right ? 0.1 : 0.0); break;
            default: phase = (front ? 0.25 : 0.0) + half; break; // walk: left hind, left fore, right hind, right fore
            }
        else if (gait == Gait::Tripod || gait == Gait::Tetrapod || gait == Gait::Trot)
            phase = (index + (right ? 1 : 0)) % 2 == 0 ? 0.0 : 0.5;
        else
            phase = half + 0.5 * double(perSide - 1 - index) / double(perSide); // a wave from the back
        out.push_back(phase - std::floor(phase));
    }
    return out;
}

Cycle measureCycle(const Clip& c)
{
    Cycle out;
    out.phase.assign(c.limbs.size(), -1.0);
    std::vector<std::vector<int>> starts(c.limbs.size());
    for (size_t l = 0; l < c.limbs.size(); ++l)
        for (int f = 0; f < c.frames; ++f)
            if (c.limbs[l].contact[size_t(f)] && (f == 0 ? false : !c.limbs[l].contact[size_t(f - 1)]))
                starts[l].push_back(f);
    double total = 0.0;
    int gaps = 0;
    for (const auto& s : starts)
        for (size_t k = 1; k < s.size(); ++k)
            total += s[k] - s[k - 1], ++gaps;
    const double clipLength = double(std::max(c.frames - 1, 1)) / c.fps;
    if (gaps > 0)
        out.period = total / gaps / c.fps;
    else
    {
        bool any = false;
        for (const auto& s : starts)
            any = any || !s.empty();
        out.period = any ? clipLength : 0.0;
    }
    if (out.period <= 0.0)
        return out;
    for (size_t l = 0; l < c.limbs.size(); ++l)
        if (!starts[l].empty())
        {
            const double t = starts[l].front() / c.fps / out.period;
            out.phase[l] = t - std::floor(t);
        }
    return out;
}

Result apply(const Clip& clip, const Skeleton& target, const Rig& rig, const Pose& current, const ApplyOptions& opts)
{
    Result out;
    const size_t n = size_t(clip.frames);
    if (n == 0)
        return out;
    const double dt = 1.0 / clip.fps;
    const glm::dvec3 up = glm::normalize(rig.up);
    const Pose pose0 = rig.retargetPose(target, current);
    const std::vector<glm::dmat4> rest = worldOf(target, pose0);
    const int p = target.find(rig.pelvis);
    if (p < 0)
        throw std::invalid_argument("the target rig has no pelvis on its skeleton");
    const double L = legLength(target, rig, rest);
    const glm::dquat restBody = bodyFrame(rig.forward, up);
    const double restHeight = glm::dot(positionOf(rest[size_t(p)]), up);

    // Which source limb drives each target limb, and how far ahead in the cycle it is sampled.
    const Cycle cycle = measureCycle(clip);
    struct Drive
    {
        const Chain* chain;
        std::vector<int> joints; // chain start to goal
        size_t source;
        double shift; // seconds
        double reach;
        glm::dvec3 rest; // the goal's offset from the root in the retarget pose, body frame
    };
    auto restOffset = [&](const std::vector<int>& j) {
        return glm::inverse(restBody) * (positionOf(rest[size_t(j.back())]) - positionOf(rest[size_t(j.front())]));
    };
    std::vector<Drive> drives;
    std::vector<std::pair<Side, int>> legs;
    std::vector<const Chain*> legChains;
    for (const Chain& c : rig.chains)
        if (c.role == Role::Leg && !c.goal.empty())
            legs.push_back({c.side, c.index}), legChains.push_back(&c);
    const std::vector<double> phases = gaitPhases(opts.gait, legs);
    std::vector<size_t> sourceLegs;
    for (size_t l = 0; l < clip.limbs.size(); ++l)
        if (clip.limbs[l].role == Role::Leg)
            sourceLegs.push_back(l);
    for (size_t k = 0; k < legChains.size(); ++k)
    {
        const Chain& c = *legChains[k];
        if (sourceLegs.empty())
        {
            out.notes.push_back("the clip has no legs to drive " + c.name);
            continue;
        }
        size_t best = sourceLegs.front();
        for (size_t l : sourceLegs)
            if (clip.limbs[l].side == c.side && (clip.limbs[best].side != c.side || clip.limbs[l].index == c.index))
                best = l;
        double shift = 0.0;
        if (cycle.period > 0.0 && cycle.phase[best] >= 0.0)
        {
            double d = cycle.phase[best] - phases[k];
            d -= std::floor(d);
            shift = d * cycle.period;
        }
        const std::vector<int> j = target.path(target.find(c.start), target.find(c.goal));
        if (j.size() >= 2)
            drives.push_back({&c, j, best, shift, L, restOffset(j)});
    }
    for (const Chain& c : rig.chains)
        if (c.role == Role::Arm && !c.goal.empty())
            for (size_t l = 0; l < clip.limbs.size(); ++l)
                if (clip.limbs[l].role == Role::Arm && clip.limbs[l].side == c.side && clip.limbs[l].index == c.index)
                {
                    const std::vector<int> j = target.path(target.find(c.start), target.find(c.goal));
                    if (j.size() >= 2)
                        drives.push_back({&c, j, l, 0.0, chainLength(target, j, rest), restOffset(j)});
                }

    // Sampling a track shifted in time, wrapping round the clip as one cycle.
    const double length = double(n - 1) * dt;
    auto at = [&](double t) {
        // Whole cycles keep a periodic clip in step, so a shifted sample past the end comes back a cycle earlier.
        if (cycle.period > 0.0)
            while (t > length && t - cycle.period >= 0.0)
                t -= cycle.period;
        t = std::clamp(t, 0.0, length);
        const double x = std::clamp(t / dt, 0.0, double(n - 1));
        const size_t a = size_t(std::floor(x));
        const size_t b = std::min(a + 1, n - 1);
        return std::make_tuple(a, b, x - double(a));
    };

    // The body path, scaled to the target's legs.
    std::vector<glm::dvec3> origin(n);
    std::vector<glm::dquat> body(n);
    origin[0] = flat(positionOf(rest[size_t(p)]), up);
    for (size_t f = 0; f < n; ++f)
    {
        body[f] = glm::normalize(clip.turn[f] * restBody);
        if (f > 0)
            origin[f] = origin[f - 1] + (body[f] * clip.velocity[f] + body[f - 1] * clip.velocity[f - 1]) * 0.5 * L * dt;
    }

    struct Frame
    {
        Pose pose;
        std::vector<glm::dmat4> world;
        std::vector<glm::dvec3> goal;
        std::vector<glm::dquat> rot;
        std::vector<bool> contact;
    };
    std::vector<Frame> frames(n);
    tbb::parallel_for(size_t(0), n, [&](size_t f) {
        Frame& fr = frames[f];
        const glm::dquat bodyTurn = body[f] * glm::inverse(restBody);
        std::vector<std::optional<glm::dquat>> want(target.size());
        want[size_t(p)] = glm::normalize(bodyTurn * clip.tilt[f] * rotationOf(rest[size_t(p)]));
        for (const Chain& c : rig.chains)
            for (const ShapeTrack& sh : clip.shapes)
                if (sh.role == c.role && sh.side == c.side && sh.index == c.index)
                {
                    const std::vector<int> j = chainJoints(target, c);
                    const std::vector<double> t = params(j, rest);
                    for (size_t k = 0; k < j.size(); ++k)
                        if (j[k] != p)
                            want[size_t(j[k])] = glm::normalize(bodyTurn * bendAt(sh.bend[f], t[k]) * rotationOf(rest[size_t(j[k])]));
                }
        fr.pose = pose0;
        fr.world.assign(target.size(), glm::dmat4(1.0));
        const glm::dvec3 pelvisPos = origin[f] + up * (restHeight + clip.height[f] * L);
        for (size_t i = 0; i < target.size(); ++i)
        {
            const glm::dmat4 parentW = parentWorldOf(target, fr.world, int(i));
            if (want[i])
                fr.pose.rotation[i] = localRotationFor(target, int(i), parentW, *want[i]);
            if (int(i) == p)
                fr.pose.translate[i] = translateFor(parentW, pelvisPos);
            fr.world[i] = parentW * localOf(target, fr.pose, int(i));
        }
        for (const Drive& d : drives)
        {
            const LimbTrack& tr = clip.limbs[d.source];
            const auto [a, b, w] = at(double(f) * dt + d.shift);
            const glm::dvec3 rel = glm::mix(tr.goal[a], tr.goal[b], w);
            const glm::dquat rot = glm::slerp(tr.rotation[a], tr.rotation[b], w);
            fr.goal.push_back(positionOf(fr.world[size_t(d.joints.front())]) + body[f] * (d.rest + rel * d.reach));
            fr.rot.push_back(glm::normalize(bodyTurn * rot * rotationOf(rest[size_t(d.joints.back())])));
            fr.contact.push_back(w < 0.5 ? tr.contact[a] : tr.contact[b]);
        }
    });

    // Planted feet hold where they touched down.
    if (opts.plant)
        for (size_t d = 0; d < drives.size(); ++d)
        {
            std::vector<bool> planted(n);
            for (size_t f = 0; f < n; ++f)
                planted[f] = frames[f].contact[d];
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
                const glm::dvec3 anchor = frames[f].goal[d];
                for (size_t k = f; k < e; ++k)
                    frames[k].goal[d] = anchor;
                f = e;
            }
            out.planted.push_back({drives[d].chain->name, std::move(planted)});
        }

    std::vector<double> err(n, 0.0);
    tbb::parallel_for(size_t(0), n, [&](size_t f) {
        Frame& fr = frames[f];
        for (size_t d = 0; d < drives.size(); ++d)
            err[f] = std::max(err[f], solveChainIk(target, fr.pose, fr.world, drives[d].joints, fr.goal[d], fr.rot[d]));
    });
    for (size_t f = 0; f < n; ++f)
    {
        out.ikError = std::max(out.ikError, err[f]);
        out.poses.push_back(std::move(frames[f].pose));
    }
    return out;
}
} // namespace lila::retarget::semantic
