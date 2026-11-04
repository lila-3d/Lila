#pragma once

// Header only, standard library only, no exceptions: LilaLink's Control Rig unit compiles a byte-identical copy
// (ue/LilaLink/Source/LilaLinkRig/Private/LilaExpressionVm.h), so Unreal evaluates expressions exactly as Lila does.
// lila_rigmath_tests checks both that the copy matches and that every op matches its rig math node.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace lila_expr
{
inline constexpr double kPi = 3.14159265358979323846;

enum class Op : uint8_t
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Pow,
    Fmod,
    Negate,
    Not,
    And,
    Or,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    Select,
    Abs,
    Min,
    Max,
    Clamp,
    Lerp,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Sqrt,
    Exp,
    Loge,
    Log,
    Sign,
    Hypotenuse,
    RadiansToDegrees,
    DegreesToRadians,
    Wrap,
    MapRangeClamped,
    Floor,
    Ceil,
    Round,
    Truncate,
    PerlinNoise1D,
    RandomFloat,
    RandomFloatInRange,
    Count,
};

struct OpInfo
{
    const char* name; // the rig math function
    int inputs;
};

inline const OpInfo& info(Op op)
{
    static const OpInfo kInfo[] = {
        {"add", 2},
        {"subtract", 2},
        {"multiply", 2},
        {"divide", 2},
        {"pow", 2},
        {"fmod", 2},
        {"negate", 1},
        {"not", 1},
        {"and", 2},
        {"or", 2},
        {"less", 2},
        {"lessEqual", 2},
        {"greater", 2},
        {"greaterEqual", 2},
        {"equal", 2},
        {"notEqual", 2},
        {"select", 3},
        {"abs", 1},
        {"min", 2},
        {"max", 2},
        {"clamp", 3},
        {"lerp", 3},
        {"sin", 1},
        {"cos", 1},
        {"tan", 1},
        {"asin", 1},
        {"acos", 1},
        {"atan", 1},
        {"atan2", 2},
        {"sqrt", 1},
        {"exp", 1},
        {"loge", 1},
        {"log", 2},
        {"sign", 1},
        {"hypotenuse", 2},
        {"radiansToDegrees", 1},
        {"degreesToRadians", 1},
        {"wrap", 3},
        {"mapRangeClamped", 5},
        {"floor", 1},
        {"ceil", 1},
        {"round", 1},
        {"truncate", 1},
        {"perlinNoise1D", 2},
        {"randomFloatFromStream", 1},
        {"randomFloatInRangeFromStream", 3},
    };
    static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == size_t(Op::Count));
    return kInfo[size_t(op)];
}

/** The op a rig math function name evaluates as, or Op::Count. */
inline Op opNamed(std::string_view name)
{
    for (size_t i = 0; i < size_t(Op::Count); ++i)
        if (name == info(Op(i)).name)
            return Op(i);
    return Op::Count;
}

/** A float plug read as a bool or an int pin, as the graph converts it across a connection. */
inline bool toBool(double v) { return v != 0.0; }
inline int64_t toInt(double v)
{
    if (!(v == v))
        return 0;
    return int64_t(std::clamp(v, -9.2e18, 9.2e18));
}

namespace detail
{
inline uint32_t rot(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

inline void finalMix(uint32_t& a, uint32_t& b, uint32_t& c)
{
    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);
}

inline void mix(uint32_t& a, uint32_t& b, uint32_t& c)
{
    a -= c;
    a ^= rot(c, 4);
    c += b;
    b -= a;
    b ^= rot(a, 6);
    a += c;
    c -= b;
    c ^= rot(b, 8);
    b += a;
    a -= c;
    a ^= rot(c, 16);
    c += b;
    b -= a;
    b ^= rot(a, 19);
    a += c;
    c -= b;
    c ^= rot(b, 4);
    b += a;
}

inline uint32_t hash4(uint32_t kx, uint32_t ky, uint32_t kz, uint32_t kw)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (4 << 2) + 13;
    a += kx;
    b += ky;
    c += kz;
    mix(a, b, c);
    a += kw;
    finalMix(a, b, c);
    return c;
}

/** Jenkins' lookup3 final mix of three words. */
inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    a += 0xdeadbeef;
    b += 0x9e3779b9;
    c += 0x85ebca6b;
    finalMix(a, b, c);
    return c;
}

