#pragma once

#include <doctest/doctest.h>

#include "ops/Command.h"
#include "retarget/Retarget.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace rtest
{
using namespace lila::retarget;

inline nlohmann::json run(const char* name, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(name, call); }

struct Spec
{
    std::string name, parent;
    glm::dvec3 pos{0.0};
    glm::dvec3 orient{0.0}; // joint orient, degrees XYZ
};

/** A skeleton from world positions; every R starts at identity, so the joint orients are the rest frames. */
inline Skeleton build(const std::vector<Spec>& specs, Pose& pose)
{
    Skeleton s;
    std::vector<glm::dmat4> world;
    for (const Spec& sp : specs)
    {
        const int parent = sp.parent.empty() ? -1 : s.find(sp.parent);
        REQUIRE((sp.parent.empty() || parent >= 0));
        const glm::dmat4 pre = glm::dmat4(glm::eulerAngleXYZ(glm::radians(sp.orient.x), glm::radians(sp.orient.y), glm::radians(sp.orient.z)));
        s.add(sp.name, parent, pre);
        const glm::dmat4 parentW = parent < 0 ? glm::dmat4(1.0) : world[size_t(parent)];
        pose.rotation.push_back(glm::dquat(1.0, 0.0, 0.0, 0.0));
        pose.translate.push_back(translateFor(parentW, sp.pos));
        world.push_back(parentW * localOf(s, pose, int(s.size()) - 1));
    }
    return s;
}

/**
 * A biped facing +Z, Y up, sized by `scale`. names picks the naming: "ue" (pelvis, thigh_l ...), "bvh" (Hips, LeftUpLeg
 * ...) or "none" (bone0 ...). spine is the number of spine joints; twist gives every joint a different orient.
 */
inline std::vector<Spec> biped(double scale = 1.0, const std::string& names = "ue", int spine = 3, bool twist = false, bool fingers = false)
{
    const bool ue = names == "ue", none = names == "none";
    int counter = 0;
    auto nm = [&](const char* u, const char* b) { return none ? "bone" + std::to_string(counter++) : std::string(ue ? u : b); };
    std::vector<Spec> out;
    auto add = [&](const std::string& n, const std::string& p, glm::dvec3 pos) {
        glm::dvec3 o(0.0);
        if (twist)
            o = glm::dvec3(double((out.size() * 37) % 90) - 45.0, double((out.size() * 53) % 70) - 35.0, double((out.size() * 29) % 80) - 40.0);
        out.push_back({n, p, pos * scale, o});
        return n;
    };
    const std::string root = add(nm("root", "Root"), "", {0, 0, 0});
    const std::string pelvis = add(nm("pelvis", "Hips"), root, {0, 100, 0});
    std::string up = pelvis;
    for (int i = 0; i < spine; ++i)
        up = add(none ? nm("", "") : (ue ? "spine_0" + std::to_string(i + 1) : "Spine" + (i ? std::to_string(i) : std::string())), up,
                 {0, 110 + 30.0 * i / std::max(spine - 1, 1), 0});
    const std::string chest = up;
    const std::string neck = add(nm("neck_01", "Neck"), chest, {0, 150, 0});
    add(nm("head", "Head"), neck, {0, 162, 1});
    for (int side = 0; side < 2; ++side)
    {
        const double s = side == 0 ? 1.0 : -1.0;
        const char* L = side == 0 ? "l" : "r";
        const char* Side = side == 0 ? "Left" : "Right";
        auto un = [&](const char* base) { return std::string(base) + "_" + L; };
        auto bn = [&](const char* base) { return std::string(Side) + base; };
        const std::string clav = add(nm(un("clavicle").c_str(), bn("Shoulder").c_str()), chest, {4 * s, 145, 0});
        const std::string upper = add(nm(un("upperarm").c_str(), bn("Arm").c_str()), clav, {18 * s, 145, 0});
        const std::string lower = add(nm(un("lowerarm").c_str(), bn("ForeArm").c_str()), upper, {46 * s, 145, -2});
        const std::string hand = add(nm(un("hand").c_str(), bn("Hand").c_str()), lower, {72 * s, 145, 0});
        if (fingers)
        {
            const std::string t1 = add(nm(un("thumb_01").c_str(), bn("HandThumb1").c_str()), hand, {75 * s, 144, 4});
            add(nm(un("thumb_02").c_str(), bn("HandThumb2").c_str()), t1, {78 * s, 144, 7});
            const char* fn[] = {"index", "middle", "ring", "pinky"};
            for (int k = 0; k < 4; ++k)
            {
                const std::string f1 = add(nm(un((std::string(fn[k]) + "_01").c_str()).c_str(), (bn("Hand") + fn[k] + "1").c_str()), hand,
                                           {80 * s, 145, 3.0 - 2.0 * k});
                add(nm(un((std::string(fn[k]) + "_02").c_str()).c_str(), (bn("Hand") + fn[k] + "2").c_str()), f1, {84 * s, 145, 3.0 - 2.0 * k});
            }
        }
        const std::string thigh = add(nm(un("thigh").c_str(), bn("UpLeg").c_str()), pelvis, {10 * s, 95, 0});
        const std::string calf = add(nm(un("calf").c_str(), bn("Leg").c_str()), thigh, {10 * s, 52, 2});
        const std::string foot = add(nm(un("foot").c_str(), bn("Foot").c_str()), calf, {10 * s, 8, 0});
        add(nm(un("ball").c_str(), bn("ToeBase").c_str()), foot, {10 * s, 2, 12});
    }
    return out;
}

/** A dog-like quadruped facing +Z: hind legs off the pelvis, front legs off the chest, a tail. */
inline std::vector<Spec> quadruped(double scale = 1.0)
{
    std::vector<Spec> out;
    auto add = [&](const std::string& n, const std::string& p, glm::dvec3 pos) { out.push_back({n, p, pos * scale, {}}); };
    add("pelvis", "", {0, 60, -30});
    add("spine1", "pelvis", {0, 62, -10});
    add("spine2", "spine1", {0, 63, 10});
    add("chest", "spine2", {0, 62, 28});
    add("neck", "chest", {0, 72, 38});
    add("head", "neck", {0, 80, 46});
    add("tail1", "pelvis", {0, 62, -40});
    add("tail2", "tail1", {0, 60, -55});
    for (int side = 0; side < 2; ++side)
    {
        const double s = side == 0 ? 1.0 : -1.0;
        const std::string L = side == 0 ? "_l" : "_r";
        add("scapula" + L, "chest", {8 * s, 58, 30});
        add("upperarm" + L, "scapula" + L, {9 * s, 45, 28});
        add("forearm" + L, "upperarm" + L, {9 * s, 22, 30});
        add("paw" + L, "forearm" + L, {9 * s, 3, 30});
        add("fingers" + L, "paw" + L, {9 * s, 1, 35});
        add("thigh" + L, "pelvis", {9 * s, 55, -30});
        add("calf" + L, "thigh" + L, {9 * s, 35, -22});
        add("hock" + L, "calf" + L, {9 * s, 15, -32});
        add("foot" + L, "hock" + L, {9 * s, 3, -30});
        add("toes" + L, "foot" + L, {9 * s, 1, -25});
    }
    return out;
}

/** An insect: six legs off a thorax, unnamed sides. */
inline std::vector<Spec> hexapod()
{
    std::vector<Spec> out;
    auto add = [&](const std::string& n, const std::string& p, glm::dvec3 pos) { out.push_back({n, p, pos, {}}); };
    add("abdomen", "", {0, 12, -10});
    add("thorax", "abdomen", {0, 12, 0});
    add("head", "thorax", {0, 13, 8});
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < 3; ++k)
        {
            const double s = side == 0 ? 1.0 : -1.0;
            const std::string n = "leg" + std::to_string(side * 3 + k);
            const double z = 4.0 - 4.0 * k;
            add(n + "a", "thorax", {3 * s, 12, z});
            add(n + "b", n + "a", {10 * s, 16, z});
            add(n + "c", n + "b", {16 * s, 6, z});
            add(n + "d", n + "c", {18 * s, 0.5, z});
            add(n + "e", n + "d", {19 * s, 0.2, z + 1});
        }
    return out;
}

