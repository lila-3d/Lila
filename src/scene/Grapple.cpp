#include "scene/Grapple.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/DominantInfluence.h"
#include "scene/Graph.h"
#include "geo/Mesh.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/RigUtil.h"
#include "scene/Relocate.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace lila::scene::grapple
{
using namespace rigutil;

namespace
{
/** kg per cm^3: a body's density, a little under water's. */
constexpr double kDensity = 985.0 / 1e6;
constexpr double kPi = 3.14159265358979323846;
/** How strongly a collision push pulls, against a contact's 1. */
constexpr double kPushWeight = 0.2;
/** Degrees a held rotation may be off before the rounds carry on, as cm of contact error per degree. */
constexpr double kCmPerDegree = 0.1;

glm::dquat rotationOf(const glm::dmat4& m) { return glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(m)))); }
glm::dvec3 pointOn(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

/** World matrix with the scale taken out. */
glm::dmat4 rigid(const glm::dmat4& m)
{
    glm::dmat4 r(orthonormal(glm::dmat3(m)));
    r[3] = m[3];
    return r;
}

/**
 * The node's whole path from the top. Stored references use it: a shortest path stops resolving once another character
 * with the same joint names comes in.
 */
std::string fullPath(const Graph& g, NodeId id)
{
    std::string p;
    for (NodeId c = id; c; c = g.parent(c))
        p = "|" + g.node(c)->name() + p;
    return p;
}

bool isUnder(const Graph& g, NodeId node, NodeId ancestor)
{
    for (NodeId c = node; c; c = g.parent(c))
        if (c == ancestor)
            return true;
    return false;
}

std::string bareName(const std::string& name)
{
    std::string s = name.substr(name.find_last_of(":|") + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool isFinger(const std::string& bare)
{
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "finger"})
        if (has(bare, f))
            return true;
    return false;
}

/** Without a template saying otherwise, by joint name for skeletons named like UE's: the spine and shoulders resist. */
double stiffnessByName(const std::string& bare)
{
    if (has(bare, "clavicle"))
        return 0.7;
    if (has(bare, "neck"))
        return 0.5;
    if (has(bare, "spine"))
        return 0.3;
    return 0.0;
}

double capsuleVolume(double radius, double length) { return kPi * radius * radius * (length + 4.0 / 3.0 * radius); }

/** One solve's per-figure working copy: its pose so far and the effectors this round adds. */
struct Working
{
    fbik::Solution pose;
    std::vector<glm::dmat4> world;
};

struct Pusher
{
    int bone = -1;
    glm::dvec3 point{0.0};
    glm::dvec3 target{0.0};
};

/**
 * A contact's gap as asked for: (1 - weight) of this frame's animated gap, in world space, so a side turning as it yields
 * doesn't swing it and every weight closes in one round.
 */
struct Gap
{
    glm::dvec3 gap{0.0};
    std::optional<glm::dquat> rotation;
};
} // namespace

namespace
{
/** Pull iterations of the rounds after the first, which start from a pose already near the answer. */
constexpr int kLaterIterations = 10;

/**
 * Takes the bones no effector hangs below out of the solve: nothing pulls them off their reference, so they stay put
 * either way, and the solve is a fraction of the size (twist and corrective joints, an idle arm).
 */
void freeze(fbik::Problem& p)
{
    std::vector<bool> carries(p.bones.size(), false);
    for (const fbik::Effector& e : p.effectors)
        for (int b = e.bone; b >= 0 && !carries[size_t(b)]; b = p.bones[size_t(b)].parent)
            carries[size_t(b)] = true;
    for (size_t k = 0; k < p.bones.size(); ++k)
        if (!carries[k] && std::abs(glm::dot(p.bones[k].rotation, p.bones[k].reference)) > 1.0 - 1e-15)
        {
            p.bones[k].solve = false;
            p.bones[k].translate = false;
        }
}
} // namespace

double shareByMass(double sourceMass, double targetMass)
{
    const double s = std::max(sourceMass, 1e-6), t = std::max(targetMass, 1e-6);
    return s / (s + t);
}

std::pair<glm::dvec3, glm::dvec3> capsuleWorld(const Capsule& c, const glm::dmat4& bone) { return {pointOn(bone, c.a), pointOn(bone, c.b)}; }

std::pair<glm::dvec3, glm::dvec3> closestPoints(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& q0, const glm::dvec3& q1)
{
    const glm::dvec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    double s = 0.0, t = 0.0;
    if (a <= 1e-12 && e <= 1e-12)
        return {p0, q0};
    if (a <= 1e-12)
        t = std::clamp(f / e, 0.0, 1.0);
    else
    {
        const double c = glm::dot(d1, r);
        if (e <= 1e-12)
            s = std::clamp(-c / a, 0.0, 1.0);
        else
        {
            const double b = glm::dot(d1, d2), den = a * e - b * b;
            s = den > 1e-12 ? std::clamp((b * f - c * e) / den, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0)
                t = 0.0, s = std::clamp(-c / a, 0.0, 1.0);
            else if (t > 1.0)
                t = 1.0, s = std::clamp((b - c) / a, 0.0, 1.0);
        }
    }
    return {p0 + d1 * s, q0 + d2 * t};
}

std::optional<double> rayCapsule(const glm::dvec3& origin, const glm::dvec3& dir0, const glm::dvec3& a, const glm::dvec3& b, double radius)
{
    const glm::dvec3 dir = glm::normalize(dir0);
    std::optional<double> best;
    auto take = [&](double t) {
        if (t >= 0.0 && (!best || t < *best))
            best = t;
    };
    auto sphere = [&](const glm::dvec3& c) {
        const glm::dvec3 m = origin - c;
        const double bq = glm::dot(m, dir), cq = glm::dot(m, m) - radius * radius, disc = bq * bq - cq;
        if (disc >= 0.0)
            take(-bq - std::sqrt(disc));
    };
    sphere(a);
    sphere(b);
    const glm::dvec3 axis = b - a;
    const double len = glm::length(axis);
    if (len > 1e-9)
    {
        const glm::dvec3 u = axis / len;
        const glm::dvec3 m = origin - a;
        const glm::dvec3 dp = dir - u * glm::dot(dir, u), mp = m - u * glm::dot(m, u);
        const double qa = glm::dot(dp, dp), qb = 2.0 * glm::dot(dp, mp), qc = glm::dot(mp, mp) - radius * radius;
        const double disc = qb * qb - 4.0 * qa * qc;
        if (qa > 1e-12 && disc >= 0.0)
        {
            const double t = (-qb - std::sqrt(disc)) / (2.0 * qa);
            const double along = glm::dot(origin + dir * t - a, u);
            if (along >= 0.0 && along <= len)
                take(t);
        }
    }
    return best;
}

