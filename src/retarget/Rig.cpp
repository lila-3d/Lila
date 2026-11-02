#include "retarget/Rig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>

namespace lila::retarget
{
namespace
{
constexpr std::array<const char*, 14> kRoleNames = {"root", "spine", "neck", "head", "jaw", "clavicle", "arm",
                                                    "leg", "toe", "finger", "tail", "wing", "antenna", "other"};
constexpr std::array<const char*, 3> kSideNames = {"center", "left", "right"};
constexpr std::array<const char*, 5> kFingerNames = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
} // namespace

const char* roleName(Role r) { return kRoleNames[size_t(r)]; }

Role roleFromName(std::string_view name)
{
    for (size_t i = 0; i < kRoleNames.size(); ++i)
        if (name == kRoleNames[i])
            return Role(i);
    throw std::invalid_argument("unknown chain role '" + std::string(name) + "'");
}

const char* sideName(Side s) { return kSideNames[size_t(s)]; }

Side sideFromName(std::string_view name)
{
    for (size_t i = 0; i < kSideNames.size(); ++i)
        if (name == kSideNames[i])
            return Side(i);
    throw std::invalid_argument("unknown side '" + std::string(name) + "'");
}

std::string chainName(Role role, Side side, int index)
{
    std::string out = side == Side::Left ? "Left" : side == Side::Right ? "Right" : "";
    if (role == Role::Finger)
        return out + (index >= 0 && index < int(kFingerNames.size()) ? std::string(kFingerNames[size_t(index)]) : "Finger" + std::to_string(index + 1));
    std::string r = roleName(role);
    r[0] = char(std::toupper(static_cast<unsigned char>(r[0])));
    out += r;
    if (index > 0)
        out += std::to_string(index + 1);
    return out;
}

const Chain* Rig::chain(std::string_view name) const
{
    for (const Chain& c : chains)
        if (c.name == name)
            return &c;
    return nullptr;
}

Chain* Rig::chain(std::string_view name)
{
    for (Chain& c : chains)
        if (c.name == name)
            return &c;
    return nullptr;
}

const Chain* Rig::chainFor(Role role, Side side, int index) const
{
    for (const Chain& c : chains)
        if (c.role == role && c.side == side && c.index == index)
            return &c;
    return nullptr;
}

Pose Rig::retargetPose(const Skeleton& s, const Pose& current) const
{
    Pose p = current;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (auto it = poseRotation.find(s.names[i]); it != poseRotation.end())
            p.rotation[i] = it->second;
        if (auto it = poseTranslate.find(s.names[i]); it != poseTranslate.end())
            p.translate[i] = it->second;
    }
    return p;
}

void Rig::setRetargetPose(const Skeleton& s, const Pose& pose)
{
    poseRotation.clear();
    poseTranslate.clear();
    for (size_t i = 0; i < s.size(); ++i)
    {
        poseRotation[s.names[i]] = pose.rotation[i];
        poseTranslate[s.names[i]] = pose.translate[i];
    }
}

