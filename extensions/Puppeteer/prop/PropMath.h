#pragma once

// Props and weapons, the maths: classes and their presets, fitting grip points to a mesh, and where a hand's grip sits.
// Pure: no scene, so tests and any thread can use it.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace puppeteer::prop
{
enum class Class : int
{
    Generic,
    Sword1h,
    Sword2h,
    Dagger,
    Axe,
    Spear,
    Shield,
    Bow,
    Pistol,
    Rifle,
    Shotgun,
    Launcher,
    Count,
};

/** What a point on a prop is for. Grip points (main, off, alt) are where hands go. */
enum class Role : int
{
    Main,
    Off,
    Alt,
    Muzzle, // the aimed axis of a firearm or bow: +X out of it
    Tip,    // the aimed axis of a blade or pole
    Stock,  // what goes to the shoulder
    Holster, // on a character: where the prop's main grip rests when put away
    Count,
};

/** How a class's shape is read when fitting points. */
enum class Shape
{
    Blade,   // long, handle at one end below a guard
    Haft,    // long, a heavy head at one end (axes, hammers)
    Pole,    // long and even (spears, staffs)
    Firearm, // a barrel with a grip below it
    Tube,    // a launcher: a tube with grips below
    Plate,   // a shield
    Bow,
    Any,
};

enum class AimStyle
{
    Turn,
    Shoulder,
    Extend,
    Guard,
};

/** Where a class's prop is put away on a character: a joint by name (first found) and the grip's frame there. */
struct HolsterDefault
{
    std::vector<std::string> joints;
    /** In the character's frame: right, up, forward (cm) from the joint. */
    glm::dvec3 offset{0.0};
    /** The grip's +X (handle toward the thumb) and +Z (palm side) in the character's frame. */
    glm::dvec3 x{0.0, 1.0, 0.0}, z{0.0, 0.0, 1.0};
    const char* label = "Hip";
};

struct Preset
{
    Class cls = Class::Generic;
    const char* id = "generic";
    const char* label = "Generic";
    const char* icon = "box";
    Shape shape = Shape::Any;
    bool twoHanded = false;
    /** Hand poses (Hands panel names) for the main and off hand. */
    const char* mainPose = "grip";
    const char* offPose = "grip";
    /** Fingers the main hand's grip leaves posed rather than wrapping (the trigger finger). */
    bool triggerFinger = false;
    AimStyle aim = AimStyle::Turn;
    /** Guard: cm below the line to the target. Extend and Guard: share of the arm the grip is held out. */
    double drop = 0.0, reach = 0.9;
    HolsterDefault holster;
};

inline const std::array<Preset, size_t(Class::Count)>& presets()
{
    static const HolsterDefault kBack{{"spine_05", "spine_04", "spine_03", "spine_02", "chest", "spine"}, {0.0, 4.0, -16.0}, glm::normalize(glm::dvec3(-0.55, -0.83, 0.0)), {0.0, 0.0, -1.0}, "Back"};
    static const HolsterDefault kHipLeft{{"pelvis", "hips", "hip"}, {14.0, -2.0, 2.0}, glm::normalize(glm::dvec3(0.25, -0.95, 0.3)), {1.0, 0.0, 0.0}, "Left hip"};
    static const HolsterDefault kThighRight{{"thigh_r", "rightupleg", "upleg_r", "pelvis", "hips"}, {-9.0, -10.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, "Right thigh"};
    static const HolsterDefault kBelt{{"pelvis", "hips", "hip"}, {0.0, 0.0, -14.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, "Belt"};
    static const std::array<Preset, size_t(Class::Count)> all = {{
        {Class::Generic, "generic", "Generic", "box", Shape::Any, false, "grip", "grip", false, AimStyle::Turn, 0.0, 0.9, kBelt},
        {Class::Sword1h, "sword1h", "Sword", "sword", Shape::Blade, false, "grip", "grip", false, AimStyle::Guard, 12.0, 0.6, kHipLeft},
        {Class::Sword2h, "sword2h", "Greatsword", "swords", Shape::Blade, true, "grip", "grip", false, AimStyle::Guard, 18.0, 0.55, kBack},
        {Class::Dagger, "dagger", "Dagger", "cut", Shape::Blade, false, "grip", "grip", false, AimStyle::Guard, 8.0, 0.6, kThighRight},
        {Class::Axe, "axe", "Axe / Hammer", "axe", Shape::Haft, false, "grip", "grip", false, AimStyle::Guard, 10.0, 0.6, kHipLeft},
        {Class::Spear, "spear", "Spear / Staff", "wand", Shape::Pole, true, "grip", "grip", false, AimStyle::Guard, 6.0, 0.6, kBack},
        {Class::Shield, "shield", "Shield", "shield", Shape::Plate, false, "grip", "grip", false, AimStyle::Guard, 0.0, 0.45, kBack},
        {Class::Bow, "bow", "Bow", "bow", Shape::Bow, true, "grip", "hook", false, AimStyle::Extend, 0.0, 0.95, kBack},
        {Class::Pistol, "pistol", "Pistol", "target", Shape::Firearm, false, "trigger", "grip", true, AimStyle::Extend, 0.0, 0.9, kThighRight},
        {Class::Rifle, "rifle", "Rifle", "crosshair", Shape::Firearm, true, "trigger", "grip", true, AimStyle::Shoulder, 0.0, 0.9, kBack},
        {Class::Shotgun, "shotgun", "Shotgun", "crosshair", Shape::Firearm, true, "trigger", "grip", true, AimStyle::Shoulder, 0.0, 0.9, kBack},
        {Class::Launcher, "launcher", "Launcher", "rocket", Shape::Tube, true, "trigger", "grip", true, AimStyle::Shoulder, 0.0, 0.9, kBack},
    }};
    return all;
}

inline const Preset& preset(Class c) { return presets()[size_t(std::clamp(int(c), 0, int(Class::Count) - 1))]; }

inline std::optional<Class> classFromId(const std::string& id)
{
    for (const Preset& p : presets())
        if (id == p.id)
            return p.cls;
    // Common names other tools and people use.
    static const std::pair<const char*, Class> kAlias[] = {
        {"sword", Class::Sword1h},  {"longsword", Class::Sword2h}, {"greatsword", Class::Sword2h}, {"knife", Class::Dagger},
        {"hammer", Class::Axe},     {"mace", Class::Axe},          {"staff", Class::Spear},        {"polearm", Class::Spear},
        {"gun", Class::Pistol},     {"handgun", Class::Pistol},    {"smg", Class::Rifle},          {"carbine", Class::Rifle},
        {"bazooka", Class::Launcher}, {"rpg", Class::Launcher},    {"rocket", Class::Launcher},    {"crossbow", Class::Rifle},
    };
    for (const auto& [name, c] : kAlias)
        if (id == name)
            return c;
    return std::nullopt;
}

inline const char* roleId(Role r)
{
    static const char* kIds[] = {"main", "off", "alt", "muzzle", "tip", "stock", "holster"};
    return kIds[std::clamp(int(r), 0, int(Role::Count) - 1)];
}
inline std::optional<Role> roleFromId(const std::string& s)
{
    for (int i = 0; i < int(Role::Count); ++i)
        if (s == roleId(Role(i)))
            return Role(i);
    return std::nullopt;
}
inline bool isGrip(Role r) { return r == Role::Main || r == Role::Off || r == Role::Alt; }

inline const char* aimStyleId(AimStyle s)
{
    static const char* kIds[] = {"turn", "shoulder", "extend", "guard"};
    return kIds[int(s)];
}

// Frames -----------------------------------------------------------------------------------------------------------

/**
 * A frame from an origin, its +X and a direction +Z should lean toward (made orthogonal to X). Grip points use X along
 * the handle toward the thumb (the blade, the slide) and Z the way the palm faces (into the handle).
 */
inline glm::dmat4 frameXZ(const glm::dvec3& origin, glm::dvec3 x, glm::dvec3 z)
{
    x = glm::normalize(x);
    z = z - x * glm::dot(z, x);
    if (glm::dot(z, z) < 1e-12)
    {
        z = std::abs(x.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        z = z - x * glm::dot(z, x);
    }
    z = glm::normalize(z);
    const glm::dvec3 y = glm::cross(z, x);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x, 0.0);
    m[1] = glm::dvec4(y, 0.0);
    m[2] = glm::dvec4(z, 0.0);
    m[3] = glm::dvec4(origin, 1.0);
    return m;
}

/** A hand's measurements in its joint's space, from the hand rig. */
struct HandShape
{
    glm::dvec3 forward{1.0, 0.0, 0.0};   // along the fingers
    glm::dvec3 thumbSide{0.0, 0.0, 1.0}; // toward the thumb
    glm::dvec3 palm{0.0, -1.0, 0.0};     // the way the fingers curl
    /** The non-thumb fingers' first joints, averaged. */
    glm::dvec3 knuckles{9.0, 0.0, 0.0};
};

/**
 * Where a hand holds a handle of the radius given, in the hand joint's space: the handle's centre across the palm under
 * the first finger bones, X toward the thumb, Z the way the palm faces. The same grip frame sits the same way in either
 * hand, mirrored: one prop point fits a right and a left hand.
 */
inline glm::dmat4 gripFrame(const HandShape& h, double radius)
{
    const double k = std::max(glm::length(h.knuckles), 1e-3);
    const glm::dvec3 f = glm::normalize(h.forward), p = glm::normalize(h.palm);
    const glm::dvec3 origin = f * (0.85 * k) + p * (0.25 * k + std::max(radius, 0.0));
    return frameXZ(origin, h.thumbSide, p);
}

/** The world matrix that puts a prop's point (local: the point's frame in the prop root's space) on a world frame. */
inline glm::dmat4 placeOn(const glm::dmat4& frameWorld, const glm::dmat4& pointLocal) { return frameWorld * glm::inverse(pointLocal); }

/** Rigid part of a matrix (scale removed from its axes). */
inline glm::dmat4 rigidOf(const glm::dmat4& m)
{
    glm::dmat4 r = m;
    for (int c = 0; c < 3; ++c)
    {
        const double l = glm::length(glm::dvec3(m[c]));
        if (l > 1e-12)
            r[c] = glm::dvec4(glm::dvec3(m[c]) / l, 0.0);
    }
    return r;
}

// Fitting ------------------------------------------------------------------------------------------------------------

struct PointDef
{
    Role role = Role::Main;
    std::string name;
    /** In the prop root's space. */
    glm::dmat4 frame{1.0};
    /** Handle radius at a grip, cm. */
    double radius = 1.6;
};

struct Fit
{
    std::vector<PointDef> points;
    /** How the points were found ("mesh", or why not). */
    std::string from;
    /** The long axis and its length, for reports. */
    glm::dvec3 axis{1.0, 0.0, 0.0};
    double length = 0.0;
};

namespace detail
{
/** Eigenvectors of a symmetric 3x3 matrix, largest eigenvalue first (Jacobi rotations). */
inline std::array<glm::dvec3, 3> principalAxes(const glm::dmat3& cov)
{
    double a[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            a[i][j] = cov[j][i];
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 50; ++sweep)
    {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-18)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::abs(a[p][q]) < 1e-30)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 3; ++k)
                {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int x, int y) { return a[x][x] > a[y][y]; });
    std::array<glm::dvec3, 3> out;
    for (int i = 0; i < 3; ++i)
        out[size_t(i)] = glm::normalize(glm::dvec3(v[0][order[size_t(i)]], v[1][order[size_t(i)]], v[2][order[size_t(i)]]));
    return out;
}

/** The point set measured along an axis: slices from its low end to its high end. */
struct Slices
{
    glm::dvec3 centre{0.0}, axis{1.0, 0.0, 0.0};
    double lo = 0.0, hi = 0.0;
    std::vector<double> radius; // largest distance from the axis line in each slice
    std::vector<glm::dvec3> mean;
    std::vector<int> count;
    double at(int i) const { return lo + (hi - lo) * (double(i) + 0.5) / double(radius.size()); }
    int sliceOf(double t) const
    {
        const int n = int(radius.size());
        return std::clamp(int((t - lo) / std::max(hi - lo, 1e-9) * n), 0, n - 1);
    }
    /** A point on the axis at a fraction from lo (0) to hi (1). */
    glm::dvec3 along(double f) const { return centre + axis * (lo + (hi - lo) * f); }
};

inline Slices slice(const std::vector<glm::dvec3>& pts, const glm::dvec3& centre, const glm::dvec3& axis, int n = 32)
{
    Slices s;
    s.centre = centre;
    s.axis = axis;
    s.lo = 1e300;
    s.hi = -1e300;
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p - centre, axis);
        s.lo = std::min(s.lo, t);
        s.hi = std::max(s.hi, t);
    }
    s.radius.assign(size_t(n), 0.0);
    s.mean.assign(size_t(n), glm::dvec3(0.0));
    s.count.assign(size_t(n), 0);
    for (const glm::dvec3& p : pts)
    {
        const double t = glm::dot(p - centre, axis);
        const int i = s.sliceOf(t);
        const glm::dvec3 off = (p - centre) - axis * t;
        s.radius[size_t(i)] = std::max(s.radius[size_t(i)], glm::length(off));
        s.mean[size_t(i)] += p;
        ++s.count[size_t(i)];
    }
    for (int i = 0; i < n; ++i)
        if (s.count[size_t(i)])
            s.mean[size_t(i)] /= double(s.count[size_t(i)]);
        else
            s.mean[size_t(i)] = s.centre + axis * s.at(i);
    // Empty slices (gaps in a sparse mesh) take their neighbours' radius.
    for (int i = 0; i < n; ++i)
        if (!s.count[size_t(i)])
        {
            double r = 0.0;
            for (int d = 1; d < n && r == 0.0; ++d)
            {
                if (i - d >= 0 && s.count[size_t(i - d)])
                    r = s.radius[size_t(i - d)];
                if (i + d < n && s.count[size_t(i + d)])
                    r = std::max(r, s.radius[size_t(i + d)]);
            }
            s.radius[size_t(i)] = r;
        }
    return s;
}