inline double hashToUnit(uint32_t h) { return double(h) / 4294967296.0; }

/** A seeded uniform value in [0, 1). */
inline double unitRandom(int64_t seed) { return hashToUnit(hash3(uint32_t(seed), uint32_t(uint64_t(seed) >> 32), 0)); }

inline uint32_t cell(double v) { return uint32_t(int64_t(v)); }

/** 4D gradient noise, roughly -1..1. */
inline double gradientNoise4(const double p[4])
{
    double i[4], f[4], u[4];
    auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    for (int k = 0; k < 4; ++k)
    {
        i[k] = std::floor(p[k]);
        f[k] = p[k] - i[k];
        u[k] = fade(f[k]);
    }
    // 32 gradients: every axis pair-free direction with one zero component and +-1 elsewhere.
    auto grad = [](uint32_t h, const double d[4]) {
        h &= 31;
        const int zero = int(h >> 3);
        const double signs[3] = {(h & 1) ? -1.0 : 1.0, (h & 2) ? -1.0 : 1.0, (h & 4) ? -1.0 : 1.0};
        double c[4];
        int k = 0;
        for (int a = 0; a < 4; ++a)
            c[a] = a == zero ? 0.0 : signs[k++] * d[a];
        return c[0] + c[1] + c[2] + c[3];
    };
    double acc[16];
    for (int corner = 0; corner < 16; ++corner)
    {
        const double o[4] = {double(corner & 1), double((corner >> 1) & 1), double((corner >> 2) & 1), double((corner >> 3) & 1)};
        const uint32_t h = hash4(cell(i[0] + o[0]), cell(i[1] + o[1]), cell(i[2] + o[2]), cell(i[3] + o[3]));
        const double d[4] = {f[0] - o[0], f[1] - o[1], f[2] - o[2], f[3] - o[3]};
        acc[corner] = grad(h, d);
    }
    for (int axis = 0, n = 16; axis < 4; ++axis, n /= 2)
        for (int k = 0; k < n / 2; ++k)
            acc[k] = acc[2 * k] + (acc[2 * k + 1] - acc[2 * k]) * u[axis];
    return acc[0] * 0.5;
}

inline double wrap(double value, double minValue, double maxValue)
{
    const double size = maxValue - minValue;
    if (size == 0.0)
        return maxValue;
    if (value < minValue)
        return maxValue - std::fmod(minValue - value, size);
    if (value > maxValue)
        return minValue + std::fmod(value - maxValue, size);
    return value;
}

inline double lerp(double a, double b, double t) { return a + t * (b - a); }

inline double mapRangeClamped(double v, double inMin, double inMax, double outMin, double outMax)
{
    const double divisor = inMax - inMin;
    const double pct = std::abs(divisor) <= 1.0e-8 ? (v >= inMax ? 1.0 : 0.0) : (v - inMin) / divisor;
    return lerp(outMin, outMax, std::clamp(pct, 0.0, 1.0));
}
} // namespace detail

/** Smooth noise in -1..1 along value; seed picks an unrelated pattern. */
inline double perlinNoise1D(double value, int64_t seed)
{
    const double p[4] = {value, 0.5, 0.25, double(seed) * 1.6180339887 + 0.37};
    return detail::gradientNoise4(p);
}