Result solve(const std::vector<Figure>& figures, const std::vector<Link>& links, const Options& o)
{
    LILA_ZONE();

    const size_t n = figures.size();
    Result out;
    std::vector<Working> w(n);
    std::vector<std::vector<glm::dmat4>> animated(n);
    for (size_t i = 0; i < n; ++i)
    {
        const fbik::Problem& p = figures[i].problem;
        const size_t bones = p.bones.size();
        w[i].pose = {std::vector<glm::dquat>(bones), std::vector<glm::dvec3>(bones, glm::dvec3(0.0)), std::vector<double>(bones, 1.0)};
        for (size_t k = 0; k < bones; ++k)
        {
            w[i].pose.rotation[k] = p.bones[k].rotation;
            w[i].pose.offset[k] = p.bones[k].offset;
        }
        w[i].world = animated[i] = fbik::forward(p, w[i].pose.rotation, w[i].pose.offset);
    }

    auto valid = [&](const Link& l) {
        return l.weight > 1e-9 && l.source >= 0 && size_t(l.source) < n && l.target >= 0 && size_t(l.target) < n && l.sourceBone >= 0 &&
               size_t(l.sourceBone) < figures[size_t(l.source)].problem.bones.size() && l.targetBone >= 0 &&
               size_t(l.targetBone) < figures[size_t(l.target)].problem.bones.size();
    };
    std::vector<Gap> gaps(links.size());
    for (size_t c = 0; c < links.size(); ++c)
    {
        const Link& l = links[c];
        if (!valid(l))
            continue;
        const glm::dmat4& s = animated[size_t(l.source)][size_t(l.sourceBone)];
        const glm::dmat4& t = animated[size_t(l.target)][size_t(l.targetBone)];
        const glm::dvec3 gap = pointOn(t, l.targetPoint) - pointOn(s, l.sourcePoint);
        gaps[c].gap = gap * (1.0 - std::clamp(l.weight, 0.0, 1.0));
        if (l.rotation)
            gaps[c].rotation = glm::slerp(glm::conjugate(rotationOf(t)) * rotationOf(s), *l.rotation, std::clamp(l.weight, 0.0, 1.0));
    }
    // The remaining error of a contact: the gap now against the gap asked for, in world space.
    auto errorOf = [&](size_t c) {
        const Link& l = links[c];
        const glm::dmat4& t = w[size_t(l.target)].world[size_t(l.targetBone)];
        const glm::dvec3 gap = pointOn(t, l.targetPoint) - pointOn(w[size_t(l.source)].world[size_t(l.sourceBone)], l.sourcePoint);
        return gap - gaps[c].gap;
    };

    // Bone pairs a contact puts against each other on purpose, so they never count as passing through each other.
    std::set<std::tuple<int, int, int, int>> touching;
    // Two bones either way: a hand on a neck touches the head and shoulders too.
    auto neighbours = [&](int fig, int bone) {
        const fbik::Problem& p = figures[size_t(fig)].problem;
        std::vector<int> out{bone};
        for (int step = 0; step < 2; ++step)
        {
            const std::vector<int> ring = out;
            for (int b : ring)
            {
                if (const int parent = p.bones[size_t(b)].parent; parent >= 0)
                    out.push_back(parent);
                for (size_t k = 0; k < p.bones.size(); ++k)
                    if (p.bones[k].parent == b)
                        out.push_back(int(k));
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    };
    for (const Link& l : links)
        if (valid(l))
            for (int a : neighbours(l.source, l.sourceBone))
                for (int b : neighbours(l.target, l.targetBone))
                {
                    touching.insert({l.source, a, l.target, b});
                    touching.insert({l.target, b, l.source, a});
                }

    std::map<std::tuple<int, int, int, int>, std::pair<Pusher, Pusher>> pushers;
    auto collisions = [&](bool update) {
        double deepest = 0.0;
        bool added = false;
        if (!o.collide)
            return std::pair{deepest, added};
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                for (size_t ci = 0; ci < figures[i].capsules.size(); ++ci)
                    for (size_t cj = 0; cj < figures[j].capsules.size(); ++cj)
                    {
                        const Capsule &a = figures[i].capsules[ci], &b = figures[j].capsules[cj];
                        if (a.bone < 0 || b.bone < 0 || touching.count({int(i), a.bone, int(j), b.bone}))
                            continue;
                        const glm::dmat4 &wa = w[i].world[size_t(a.bone)], &wb = w[j].world[size_t(b.bone)];
                        const auto [a0, a1] = capsuleWorld(a, wa);
                        const auto [b0, b1] = capsuleWorld(b, wb);
                        const auto [pa, pb] = closestPoints(a0, a1, b0, b1);
                        const double d = glm::length(pa - pb);
                        const double overlap = a.radius + b.radius - d;
                        deepest = std::max(deepest, overlap);
                        const auto key = std::tuple{int(i), int(ci), int(j), int(cj)};
                        const double push = overlap + o.margin;
                        const auto it = pushers.find(key);
                        if (!update || (push <= 0.0 && it == pushers.end()))
                            continue;
                        glm::dvec3 nrm = d > 1e-9 ? (pa - pb) / d : glm::dvec3(0.0, 1.0, 0.0);
                        const double shareA = shareByMass(figures[j].mass, figures[i].mass);
                        Pusher pa2{a.bone, pointOn(glm::inverse(wa), pa), pa + nrm * std::max(push, 0.0) * shareA};
                        Pusher pb2{b.bone, pointOn(glm::inverse(wb), pb), pb - nrm * std::max(push, 0.0) * (1.0 - shareA)};
                        added = added || (it == pushers.end() && overlap > o.tolerance);
                        pushers[key] = {pa2, pb2};
                    }
        return std::pair{deepest, added};
    };

    const autopose::Model* model = o.autoPose ? autopose::shipped() : nullptr;
    for (int round = 0; round < std::max(o.iterations, 1); ++round)
    {
        std::vector<fbik::Problem> problems(n);
        std::vector<bool> moved(n, false);
        for (size_t i = 0; i < n; ++i)
        {
            problems[i] = figures[i].problem;
            for (size_t k = 0; k < problems[i].bones.size(); ++k)
            {
                problems[i].bones[k].rotation = w[i].pose.rotation[k];
                problems[i].bones[k].offset = w[i].pose.offset[k];
            }
        }
        // Every contact's targets from where both sides are now, so each figure closes its own share in the same round.
        for (size_t c = 0; c < links.size(); ++c)
        {
            const Link& l = links[c];
            if (!valid(l))
                continue;
            const glm::dvec3 e = errorOf(c);
            const double share = std::clamp(l.share, 0.0, 1.0);
            const glm::dvec3 s = pointOn(w[size_t(l.source)].world[size_t(l.sourceBone)], l.sourcePoint);
            const glm::dvec3 t = pointOn(w[size_t(l.target)].world[size_t(l.targetBone)], l.targetPoint);
            // A held rotation is shared like the gap: the target's bone turns its share too (a twisted wrist turns the forearm).
            const glm::dquat rs = rotationOf(w[size_t(l.source)].world[size_t(l.sourceBone)]);
            const glm::dquat rt = rotationOf(w[size_t(l.target)].world[size_t(l.targetBone)]);
            if (share < 1.0 - 1e-9 || gaps[c].rotation)
            {
                fbik::Effector eff{.bone = l.sourceBone, .point = l.sourcePoint, .position = s + e * (1.0 - share)};
                if (gaps[c].rotation)
                    eff.rotation = glm::normalize(glm::slerp(rs, glm::normalize(rt * *gaps[c].rotation), 1.0 - share));
                problems[size_t(l.source)].effectors.push_back(eff);
                moved[size_t(l.source)] = true;
            }
            if (share > 1e-9)
            {
                fbik::Effector eff{.bone = l.targetBone, .point = l.targetPoint, .position = t - e * share};
                if (gaps[c].rotation)
                    eff.rotation = glm::normalize(glm::slerp(rt, glm::normalize(rs * glm::conjugate(*gaps[c].rotation)), share));
                problems[size_t(l.target)].effectors.push_back(eff);
                moved[size_t(l.target)] = true;
            }
        }
        for (const auto& [key, pair] : pushers)
        {
            const auto [fi, ci, fj, cj] = key;
            // Lighter than a contact, so where the two disagree the contact holds and the bodies part around it.
            problems[size_t(fi)].effectors.push_back({.bone = pair.first.bone, .point = pair.first.point, .position = pair.first.target, .weight = kPushWeight});
            problems[size_t(fj)].effectors.push_back({.bone = pair.second.bone, .point = pair.second.point, .position = pair.second.target, .weight = kPushWeight});
            moved[size_t(fi)] = moved[size_t(fj)] = true;
        }

        for (size_t i = 0; i < n; ++i)
        {
            if (!moved[i])
                continue;
            fbik::Problem& p = problems[i];
            for (int h : figures[i].holds)
                if (h >= 0 && size_t(h) < p.bones.size())
                {
                    const glm::dmat4& a = animated[i][size_t(h)];
                    p.effectors.push_back({.bone = h, .position = glm::dvec3(a[3]), .rotation = rotationOf(a)});
                }
            if (model && std::ranges::all_of(figures[i].points, [&](int b) { return b >= 0 && size_t(b) < p.bones.size(); }))
            {
                autopose::Points current;
                autopose::Pins pins;
                for (int k = 0; k < autopose::kPointCount; ++k)
                {
                    const int bone = figures[i].points[size_t(k)];
                    current[size_t(k)] = glm::dvec3(w[i].world[size_t(bone)][3]);
                    for (const fbik::Effector& e : p.effectors)
                        if (e.bone == bone && e.position && glm::length2(e.point) < 1e-12)
                            pins[size_t(k)] = e.position;
                }
                const autopose::Points want = model->predict(current, pins);
                for (int k = 0; k < autopose::kPointCount; ++k)
                    if (!pins[size_t(k)])
                        p.effectors.push_back({.bone = figures[i].points[size_t(k)], .position = want[size_t(k)], .weight = 0.5, .soft = true});
            }
            freeze(p);
            if (round > 0)
                p.iterations = kLaterIterations;
            const fbik::Solution s = fbik::solve(p);
            w[i].pose = s;
            w[i].world = fbik::forward(figures[i].problem, s.rotation, s.offset, s.stretch);
        }

        out.iterations = round + 1;
        const auto [deepest, added] = collisions(true);
        double worst = 0.0, turned = 0.0;
        for (size_t c = 0; c < links.size(); ++c)
            if (valid(links[c]))
            {
                worst = std::max(worst, glm::length(errorOf(c)));
                if (gaps[c].rotation)
                {
                    const Link& l = links[c];
                    const glm::dquat want = rotationOf(w[size_t(l.target)].world[size_t(l.targetBone)]) * *gaps[c].rotation;
                    const glm::dquat now = rotationOf(w[size_t(l.source)].world[size_t(l.sourceBone)]);
                    turned = std::max(turned, glm::degrees(2.0 * std::acos(std::clamp(std::abs(glm::dot(want, now)), 0.0, 1.0))));
                }
            }
        out.contactError = worst;
        out.penetration = std::max(deepest, 0.0);
        if (worst <= o.tolerance && turned * kCmPerDegree <= o.tolerance && deepest <= o.tolerance && !added)
            break;
    }
    for (size_t i = 0; i < n; ++i)
        out.poses.push_back(std::move(w[i].pose));
    return out;
}

// Scene ------------------------------------------------------------------------------------------------------------

const char* fitName(Fit f)
{
    switch (f)
    {
    case Fit::Markers: return "markers";
    case Fit::Skin: return "skin";
    case Fit::Bones: return "bones";
    }
    return "bones";
}

namespace
{
Fit fitFrom(const std::string& s) { return s == "markers" ? Fit::Markers : s == "skin" ? Fit::Skin : Fit::Bones; }

int attrIndex(const Node& n, const char* name) { return n.type().attrIndex(name); }
double num(const Node& n, const char* name, double fallback = 0.0)
{
    const int a = attrIndex(n, name);
    return a < 0 ? fallback : n.as<double>(a);
}
bool flag(const Node& n, const char* name, bool fallback = false)
{
    const int a = attrIndex(n, name);
    return a < 0 ? fallback : n.as<bool>(a);
}
int64_t whole(const Node& n, const char* name, int64_t fallback = 0)
{
    const int a = attrIndex(n, name);
    return a < 0 ? fallback : n.as<int64_t>(a);
}
std::string text(const Node& n, const char* name)
{
    const int a = attrIndex(n, name);
    return a < 0 ? std::string() : n.as<std::string>(a);
}
glm::dvec3 vec(const Node& n, const char* name)
{
    const int a = attrIndex(n, name);
    return a < 0 ? glm::dvec3(0.0) : n.as<glm::dvec3>(a);
}
Node* named(const Graph& g, const std::string& path)
{
    try
    {
        return path.empty() ? nullptr : g.find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

bool isGrapple(const Node& n) { return n.type().isA(kType); }
bool isContact(const Node& n) { return n.type().isA(kContactType); }

std::vector<NodeId> deformJoints(Graph& g, NodeId root)
{
    std::vector<NodeId> out{root};
    for (NodeId d : g.descendants(root))
        if (Node* n = g.node(d); n && rig::isJoint(*n) && rigel::isDeform(*n))
            out.push_back(d);
    return out;
}

/** The hips by name (pelvis, hips), else the highest joint that branches into two or more deform joints. */
NodeId hipsJoint(Graph& g, NodeId root)
{
    for (NodeId j : deformJoints(g, root))
        if (const std::string b = bareName(g.node(j)->name()); b == "pelvis" || b == "hips" || b.ends_with("_hips") || b.ends_with("_pelvis"))
            return j;
    for (NodeId j : deformJoints(g, root))
    {
        int kids = 0;
        for (NodeId c : g.children(j))
            if (Node* n = g.node(c); n && rig::isJoint(*n) && rigel::isDeform(*n))
                ++kids;
        if (kids >= 2)
            return j;
    }
    return root;
}

/** The child joint a bone runs to: the one continuing the line from its parent, else its first. */
std::optional<NodeId> boneEnd(Graph& g, NodeId joint)
{
    std::vector<NodeId> kids;
    for (NodeId c : g.children(joint))
        if (Node* n = g.node(c); n && rig::isJoint(*n) && rigel::isDeform(*n))
            kids.push_back(c);
    if (kids.empty())
        return std::nullopt;
    const glm::dvec3 at = positionOf(*g.node(joint));
    const NodeId parent = g.parent(joint);
    if (!parent || !rig::isJoint(*g.node(parent)) || kids.size() == 1)
        return kids.front();
    const glm::dvec3 in = at - positionOf(*g.node(parent));
    NodeId best = kids.front();
    double bestDot = -2.0;
    for (NodeId k : kids)
    {
        const glm::dvec3 d = positionOf(*g.node(k)) - at;
        if (glm::length(d) < 1e-6 || glm::length(in) < 1e-6)
            continue;
        if (const double c = glm::dot(glm::normalize(d), glm::normalize(in)); c > bestDot)
            bestDot = c, best = k;
    }
    return best;
}

/** A bone's axis and length in its joint's space (rigid): toward its end joint, else on from its parent. */
std::pair<glm::dvec3, double> boneAxis(Graph& g, NodeId joint)
{
    const glm::dmat4 inv = glm::inverse(rigid(worldOf(*g.node(joint))));
    if (const auto end = boneEnd(g, joint))
    {
        const glm::dvec3 d = pointOn(inv, positionOf(*g.node(*end)));
        if (const double l = glm::length(d); l > 1e-6)
            return {d / l, l};
    }
    const NodeId parent = g.parent(joint);
    if (parent && rig::isJoint(*g.node(parent)))
    {
        const glm::dvec3 d = -pointOn(inv, positionOf(*g.node(parent)));
        if (const double l = glm::length(d); l > 1e-6)
            return {d / l, l * 0.5};
    }
    return {glm::dvec3(1.0, 0.0, 0.0), 10.0};
}

std::optional<FigureSetup::Body> fromMarker(Graph& g, const Node& m, NodeId joint, double& mass)
{
    const int64_t shape = whole(m, "shape");
    const double r = std::max(num(m, "radius", 5.0), 0.5);
    const glm::dvec3 off = vec(m, "shapeOffset");
    const glm::dmat3 rot = glm::dmat3(eulerToMatrix(vec(m, "shapeRotate"), RotateOrder::XYZ));
    FigureSetup::Body b{.joint = joint, .a = off, .b = off, .radius = r};
    double volume = 0.0;
    if (shape == 0)
    {
        const double half = std::max(num(m, "length", 20.0), 0.0) * 0.5;
        b.a = off - rot * glm::dvec3(half, 0.0, 0.0);
        b.b = off + rot * glm::dvec3(half, 0.0, 0.0);
        volume = capsuleVolume(r, half * 2.0);
    }
    else if (shape == 1)
    {
        // A box as the capsule along its longest side, as thick as its smaller sides.
        const glm::dvec3 e = vec(m, "extents") * 0.5;
        int longest = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
        glm::dvec3 axis(0.0);
        axis[longest] = 1.0;
        b.radius = std::max(0.5, (e[(longest + 1) % 3] + e[(longest + 2) % 3]) * 0.5);
        const double half = std::max(e[longest] - b.radius, 0.0);
        b.a = off - rot * axis * half;
        b.b = off + rot * axis * half;
        volume = 8.0 * e.x * e.y * e.z;
    }
    else
        volume = 4.0 / 3.0 * kPi * r * r * r;
    const double given = num(m, "mass", 0.0);
    mass += given > 0.0 ? given : volume * std::max(num(m, "density", 1000.0), 1e-3) / 1e6;
    (void)g;
    return b;
}

std::optional<FigureSetup::Body> fromSkin(Graph& g, NodeId joint)
{
    const glm::dmat4 toJoint = glm::inverse(rigid(worldOf(*g.node(joint))));
    std::vector<glm::dvec3> pts;
    for (NodeId id : g.nodes())
    {
        Node* mesh = g.node(id);
        if (!mesh || !mesh->type().isA("mesh"))
            continue;
        const std::vector<uint32_t> faces = rig::dominatedFaces(g, id, joint);
        if (faces.empty())
            continue;
        const int outMesh = attrIndex(*mesh, "outMesh");
        if (outMesh < 0)
            continue;
        const PlugValue v = g.value({id, outMesh, -1});
        const MeshPtr* mp = std::get_if<MeshPtr>(&v);
        if (!mp || !*mp)
            continue;
        const geo::Mesh& m = **mp;
        const glm::dmat4 toBone = toJoint * mesh->as<glm::dmat4>(Transform::kWorldMatrix);
        std::set<geo::Index> seen;
        for (uint32_t f : faces)
            if (m.faceValid(f))
                m.forEachFaceCorner(f, [&](geo::Index c) {
                    if (const geo::Index p = m.cornerPoint(c); seen.insert(p).second)
                        pts.push_back(pointOn(toBone, glm::dvec3(m.position(p))));
                });
    }
    if (pts.size() < 4)
        return std::nullopt;
    const auto [axis, length] = boneAxis(g, joint);
    double lo = 1e300, hi = -1e300;
    glm::dvec3 centre(0.0);
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p, axis);
        lo = std::min(lo, t), hi = std::max(hi, t);
        centre += p - axis * t;
    }
    centre /= double(pts.size());
    double r = 0.0;
    for (const glm::dvec3& p : pts)
        r += glm::length(p - axis * glm::dot(p, axis) - centre);
    r = std::max(r / double(pts.size()) * 1.1, 0.5);
    const double a = std::min(lo + r, (lo + hi) * 0.5), b = std::max(hi - r, (lo + hi) * 0.5);
    (void)length;
    return FigureSetup::Body{.joint = joint, .a = centre + axis * a, .b = centre + axis * b, .radius = r};
}

FigureSetup::Body fromBone(Graph& g, NodeId joint)
{
    const auto [axis, length] = boneAxis(g, joint);
    const double r = std::clamp(length * 0.2, 2.5, 14.0);
    return {.joint = joint, .a = glm::dvec3(0.0), .b = axis * length, .radius = r};
}

json vec3Json(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 jsonVec3(const json& j) { return j.is_array() && j.size() == 3 ? glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : glm::dvec3(0.0); }
} // namespace

NodeId skeletonRoot(Graph& g, NodeId joint)
{
    while (g.parent(joint) != kNoNode && rig::isJoint(*g.node(g.parent(joint))))
        joint = g.parent(joint);
    return joint;
}

FigureSetup fitFigure(Graph& g, NodeId root)
{
    LILA_ZONE();
    g.evaluate();
    FigureSetup out;
    out.root = root;
    const NodeId hips = hipsJoint(g, root);
    std::map<NodeId, const Node*> markers;
    for (NodeId id : g.nodes())
        if (Node* m = g.node(id); m && m->type().name == "physicsMarker" && flag(*m, "enabled", true) && whole(*m, "behaviour") != 3)
            if (Node* j = named(g, text(*m, "joint")))
                markers.emplace(j->id(), m);

    std::vector<NodeId> joints;
    for (NodeId j : deformJoints(g, root))
    {
        const std::string bare = bareName(g.node(j)->name());
        const bool aboveHips = j != hips && !isUnder(g, j, hips);
        if (aboveHips || isFinger(bare) || has(bare, "twist") || bare.starts_with("ik_"))
            continue;
        joints.push_back(j);
    }
    double markerMass = 0.0;
    int fromMarkers = 0, fromSkins = 0;
    for (NodeId j : joints)
        if (auto it = markers.find(j); it != markers.end())
        {
            if (auto b = fromMarker(g, *it->second, j, markerMass))
            {
                out.bodies.push_back(*b);
                ++fromMarkers;
            }
        }
    if (fromMarkers == 0)
        for (NodeId j : joints)
            if (auto b = fromSkin(g, j))
            {
                out.bodies.push_back(*b);
                ++fromSkins;
            }
    if (fromMarkers == 0 && fromSkins == 0)
        for (NodeId j : joints)
            if (boneAxis(g, j).second >= 2.0)
                out.bodies.push_back(fromBone(g, j));
    out.fit = fromMarkers ? Fit::Markers : fromSkins ? Fit::Skin : Fit::Bones;
    if (out.fit == Fit::Markers)
        out.mass = markerMass;
    else
    {
        double volume = 0.0;
        for (const FigureSetup::Body& b : out.bodies)
            volume += capsuleVolume(b.radius, glm::length(b.b - b.a));
        out.mass = volume * kDensity;
    }
    out.mass = std::clamp(out.mass, 1.0, 1000.0);
    return out;
}

std::vector<FigureSetup> figuresOf(Graph& g, NodeId grapple)
{
    std::vector<FigureSetup> out;
    Node* n = g.node(grapple);
    if (!n || !isGrapple(*n))
        return out;
    json j = json::parse(text(*n, "figures"), nullptr, false);
    if (!j.is_array())
        return out;
    for (const json& f : j)
    {
        Node* root = named(g, f.value("root", std::string()));
        if (!root)
            continue;
        FigureSetup s;
        s.root = root->id();
        s.mass = f.value("mass", 70.0);
        s.plant = f.value("plant", true);
        s.fit = fitFrom(f.value("fit", std::string("bones")));
        for (const json& b : f.value("bodies", json::array()))
            if (Node* joint = named(g, b.value("joint", std::string())))
                s.bodies.push_back({.joint = joint->id(), .a = jsonVec3(b.value("a", json())), .b = jsonVec3(b.value("b", json())), .radius = b.value("radius", 5.0)});
        out.push_back(std::move(s));
    }
    return out;
}

void setFigures(Graph& g, NodeId grapple, const std::vector<FigureSetup>& figures)
{
    json j = json::array();
    for (const FigureSetup& f : figures)
    {
        json bodies = json::array();
        for (const FigureSetup::Body& b : f.bodies)
            bodies.push_back({{"joint", fullPath(g, b.joint)}, {"a", vec3Json(b.a)}, {"b", vec3Json(b.b)}, {"radius", b.radius}});
        j.push_back({{"root", fullPath(g, f.root)}, {"mass", f.mass}, {"plant", f.plant}, {"fit", fitName(f.fit)}, {"bodies", bodies}});
    }
    g.setValue({grapple, attrIndex(*g.node(grapple), "figures"), -1}, j.dump());
}

std::vector<NodeId> contactsOf(Graph& g, NodeId grapple)
{
    std::vector<NodeId> out;
    for (NodeId c : g.children(grapple))
        if (Node* n = g.node(c); n && isContact(*n))
            out.push_back(c);
    std::sort(out.begin(), out.end());
    return out;
}

NodeId grappleOf(Graph& g, NodeId node)
{
    for (NodeId c = node; c; c = g.parent(c))
        if (Node* n = g.node(c); n && isGrapple(*n))
            return c;
    return kNoNode;
}

std::pair<NodeId, NodeId> contactJoints(Graph& g, NodeId contact)
{
    Node* c = g.node(contact);
    if (!c || !isContact(*c))
        return {kNoNode, kNoNode};
    Node* s = named(g, text(*c, "source"));
    Node* t = named(g, text(*c, "target"));
    return {s ? s->id() : kNoNode, t ? t->id() : kNoNode};
}

std::pair<glm::dvec3, glm::dvec3> contactPoints(Graph& g, NodeId contact)
{
    const auto [s, t] = contactJoints(g, contact);
    if (!s || !t)
        return {glm::dvec3(0.0), glm::dvec3(0.0)};
    return {positionOf(*g.node(s)), pointOn(rigid(worldOf(*g.node(t))), vec(*g.node(contact), "point"))};
}

std::vector<PosedCapsule> posedCapsules(Graph& g, const FigureSetup& figure)
{
    std::vector<PosedCapsule> out;
    for (const FigureSetup::Body& b : figure.bodies)
        if (Node* j = g.node(b.joint))
        {
            const glm::dmat4 w = rigid(worldOf(*j));
            out.push_back({b.joint, pointOn(w, b.a), pointOn(w, b.b), b.radius});
        }
    return out;
}

// The simulation ---------------------------------------------------------------------------------------------------

namespace
{
enum Lead : int64_t
{
    kLeadAuto,
    kLeadSource,
    kLeadTarget,
    kLeadEven,
};

/** One character as the solver reads it once per configure. */
struct FigureRig
{
    FigureSetup setup;
    std::vector<NodeId> joints;
    fbik::Problem base;
    std::vector<int> holds;
    std::vector<std::pair<NodeId, int>> chains;
    Figure figure;
};

struct LinkRig
{
    NodeId node = kNoNode;
    int weightAttr = -1;
    Link link;
};

constexpr size_t kBoneInputs = 36;

void putMatrix(std::vector<double>& out, const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out.push_back(m[c][r]);
}
glm::dmat4 getMatrix(const double* in)
{
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = in[c * 4 + r];
    return m;
}

/** Puppeteer characters' pinned joints under root, read by name (the character type is an extension's). */
std::vector<NodeId> characterPins(Graph& g, NodeId root)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
    {
        Node* c = g.node(id);
        if (!c || c->type().name != "puppeteerCharacter")
            continue;
        Node* r = named(g, text(*c, "root"));
        if (!r || r->id() != root)
            continue;
        std::istringstream in(text(*c, "pins"));
        for (std::string w; in >> w;)
            if (Node* n = named(g, w); n && rig::isJoint(*n))
                out.push_back(n->id());
    }
    return out;
}

class GrappleSolver final : public sim::Solver
{
public:
    explicit GrappleSolver(NodeId node) : mNode(node) {}

    sim::Setup configure(Graph& g) override
    {
        Node* gn = g.node(mNode);
        if (!gn || !isGrapple(*gn))
            throw std::invalid_argument("the grapple is gone");
        mEnabled = flag(*gn, "enabled", true);
        mOptions.collide = flag(*gn, "collide", true);
        mOptions.margin = std::max(num(*gn, "margin", 0.5), 0.0);
        mOptions.autoPose = flag(*gn, "autoPose", true);
        mOptions.iterations = int(std::clamp<int64_t>(whole(*gn, "iterations", 12), 1, 64));

        std::vector<FigureSetup> stored = figuresOf(g, mNode);
        mFigures.clear();
        mLinks.clear();
        auto figureFor = [&](NodeId joint) -> int {
            const NodeId root = skeletonRoot(g, joint);
            for (size_t i = 0; i < mFigures.size(); ++i)
                if (mFigures[i].setup.root == root)
                    return int(i);
            FigureRig f;
            auto it = std::find_if(stored.begin(), stored.end(), [&](const FigureSetup& s) { return s.root == root; });
            f.setup = it != stored.end() ? *it : fitFigure(g, root);
            build(g, f);
            mFigures.push_back(std::move(f));
            return int(mFigures.size()) - 1;
        };
        // Figures stored on the grapple come first, in their order, so the solve's order doesn't depend on the contacts'.
        for (const FigureSetup& s : stored)
            figureFor(s.root);

        uint64_t h = sim::hashBytes(&mEnabled, sizeof mEnabled);
        const std::string figuresText = text(*gn, "figures");
        h = sim::hashBytes(figuresText.data(), figuresText.size(), h);
        for (double v : {double(mOptions.collide), mOptions.margin, double(mOptions.autoPose), double(mOptions.iterations)})
            h = sim::hashBytes(&v, sizeof v, h);
        for (NodeId c : contactsOf(g, mNode))
        {
            const Node& cn = *g.node(c);
            if (!flag(cn, "enabled", true))
                continue;
            const auto [s, t] = contactJoints(g, c);
            if (!s || !t)
                continue;
            LinkRig l;
            l.node = c;
            l.weightAttr = attrIndex(cn, "weight");
            l.link.source = figureFor(s);
            l.link.target = figureFor(t);
            const FigureRig &fs = mFigures[size_t(l.link.source)], &ft = mFigures[size_t(l.link.target)];
            const auto sb = std::find(fs.joints.begin(), fs.joints.end(), s), tb = std::find(ft.joints.begin(), ft.joints.end(), t);
            if (sb == fs.joints.end() || tb == ft.joints.end())
                continue;
            l.link.sourceBone = int(sb - fs.joints.begin());
            l.link.targetBone = int(tb - ft.joints.begin());
            // The point rides the target joint rigidly; the solve's bone frames carry the joint's scale.
            const glm::dmat4 tw = worldOf(*g.node(t));
            l.link.targetPoint = pointOn(glm::inverse(tw), pointOn(rigid(tw), vec(cn, "point")));
            if (flag(cn, "orient"))
                l.link.rotation = glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(vec(cn, "rotation"), RotateOrder::XYZ))));
            const double ms = fs.setup.mass, mt = ft.setup.mass;
            switch (whole(cn, "lead"))
            {
            case kLeadSource: l.link.share = 1.0; break;
            case kLeadTarget: l.link.share = 0.0; break;
            case kLeadEven: l.link.share = 0.5; break;
            default: l.link.share = l.link.source == l.link.target ? 0.5 : shareByMass(ms, mt); break;
            }
            for (const std::string& s2 : {text(cn, "source"), text(cn, "target")})
                h = sim::hashBytes(s2.data(), s2.size(), h);
            const glm::dquat q = l.link.rotation.value_or(glm::dquat(0, 0, 0, 0));
            for (double v : {l.link.targetPoint.x, l.link.targetPoint.y, l.link.targetPoint.z, l.link.share, q.w, q.x, q.y, q.z})
                h = sim::hashBytes(&v, sizeof v, h);
            mLinks.push_back(l);
        }

        sim::Setup su;
        su.startFrame = whole(*gn, "startFrame", 0);
        size_t outputs = 0;
        for (const FigureRig& f : mFigures)
        {
            su.outputs.insert(su.outputs.end(), f.joints.begin(), f.joints.end());
            outputs += f.joints.size();
            h = sim::hashBytes(f.joints.data(), f.joints.size() * sizeof(NodeId), h);
        }
        mOutputs = outputs;
        su.layout.push_back({sim::Channel::Ignore, uint32_t(outputs * 16 + 3)});
        su.sources.push_back(mNode);
        for (const LinkRig& l : mLinks)
            su.sources.push_back(l.node);
        su.reads = su.outputs;
        su.reads.insert(su.reads.end(), su.sources.begin(), su.sources.end());
        su.paramsHash = h;
        return su;
    }

    void sample(Graph& g, std::vector<double>& in) override
    {
        for (const FigureRig& f : mFigures)
        {
            for (NodeId j : f.joints)
            {
                const Node& n = *g.node(j);
                glm::dmat4 before, after;
                Transform::splitLocal(n, before, after);
                putMatrix(in, before);
                putMatrix(in, after);
                const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(static_cast<const Transform&>(n).rotation())));
                in.insert(in.end(), {q.w, q.x, q.y, q.z});
            }
            putMatrix(in, g.node(f.joints.front())->as<glm::dmat4>(Transform::kParentMatrix));
            for (const auto& [chain, bone] : f.chains)
                in.push_back(rigel::locked(g, chain, g.time()) ? 1.0 : 0.0);
        }
        for (const LinkRig& l : mLinks)
            in.push_back(l.weightAttr >= 0 ? std::get<double>(g.value({l.node, l.weightAttr, -1})) : 1.0);
    }

    void reset(const std::vector<double>& in, std::vector<double>& state) override { solveFrame(in, state); }
    void step(const std::vector<double>& in, std::vector<double>& state, const sim::StepInfo&) override { solveFrame(in, state); }

    void pose(const std::vector<double>&, const std::vector<double>& state, std::vector<glm::dmat4>& world) override
    {
        world.assign(mOutputs, glm::dmat4(1.0));
        for (size_t i = 0; i < mOutputs && (i + 1) * 16 <= state.size(); ++i)
            world[i] = getMatrix(&state[i * 16]);
    }

