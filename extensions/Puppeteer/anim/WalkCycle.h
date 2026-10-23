#pragma once

// The parametric walk/run generator's maths: a rest skeleton (world positions and rotations, parents first) posed at a
// phase of the cycle. No graph, so it is deterministic by construction and the tests drive it directly.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace puppeteer::anim
{
struct WalkParams
{
    bool run = false;
    double stride = 50.0;       // cm each foot travels under the body while planted
    double speed = 0.0;         // cm/s; above zero it sets the cycle length from the stride
    double cycleFrames = 32.0;  // frames per cycle (two steps)
    double fps = 30.0;
    double bounce = 3.0;        // cm of pelvis rise and fall, peak to peak
    double stepHeight = 10.0;   // cm the swinging foot lifts
    double armSwing = 20.0;     // degrees each upper arm swings either way
    double lean = 4.0;          // degrees the spine leans forward
    double sway = 2.0;          // cm the pelvis shifts over the planted foot
    double twist = 6.0;         // degrees the pelvis turns with the legs
    int keysPerStep = 4;

    /** The share of the cycle each foot is planted. */
    double stance() const { return run ? 0.38 : 0.6; }

    /** Frames per cycle: from the speed when it is set, else cycleFrames; at least four. */
    double frames() const
    {
        double f = cycleFrames;
        if (speed > 0.0)
            f = stride / (stance() * speed) * fps;
        return std::max(4.0, std::round(f));
    }
};

/** A joint of the rest skeleton, in world space. */
struct RigJoint
{
    std::string label; // semantic label, empty for joints the generator leaves alone
    int parent = -1;   // index of the parent joint, earlier in the list; -1 for a top joint
    glm::dvec3 position{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
};

struct WalkPose
{
    std::vector<glm::dvec3> position;
    std::vector<glm::dquat> rotation;
};

namespace walk_detail
{
constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }
inline double wrap(double p) { return p - std::floor(p); }

/** Orthonormal frame with x along dir and y toward pole. */
inline glm::dquat frame(const glm::dvec3& dir, const glm::dvec3& pole)
{
    const glm::dvec3 x = glm::normalize(dir);
    glm::dvec3 y = pole - x * glm::dot(pole, x);
    if (glm::length(y) < 1e-9)
        y = std::abs(x.y) < 0.9 ? glm::dvec3(0, 1, 0) - x * x.y : glm::dvec3(1, 0, 0) - x * x.x;
    y = glm::normalize(y);
    return glm::quat_cast(glm::dmat3(x, y, glm::cross(x, y)));
}
} // namespace walk_detail

/**
 * Poses a labelled rest skeleton through a walk or run cycle. Needs pelvis, thigh, calf and foot on both sides; uses
 * ball, spine_*, neck_01, head, upperarm and lowerarm when present. The character faces away from its left side
 * crossed with up, moves in place, and every channel is periodic in the cycle, so phase 1 equals phase 0.
 */
class WalkSolver
{
public:
    WalkSolver(std::vector<RigJoint> rest, const WalkParams& params) : mRest(std::move(rest)), mParams(params)
    {
        for (size_t i = 0; i < mRest.size(); ++i)
        {
            if (mRest[i].parent >= int(i))
                throw std::invalid_argument("joints must come after their parents");
            if (!mRest[i].label.empty())
                mByLabel[mRest[i].label] = int(i);
        }
        for (const char* need : {"pelvis", "thigh_l", "thigh_r", "calf_l", "calf_r", "foot_l", "foot_r"})
            if (!mByLabel.contains(need))
                throw std::invalid_argument(std::string("the walk generator needs a joint labelled ") + need);
        for (size_t i = 0; i < mRest.size(); ++i)
            if (mRest[i].label.rfind("spine_", 0) == 0)
                mSpine.push_back(int(i));

        const glm::dvec3 feet = (pos("foot_l") + pos("foot_r")) * 0.5;
        mUp = axisNear(pos("pelvis") - feet);
        glm::dvec3 left = pos("thigh_l") - pos("thigh_r");
        left -= mUp * glm::dot(left, mUp);
        if (glm::length(left) < 1e-9)
            throw std::invalid_argument("the thighs are not side by side");
        mLeft = glm::normalize(left);
        mForward = glm::normalize(glm::cross(mLeft, mUp));

        // Lower the hips enough that the legs still reach the ground at the ends of the stride.
        const double leg = glm::length(pos("calf_l") - pos("thigh_l")) + glm::length(pos("foot_l") - pos("calf_l"));
        const double half = mParams.stride * 0.5;
        mDrop = std::max(0.0, leg - std::sqrt(std::max(leg * leg * 0.99 - half * half, 0.25 * leg * leg)));
    }

    const glm::dvec3& up() const { return mUp; }
    const glm::dvec3& forward() const { return mForward; }
    const glm::dvec3& left() const { return mLeft; }
    const std::vector<RigJoint>& rest() const { return mRest; }
    std::optional<int> joint(const std::string& label) const
    {
        auto it = mByLabel.find(label);
        return it == mByLabel.end() ? std::nullopt : std::optional<int>(it->second);
    }

    /** The skeleton at phase (0..1 over the cycle; any value wraps). */
    WalkPose pose(double phase) const
    {
        using namespace walk_detail;
        const double p = wrap(phase);
        const double s = mParams.stance();
        const size_t n = mRest.size();
        WalkPose out;
        out.position.resize(n);
        out.rotation.resize(n);

        // World-space rotation added to a joint on top of what its parent carries, and joints whose world rotation
        // is set outright (the legs from the IK, the head held steady).
        std::vector<glm::dquat> delta(n, glm::dquat(1, 0, 0, 0));
        std::vector<std::optional<glm::dquat>> fixed(n);

        const double twist = rad(mParams.twist) * std::cos(2.0 * kPi * p);
        if (auto pel = joint("pelvis"))
            delta[size_t(*pel)] = glm::angleAxis(twist, mUp);
        if (!mSpine.empty())
        {
            // The spine leans and turns the chest back against the hips.
            const glm::dquat each = glm::angleAxis(rad(mParams.lean) / double(mSpine.size()), mLeft) *
                                    glm::angleAxis(-1.5 * twist / double(mSpine.size()), mUp);
            for (int j : mSpine)
                delta[size_t(j)] = each;
        }
        for (const char* side : {"l", "r"})
        {
            const bool isLeft = side[0] == 'l';
            // Each arm swings with the opposite leg: the left arm is forward when the right foot strikes at phase 0.
            const double c = std::cos(2.0 * kPi * p) * (isLeft ? 1.0 : -1.0);
            if (auto ua = joint(std::string("upperarm_") + side))
                delta[size_t(*ua)] = glm::angleAxis(-rad(mParams.armSwing) * c, mLeft);
            if (auto la = joint(std::string("lowerarm_") + side))
                delta[size_t(*la)] = glm::angleAxis(-rad(8.0 + 0.4 * mParams.armSwing * std::max(0.0, c)), mLeft);
        }

        const int pelvis = *joint("pelvis");
        const double bounceWave = std::cos(4.0 * kPi * (p - s * 0.5));
        const glm::dvec3 pelvisOffset = mUp * ((mParams.run ? -0.5 : 0.5) * mParams.bounce * bounceWave - 0.5 * mParams.bounce - mDrop) -
                                        mLeft * (mParams.sway * std::cos(2.0 * kPi * (p - s * 0.5)));

        // The leg solve for each side, run when the walk reaches that side's thigh.
        struct Leg
        {
            int thigh, calf, foot, ball;
            double phase;
        };
        std::vector<Leg> legs;
        for (const char* side : {"l", "r"})
        {
            const std::string sfx = side;
            auto ball = joint("ball_" + sfx);
            legs.push_back({*joint("thigh_" + sfx), *joint("calf_" + sfx), *joint("foot_" + sfx), ball ? *ball : -1,
                            wrap(p + (side[0] == 'l' ? 0.5 : 0.0))});
        }

        for (size_t i = 0; i < n; ++i)
        {
            const RigJoint& r = mRest[i];
            if (r.parent < 0)
            {
                out.position[i] = r.position;
                out.rotation[i] = r.rotation;
            }
            else
            {
                const size_t pi = size_t(r.parent);
                const RigJoint& pr = mRest[pi];
                const glm::dvec3 offset = glm::conjugate(pr.rotation) * (r.position - pr.position);
                const glm::dquat local = glm::conjugate(pr.rotation) * r.rotation;
                out.position[i] = out.position[pi] + out.rotation[pi] * offset;
                out.rotation[i] = glm::normalize(out.rotation[pi] * local);
            }
            if (int(i) == pelvis)
                out.position[i] += pelvisOffset;
            out.rotation[i] = fixed[i] ? *fixed[i] : glm::normalize(delta[i] * out.rotation[i]);

            for (const Leg& leg : legs)
            {
                if (int(i) == leg.thigh)
                    solveLeg(leg.thigh, leg.calf, leg.foot, leg.ball, leg.phase, out, fixed, delta);
            }
            // The head keeps its rest orientation (with a little of the lean) so the view stays steady.
            if (r.label == "head")
                out.rotation[i] = glm::normalize(glm::angleAxis(rad(mParams.lean) * 0.3, mLeft) * r.rotation);
        }
        return out;
    }

private:
    glm::dvec3 pos(const std::string& label) const { return mRest[size_t(mByLabel.at(label))].position; }

    static glm::dvec3 axisNear(const glm::dvec3& v)
    {
        const glm::dvec3 a = glm::abs(v);
        if (a.x >= a.y && a.x >= a.z)
            return {v.x >= 0 ? 1.0 : -1.0, 0, 0};
        if (a.y >= a.z)
            return {0, v.y >= 0 ? 1.0 : -1.0, 0};
        return {0, 0, v.z >= 0 ? 1.0 : -1.0};
    }

    /** Foot travel along forward (cm), lift (cm) and pitch (radians, positive raises the heel) at a foot's phase. */
    void footAt(double q, double& along, double& lift, double& pitch) const
    {
        using namespace walk_detail;
        const double s = mParams.stance();
        const double half = mParams.stride * 0.5;
        const double heel = rad(mParams.run ? 8.0 : 15.0), toe = rad(mParams.run ? 35.0 : 30.0);
        if (q < s)
        {
            const double u = q / s;
            along = half - u * mParams.stride;
            lift = 0.0;
            // Toes come down after the strike, the heel peels up before toe off.
            pitch = -heel * std::pow(std::max(0.0, 1.0 - u / 0.2), 2.0) + toe * std::pow(std::max(0.0, (u - 0.7) / 0.3), 2.0);
        }
        else
        {
            const double u = (q - s) / (1.0 - s);
            const double ease = 0.5 - 0.5 * std::cos(kPi * u);
            along = -half + ease * mParams.stride;
            lift = mParams.stepHeight * (0.5 - 0.5 * std::cos(2.0 * kPi * u));
            pitch = toe * std::pow(std::max(0.0, 1.0 - u / 0.4), 2.0) - heel * std::pow(std::max(0.0, (u - 0.6) / 0.4), 2.0);
        }
    }

    void solveLeg(int thigh, int calf, int foot, int ball, double q, WalkPose& out, std::vector<std::optional<glm::dquat>>& fixed,
                  std::vector<glm::dquat>& delta) const
    {
        using walk_detail::frame;
        const RigJoint &t0 = mRest[size_t(thigh)], &c0 = mRest[size_t(calf)], &f0 = mRest[size_t(foot)];
        double along = 0, lift = 0, pitch = 0;
        footAt(q, along, lift, pitch);
        const glm::dquat footRot = glm::angleAxis(pitch, mLeft);

        // A raised heel pivots the foot about the ball, which stays on the ground.
        glm::dvec3 ankle = f0.position + mForward * along + mUp * lift;
        if (ball >= 0 && pitch > 0.0)
        {
            const glm::dvec3 b0 = mRest[size_t(ball)].position + mForward * along + mUp * lift;
            ankle = b0 + footRot * (f0.position - mRest[size_t(ball)].position);
        }

        const glm::dvec3 hip = out.position[size_t(thigh)];
        const double l1 = glm::length(c0.position - t0.position), l2 = glm::length(f0.position - c0.position);
        glm::dvec3 toAnkle = ankle - hip;
        double d = glm::length(toAnkle);
        const glm::dvec3 dir = d > 1e-9 ? toAnkle / d : -mUp;
        d = std::clamp(d, std::abs(l1 - l2) + 1e-6, (l1 + l2) * 0.9999);
        const double cosA = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0 * l1 * d), -1.0, 1.0);
        glm::dvec3 pole = mForward - dir * glm::dot(mForward, dir);
        pole = glm::length(pole) > 1e-9 ? glm::normalize(pole) : mForward;
        const glm::dvec3 knee = hip + dir * (l1 * cosA) + pole * (l1 * std::sqrt(1.0 - cosA * cosA));
        const glm::dvec3 reach = hip + dir * d;

        const glm::dquat thighRot = frame(knee - hip, mForward) * glm::conjugate(frame(c0.position - t0.position, mForward));
        const glm::dquat calfRot = frame(reach - knee, mForward) * glm::conjugate(frame(f0.position - c0.position, mForward));
        out.rotation[size_t(thigh)] = glm::normalize(thighRot * t0.rotation);
        fixed[size_t(calf)] = glm::normalize(calfRot * c0.rotation);
        fixed[size_t(foot)] = glm::normalize(footRot * f0.rotation);
        // Toes stay flat while the heel is up, letting go early in the swing.
        if (ball >= 0 && pitch > 0.0)
        {
            const double s = mParams.stance();
            const double flat = q < s ? 1.0 : std::max(0.0, 1.0 - (q - s) / (0.25 * (1.0 - s)));
            delta[size_t(ball)] = glm::angleAxis(-pitch * flat, mLeft);
        }
    }

    std::vector<RigJoint> mRest;
    WalkParams mParams;
    std::map<std::string, int> mByLabel;
    std::vector<int> mSpine;
    double mDrop = 0.0;
    glm::dvec3 mUp{0, 1, 0}, mLeft{1, 0, 0}, mForward{0, 0, 1};
};

/** Key frames of one cycle: keysPerStep per step, whole frames, the loop key at the end included. */
inline std::vector<int> walkKeyFrames(const WalkParams& p)
{
    const int frames = int(p.frames());
    const int perCycle = std::max(2, p.keysPerStep * 2);
    std::vector<int> out;
    for (int k = 0; k <= perCycle; ++k)
    {
        const int f = int(std::lround(double(k) * frames / perCycle));
        if (out.empty() || f > out.back())
            out.push_back(f);
    }
    return out;
}
} // namespace puppeteer::anim