/** One op's outputs from its inputs, in the rig math node's pin order; out[1] is the second output (fmod's remainder). */
inline void apply(Op op, const double* a, double out[2])
{
    using namespace detail;
    auto b = [](bool v) { return v ? 1.0 : 0.0; };
    out[1] = 0.0;
    double& r = out[0];
    switch (op)
    {
    case Op::Add: r = a[0] + a[1]; break;
    case Op::Subtract: r = a[0] - a[1]; break;
    case Op::Multiply: r = a[0] * a[1]; break;
    case Op::Divide: r = a[1] == 0.0 ? 0.0 : a[0] / a[1]; break;
    case Op::Pow: r = a[0] < 0.0 && a[1] != std::floor(a[1]) ? 0.0 : std::pow(a[0], a[1]); break;
    case Op::Fmod:
        if (a[1] == 0.0)
            r = 0.0;
        else
        {
            r = double(toInt(std::trunc(a[0] / a[1])));
            out[1] = std::fmod(a[0], a[1]);
        }
        break;
    case Op::Negate: r = -a[0]; break;
    case Op::Not: r = b(!toBool(a[0])); break;
    case Op::And: r = b(toBool(a[0]) && toBool(a[1])); break;
    case Op::Or: r = b(toBool(a[0]) || toBool(a[1])); break;
    case Op::Less: r = b(a[0] < a[1]); break;
    case Op::LessEqual: r = b(a[0] <= a[1]); break;
    case Op::Greater: r = b(a[0] > a[1]); break;
    case Op::GreaterEqual: r = b(a[0] >= a[1]); break;
    case Op::Equal: r = b(a[0] == a[1]); break;
    case Op::NotEqual: r = b(a[0] != a[1]); break;
    case Op::Select: r = toBool(a[2]) ? a[0] : a[1]; break;
    case Op::Abs: r = std::abs(a[0]); break;
    case Op::Min: r = std::min(a[0], a[1]); break;
    case Op::Max: r = std::max(a[0], a[1]); break;
    case Op::Clamp: r = std::max(a[1], std::min(a[0], a[2])); break;
    case Op::Lerp: r = lerp(a[0], a[1], a[2]); break;
    case Op::Sin: r = std::sin(a[0]); break;
    case Op::Cos: r = std::cos(a[0]); break;
    case Op::Tan: r = std::tan(a[0]); break;
    case Op::Asin: r = std::asin(std::clamp(a[0], -1.0, 1.0)); break;
    case Op::Acos: r = std::acos(std::clamp(a[0], -1.0, 1.0)); break;
    case Op::Atan: r = std::atan(a[0]); break;
    case Op::Atan2: r = std::atan2(a[0], a[1]); break;
    case Op::Sqrt: r = a[0] >= 0.0 ? std::sqrt(a[0]) : 0.0; break;
    case Op::Exp: r = std::exp(a[0]); break;
    case Op::Loge: r = a[0] > 0.0 ? std::log(a[0]) : 0.0; break;
    case Op::Log: r = a[0] > 0.0 && a[1] > 0.0 && a[1] != 1.0 ? std::log(a[0]) / std::log(a[1]) : 0.0; break;
    case Op::Sign: r = a[0] > 0.0 ? 1.0 : (a[0] < 0.0 ? -1.0 : 0.0); break;
    case Op::Hypotenuse: r = std::hypot(a[0], a[1]); break;
    case Op::RadiansToDegrees: r = a[0] * (180.0 / kPi); break;
    case Op::DegreesToRadians: r = a[0] * (kPi / 180.0); break;
    case Op::Wrap: r = wrap(a[0], a[1], a[2]); break;
    case Op::MapRangeClamped: r = mapRangeClamped(a[0], a[1], a[2], a[3], a[4]); break;
    case Op::Floor: r = std::floor(a[0]); out[1] = double(toInt(r)); break;
    case Op::Ceil: r = std::ceil(a[0]); out[1] = double(toInt(r)); break;
    case Op::Round: r = std::round(a[0]); out[1] = double(toInt(r)); break;
    case Op::Truncate: r = std::trunc(a[0]); out[1] = double(toInt(r)); break;
    case Op::PerlinNoise1D: r = perlinNoise1D(a[0], toInt(a[1])); break;
    case Op::RandomFloat: r = unitRandom(toInt(a[0])); break;
    case Op::RandomFloatInRange: r = lerp(a[0], a[1], unitRandom(toInt(a[2]))); break;
    case Op::Count: r = 0.0; break;
    }
    // Rig math nodes output 0 for anything not finite.
    for (int k = 0; k < 2; ++k)
        if (!std::isfinite(out[k]))
            out[k] = 0.0;
}

/** Where an op input comes from. */
struct Arg
{
    enum Kind : uint8_t
    {
        Constant,
        Input, // the program's index-th input value
        Time,  // seconds
        Frame, // seconds * fps
        Node,  // an earlier node's output
    };
    Kind kind = Constant;
    double value = 0.0;
    int index = 0;
    int output = 0;
};

struct Step
{
    Op op = Op::Count;
    std::vector<Arg> args;
};

/**
 * An expression as ops in evaluation order. Text form, one line each:
 *   fps 30
 *   inputs 2
 *   add i0 0.5        (i<k> input, t time, f frame, n<k>.<o> node k's output o, else a number)
 *   sin n0.0
 *   = n1.0            (the result; a bare argument such as "= i0" or "= 2" works too)
 */