private:
    /** The static half of a figure's problem: its bones, what solves, stiffness, its holds and auto-pose points. */
    static void build(Graph& g, FigureRig& f)
    {
        fbik::PoseOptions opts;
        opts.moveRoot = true;
        std::vector<NodeId> all = deformJoints(g, f.setup.root);
        for (NodeId j : all)
            if (const double s = stiffnessByName(bareName(g.node(j)->name())); s > 0.0)
                opts.stiffness.push_back({j, s});
        f.base = fbik::problemFor(g, f.setup.root, opts, f.joints);
        const NodeId hips = hipsJoint(g, f.setup.root);
        // Above the hips is root motion, kept as animated; fingers never carry a contact, and leaving them out keeps the solve small.
        for (size_t k = 0; k < f.joints.size(); ++k)
            if ((f.joints[k] != hips && !isUnder(g, f.joints[k], hips)) || isFinger(bareName(g.node(f.joints[k])->name())))
                f.base.bones[k].solve = false;
        auto boneOf = [&](NodeId j) {
            const auto it = std::find(f.joints.begin(), f.joints.end(), j);
            return it == f.joints.end() ? -1 : int(it - f.joints.begin());
        };
        if (f.setup.plant)
            for (size_t k = 0; k < f.joints.size(); ++k)
                if (const std::string b = bareName(g.node(f.joints[k])->name()); has(b, "foot") && !b.starts_with("ik_") && f.base.bones[k].solve)
                    f.holds.push_back(int(k));
        for (NodeId p : characterPins(g, f.setup.root))
            if (const int b = boneOf(p); b >= 0)
                f.holds.push_back(b);
        for (NodeId chain : rigel::chains(g))
            if (const std::vector<NodeId> js = rigel::chainJoints(g, chain); !js.empty())
                if (const int b = boneOf(js.back()); b >= 0)
                    f.chains.push_back({chain, b});
        for (const FigureSetup::Body& b : f.setup.bodies)
            if (const int bone = boneOf(b.joint); bone >= 0)
            {
                // Stored in the joint's rigid frame; the solve's bone frames carry its scale.
                const glm::dmat4 w = worldOf(*g.node(b.joint));
                const glm::dmat4 toBone = glm::inverse(w) * rigid(w);
                f.figure.capsules.push_back({bone, pointOn(toBone, b.a), pointOn(toBone, b.b), b.radius});
            }
        f.figure.mass = f.setup.mass;
        for (int k = 0; k < autopose::kPointCount; ++k)
            for (size_t b = 0; b < f.joints.size(); ++b)
                if (bareName(g.node(f.joints[b])->name()) == autopose::kPoints[size_t(k)])
                    f.figure.points[size_t(k)] = int(b);
    }

    void solveFrame(const std::vector<double>& in, std::vector<double>& state)
    {
        state.assign(mOutputs * 16 + 3, 0.0);
        std::vector<Figure> figures;
        size_t at = 0;
        for (FigureRig& f : mFigures)
        {
            Figure fig = f.figure;
            fig.problem = f.base;
            for (size_t k = 0; k < f.joints.size(); ++k)
            {
                fbik::Bone& b = fig.problem.bones[k];
                b.before = getMatrix(&in[at]);
                b.after = getMatrix(&in[at + 16]);
                b.rotation = b.reference = glm::normalize(glm::dquat(in[at + 32], in[at + 33], in[at + 34], in[at + 35]));
                at += kBoneInputs;
            }
            fig.problem.bones.front().parentWorld = getMatrix(&in[at]);
            at += 16;
            fig.holds = f.holds;
            for (const auto& [chain, bone] : f.chains)
                if (in[at++] > 0.5)
                    fig.holds.push_back(bone);
            figures.push_back(std::move(fig));
        }
        std::vector<Link> links;
        for (const LinkRig& l : mLinks)
        {
            Link k = l.link;
            k.weight = mEnabled ? std::clamp(in[at++], 0.0, 1.0) : 0.0;
            links.push_back(k);
        }
        Result r;
        const bool any = mEnabled && std::ranges::any_of(links, [](const Link& l) { return l.weight > 1e-9; });
        if (mEnabled && (any || mOptions.collide))
            r = solve(figures, links, mOptions);
        size_t o = 0;
        for (size_t i = 0; i < figures.size(); ++i)
        {
            const fbik::Problem& p = figures[i].problem;
            std::vector<glm::dmat4> w;
            if (i < r.poses.size())
                w = fbik::forward(p, r.poses[i].rotation, r.poses[i].offset, r.poses[i].stretch);
            else
            {
                std::vector<glm::dquat> rot;
                for (const fbik::Bone& b : p.bones)
                    rot.push_back(b.rotation);
                w = fbik::forward(p, rot, std::vector<glm::dvec3>(p.bones.size(), glm::dvec3(0.0)));
            }
            for (const glm::dmat4& m : w)
            {
                std::vector<double> flat;
                putMatrix(flat, m);
                std::copy(flat.begin(), flat.end(), state.begin() + std::ptrdiff_t(o * 16));
                ++o;
            }
        }
        state[mOutputs * 16] = r.contactError;
        state[mOutputs * 16 + 1] = r.penetration;
        state[mOutputs * 16 + 2] = double(r.iterations);
    }

    NodeId mNode = kNoNode;
    bool mEnabled = true;
    Options mOptions;
    std::vector<FigureRig> mFigures;
    std::vector<LinkRig> mLinks;
    size_t mOutputs = 0;
};