namespace
{
json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
json quat(const glm::dquat& q) { return json::array({q.x, q.y, q.z, q.w}); }

glm::dvec3 vecFrom(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument(std::string(what) + " should be [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

glm::dquat quatFrom(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 4)
        throw std::invalid_argument(std::string(what) + " should be [x, y, z, w]");
    return glm::normalize(glm::dquat(j[3].get<double>(), j[0].get<double>(), j[1].get<double>(), j[2].get<double>()));
}
} // namespace

json Rig::toJson() const
{
    json cs = json::array();
    for (const Chain& c : chains)
    {
        json o = {{"name", c.name}, {"role", roleName(c.role)}, {"side", sideName(c.side)}, {"index", c.index}, {"start", c.start}, {"end", c.end}};
        if (!c.goal.empty())
            o["goal"] = c.goal;
        cs.push_back(std::move(o));
    }
    json rot = json::object(), tr = json::object();
    for (const auto& [n, q] : poseRotation)
        rot[n] = quat(q);
    for (const auto& [n, t] : poseTranslate)
        tr[n] = vec(t);
    return {{"format", "lilaRetargetRig"}, {"version", 1}, {"pelvis", pelvis},   {"chains", cs},   {"poseRotation", rot},
            {"poseTranslate", tr},         {"ground", ground}, {"up", vec(up)}, {"forward", vec(forward)}, {"notes", notes}};
}

Rig Rig::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a retarget rig is a JSON object");
    Rig r;
    r.pelvis = j.value("pelvis", std::string());
    for (const json& c : j.value("chains", json::array()))
    {
        Chain ch;
        ch.name = c.at("name").get<std::string>();
        ch.role = roleFromName(c.value("role", std::string("other")));
        ch.side = sideFromName(c.value("side", std::string("center")));
        ch.index = c.value("index", 0);
        ch.start = c.at("start").get<std::string>();
        ch.end = c.value("end", ch.start);
        ch.goal = c.value("goal", std::string());
        r.chains.push_back(std::move(ch));
    }
    const json rotations = j.value("poseRotation", json::object()), translations = j.value("poseTranslate", json::object());
    for (const auto& [n, q] : rotations.items())
        r.poseRotation[n] = quatFrom(q, "poseRotation");
    for (const auto& [n, t] : translations.items())
        r.poseTranslate[n] = vecFrom(t, "poseTranslate");
    r.ground = j.value("ground", 0.0);
    if (j.contains("up"))
        r.up = glm::normalize(vecFrom(j["up"], "up"));
    if (j.contains("forward"))
        r.forward = glm::normalize(vecFrom(j["forward"], "forward"));
    r.notes = j.value("notes", std::vector<std::string>());
    return r;
}

std::vector<int> chainJoints(const Skeleton& s, const Chain& c) { return s.path(s.find(c.start), s.find(c.end)); }

double chainLength(const Skeleton&, const std::vector<int>& joints, const std::vector<glm::dmat4>& world)
{
    double len = 0.0;
    for (size_t i = 1; i < joints.size(); ++i)
        len += glm::length(positionOf(world[size_t(joints[i])]) - positionOf(world[size_t(joints[i - 1])]));
    return len;
}

// Characterising -------------------------------------------------------------------------------------------------

namespace
{
/** Lower-case words of a joint name: LeftUpLeg is left up leg, LHipJoint l hip joint, spine_01 spine 01. */
std::vector<std::string> tokens(std::string_view name)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.empty())
            return;
        std::transform(cur.begin(), cur.end(), cur.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
        out.push_back(cur);
        cur.clear();
    };
    auto up = [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; };
    auto low = [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; };
    auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (!std::isalnum(static_cast<unsigned char>(c)))
        {
            flush();
            continue;
        }
        if (!cur.empty() && digit(c) != digit(cur.back()))
            flush();
        else if (up(c) && !cur.empty() && (low(cur.back()) || (up(cur.back()) && i + 1 < name.size() && low(name[i + 1]))))
            flush();
        cur += c;
    }
    flush();
    return out;
}

bool hasAny(const std::vector<std::string>& t, std::initializer_list<const char*> words)
{
    for (const std::string& w : t)
        for (const char* x : words)
            if (w == x)
                return true;
    return false;
}

std::optional<Side> sideOfName(const std::vector<std::string>& t)
{
    if (hasAny(t, {"l", "left", "lft", "lt"}))
        return Side::Left;
    if (hasAny(t, {"r", "right", "rgt", "rt"}))
        return Side::Right;
    return std::nullopt;
}

struct Finder
{
    const Skeleton& s;
    const CharacterizeOptions& o;
    std::vector<glm::dmat4> world;
    std::vector<std::vector<std::string>> tok;
    std::vector<bool> usable;
    std::vector<std::vector<int>> kids;
    std::vector<double> reach; // longest distance to a leaf through usable joints
    std::vector<int> size;     // usable joints in the subtree
    double span = 1.0;
    std::set<int> used;
    Rig rig;