struct Program
{
    double fps = 30.0;
    int inputs = 0;
    std::vector<Step> steps;
    Arg result;
};

namespace detail
{
inline std::string_view nextWord(std::string_view& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t')
        ++j;
    std::string_view w = s.substr(i, j - i);
    s.remove_prefix(j);
    return w;
}

inline bool number(std::string_view w, double& out)
{
    if (w.empty())
        return false;
    std::string s(w);
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

inline bool parseArg(std::string_view w, Arg& a, int steps)
{
    double n = 0.0;
    if (w == "t")
        a.kind = Arg::Time;
    else if (w == "f")
        a.kind = Arg::Frame;
    else if (w.size() > 1 && w[0] == 'i' && number(w.substr(1), n))
    {
        a.kind = Arg::Input;
        a.index = int(n);
    }
    else if (w.size() > 1 && w[0] == 'n')
    {
        const size_t dot = w.find('.');
        double o = 0.0;
        if (!number(w.substr(1, dot == std::string_view::npos ? std::string_view::npos : dot - 1), n) ||
            (dot != std::string_view::npos && !number(w.substr(dot + 1), o)))
            return false;
        a.kind = Arg::Node;
        a.index = int(n);
        a.output = int(o);
        if (a.index < 0 || a.index >= steps || a.output < 0 || a.output > 1)
            return false;
    }
    else if (number(w, n))
    {
        a.kind = Arg::Constant;
        a.value = n;
    }
    else
        return false;
    return true;
}
} // namespace detail

/** Reads a program's text; false with why set when it can't. */
inline bool parse(std::string_view text, Program& out, std::string* why = nullptr)
{
    out = Program();
    bool haveResult = false;
    int lineNo = 0;
    auto fail = [&](const std::string& what) {
        if (why)
            *why = "line " + std::to_string(lineNo) + ": " + what;
        return false;
    };
    while (!text.empty())
    {
        ++lineNo;
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        std::string_view head = detail::nextWord(line);
        if (head.empty() || head[0] == '#')
            continue;
        if (head == "fps" || head == "inputs")
        {
            double v = 0.0;
            if (!detail::number(detail::nextWord(line), v))
                return fail("expected a number after " + std::string(head));
            if (head == "fps")
                out.fps = v;
            else
                out.inputs = int(v);
            continue;
        }
        if (head == "=")
        {
            if (!detail::parseArg(detail::nextWord(line), out.result, int(out.steps.size())))
                return fail("bad result");
            haveResult = true;
            continue;
        }
        Step s;
        s.op = opNamed(head);
        if (s.op == Op::Count)
            return fail("unknown op " + std::string(head));
        for (std::string_view w = detail::nextWord(line); !w.empty(); w = detail::nextWord(line))
        {
            Arg a;
            if (!detail::parseArg(w, a, int(out.steps.size())))
                return fail("bad argument " + std::string(w));
            s.args.push_back(a);
        }
        if (int(s.args.size()) != info(s.op).inputs)
            return fail(std::string(info(s.op).name) + " takes " + std::to_string(info(s.op).inputs) + " arguments");
        out.steps.push_back(std::move(s));
    }
    for (const Step& s : out.steps)
        for (const Arg& a : s.args)
            if (a.kind == Arg::Input && (a.index < 0 || a.index >= out.inputs))
                return fail("input i" + std::to_string(a.index) + " is past inputs");
    if (!haveResult)
        return fail("no result line (= ...)");
    return true;
}

/**
 * A joint's channels (translate xyz, rotate xyz in degrees, scale xyz) against an Unreal bone's local transform. Lila's local
 * matrix is T * jointOrient * R * rotateAxis * S (no pivots); Unreal's is K * that * K^T with translation times unitScale,
 * K the axis change from Lila's space to Unreal's. Matrices act on column vectors and are indexed [row][column].
 */
namespace channels
{
struct M3
{
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

inline M3 mul(const M3& a, const M3& b)
{
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

inline M3 transpose(const M3& a)
{
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[j][i];
    return r;
}

inline void apply(const M3& a, const double v[3], double out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
}

/** Axis indices in application order, by Lila's RotateOrder: xyz, yzx, zxy, xzy, yxz, zyx. */
inline const int* orderAxes(int order)
{
    static const int kAxes[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
    return kAxes[std::clamp(order, 0, 5)];
}

inline M3 axisRotation(int axis, double radians)
{
    M3 r;
    const double c = std::cos(radians), s = std::sin(radians);
    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    r.m[a][a] = c;
    r.m[a][b] = -s;
    r.m[b][a] = s;
    r.m[b][b] = c;
    return r;
}

/** Rotation from Euler degrees; the order's first axis is applied first. */
inline M3 eulerToMatrix(const double degrees[3], int order)
{
    const int* ax = orderAxes(order);
    const double k = kPi / 180.0;
    return mul(axisRotation(ax[2], degrees[ax[2]] * k), mul(axisRotation(ax[1], degrees[ax[1]] * k), axisRotation(ax[0], degrees[ax[0]] * k)));
}

/** Euler degrees of a rotation in order: one of its two solutions. */
inline void matrixToEuler(const M3& rot, int order, double out[3])
{
    const int* ax = orderAxes(order);
    const int i = ax[0], j = ax[1], k = ax[2];
    // +1 when i, j, k run cyclically (x y z, y z x, z x y).
    const double s = (j - i + 3) % 3 == 1 ? 1.0 : -1.0;
    const auto& R = rot.m;
    const double c = std::atan2(s * R[j][i], R[i][i]);
    const double b = std::atan2(-s * R[k][i], std::sqrt(R[i][i] * R[i][i] + R[j][i] * R[j][i]));
    const double sc = s * std::sin(c), cc = std::cos(c);
    const double a = std::atan2(s * (sc * R[i][k] - cc * R[j][k]), cc * R[j][j] - sc * R[i][j]);
    const double d = 180.0 / kPi;
    out[i] = a * d;
    out[j] = b * d;
    out[k] = c * d;
}

/** Euler degrees of rotation in order, wrapped and flipped to lie closest to ref. */
inline void eulerNear(const M3& rot, int order, const double ref[3], double out[3])
{
    double a[3], b[3];
    matrixToEuler(rot, order, a);
    const int* s = orderAxes(order);
    for (int i = 0; i < 3; ++i)
        b[i] = a[i];
    b[s[0]] += 180.0;
    b[s[1]] = 180.0 - b[s[1]];
    b[s[2]] += 180.0;
    double da = 0.0, db = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        a[i] += 360.0 * std::round((ref[i] - a[i]) / 360.0);
        b[i] += 360.0 * std::round((ref[i] - b[i]) / 360.0);
        da += (a[i] - ref[i]) * (a[i] - ref[i]);
        db += (b[i] - ref[i]) * (b[i] - ref[i]);
    }
    const double* pick = db < da ? b : a;
    for (int i = 0; i < 3; ++i)
        out[i] = pick[i];
}

/** A unit quaternion (x, y, z, w) as a rotation matrix. */
inline M3 quatToMatrix(const double q[4])
{
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    M3 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z);
    r.m[0][1] = 2 * (x * y - w * z);
    r.m[0][2] = 2 * (x * z + w * y);
    r.m[1][0] = 2 * (x * y + w * z);
    r.m[1][1] = 1 - 2 * (x * x + z * z);
    r.m[1][2] = 2 * (y * z - w * x);
    r.m[2][0] = 2 * (x * z - w * y);
    r.m[2][1] = 2 * (y * z + w * x);
    r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

inline void matrixToQuat(const M3& rot, double q[4])
{
    const auto& m = rot.m;
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0)
    {
        const double s = 0.5 / std::sqrt(trace + 1.0);
        q[3] = 0.25 / s;
        q[0] = (m[2][1] - m[1][2]) * s;
        q[1] = (m[0][2] - m[2][0]) * s;
        q[2] = (m[1][0] - m[0][1]) * s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const double s = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25 * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        const double s = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25 * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        const double s = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25 * s;
    }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] /= n;
}

/** An Unreal bone's local transform. */
struct BoneTransform
{
    double rotation[4] = {0, 0, 0, 1}; // x, y, z, w
    double translation[3] = {0, 0, 0};
    double scale[3] = {1, 1, 1};
};

struct Joint
{
    M3 orient;    // jointOrient
    M3 axis;      // rotateAxis
    int order = 0; // rotateOrder
    double rest[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};
    // For a root bone under transforms that aren't bones: their world matrix, rotation and translation in Lila's space.
    M3 parentRotation;
    double parentTranslation[3] = {0, 0, 0};
};

struct Basis
{
    M3 k; // Unreal axes from Lila axes
    double unitScale = 1.0;
};

/** The joint's nine channels from the bone's local transform. */
inline void read(const BoneTransform& bone, const Joint& j, const Basis& basis, double out[9])
{
    const M3 kt = transpose(basis.k);
    // Lila's linear part and translation, with any non-bone parent taken off.
    M3 r = mul(kt, mul(quatToMatrix(bone.rotation), basis.k));
    const M3 sUe = [&] {
        M3 s;
        for (int i = 0; i < 3; ++i)
            s.m[i][i] = bone.scale[i];
        return mul(kt, mul(s, basis.k));
    }();
    M3 linear = mul(r, sUe);
    double t[3];
    apply(kt, bone.translation, t);
    for (double& v : t)
        v /= basis.unitScale;
    const M3 pInv = transpose(j.parentRotation);
    double tp[3] = {t[0] - j.parentTranslation[0], t[1] - j.parentTranslation[1], t[2] - j.parentTranslation[2]};
    apply(pInv, tp, t);
    linear = mul(pInv, linear);

    double scale[3];
    for (int c = 0; c < 3; ++c)
    {
        scale[c] = std::sqrt(linear.m[0][c] * linear.m[0][c] + linear.m[1][c] * linear.m[1][c] + linear.m[2][c] * linear.m[2][c]);
        const double sign = j.rest[6 + c] < 0.0 ? -1.0 : 1.0;
        scale[c] *= sign;
    }
    M3 rot = linear;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            rot.m[i][c] = scale[c] == 0.0 ? (i == c ? 1.0 : 0.0) : rot.m[i][c] / scale[c];
    const M3 rotate = mul(transpose(j.orient), mul(rot, transpose(j.axis)));
    double e[3];
    eulerNear(rotate, j.order, j.rest + 3, e);
    for (int i = 0; i < 3; ++i)
    {
        out[i] = t[i];
        out[3 + i] = e[i];
        out[6 + i] = scale[i];
    }
}

/** The bone's local transform for the joint's nine channels. */
inline BoneTransform write(const double ch[9], const Joint& j, const Basis& basis)
{
    const M3 rot = mul(j.parentRotation, mul(j.orient, mul(eulerToMatrix(ch + 3, j.order), j.axis)));
    double t[3], tl[3] = {ch[0], ch[1], ch[2]};
    apply(j.parentRotation, tl, t);
    for (int i = 0; i < 3; ++i)
        t[i] += j.parentTranslation[i];
    BoneTransform out;
    const M3 kt = transpose(basis.k);
    matrixToQuat(mul(basis.k, mul(rot, kt)), out.rotation);
    double tu[3];
    apply(basis.k, t, tu);
    // K S K^T stays diagonal for an axis permutation: Unreal's scale is Lila's, reordered.
    M3 s;
    for (int i = 0; i < 3; ++i)
        s.m[i][i] = ch[6 + i];
    const M3 su = mul(basis.k, mul(s, kt));
    for (int i = 0; i < 3; ++i)
    {
        out.translation[i] = tu[i] * basis.unitScale;
        out.scale[i] = su.m[i][i];
    }
    return out;
}
} // namespace channels

/** Evaluates a parsed program; inputs holds program.inputs values. */
inline double evaluate(const Program& p, const double* inputs, double seconds)
{
    std::vector<double> results(p.steps.size() * 2, 0.0);
    auto read = [&](const Arg& a) {
        switch (a.kind)
        {
        case Arg::Constant: return a.value;
        case Arg::Input: return inputs[a.index];
        case Arg::Time: return seconds;
        case Arg::Frame: return seconds * p.fps;
        case Arg::Node: return results[size_t(a.index) * 2 + size_t(a.output)];
        }
        return 0.0;
    };
    double args[5] = {};
    for (size_t i = 0; i < p.steps.size(); ++i)
    {
        const Step& s = p.steps[i];
        for (size_t k = 0; k < s.args.size() && k < 5; ++k)
            args[k] = read(s.args[k]);
        apply(s.op, args, &results[i * 2]);
    }
    return read(p.result);
}
} // namespace lila_expr