AttrDef attr(const char* name, PlugType type, PlugValue def, uint32_t extra, const char* doc)
{
    AttrDef a{.name = name, .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | extra, .doc = doc};
    if (type == PlugType::Vec3)
    {
        const std::string n = name;
        a.childNames = {n + "X", n + "Y", n + "Z"};
    }
    return a;
}

AttrDef bounded(AttrDef a, std::optional<double> lo, std::optional<double> hi = {})
{
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

AttrDef choice(const char* name, int64_t def, std::vector<std::string> names, const char* doc)
{
    AttrDef a{.name = name, .type = PlugType::Enum, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
    a.enumNames = std::move(names);
    return a;
}
} // namespace

void registerTypes()
{
    if (types::find(kType))
        return;
    const NodeType* transform = types::find("transform");
    if (!transform)
        return;

    NodeType grapple{.name = kType, .base = transform, .dag = true,
                     .doc = "Grapple: contacts between characters, solved together every frame (each side yields its share, the "
                            "auto-poser places the rest, bodies kept apart) and baked onto an additive layer as plain keys.",
                     .icon = "users", .create = transform->create};
    grapple.ownAttrs = {
        attr("enabled", PlugType::Bool, true, 0, "Solve the contacts. Off, every character keeps its animation."),
        attr("startFrame", PlugType::Int, int64_t(0), 0, "First frame solved; frames before it use it."),
        attr("collide", PlugType::Bool, true, 0, "Keep the characters' bodies from passing through each other."),
        bounded(attr("margin", PlugType::Float, 0.5, 0, "Gap kept between bodies, cm."), 0.0),
        attr("autoPose", PlugType::Bool, true, 0, "Let the auto-poser place what the contacts don't hold (UE5 Manny joint names)."),
        bounded(attr("iterations", PlugType::Int, int64_t(12), 0, "Rounds of solving the characters against each other per frame."), 1.0, 64.0),
        attr("figures", PlugType::String, std::string("[]"), AttrHidden, "Each character's collision capsules, mass and planted feet, JSON."),
        attr(sim::kRecordingsAttr, PlugType::String, std::string(), AttrHidden, "The layer this grapple was baked to, muted while it samples."),
    };

    NodeType contact{.name = kContactType, .base = transform, .dag = true,
                     .doc = "A grapple contact: holds a joint of one character to a point riding a joint of another, blended in "
                            "and out by its keyable weight.",
                     .icon = "hand-grab", .create = transform->create};
    contact.ownAttrs = {
        attr("enabled", PlugType::Bool, true, 0, "Take part in the solve."),
        attr("source", PlugType::String, std::string(), 0, "The joint that reaches (a hand, an arm, a shoulder)."),
        attr("target", PlugType::String, std::string(), 0, "The joint the contact point rides."),
        attr("point", PlugType::Vec3, glm::dvec3(0.0), 0, "The contact point in the target joint's space, cm."),
        bounded(attr("weight", PlugType::Float, 1.0, AttrKeyable, "0 leaves both as animated, 1 holds the contact. Key it to blend in and out."), 0.0, 1.0),
        attr("orient", PlugType::Bool, false, 0, "Hold the source's rotation against the target too (a grip), not only its position."),
        attr("rotation", PlugType::Vec3, glm::dvec3(0.0), 0, "The source's rotation in the target joint's space when orient is on, XYZ degrees."),
        choice("lead", kLeadAuto, {"auto", "source", "target", "even"},
               "Who yields: auto shares the gap by mass, source brings the target to it, target makes the source reach, even splits it."),
        bounded(attr("stiffness", PlugType::Float, 12.0, AttrKeyable, "Physics: the contact's spring frequency, Hz, at full weight."), 0.0),
    };
    for (NodeType* t : {&grapple, &contact})
    {
        // Contact points live in their target joint's space and figures' capsules in their joints': nothing is world space.
        relocate::registerNeutral(t->name);
        types::registerType(std::move(*t));
    }
    sim::registerSolverType({.nodeType = kType, .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> {
                                 return std::make_unique<GrappleSolver>(node);
                             }});
}

// Commands ---------------------------------------------------------------------------------------------------------

namespace
{
Node& jointArg(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!rig::isJoint(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
    return n;
}

NodeId grappleArg(Graph& g, const Args& a, const char* cmd)
{
    std::vector<std::string> names = a.objects();
    if (a.has("grapple"))
        names.insert(names.begin(), a.get<std::string>("grapple"));
    for (const std::string& s : names)
        if (Node* n = named(g, s))
            if (const NodeId gr = grappleOf(g, n->id()))
                return gr;
    NodeId only = kNoNode;
    for (NodeId id : g.nodes())
        if (isGrapple(*g.node(id)))
        {
            if (only)
                throw CommandError(std::string(cmd) + ": the scene has several grapples; name one");
            only = id;
        }
    if (!only)
        throw CommandError(std::string(cmd) + ": no grapple; make contacts with grappleContact first");
    return only;
}

void setAttr(Graph& g, NodeId id, const char* name, PlugValue v) { g.setValue({id, attrIndex(*g.node(id), name), -1}, std::move(v)); }

/** Adds figures for skeleton roots the grapple doesn't have yet. Returns what each new one was fitted from. */
json ensureFigures(Graph& g, NodeId grapple, const std::vector<NodeId>& roots)
{
    std::vector<FigureSetup> figures = figuresOf(g, grapple);
    json fitted = json::array();
    for (NodeId r : roots)
        if (std::none_of(figures.begin(), figures.end(), [&](const FigureSetup& f) { return f.root == r; }))
        {
            figures.push_back(fitFigure(g, r));
            const FigureSetup& f = figures.back();
            fitted.push_back({{"root", g.path(r)}, {"fit", fitName(f.fit)}, {"bodies", f.bodies.size()}, {"mass", f.mass}});
            if (f.fit == Fit::Bones)
                log::warn("grapple: {} has no skinned mesh or physics markers to fit its bodies to; it collides as bone-sized capsules", g.path(r));
        }
    if (!fitted.empty())
        setFigures(g, grapple, figures);
    return fitted;
}

double weightAt(Graph& g, NodeId contact, double time)
{
    const double now = g.time();
    g.setTime(time, false);
    g.evaluate();
    const double v = std::get<double>(g.value({contact, attrIndex(*g.node(contact), "weight"), -1}));
    g.setTime(now, false);
    g.evaluate();
    return v;
}

/** Keys a contact's weight to on (1) or off (0) at time, from what it is blend seconds earlier. */
void keyWeight(Graph& g, NodeId contact, bool on, double time, double blend)
{
    const PlugRef plug{contact, attrIndex(*g.node(contact), "weight"), -1};
    if (blend > 1e-9)
    {
        const double from = weightAt(g, contact, time - blend);
        anim::setKey(g, plug, time - blend, from);
    }
    anim::setKey(g, plug, time, on ? 1.0 : 0.0);
}

/** The point on the target's capsules nearest p, or the target joint's origin when it has none. */
glm::dvec3 nearestOnBody(Graph& g, NodeId grapple, NodeId target, const glm::dvec3& p)
{
    const NodeId root = skeletonRoot(g, target);
    glm::dvec3 best = positionOf(*g.node(target));
    double bestD = 1e300;
    for (const FigureSetup& f : figuresOf(g, grapple))
        if (f.root == root)
            for (const PosedCapsule& c : posedCapsules(g, f))
                if (c.joint == target)
                {
                    const auto [q, unused] = closestPoints(c.a, c.b, p, p);
                    (void)unused;
                    const glm::dvec3 d = p - q;
                    const double l = glm::length(d);
                    const glm::dvec3 surface = l > 1e-9 ? q + d / l * c.radius : q;
                    if (const double dist = glm::length(surface - p); dist < bestD)
                        bestD = dist, best = surface;
                }
    return best;
}

std::vector<std::string> recordedLayers(Graph& g, NodeId grapple)
{
    std::vector<std::string> out;
    json j = json::parse(text(*g.node(grapple), sim::kRecordingsAttr), nullptr, false);
    if (j.is_array())
        for (const json& s : j)
            if (s.is_string())
                out.push_back(s.get<std::string>());
    return out;
}

json contactJson(Graph& g, NodeId c)
{
    const Node& n = *g.node(c);
    static const char* kLeads[] = {"auto", "source", "target", "even"};
    return {{"contact", g.path(c)}, {"source", text(n, "source")}, {"target", text(n, "target")}, {"weight", num(n, "weight")},
            {"orient", flag(n, "orient")}, {"lead", kLeads[std::clamp<int64_t>(whole(n, "lead"), 0, 3)]}, {"enabled", flag(n, "enabled", true)}};
}

json figuresJson(Graph& g, NodeId grapple)
{
    json out = json::array();
    for (const FigureSetup& f : figuresOf(g, grapple))
        out.push_back({{"root", g.path(f.root)}, {"mass", f.mass}, {"plant", f.plant}, {"fit", fitName(f.fit)}, {"bodies", f.bodies.size()}});
    return out;
}
} // namespace

void registerCommands()
{
    if (ops::find("grappleContact"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "grappleContact",
        .doc = "Hold a joint of one character to a point on another (grab a wrist, a collar, an arm round a neck, hands under a body "
               "to lift it). The characters are solved together every frame by their grapple; key the contact's weight to blend it "
               "in and out. Returns the contact.",
        .positionalDoc = "source joint, target joint",
        .flags = {
            {"point", "p", T::Vec3, nullptr, "World point the source is held to, riding the target joint. Default: the target's body surface nearest the source."},
            {"kind", "k", T::String, "grab", "grab holds position and rotation; hold position only; push lets the source lead; lift lets the source "
                                            "lead and frees the target's feet.", {"grab", "hold", "push", "lift"}},
            {"orient", "o", T::Bool, nullptr, "Hold the source's rotation against the target too. Default by kind."},
            {"lead", "l", T::String, nullptr, "Who yields. Default by kind: auto (by mass), or source for push and lift.", {"auto", "source", "target", "even"}},
            nodeFlag({"grapple", "g", T::String, nullptr, "The grapple to add it to. Default: the one holding either character, else a new one."}, kType),
            {"time", "t", T::Float, nullptr, "Seconds the contact comes on at. Default: now."},
            {"blend", "b", T::Float, 0.2, "Seconds it blends in over, keyed from what it was before. 0 makes it on at every time.", {}, {}, 0.0},
            {"name", "n", T::String, nullptr, "Name for the contact."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            const std::vector<std::string> objs = a.objects();
            if (objs.size() != 2)
                throw CommandError("grappleContact: give the source joint and the target joint");
            const NodeId source = jointArg(objs[0], "grappleContact").id(), target = jointArg(objs[1], "grappleContact").id();
            if (source == target)
                throw CommandError("grappleContact: the source and target are the same joint");
            const NodeId rs = skeletonRoot(g, source), rt = skeletonRoot(g, target);
            NodeId grapple = kNoNode;
            if (a.has("grapple"))
            {
                Node& n = nodeNamed(a.get<std::string>("grapple"), "grappleContact");
                if (!isGrapple(n))
                    throw CommandError("grappleContact: " + n.path() + " is not a grapple");
                grapple = n.id();
            }
            else
                for (NodeId id : g.nodes())
                    if (isGrapple(*g.node(id)))
                        for (const FigureSetup& f : figuresOf(g, id))
                            if (!grapple && (f.root == rs || f.root == rt))
                                grapple = id;
            if (!grapple)
                grapple = g.createNode(*types::find(kType), "grapple1")->id();
            const json fitted = ensureFigures(g, grapple, {rs, rt});

            const std::string kind = a.get<std::string>("kind", "grab");
            const bool orient = a.has("orient") ? a.get<bool>("orient") : kind == "grab";
            std::string lead = a.has("lead") ? a.get<std::string>("lead") : (kind == "push" || kind == "lift") ? "source" : "auto";
            const glm::dvec3 sp = positionOf(*g.node(source));
            const glm::dvec3 world = a.has("point") ? vec3Of(a.flags["point"]) : nearestOnBody(g, grapple, target, sp);
            const glm::dmat4 tw = rigid(worldOf(*g.node(target)));

            std::string name = a.has("name") ? a.get<std::string>("name") : g.node(source)->name() + "_to_" + g.node(target)->name();
            std::replace(name.begin(), name.end(), ':', '_');
            const NodeId c = g.createNode(*types::find(kContactType), name, grapple)->id();
            setAttr(g, c, "source", fullPath(g, source));
            setAttr(g, c, "target", fullPath(g, target));
            setAttr(g, c, "point", pointOn(glm::inverse(tw), world));
            setAttr(g, c, "orient", orient);
            const glm::dmat3 rel = glm::transpose(orthonormal(glm::dmat3(tw))) * orthonormal(glm::dmat3(worldOf(*g.node(source))));
            setAttr(g, c, "rotation", xformops::closestEuler(rel, RotateOrder::XYZ, glm::dvec3(0.0)));
            static const std::vector<std::string> kLeads = {"auto", "source", "target", "even"};
            setAttr(g, c, "lead", int64_t(std::find(kLeads.begin(), kLeads.end(), lead) - kLeads.begin()));
            if (kind == "lift")
            {
                std::vector<FigureSetup> figures = figuresOf(g, grapple);
                for (FigureSetup& f : figures)
                    if (f.root == rt && rt != rs)
                        f.plant = false;
                setFigures(g, grapple, figures);
            }
            const double blend = a.get<double>("blend", 0.2);
            if (blend > 0.0)
            {
                const double time = a.has("time") ? a.get<double>("time") : g.time();
                const PlugRef plug{c, attrIndex(*g.node(c), "weight"), -1};
                anim::setKey(g, plug, time - blend, 0.0);
                anim::setKey(g, plug, time, 1.0);
            }
            g.evaluate();
            json out = contactJson(g, c);
            out["grapple"] = g.path(grapple);
            if (!fitted.empty())
                out["fitted"] = fitted;
            return out;
        },
        .examples = {"grappleContact(\"attacker:hand_r\", \"defender:lowerarm_l\", kind=\"grab\")",
                     "grappleContact(\"lifter:hand_l\", \"lifted:spine_02\", kind=\"lift\", blend=0.3)"},
    });

    ops::registerCommand({
        .name = "grappleKey",
        .doc = "Key contacts on or off: their weight reaches 1 (on) or 0 (off) at the time, blending from what it was before. "
               "Returns the contacts.",
        .positionalDoc = "contacts (default the selected ones)",
        .flags = {
            {"on", "", T::Bool, true, "True keys them on, False off."},
            {"time", "t", T::Float, nullptr, "Seconds. Default: now."},
            {"blend", "b", T::Float, 0.2, "Seconds the blend takes.", {}, {}, 0.0},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> contacts;
            for (const std::string& s : a.objects())
            {
                Node& n = nodeNamed(s, "grappleKey");
                if (isContact(n))
                    contacts.push_back(n.id());
                else if (isGrapple(n))
                    for (NodeId c : contactsOf(g, n.id()))
                        contacts.push_back(c);
                else
                    throw CommandError("grappleKey: " + n.path() + " is not a grapple contact");
            }
            if (contacts.empty())
                throw CommandError("grappleKey: give or select contacts");
            const double time = a.has("time") ? a.get<double>("time") : g.time();
            json out = json::array();
            for (NodeId c : contacts)
            {
                keyWeight(g, c, a.get<bool>("on", true), time, a.get<double>("blend", 0.2));
                out.push_back(g.path(c));
            }
            g.evaluate();
            return out;
        },
        .examples = {"grappleKey(\"hand_r_to_lowerarm_l\", on=False, time=2.0)"},
    });

    ops::registerCommand({
        .name = "grappleFigure",
        .doc = "A character's settings in its grapple: its mass (who yields: the heavier side moves less) and whether its feet stay "
               "where the animation plants them. Returns every figure of the grapple.",
        .positionalDoc = "a joint of the character",
        .flags = {
            nodeFlag({"grapple", "g", T::String, nullptr, "The grapple. Default: the one holding the character."}, kType),
            {"mass", "m", T::Float, nullptr, "kg.", {}, {}, 1.0},
            {"plant", "pl", T::Bool, nullptr, "Keep the feet where the animation puts them."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.objects().size() != 1)
                throw CommandError("grappleFigure: give a joint of the character");
            const NodeId root = skeletonRoot(g, jointArg(a.objects()[0], "grappleFigure").id());
            NodeId grapple = kNoNode;
            if (a.has("grapple"))
                grapple = grappleArg(g, a, "grappleFigure");
            else
                for (NodeId id : g.nodes())
                    if (isGrapple(*g.node(id)))
                        for (const FigureSetup& f : figuresOf(g, id))
                            if (f.root == root)
                                grapple = id;
            if (!grapple)
                throw CommandError("grappleFigure: " + g.path(root) + " is in no grapple");
            ensureFigures(g, grapple, {root});
            std::vector<FigureSetup> figures = figuresOf(g, grapple);
            for (FigureSetup& f : figures)
                if (f.root == root)
                {
                    if (a.has("mass"))
                        f.mass = std::max(a.get<double>("mass"), 1.0);
                    if (a.has("plant"))
                        f.plant = a.get<bool>("plant");
                }
            setFigures(g, grapple, figures);
            return figuresJson(g, grapple);
        },
    });

    ops::registerCommand({
        .name = "fitGrappleBodies",
        .doc = "Refit the grapple's collision capsules and masses at the current pose: from the characters' physics markers, else "
               "their skinned meshes, else bone-sized. Keeps each figure's planted feet. Returns the figures with what each was fitted from.",
        .positionalDoc = "grapple or one of its contacts (default the selection, else the scene's only grapple)",
        .flags = {nodeFlag({"grapple", "g", T::String, nullptr, "The grapple."}, kType),
                  {"keepMass", "km", T::Bool, false, "Keep each figure's mass as it is."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId grapple = grappleArg(g, a, "fitGrappleBodies");
            std::vector<FigureSetup> figures = figuresOf(g, grapple);
            for (FigureSetup& f : figures)
            {
                FigureSetup fresh = fitFigure(g, f.root);
                fresh.plant = f.plant;
                if (a.get<bool>("keepMass", false))
                    fresh.mass = f.mass;
                if (fresh.fit == Fit::Bones)
                    log::warn("grapple: {} has no skinned mesh or physics markers to fit its bodies to; it collides as bone-sized capsules", g.path(f.root));
                f = std::move(fresh);
            }
            setFigures(g, grapple, figures);
            return figuresJson(g, grapple);
        },
    });

    ops::registerCommand({
        .name = "bakeGrapple",
        .doc = "Bake the grapple onto the characters' joints as plain keys, on its own additive layer (replacing the one it baked "
               "before; mute the layer to compare, delete it to go back). Every frame is solved with the characters' animation below "
               "the layer. Returns {'layer', 'frames', 'contactError', 'penetration'}: the worst contact gap and body overlap left, cm.",
        .positionalDoc = "grapple or one of its contacts (default the selection, else the scene's only grapple)",
        .flags = {nodeFlag({"grapple", "g", T::String, nullptr, "The grapple."}, kType),
                  {"start", "", T::Int, nullptr, "First frame. Default: the playback range."},
                  {"end", "", T::Int, nullptr, "Last frame."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = graph();
            const NodeId grapple = grappleArg(g, a, "bakeGrapple");
            for (const std::string& layer : recordedLayers(g, grapple))
                if (named(g, layer))
                    ops::execute("deleteAnimLayer", {{"args", {layer}}});
            setAttr(g, grapple, sim::kRecordingsAttr, std::string("[]"));
            sim::RecordOptions o;
            if (a.has("start"))
                o.start = a.get<int64_t>("start");
            if (a.has("end"))
                o.end = a.get<int64_t>("end");
            o.layer = g.node(grapple)->name() + "_bake";
            o.loop = sim::LoopMode::Off;
            const anim::Playback& pb = anim::playback();
            const int64_t start = o.start.value_or(sim::frameOf(pb.minTime)), end = o.end.value_or(sim::frameOf(pb.maxTime));
            NodeId layer = kNoNode;
            try
            {
                layer = sim::record(g, grapple, o);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("bakeGrapple: ") + e.what());
            }
            double worst = 0.0, deepest = 0.0;
            for (int64_t f = std::min(start, end); f <= std::max(start, end); ++f)
                if (auto fr = sim::simulate(g, grapple, f, sim::LoopMode::Off); fr && fr->state.size() >= 3)
                {
                    worst = std::max(worst, fr->state[fr->state.size() - 3]);
                    deepest = std::max(deepest, fr->state[fr->state.size() - 2]);
                }
            return {{"layer", g.path(layer)}, {"frames", std::abs(end - start) + 1}, {"contactError", worst}, {"penetration", deepest}};
        },
        .examples = {"bakeGrapple(\"grapple1\", start=0, end=120)"},
    });

    ops::registerCommand({
        .name = "grappleInfo",
        .doc = "The grapple's figures and contacts, and how its solve does at a frame: {'figures', 'contacts', 'contactError', "
               "'penetration', 'iterations'} (cm). Solves up to the frame if it isn't cached.",
        .positionalDoc = "grapple or one of its contacts (default the selection, else the scene's only grapple)",
        .flags = {nodeFlag({"grapple", "g", T::String, nullptr, "The grapple."}, kType), {"frame", "f", T::Int, nullptr, "Frame. Default: the current one."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId grapple = grappleArg(g, a, "grappleInfo");
            json contacts = json::array();
            for (NodeId c : contactsOf(g, grapple))
                contacts.push_back(contactJson(g, c));
            json out = {{"grapple", g.path(grapple)}, {"figures", figuresJson(g, grapple)}, {"contacts", contacts}};
            const std::vector<std::string> layers = recordedLayers(g, grapple);
            out["layer"] = layers.empty() ? json(nullptr) : json(layers.back());
            try
            {
                const int64_t frame = a.has("frame") ? a.get<int64_t>("frame") : sim::frameOf(g.time());
                auto fr = sim::simulate(g, grapple, frame, sim::LoopMode::Off);
                if (fr && fr->state.size() >= 3)
                {
                    out["contactError"] = fr->state[fr->state.size() - 3];
                    out["penetration"] = fr->state[fr->state.size() - 2];
                    out["iterations"] = fr->state[fr->state.size() - 1];
                }
            }
            catch (const std::exception& e)
            {
                out["error"] = e.what();
            }
            return out;
        },
    });
}
} // namespace lila::scene::grapple