    Finder(const Skeleton& sk, const Pose& p, const CharacterizeOptions& opts) : s(sk), o(opts), world(worldOf(sk, p))
    {
        const size_t n = s.size();
        tok.resize(n);
        usable.assign(n, true);
        kids.resize(n);
        reach.assign(n, 0.0);
        size.assign(n, 1);
        double top = -1e300;
        for (size_t i = 0; i < n; ++i)
        {
            tok[i] = tokens(s.names[i]);
            if ((o.skip.size() == n && o.skip[i]) ||
                hasAny(tok[i], {"twist", "ik", "roll", "helper", "corrective", "nub", "end", "dummy", "socket", "weapon", "prop", "attach", "ctrl",
                                "control", "effector", "pole", "null"}))
                usable[i] = false;
            // Anything under an unusable joint is a helper's too.
            if (s.parent[i] >= 0 && !usable[size_t(s.parent[i])])
                usable[i] = false;
            top = std::max(top, height(int(i)));
        }
        span = std::max(top, 1e-6);
        for (size_t i = 0; i < n; ++i)
            if (usable[i] && s.parent[i] >= 0)
                kids[size_t(s.parent[i])].push_back(int(i));
        for (size_t k = n; k-- > 0;)
        {
            if (!usable[k])
                continue;
            for (int c : kids[k])
            {
                reach[k] = std::max(reach[k], dist(int(k), c) + reach[size_t(c)]);
                size[k] += size[size_t(c)];
            }
        }
        rig.up = o.up;
        rig.ground = o.ground;
    }

    glm::dvec3 pos(int j) const { return positionOf(world[size_t(j)]); }
    double height(int j) const { return glm::dot(pos(j), o.up) - o.ground; }
    double dist(int a, int b) const { return glm::length(pos(b) - pos(a)); }
    bool has(int j, std::initializer_list<const char*> words) const { return hasAny(tok[size_t(j)], words); }
    /** How upright the bone a -> b is: 1 straight up or down, 0 level. */
    double vertical(int a, int b) const
    {
        const glm::dvec3 d = pos(b) - pos(a);
        const double len = glm::length(d);
        return len < 1e-9 ? 0.0 : std::abs(glm::dot(d, o.up)) / len;
    }

    /** From j down the child that leads furthest, to a leaf. */
    std::vector<int> mainPath(int j) const
    {
        std::vector<int> out{j};
        for (int cur = j; !kids[size_t(cur)].empty();)
        {
            int best = -1;
            double bestReach = -1.0;
            for (int c : kids[size_t(cur)])
                if (double r = dist(cur, c) + reach[size_t(c)]; r > bestReach)
                    best = c, bestReach = r;
            out.push_back(best);
            cur = best;
        }
        return out;
    }

    /** A branch whose joints are named as a leg's (for poses where the feet are off the ground: crawling, sitting). */
    bool namedLeg(int c) const
    {
        const std::vector<int> path = mainPath(c);
        if (path.size() < 2)
            return false;
        for (int j : path)
            if (has(j, {"hand", "wrist", "finger", "thumb", "arm", "spine", "neck", "head", "tail"}))
                return false;
        for (int j : path)
            if (isLegName(j))
                return true;
        return false;
    }

    bool isLegName(int j) const { return has(j, {"thigh", "upleg", "leg", "calf", "shin", "knee", "foot", "ankle", "toe", "femur", "tibia", "paw", "hoof"}); }

    /** At least three joints reaching a tenth of the body's height: not a helper bone (a pectoral, a latissimus). */
    bool limbShaped(int c) const { return mainPath(c).size() >= 3 && reach[size_t(c)] >= 0.1 * span; }

    /** A branch that ends at the ground without branching above it: a leg, whatever it hangs off. */
    bool legShaped(int c) const
    {
        const std::vector<int> path = mainPath(c);
        if (path.size() < 2 || height(path.back()) > 0.2 * span)
            return false;
        for (int j : path)
            if ((kids[size_t(j)].size() >= 2 && height(j) > 0.3 * span) || has(j, {"hand", "wrist", "finger", "thumb", "palm"}))
                return false; // a hand on the ground is still a hand: a person crawling, not a forelimb
        return true;
    }