/** Median of a range of slice radii, [from, to] inclusive. */
inline double medianRadius(const Slices& s, int from, int to)
{
    std::vector<double> r;
    for (int i = std::max(0, std::min(from, to)); i <= std::min(int(s.radius.size()) - 1, std::max(from, to)); ++i)
        r.push_back(s.radius[size_t(i)]);
    if (r.empty())
        return 1.6;
    std::sort(r.begin(), r.end());
    return r[r.size() / 2];
}

/** The centre of the slice at a fraction along the axis (its points' mean, kept on the axis line where it has none). */
inline glm::dvec3 centreAt(const Slices& s, double f)
{
    const int i = s.sliceOf(s.lo + (s.hi - s.lo) * f);
    if (!s.count[size_t(i)])
        return s.along(f);
    const glm::dvec3 m = s.mean[size_t(i)];
    const double t = s.lo + (s.hi - s.lo) * f;
    return m + s.axis * (t - glm::dot(m - s.centre, s.axis));
}
} // namespace detail

/**
 * Grip points for a prop of a class, fitted to its mesh points (in the prop root's space): the long axis by principal
 * components, which end is the handle by the cross-section along it (a guard, a head, a grip below a barrel), and the
 * grips, muzzle or tip, and stock where the class puts them. up is the world's up in the prop root's space, which
 * breaks ties for shapes that don't tell (a plain tube). Always gives points; `from` says how they were placed.
 */
