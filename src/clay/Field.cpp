#include "clay/Clay.h"

#include <algorithm>
#include <cmath>

namespace lila::clay
{
namespace
{
using glm::dvec2;
using glm::dvec3;

double dot2(const dvec2& v) { return glm::dot(v, v); }

double roundCone(const dvec3& p, double r1, double r2, double h)
{
    // A sphere swallows the other: the cone has no side.
    if (r1 - r2 >= h)
        return glm::length(p) - r1;
    if (r2 - r1 >= h)
        return glm::length(p - dvec3(0.0, h, 0.0)) - r2;
    const dvec2 q(glm::length(dvec2(p.x, p.z)), p.y);
    const double b = (r1 - r2) / h;
    const double a = std::sqrt(1.0 - b * b);
    const double k = glm::dot(q, dvec2(-b, a));
    if (k < 0.0)
        return glm::length(q) - r1;
    if (k > a * h)
        return glm::length(q - dvec2(0.0, h)) - r2;
    return glm::dot(q, dvec2(a, b)) - r1;
}

double cappedCone(const dvec3& p, double h, double r1, double r2)
{
    const dvec2 q(glm::length(dvec2(p.x, p.z)), p.y);
    const dvec2 k1(r2, h);
    const dvec2 k2(r2 - r1, 2.0 * h);
    const dvec2 ca(q.x - std::min(q.x, q.y < 0.0 ? r1 : r2), std::abs(q.y) - h);
    const dvec2 cb = q - k1 + k2 * std::clamp(glm::dot(k1 - q, k2) / dot2(k2), 0.0, 1.0);
    const double s = cb.x < 0.0 && ca.y < 0.0 ? -1.0 : 1.0;
    return s * std::sqrt(std::min(dot2(ca), dot2(cb)));
}

Sample negated(Sample s)
{
    s.distance = -s.distance;
    return s;
}

/** Polynomial smooth minimum carrying colour: within k of each other the two blend, else the nearer one wins outright. */
Sample smin(const Sample& a, const Sample& b, double k)
{
    if (k <= 0.0)
        return a.distance <= b.distance ? a : b;
    const double h = std::clamp(0.5 + 0.5 * (b.distance - a.distance) / k, 0.0, 1.0);
    return {glm::mix(b.distance, a.distance, h) - k * h * (1.0 - h), glm::mix(b.color, a.color, float(h))};
}

Sample combine(const Sample& acc, const Sample& s, Op op, double k)
{
    switch (op)
    {
    case Op::Add:
        return smin(acc, s, k);
    case Op::Subtract:
        return negated(smin(negated(acc), s, k));
    case Op::Intersect:
        return negated(smin(negated(acc), negated(s), k));
    }
    return acc;
}

Bounds transformed(const Bounds& b, const glm::dmat4& m)
{
    Bounds out;
    for (int c = 0; c < 8; ++c)
    {
        dvec3 p((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
        out.extend(dvec3(m * glm::dvec4(p, 1.0)));
    }
    return out;
}

double maxBlend(const Item& item)
{
    double k = std::max(item.blend, 0.0);
    for (const Item& c : item.children)
        k = std::max(k, maxBlend(c));
    return k;
}

Bounds mirrored(const Bounds& b)
{
    if (b.empty())
        return b;
    return {dvec3(-b.max.x, b.min.y, b.min.z), dvec3(-b.min.x, b.max.y, b.max.z)};
}
} // namespace

double distance(const Primitive& prim, const dvec3& p)
{
    switch (prim.shape)
    {
    case Shape::Sphere:
        return glm::length(p) - prim.radius;
    case Shape::Ellipsoid:
    {
        const dvec3 r = glm::max(prim.extents, dvec3(1e-6));
        const double k0 = glm::length(p / r);
        const double k1 = glm::length(p / (r * r));
        return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -std::min({r.x, r.y, r.z});
    }
    case Shape::Box:
    {
        const dvec3 e = glm::max(prim.extents, dvec3(0.0));
        const double rnd = std::clamp(prim.rounding, 0.0, std::min({e.x, e.y, e.z}));
        const dvec3 q = glm::abs(p) - (e - rnd);
        return glm::length(glm::max(q, 0.0)) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0) - rnd;
    }
    case Shape::Capsule:
    {
        const double h = std::max(prim.length, 0.0);
        return roundCone(p + dvec3(0.0, 0.5 * h, 0.0), std::max(prim.radius, 0.0), std::max(prim.endRadius, 0.0), h);
    }
    case Shape::Cylinder:
    {
        const double ra = std::max(prim.radius, 0.0), h = 0.5 * std::max(prim.length, 0.0);
        const double rb = std::clamp(prim.rounding, 0.0, std::min(ra, h));
        const dvec2 d(glm::length(dvec2(p.x, p.z)) - ra + rb, std::abs(p.y) - h + rb);
        return std::min(std::max(d.x, d.y), 0.0) + glm::length(glm::max(d, 0.0)) - rb;
    }
    case Shape::Cone:
    {
        const double h = 0.5 * std::max(prim.length, 0.0);
        const double r1 = std::max(prim.radius, 0.0), r2 = std::max(prim.endRadius, 0.0);
        const double rnd = std::clamp(prim.rounding, 0.0, std::min(h, std::max(r1, r2)));
        return cappedCone(p, h - rnd, std::max(r1 - rnd, 0.0), std::max(r2 - rnd, 0.0)) - rnd;
    }
    case Shape::Torus:
    {
        const dvec2 q(glm::length(dvec2(p.x, p.z)) - prim.radius, p.y);
        return glm::length(q) - std::max(prim.endRadius, 0.0);
    }
    }
    return 1e30;
}

Bounds localBounds(const Primitive& prim)
{
    auto box = [](const dvec3& half) { return Bounds{-half, half}; };
    switch (prim.shape)
    {
    case Shape::Sphere:
        return box(dvec3(prim.radius));
    case Shape::Ellipsoid:
    case Shape::Box:
        return box(glm::abs(prim.extents));
    case Shape::Capsule:
    {
        const double r = std::max(prim.radius, prim.endRadius), h = 0.5 * std::max(prim.length, 0.0);
        return {dvec3(-r, -h - prim.radius, -r), dvec3(r, h + prim.endRadius, r)};
    }
    case Shape::Cylinder:
        return box(dvec3(prim.radius, 0.5 * prim.length, prim.radius));
    case Shape::Cone:
    {
        const double r = std::max(prim.radius, prim.endRadius);
        return box(dvec3(r, 0.5 * prim.length, r));
    }
    case Shape::Torus:
    {
        const double r = std::abs(prim.radius) + prim.endRadius;
        return box(dvec3(r, prim.endRadius, r));
    }
    }
    return {};
}

Field::Field(const Item& root, bool symmetryX) : mSymmetric(symmetryX)
{
    // A shape can change the field wherever any later join's blend reaches past it, not only its own.
    mReach = maxBlend(root);
    // The root is a group whose own op and blend have nothing before them to join.
    for (const Item& child : root.children)
        flatten(child, root.matrix);
    if (root.shape)
        flatten(Item{.matrix = root.matrix, .shape = root.shape}, glm::dmat4(1.0));

    int depth = 0;
    for (size_t i = 0; i < mCode.size(); ++i)
    {
        const Instr& in = mCode[i];
        if (depth == 0 && in.kind != Kind::End && in.op == Op::Add && !in.bounds.empty())
        {
            mAddBounds.push_back(in.bounds);
            if (mSymmetric)
                mAddBounds.push_back(mirrored(in.bounds));
        }
        depth += in.kind == Kind::Begin ? 1 : in.kind == Kind::End ? -1 : 0;
    }
}

void Field::flatten(const Item& item, const glm::dmat4& parent)
{
    const glm::dmat4 m = parent * item.matrix;
    const double blend = std::max(item.blend, 0.0);
    if (item.shape)
    {
        Prim p;
        p.shape = *item.shape;
        p.toLocal = glm::inverse(m);
        p.scale = std::min({glm::length(dvec3(m[0])), glm::length(dvec3(m[1])), glm::length(dvec3(m[2]))});
        p.bounds = transformed(localBounds(p.shape), m).grown(mReach);
        mPrims.push_back(p);
        mCode.push_back({.kind = Kind::Prim, .op = item.op, .blend = blend, .prim = int(mPrims.size()) - 1, .bounds = p.bounds});
        return;
    }
    const size_t begin = mCode.size();
    mCode.push_back({.kind = Kind::Begin, .op = item.op, .blend = blend});
    for (const Item& child : item.children)
        flatten(child, m);
    // A group can only be solid where one of its added children is; its subtracts and intersects only take away.
    Bounds solid;
    for (size_t i = begin + 1; i < mCode.size();)
    {
        const Instr& in = mCode[i];
        if (in.op == Op::Add)
            solid.extend(in.bounds);
        i = in.kind == Kind::Begin ? size_t(in.match) + 1 : i + 1;
    }
    mCode[begin].bounds = solid;
    mCode[begin].match = int(mCode.size());
    mCode.push_back({.kind = Kind::End, .op = item.op, .blend = blend});
}

Sample Field::run(const std::vector<int>* code, const dvec3& p) const
{
    Sample stack[64];
    int top = 0;
    stack[0] = Sample{};
    const size_t n = code ? code->size() : mCode.size();
    for (size_t k = 0; k < n; ++k)
    {
        const int i = code ? (*code)[k] : int(k);
        if (i == kEmpty)
        {
            stack[top] = Sample{};
            continue;
        }
        const Instr& in = mCode[size_t(i)];
        switch (in.kind)
        {
        case Kind::Prim:
        {
            const Prim& prim = mPrims[size_t(in.prim)];
            const Sample s{distance(prim.shape, dvec3(prim.toLocal * glm::dvec4(p, 1.0))) * prim.scale, prim.shape.color};
            stack[top] = combine(stack[top], s, in.op, in.blend);
            break;
        }
        case Kind::Begin:
            // Deeper nesting than the stack holds joins into the level above instead.
            if (top < 63)
                stack[++top] = Sample{};
            break;
        case Kind::End:
            if (top > 0)
            {
                const Sample group = stack[top--];
                stack[top] = combine(stack[top], group, in.op, in.blend);
            }
            break;
        }
    }
    return stack[0];
}

Sample Field::eval(const dvec3& p) const
{
    Sample s = run(nullptr, p);
    if (mSymmetric)
        if (Sample m = run(nullptr, dvec3(-p.x, p.y, p.z)); m.distance < s.distance)
            s = m;
    return s;
}

Sample Field::Local::eval(const dvec3& p) const
{
    Sample s = mField->run(&mCode, p);
    if (mField->mSymmetric)
        if (Sample m = mField->run(&mCode, dvec3(-p.x, p.y, p.z)); m.distance < s.distance)
            s = m;
    return s;
}

Field::Local Field::local(const Bounds& region, double pad) const
{
    Local out;
    out.mField = this;
    const Bounds near = region.grown(pad);
    const Bounds far = mSymmetric ? mirrored(near) : Bounds{};
    auto relevant = [&](const Bounds& b) { return b.overlaps(near) || b.overlaps(far); };
    for (size_t i = 0; i < mCode.size();)
    {
        const Instr& in = mCode[i];
        if (in.kind == Kind::End || relevant(in.bounds))
        {
            out.mCode.push_back(int(i));
            ++i;
            continue;
        }
        // Out of reach: an add or subtract leaves the field here as it was, an intersect empties it.
        if (in.op == Op::Intersect)
            out.mCode.push_back(kEmpty);
        i = in.kind == Kind::Begin ? size_t(in.match) + 1 : i + 1;
    }
    return out;
}

std::optional<std::vector<Bounds>> Field::changes(const Field& a, const Field& b)
{
    if (a.mSymmetric != b.mSymmetric || a.mCode.size() != b.mCode.size())
        return std::nullopt;
    std::vector<Bounds> out;
    for (size_t i = 0; i < a.mCode.size(); ++i)
    {
        const Instr& x = a.mCode[i];
        const Instr& y = b.mCode[i];
        if (x.kind != y.kind || x.op != y.op || x.match != y.match)
            return std::nullopt;
        bool same = x.blend == y.blend && x.bounds == y.bounds;
        if (x.kind == Kind::Prim)
            same = same && a.mPrims[size_t(x.prim)] == b.mPrims[size_t(y.prim)];
        if (same || x.kind == Kind::End)
            continue;
        // A changed intersect reaches everything it cut away.
        if (x.op == Op::Intersect)
            return std::nullopt;
        // A group's own change is its join; its children report their own.
        if (x.kind == Kind::Begin && x.blend == y.blend)
            continue;
        for (const Bounds& r : {x.bounds, y.bounds})
            if (!r.empty())
            {
                out.push_back(r);
                if (a.mSymmetric)
                    out.push_back(mirrored(r));
            }
    }
    return out;
}
} // namespace lila::clay