    void addChain(Role role, const std::vector<int>& joints, int goal = -1, std::optional<Side> side = {})
    {
        if (joints.empty())
            return;
        Chain c;
        c.role = role;
        c.start = s.names[size_t(joints.front())];
        c.end = s.names[size_t(joints.back())];
        if (goal >= 0)
            c.goal = s.names[size_t(goal)];
        if (side)
            c.side = *side;
        rig.chains.push_back(std::move(c));
        used.insert(joints.begin(), joints.end());
    }

    int findPelvis() const
    {
        for (size_t i = 0; i < s.size(); ++i)
            if (usable[i] && hasAny(tok[i], {"pelvis", "hips"}) && !sideOfName(tok[i]))
                return int(i);
        int best = -1, bestLegs = 1;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (!usable[i])
                continue;
            int legs = 0;
            for (int c : kids[i])
                legs += legShaped(c) ? 1 : 0;
            if (legs > bestLegs)
                best = int(i), bestLegs = legs;
        }
        if (best >= 0)
            return best;
        for (size_t i = 0; i < s.size(); ++i)
            if (usable[i] && kids[i].size() >= 2)
                return int(i);
        for (size_t i = 0; i < s.size(); ++i)
            if (usable[i])
                return int(i);
        return -1;
    }

    void leg(int c)
    {
        std::vector<int> path = mainPath(c);
        // Level bones leading in (a hip joint off the pelvis) are not part of the leg.
        while (path.size() > 2 && vertical(path[0], path[1]) < 0.5 && !isLegName(path[0]))
            path.erase(path.begin());
        size_t ankle = path.size() - 1;
        bool named = false;
        for (size_t i = 0; i < path.size(); ++i)
            if (has(path[i], {"foot", "ankle", "paw", "hoof"}))
            {
                ankle = i, named = true;
                break;
            }
        if (!named)
            for (size_t i = 1; i + 1 < path.size(); ++i)
                if (vertical(path[i], path[i + 1]) < 0.5 && height(path[i]) < 0.25 * span)
                {
                    ankle = i;
                    break;
                }
        addChain(Role::Leg, {path.begin(), path.begin() + std::ptrdiff_t(ankle) + 1}, path[ankle]);
        if (ankle + 1 < path.size())
            addChain(Role::Toe, {path.begin() + std::ptrdiff_t(ankle) + 1, path.end()});
    }

    void arm(int c)
    {
        const std::vector<int> path = mainPath(c);
        int hand = -1;
        for (size_t i = 0; i < path.size(); ++i)
            if (has(path[i], {"hand", "wrist"}))
            {
                hand = int(i);
                break;
            }
        if (hand < 0)
            for (size_t i = 1; i < path.size(); ++i)
                if (kids[size_t(path[i])].size() >= 3)
                {
                    hand = int(i);
                    break;
                }
        if (hand < 0)
        {
            const bool clavicle = path.size() >= 4 && (has(path[0], {"clavicle", "collar", "shoulder", "scapula"}) ||
                                                       dist(path[0], path[1]) < 0.6 * dist(path[1], path[2]));
            hand = std::min(int(path.size()) - 1, clavicle ? 3 : 2);
        }
        const int upper = std::max(0, hand - 2);
        if (upper > 0)
            addChain(Role::Clavicle, {path.begin(), path.begin() + upper});
        addChain(Role::Arm, {path.begin() + upper, path.begin() + hand + 1}, path[size_t(hand)]);
        fingers(path[size_t(hand)]);
    }

    void fingers(int hand)
    {
        std::vector<std::vector<int>> fs;
        // Single helper joints off the wrist are not fingers.
        for (int c : kids[size_t(hand)])
            if (!used.contains(c) && mainPath(c).size() >= 2)
                fs.push_back(mainPath(c));
        if (fs.empty())
            return;
        std::vector<int> index(fs.size(), -1);
        std::vector<bool> taken(5, false);
        for (size_t k = 0; k < fs.size(); ++k)
        {
            const int base = fs[k].front();
            const int byName = has(base, {"thumb"})                 ? 0
                               : has(base, {"index", "pointer"})    ? 1
                               : has(base, {"middle"})              ? 2
                               : has(base, {"ring"})                ? 3
                               : has(base, {"pinky", "little", "pinkie"}) ? 4
                                                                    : -1;
            if (byName >= 0 && !taken[size_t(byName)])
                index[k] = byName, taken[size_t(byName)] = true;
        }
        std::vector<size_t> rest;
        for (size_t k = 0; k < fs.size(); ++k)
            if (index[k] < 0)
                rest.push_back(k);
        if (!rest.empty())
        {
            // The thumb stands apart from the others; the rest run front to back.
            if (!taken[0] && fs.size() >= 3)
            {
                glm::dvec3 mean(0.0);
                for (const auto& f : fs)
                    mean += pos(f.front());
                mean /= double(fs.size());
                size_t thumb = rest.front();
                for (size_t k : rest)
                    if (glm::length(pos(fs[k].front()) - mean) > glm::length(pos(fs[thumb].front()) - mean))
                        thumb = k;
                index[thumb] = 0;
                taken[0] = true;
                rest.erase(std::find(rest.begin(), rest.end(), thumb));
            }
            std::sort(rest.begin(), rest.end(),
                      [&](size_t a, size_t b) { return glm::dot(pos(fs[a].front()), rig.forward) > glm::dot(pos(fs[b].front()), rig.forward); });
            int next = 1;
            for (size_t k : rest)
            {
                while (next < 5 && taken[size_t(next)])
                    ++next;
                index[k] = next++;
            }
        }
        for (size_t k = 0; k < fs.size(); ++k)
        {
            addChain(Role::Finger, fs[k]);
            rig.chains.back().index = index[k];
        }
    }

    /** Sum of ankle-to-toe directions, level. */
    glm::dvec3 toesForward() const
    {
        glm::dvec3 fwd(0.0);
        for (const Chain& leg : rig.chains)
            if (leg.role == Role::Leg && !leg.goal.empty())
                for (const Chain& toe : rig.chains)
                    if (toe.role == Role::Toe && s.parent[size_t(s.find(toe.start))] == s.find(leg.goal))
                    {
                        const glm::dvec3 d = pos(s.find(toe.start)) - pos(s.find(leg.goal));
                        fwd += d - glm::dot(d, o.up) * o.up;
                    }
        return fwd;
    }

    /** Main path from c, with chains of a role named on the way (tail, wing, antenna). */
    void named(int c, Role role) { addChain(role, mainPath(c)); }

    void run()
    {
        if (s.size() == 0)
        {
            rig.notes.push_back("the skeleton has no joints");
            return;
        }
        const int pelvis = findPelvis();
        if (pelvis < 0)
        {
            rig.notes.push_back("no joint could be the pelvis");
            return;
        }
        rig.pelvis = s.names[size_t(pelvis)];
        used.insert(pelvis);
        int top = pelvis;
        while (s.parent[size_t(top)] >= 0)
            top = s.parent[size_t(top)];
        if (top != pelvis)
        {
            addChain(Role::Root, {top});
            rig.chains.back().side = Side::Center;
        }

        // The spine is the biggest branch off the pelvis that is not a leg; the other branches are legs and tails.
        std::vector<int> legs, others;
        int spine = -1;
        for (int c : kids[size_t(pelvis)])
        {
            if (legShaped(c) || namedLeg(c))
                legs.push_back(c);
            else if (spine < 0 || size[size_t(c)] > size[size_t(spine)])
            {
                if (spine >= 0)
                    others.push_back(spine);
                spine = c;
            }
            else
                others.push_back(c);
        }
        // Toes point forward; failing that the spine runs from the pelvis toward the head on a level body.
        for (int c : legs)
            leg(c);
        glm::dvec3 fwd = toesForward();

        std::vector<int> chestKids;
        if (spine >= 0)
        {
            std::vector<int> run{spine};
            int cur = spine;
            while (kids[size_t(cur)].size() == 1)
            {
                cur = kids[size_t(cur)].front();
                run.push_back(cur);
            }
            // A lone joint ahead of the body (an insect's head off its thorax) is the head, not a spine.
            addChain(run.size() == 1 && kids[size_t(cur)].empty() ? Role::Head : Role::Spine, run);
            chestKids = kids[size_t(cur)];
            if (glm::length(fwd) < 1e-6 && vertical(pelvis, cur) < 0.5)
                fwd = pos(cur) - pos(pelvis);
            // Legs hung off the spine (front legs, insect legs).
            for (size_t k = 0; k + 1 < run.size(); ++k)
                for (int c : kids[size_t(run[k])])
                    if (!used.contains(c) && legShaped(c))
                        leg(c);
        }
        fwd -= glm::dot(fwd, o.up) * o.up;
        if (glm::length(fwd) < 1e-6)
        {
            fwd = o.forward - glm::dot(o.forward, o.up) * o.up;
            rig.notes.push_back("could not tell which way the body faces; assumed the scene's forward");
        }
        rig.forward = glm::normalize(fwd);

        // Off the chest: legs, the neck (the branch reaching highest), arms.
        int neck = -1;
        std::vector<int> arms;
        for (int c : chestKids)
        {
            if (used.contains(c))
                continue;
            if (legShaped(c))
                leg(c);
            else if (has(c, {"neck", "head"}))
                neck = c;
        }
        if (neck < 0)
            for (int c : chestKids)
                if (!used.contains(c) && !legShaped(c) && !has(c, {"tail", "wing", "antenna"}))
                    if (neck < 0 || height(mainPath(c).back()) > height(mainPath(neck).back()))
                        neck = c;
        for (int c : chestKids)
            if (!used.contains(c) && c != neck)
            {
                if (has(c, {"wing"}))
                    named(c, Role::Wing);
                else if (has(c, {"tail"}))
                    named(c, Role::Tail);
                else if (limbShaped(c))
                    arms.push_back(c);
            }
        if (neck >= 0)
        {
            const std::vector<int> path = mainPath(neck);
            size_t head = path.size() - 1;
            for (size_t i = 0; i < path.size(); ++i)
                if (has(path[i], {"head", "skull"}))
                {
                    head = i;
                    break;
                }
                else if (i > 0 && kids[size_t(path[i])].size() >= 2)
                {
                    head = i;
                    break;
                }
            if (head > 0)
                addChain(Role::Neck, {path.begin(), path.begin() + std::ptrdiff_t(head)});
            addChain(Role::Head, {path[head]});
            for (int c : kids[size_t(path[head])])
                if (!used.contains(c))
                {
                    if (has(c, {"jaw", "chin", "mandible", "chelicera", "chelicerae"}))
                        named(c, Role::Jaw);
                    else if (has(c, {"antenna", "antennae", "feeler"}))
                        named(c, Role::Antenna);
                }
        }
        for (int c : arms)
            arm(c);
        for (int c : others)
        {
            const glm::dvec3 d = pos(mainPath(c).back()) - pos(pelvis);
            if (has(c, {"tail"}) || glm::dot(d, rig.forward) < 0.0)
                named(c, Role::Tail);
            else
                rig.notes.push_back(s.names[size_t(c)] + " branches off the pelvis but is neither a leg nor a tail; left out");
        }
        // Named parts anywhere else.
        for (size_t i = 0; i < s.size(); ++i)
            if (usable[i] && !used.contains(int(i)) && (s.parent[i] < 0 || !has(s.parent[i], {"tail", "wing", "antenna"})))
            {
                if (has(int(i), {"tail"}))
                    named(int(i), Role::Tail);
                else if (has(int(i), {"wing"}))
                    named(int(i), Role::Wing);
                else if (has(int(i), {"antenna", "antennae", "feeler"}))
                    named(int(i), Role::Antenna);
            }
        assignSides(pelvis);
        assignIndices();
        for (Chain& c : rig.chains)
            c.name = chainName(c.role, c.side, c.index);
        int legCount = 0;
        for (const Chain& c : rig.chains)
            legCount += c.role == Role::Leg ? 1 : 0;
        if (legCount == 0)
            rig.notes.push_back("found no legs (nothing reaches the ground in this pose)");
        else if (legCount % 2 != 0)
            rig.notes.push_back("found an odd number of legs");
    }

    void assignSides(int pelvis)
    {
        const glm::dvec3 left = rig.left();
        for (Chain& c : rig.chains)
        {
            if (c.role == Role::Root)
                continue;
            const int start = s.find(c.start);
            std::optional<Side> byName;
            for (int j : s.path(start, s.find(c.end)))
                if ((byName = sideOfName(tok[size_t(j)])))
                    break;
            const bool axial = c.role == Role::Spine || c.role == Role::Neck || c.role == Role::Head || c.role == Role::Jaw;
            if (byName)
                c.side = *byName;
            else if (axial)
                c.side = Side::Center;
            else
            {
                const double d = glm::dot(pos(s.find(c.end)) - pos(pelvis), left);
                c.side = std::abs(d) < 0.02 * span ? Side::Center : d > 0.0 ? Side::Left : Side::Right;
            }
        }
        // Fingers and toes take their limb's side.
        for (Chain& c : rig.chains)
            if (c.role == Role::Finger || c.role == Role::Toe)
                for (const Chain& limb : rig.chains)
                    if ((limb.role == Role::Arm || limb.role == Role::Leg) && !limb.goal.empty() && s.isBelow(s.find(c.start), s.find(limb.goal)))
                        c.side = limb.side;
    }

    void assignIndices()
    {
        // Repeated limbs count front to back per side; toes and clavicles follow their limb's number.
        for (Role role : {Role::Leg, Role::Arm, Role::Tail, Role::Wing, Role::Antenna, Role::Spine, Role::Neck, Role::Head, Role::Jaw})
            for (Side side : {Side::Center, Side::Left, Side::Right})
            {
                std::vector<Chain*> group;
                for (Chain& c : rig.chains)
                    if (c.role == role && c.side == side)
                        group.push_back(&c);
                std::stable_sort(group.begin(), group.end(), [&](const Chain* a, const Chain* b) {
                    return glm::dot(pos(s.find(a->start)), rig.forward) > glm::dot(pos(s.find(b->start)), rig.forward);
                });
                for (size_t k = 0; k < group.size(); ++k)
                    group[k]->index = int(k);
            }
        for (Chain& c : rig.chains)
            if (c.role == Role::Toe || c.role == Role::Clavicle)
                for (const Chain& limb : rig.chains)
                {
                    const bool below = c.role == Role::Toe && limb.role == Role::Leg && !limb.goal.empty() && s.isBelow(s.find(c.start), s.find(limb.goal));
                    const bool above = c.role == Role::Clavicle && limb.role == Role::Arm && s.isBelow(s.find(limb.start), s.find(c.end));
                    if (below || above)
                        c.index = limb.index;
                }
    }
};
} // namespace

Rig characterize(const Skeleton& s, const Pose& pose, const CharacterizeOptions& opts)
{
    Finder f(s, pose, opts);
    f.rig.forward = opts.forward;
    f.run();
    f.rig.setRetargetPose(s, pose);
    // Clavicles before their arms, spine before neck: a stable, readable order.
    std::stable_sort(f.rig.chains.begin(), f.rig.chains.end(), [](const Chain& a, const Chain& b) {
        if (a.role != b.role)
            return a.role < b.role;
        if (a.side != b.side)
            return a.side < b.side;
        return a.index < b.index;
    });
    return f.rig;
}
} // namespace lila::retarget