inline Fit fitPoints(Class cls, const std::vector<glm::dvec3>& pts, const glm::dvec3& upIn = {0.0, 1.0, 0.0})
{
    using namespace detail;
    const Preset& pr = preset(cls);
    const glm::dvec3 up = glm::length(upIn) > 1e-9 ? glm::normalize(upIn) : glm::dvec3(0, 1, 0);
    Fit fit;
    auto add = [&](Role r, const std::string& name, const glm::dmat4& frame, double radius = 1.6) {
        fit.points.push_back({r, name, frame, radius});
    };
    if (pts.size() < 4)
    {
        // Nothing to measure: points at the origin, main along +X, for the user to place.
        fit.from = "no mesh under the prop: points start at its origin";
        add(Role::Main, "main", frameXZ(glm::dvec3(0.0), {0, 1, 0}, {0, 0, 1}));
        if (pr.twoHanded)
            add(Role::Off, "off", frameXZ(glm::dvec3(0.0, -10.0, 0.0), {0, 1, 0}, {0, 0, 1}));
        add(pr.shape == Shape::Firearm || pr.shape == Shape::Tube || pr.shape == Shape::Bow ? Role::Muzzle : Role::Tip, "aim",
            frameXZ(glm::dvec3(0.0, 0.0, 20.0), {0, 0, 1}, up));
        return fit;
    }
    fit.from = "mesh";
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : pts)
        c += p;
    c /= double(pts.size());
    glm::dmat3 cov(0.0);
    for (const glm::dvec3& p : pts)
    {
        const glm::dvec3 d = p - c;
        cov += glm::outerProduct(d, d);
    }
    cov /= double(pts.size());
    std::array<glm::dvec3, 3> e = principalAxes(cov);
    // A firearm's grip and stock pull principal axes off the barrel; props are usually modelled square to their own
    // axes, so those win when the longest of them is about as long as the principal one.
    if (pr.shape == Shape::Firearm || pr.shape == Shape::Tube)
    {
        glm::dvec3 lo(1e300), hi(-1e300);
        for (const glm::dvec3& p : pts)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        const glm::dvec3 ext = hi - lo;
        std::array<int, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int a, int b) { return ext[a] > ext[b]; });
        double pcaLen = 0.0;
        {
            double a = 1e300, b = -1e300;
            for (const glm::dvec3& p : pts)
            {
                a = std::min(a, glm::dot(p - c, e[0]));
                b = std::max(b, glm::dot(p - c, e[0]));
            }
            pcaLen = b - a;
        }
        if (ext[order[0]] >= 0.75 * pcaLen)
            for (int k = 0; k < 3; ++k)
            {
                glm::dvec3 axis(0.0);
                axis[order[size_t(k)]] = 1.0;
                e[size_t(k)] = axis;
            }
    }
    Slices s = slice(pts, c, e[0]);
    const int n = int(s.radius.size());
    fit.length = s.hi - s.lo;
    const double len = fit.length;

    // Flip the long axis so +axis runs from the handle end toward the business end; returns the slices re-measured.
    auto orient = [&](bool flip) {
        if (flip)
        {
            e[0] = -e[0];
            s = slice(pts, c, e[0]);
        }
        fit.axis = e[0];
    };

    switch (pr.shape)
    {
    case Shape::Blade:
    {
        // The guard is the widest slice; the handle is the shorter side of it (pommel to guard).
        int guard = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        orient(guard > n / 2);
        guard = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        double guardT = s.at(guard);
        // A blade with no guard (widest slice in the middle): the narrower end is the handle.
        if (guard > n * 2 / 5)
        {
            const double a = medianRadius(s, 0, n / 6), b = medianRadius(s, n - 1 - n / 6, n - 1);
            if (a > b)
                orient(true);
            guardT = s.lo + 0.22 * len;
            guard = s.sliceOf(guardT);
        }
        const double handleLen = guardT - s.lo;
        const double r = medianRadius(s, 0, std::max(0, guard - 1));
        // The handle's cross-section: palms face its flat side, the blade's edge leads the fingers.
        const glm::dvec3 edge = e[1], flat = e[2];
        const double fromGuard = std::min(4.5, 0.3 * handleLen);
        const glm::dvec3 main = centreAt(s, (guardT - fromGuard - s.lo) / len);
        add(Role::Main, "main", frameXZ(main, e[0], flat), r);
        if (pr.twoHanded || handleLen > 15.0)
        {
            const double offT = std::max(guardT - fromGuard - 9.0, s.lo + 0.25 * handleLen);
            add(pr.twoHanded ? Role::Off : Role::Alt, pr.twoHanded ? "off" : "pommel", frameXZ(centreAt(s, (offT - s.lo) / len), e[0], flat), r);
        }
        add(Role::Tip, "tip", frameXZ(centreAt(s, 1.0) + e[0] * (s.hi - glm::dot(centreAt(s, 1.0) - c, e[0])), e[0], edge));

        break;
    }
    case Shape::Haft:
    {
        // The head is the widest slice and sits at the business end; the grip is near the other.
        const int head = int(std::max_element(s.radius.begin(), s.radius.end()) - s.radius.begin());
        orient(head < n / 2);
        const double r = medianRadius(s, 0, n / 3);
        add(Role::Main, "main", frameXZ(centreAt(s, 0.1), e[0], e[2]), r);
        add(pr.twoHanded ? Role::Off : Role::Alt, pr.twoHanded ? "off" : "choke", frameXZ(centreAt(s, 0.4), e[0], e[2]), r);
        add(Role::Tip, "head", frameXZ(centreAt(s, 1.0), e[0], e[1]));
        break;
    }
    case Shape::Pole:
    {
        // A spearhead or a heavier end leads; an even staff leads with whichever end faces the world's forward.
        const double a = medianRadius(s, 0, n / 8), b = medianRadius(s, n - 1 - n / 8, n - 1);
        orient(std::abs(a - b) > 0.15 * std::max(a, b) ? a > b : false);
        const double r = medianRadius(s, n / 4, n * 3 / 4);
        add(Role::Main, "main", frameXZ(centreAt(s, 0.32), e[0], e[2]), r);
        add(Role::Off, "off", frameXZ(centreAt(s, 0.58), e[0], e[2]), r);
        add(Role::Alt, "middle", frameXZ(centreAt(s, 0.5), e[0], e[2]), r);
        add(Role::Tip, "tip", frameXZ(s.along(1.0), e[0], e[1]));
        break;
    }
    case Shape::Firearm:
    case Shape::Tube:
    {
        // Down is the side the grip (and magazine) hangs: the cross axis whose far side reaches furthest.
        glm::dvec3 down(0.0);
        double best = -1.0;
        for (int k = 1; k < 3; ++k)
            for (double sign : {1.0, -1.0})
            {
                const glm::dvec3 d = e[size_t(k)] * sign;
                double reach = 0.0, mean = 0.0;
                for (const glm::dvec3& p : pts)
                {
                    const double v = glm::dot(p - c, d);
                    reach = std::max(reach, v);
                    mean += v;
                }
                // A tube with nothing hanging off it: down is the world's down.
                const double score = reach + (pr.shape == Shape::Tube ? 0.5 * std::max(0.0, glm::dot(d, -up)) * reach : 0.0);
                if (score > best)
                {
                    best = score;
                    down = d;
                }
            }
        if (glm::dot(down, -up) < -0.5 && pr.shape == Shape::Tube)
            down = -down;
        const glm::dvec3 upAxis = -down;
        double topV = -1e300, botV = 1e300;
        for (const glm::dvec3& p : pts)
        {
            topV = std::max(topV, glm::dot(p - c, upAxis));
            botV = std::min(botV, glm::dot(p - c, upAxis));
        }
        const double height = topV - botV;
        // The grip region: the lowest part, below the barrel.
        std::vector<glm::dvec3> low;
        for (const glm::dvec3& p : pts)
            if (glm::dot(p - c, upAxis) < botV + 0.45 * height)
                low.push_back(p);
        glm::dvec3 lowMean(0.0);
        for (const glm::dvec3& p : low)
            lowMean += p;
        lowMean = low.empty() ? c : lowMean / double(low.size());
        // The muzzle is the end further from where the grip hangs.
        orient(glm::dot(lowMean - c, e[0]) > 0.0);
        const glm::dvec3 fwd = e[0];
        // The barrel line: the top band's centre height.
        const double barrelV = topV - 0.2 * height;
        auto onBarrel = [&](double f) {
            glm::dvec3 p = s.along(f);
            return p + upAxis * (barrelV - glm::dot(p - c, upAxis));
        };
        if (pr.shape == Shape::Tube)
        {
            const double rad = 0.5 * height;
            const glm::dvec3 axisLine = s.along(0.0) + upAxis * (topV - rad - glm::dot(s.along(0.0) - c, upAxis));
            auto onTube = [&](double f) { return axisLine + fwd * (len * f); };
            add(Role::Main, "main", frameXZ(onTube(0.45) - upAxis * (rad + 4.0), upAxis, fwd), 1.6);
            add(Role::Off, "off", frameXZ(onTube(0.62) - upAxis * (rad + 4.0), upAxis, fwd), 1.6);
            add(Role::Stock, "shoulder", frameXZ(onTube(0.4) - upAxis * rad, fwd, upAxis));
            add(Role::Muzzle, "muzzle", frameXZ(onTube(1.0), fwd, upAxis));
            break;
        }
        // The main grip: the rear-most part of the grip region, its handle running up toward the barrel.
        std::vector<glm::dvec3> grip;
        double rear = 1e300;
        for (const glm::dvec3& p : low)
            rear = std::min(rear, glm::dot(p - c, fwd));
        const bool pistol = cls == Class::Pistol;
        for (const glm::dvec3& p : low)
        {
            const double t = glm::dot(p - c, fwd);
            // A long gun's stock reaches lowest at the back: its grip is ahead of it, a fifth of the way along.
            const double from = pistol ? rear : s.lo + 0.18 * len, to = pistol ? rear + 0.6 * len : s.lo + 0.42 * len;
            if (t >= from && t <= to)
                grip.push_back(p);
        }
        glm::dvec3 g(0.0);
        for (const glm::dvec3& p : grip)
            g += p;
        g = grip.empty() ? (pistol ? lowMean : onBarrel(0.3) - upAxis * 5.0) : g / double(grip.size());
        double gripR = 0.0;
        for (const glm::dvec3& p : grip)
        {
            const glm::dvec3 d = p - g;
            gripR = std::max(gripR, glm::length(d - upAxis * glm::dot(d, upAxis)));
        }
        gripR = grip.empty() ? 1.6 : std::clamp(0.6 * gripR, 1.0, 3.5);
        // The handle leans back as it goes down: its axis from the grip's bottom toward where it meets the barrel.
        const glm::dvec3 handleUp = glm::normalize(upAxis - 0.25 * fwd);
        add(Role::Main, "main", frameXZ(g, handleUp, fwd), gripR);
        if (pistol)
        {
            // The support hand cups the shooting hand: a little ahead and to the side.
            const glm::dvec3 side = glm::normalize(glm::cross(upAxis, fwd));
            add(Role::Off, "support", frameXZ(g - side * 2.5 + fwd * 1.5 - upAxis * 1.5, handleUp, fwd), gripR);
            add(Role::Muzzle, "muzzle", frameXZ(onBarrel(1.0), fwd, upAxis));
        }
        else
        {
            // The off hand under the handguard, palm up, thumb along the barrel.
            const double offF = cls == Class::Shotgun ? 0.66 : 0.6;
            const glm::dvec3 under = onBarrel(offF) - upAxis * (0.08 * height + 2.0);
            add(Role::Off, "off", frameXZ(under, fwd, upAxis), std::clamp(0.12 * height, 1.5, 3.0));
            add(Role::Alt, "magwell", frameXZ(onBarrel(0.45) - upAxis * (0.25 * height), upAxis, fwd), 2.0);
            add(Role::Stock, "stock", frameXZ(onBarrel(0.0) - upAxis * (0.15 * height), fwd, upAxis));
            add(Role::Muzzle, "muzzle", frameXZ(onBarrel(1.0), fwd, upAxis));
        }
        break;
    }
    case Shape::Plate:
    {
        // The normal is the thin axis; the handle side is the one things stand out on (straps, a grip).
        glm::dvec3 nrm = e[2];
        double front = 0.0, back = 0.0;
        for (const glm::dvec3& p : pts)
        {
            const double v = glm::dot(p - c, nrm);
            front = std::max(front, v);
            back = std::max(back, -v);
        }
        if (front > back)
        {
            nrm = -nrm;
            std::swap(front, back);
        }
        // nrm now points out of the face; the hand is behind, gripping a handle that runs up the shield.
        glm::dvec3 vertical = std::abs(glm::dot(e[0], up)) > std::abs(glm::dot(e[1], up)) ? e[0] : e[1];
        if (glm::dot(vertical, up) < 0.0)
            vertical = -vertical;
        fit.axis = nrm;
        const glm::dvec3 grip = c - nrm * (0.6 * back + 2.0);
        add(Role::Main, "main", frameXZ(grip, vertical, nrm), 1.6);
        add(Role::Tip, "face", frameXZ(c + nrm * front, nrm, vertical));
        break;
    }
    case Shape::Bow:
    {
        // Upright; the arrow flies across the bow, away from the string side, which the limbs' tips lean toward.
        glm::dvec3 vertical = e[0];
        if (glm::dot(vertical, up) < 0.0)
            vertical = -vertical;
        fit.axis = vertical;
        Slices sv = slice(pts, c, vertical);
        const glm::dvec3 tips = 0.5 * (sv.mean.front() + sv.mean.back());
        const glm::dvec3 mid = centreAt(sv, 0.5);
        glm::dvec3 aim = mid - tips;
        aim -= vertical * glm::dot(aim, vertical);
        if (glm::length(aim) < 1e-6)
            aim = e[1];
        aim = glm::normalize(aim);
        const double r = medianRadius(sv, n * 2 / 5, n * 3 / 5);
        add(Role::Main, "main", frameXZ(mid, vertical, aim), r);
        // The draw hand on the string at rest, level with the grip.
        const double stringOff = std::max(0.0, glm::dot(mid - tips, aim)) + 1.0;
        add(Role::Off, "string", frameXZ(mid - aim * stringOff + vertical * 2.0, vertical, -aim), 0.5);
        add(Role::Muzzle, "rest", frameXZ(mid + vertical * (r + 2.0), aim, vertical));
        break;
    }
    case Shape::Any:
    {
        // The narrower end is the handle.
        const double a = medianRadius(s, 0, n / 5), b = medianRadius(s, n - 1 - n / 5, n - 1);
        orient(a > b);
        const double r = medianRadius(s, 0, n / 4);
        add(Role::Main, "main", frameXZ(centreAt(s, 0.12), e[0], e[2]), r);
        add(Role::Tip, "tip", frameXZ(s.along(1.0), e[0], e[1]));
        break;
    }
    }
    return fit;
}
} // namespace puppeteer::prop