inline glm::dvec3 pos(const std::vector<glm::dmat4>& w, const Skeleton& s, const char* name)
{
    const int i = s.find(name);
    REQUIRE(i >= 0);
    return positionOf(w[size_t(i)]);
}

/**
 * A walk: the pelvis moves forward along `forward` at speed, the legs (by the rig) step in turn with their feet held
 * still on the ground through each stance, solved by chain IK. legPhase gives each leg chain its phase; the pelvis
 * sits crouch lower than at rest so the feet can reach.
 */
inline Frames walk(const Skeleton& s, const Rig& rig, const Pose& rest, int frames, double fps, double speed, double stride,
                   const std::map<std::string, double>& legPhase, double duty = 0.6, double crouch = 8.0)
{
    Frames out;
    out.fps = fps;
    const std::vector<glm::dmat4> w0 = worldOf(s, rest);
    const int pelvis = s.find(rig.pelvis);
    const double period = stride / (speed * duty);
    const glm::dvec3 fwd = rig.forward, up = rig.up;
    for (int f = 0; f < frames; ++f)
    {
        const double t = f / fps;
        Pose p = rest;
        std::vector<glm::dmat4> w = w0;
        const glm::dmat4 parentW = parentWorldOf(s, w, pelvis);
        p.translate[size_t(pelvis)] = translateFor(parentW, positionOf(w0[size_t(pelvis)]) + fwd * speed * t - up * crouch);
        w = worldOf(s, p);
        for (const Chain& c : rig.chains)
        {
            if (c.role != Role::Leg || !legPhase.contains(c.name))
                continue;
            const std::vector<int> j = s.path(s.find(c.start), s.find(c.goal));
            const glm::dvec3 restFoot = positionOf(w0[size_t(j.back())]);
            const double cycle = t / period + legPhase.at(c.name);
            const double u = cycle - std::floor(cycle);
            const double cycleStart = (std::floor(cycle) - legPhase.at(c.name)) * period;
            // Touchdown half a stride ahead of where the pelvis is then; stance holds it, swing arcs to the next.
            const double touch = speed * cycleStart + stride * 0.5;
            double along, lift = 0.0;
            if (u < duty)
                along = touch;
            else
            {
                const double k = (u - duty) / (1.0 - duty);
                along = glm::mix(touch, touch + speed * period, k);
                lift = std::sin(k * glm::pi<double>()) * stride * 0.15;
            }
            const glm::dvec3 target = restFoot + fwd * along + up * lift;
            solveChainIk(s, p, w, j, target, rotationOf(w0[size_t(j.back())]));
        }
        out.world.push_back(worldOf(s, p));
    }
    return out;
}
} // namespace rtest
