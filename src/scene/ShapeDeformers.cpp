#include "scene/ShapeDeformers.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "deform/Deform.h"
#include "deform/Handles.h"
#include "deform/Shape.h"
#include "deform/Surface.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/DeformEval.h"
#include "scene/DeformWeights.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/ObjectSets.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"

#include <glm/gtc/constants.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lila::scene
{
using namespace rigutil;

namespace
{
// ------------------------------------------------------------------------------------------------ data kinds

template <class V> void readArray(const json& j, const char* key, std::vector<V>& out)
{
    out.clear();
    if (!j.contains(key) || !j[key].is_array())
        return;
    out.reserve(j[key].size());
    for (const json& x : j[key])
        out.push_back(V(x.get<double>()));
}

std::vector<double> flatten(std::span<const glm::dvec3> v)
{
    std::vector<double> out;
    out.reserve(v.size() * 3);
    for (const glm::dvec3& p : v)
        out.insert(out.end(), {p.x, p.y, p.z});
    return out;
}

std::vector<glm::dvec3> unflatten(const std::vector<double>& f)
{
    std::vector<glm::dvec3> out(f.size() / 3);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
    return out;
}

/** Point id lists: handle regions indexed by handle link index, or one list of anchors. */
struct PointGroups final : Data
{
    std::vector<std::vector<uint32_t>> groups;
    const char* kind() const override { return "pointGroups"; }
    size_t bytes() const override
    {
        size_t n = 0;
        for (const auto& g : groups)
            n += g.size() * 4;
        return n;
    }
    json toJson() const override { return {{"kind", kind()}, {"groups", groups}}; }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<PointGroups>();
        if (j.contains("groups") && j["groups"].is_array())
            for (const json& g : j["groups"])
            {
                d->groups.emplace_back();
                if (g.is_array())
                    for (const json& p : g)
                        d->groups.back().push_back(p.get<uint32_t>());
            }
        return d;
    }
};

/** The deformer's input points when it was bound, in the mesh's object space. */
struct ShapeRest final : Data
{
    std::vector<glm::dvec3> points;
    const char* kind() const override { return "shapeRest"; }
    size_t bytes() const override { return points.size() * sizeof(glm::dvec3); }
    json toJson() const override { return {{"kind", kind()}, {"points", flatten(points)}}; }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<ShapeRest>();
        std::vector<double> f;
        readArray(j, "points", f);
        d->points = unflatten(f);
        return d;
    }
};

/** Bounded biharmonic weights, points x handles, row-major; column k follows the handle link with index indices[k]. */
struct BbwBinding final : Data
{
    uint32_t points = 0;
    std::vector<uint32_t> indices;
    std::vector<float> weights;
    const char* kind() const override { return "bbwBinding"; }
    size_t bytes() const override { return indices.size() * 4 + weights.size() * 4; }
    json toJson() const override { return {{"kind", kind()}, {"points", points}, {"indices", indices}, {"weights", weights}}; }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<BbwBinding>();
        d->points = j.value("points", 0u);
        readArray(j, "indices", d->indices);
        readArray(j, "weights", d->weights);
        if (d->weights.size() != size_t(d->points) * d->indices.size())
            d->indices.clear();
        return d;
    }
};

enum class CageMode : int64_t
{
    MeanValue,
    Green,
    Somigliana,
    Harmonic,
};

/**
 * A cage bound for coordinates: the rest cage, its triangles and the bound points, all world space. The coordinates
 * themselves are recomputed from these when first evaluated, so switching modes needs no rebind and files stay small.
 */
struct CageCoordinates final : Data
{
    std::vector<glm::dvec3> rest;
    std::vector<uint32_t> triangles;
    std::vector<glm::dvec3> targets;
    const char* kind() const override { return "cageCoordinates"; }
    size_t bytes() const override { return (rest.size() + targets.size()) * sizeof(glm::dvec3) + triangles.size() * 4; }
    json toJson() const override
    {
        return {{"kind", kind()}, {"rest", flatten(rest)}, {"triangles", triangles}, {"targets", flatten(targets)}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<CageCoordinates>();
        std::vector<double> f;
        readArray(j, "rest", f);
        d->rest = unflatten(f);
        readArray(j, "targets", f);
        d->targets = unflatten(f);
        readArray(j, "triangles", d->triangles);
        d->triangles.resize(d->triangles.size() / 3 * 3);
        for (uint32_t t : d->triangles)
            if (t >= d->rest.size())
            {
                d->triangles.clear();
                break;
            }
        return d;
    }
};

// ------------------------------------------------------------------------------------------------ attr helpers

AttrDef keyable(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}
AttrDef stored(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable, .doc = std::move(doc),
            .minValue = lo, .maxValue = hi};
}
AttrDef hiddenStored(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrHidden,
            .doc = std::move(doc)};
}
AttrDef connected(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}
AttrDef output(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrOutput, .doc = std::move(doc)};
}
AttrDef enumAttr(std::string name, int64_t def, std::vector<std::string> names, std::string doc, bool key = false)
{
    AttrDef a = key ? keyable(std::move(name), PlugType::Enum, def, std::move(doc)) : stored(std::move(name), PlugType::Enum, def, std::move(doc));
    a.enumNames = std::move(names);
    return a;
}

AttrDef geomMatrixAttr() { return hiddenStored("geomMatrix", PlugType::Matrix, glm::dmat4(1.0), "The mesh's world matrix when the deformer was made."); }
AttrDef handleMatrixAttr() { return connected("handleMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's worldMatrix."); }
AttrDef handlesAttr() { return connected("handles", PlugType::Data, DataPtr(), "The last handle link (a skinInfluence per handle)."); }
AttrDef restAttr() { return hiddenStored("rest", PlugType::Data, DataPtr(), "The input shape captured at bind time."); }

template <class F> void parallelPoints(size_t n, F&& fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 512), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
            fn(i);
    });
}

glm::dmat4 safeInverse(const glm::dmat4& m, const char* what)
{
    if (std::abs(glm::determinant(m)) < 1e-18)
        throw std::runtime_error(std::string(what) + " has zero scale");
    return glm::inverse(m);
}

glm::dvec3 xform(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

const ShapeRest& restOf(const DeformerNode& d, size_t points)
{
    const auto* rest = dynamic_cast<const ShapeRest*>(d.as<DataPtr>(d.attr("rest")).get());
    if (!rest)
        throw Unbound();
    if (rest->points.size() != points)
        throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
    return *rest;
}

/** Handle link matrices (handle world * inverse bind world) by link index; missing indices are the identity. */
std::vector<glm::dmat4> handlePalette(const DeformerNode& d)
{
    std::vector<glm::dmat4> out;
    int a = d.attr("handles");
    for (DataPtr p = a >= 0 ? d.as<DataPtr>(a) : DataPtr(); p;)
    {
        const auto* link = dynamic_cast<const InfluenceLink*>(p.get());
        if (!link)
            break;
        if (link->index >= out.size())
            out.resize(link->index + 1, glm::dmat4(1.0));
        out[link->index] = link->matrix;
        p = link->prev;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ kelvinlet

class KelvinletNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const glm::dmat4 bind = matrix("bindMatrix");
        const glm::dmat4 move = matrix("handleMatrix") * safeInverse(bind, "the bind matrix");
        const glm::dvec3 centre(bind[3]);
        const deform::KelvinletMotion motion = deform::kelvinletMotion(
            glm::dmat3(move), xform(move, centre) - centre, {num("grab"), num("twist"), num("scale"), num("pinch")});
        const deform::KelvinletBrush brush{std::max(num("radius"), 1e-6), num("poissonRatio"), int(integer("falloff")) + 1};
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 w = xform(geom, points[i]);
            points[i] = xform(geomInv, w + deform::kelvinletDisplacement(w - centre, motion, brush));
        });
    }

    bool gpuProgram(const geo::Mesh&, deformEval::Program& out) const override
    {
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 bind = matrix("bindMatrix");
        const glm::dmat4 move = matrix("handleMatrix") * safeInverse(bind, "the bind matrix");
        const glm::dvec3 centre(bind[3]);
        const deform::KelvinletMotion m = deform::kelvinletMotion(
            glm::dmat3(move), xform(move, centre) - centre, {num("grab"), num("twist"), num("scale"), num("pinch")});
        const int scales = std::clamp(int(integer("falloff")) + 1, 1, 3);
        const double radius = std::max(std::max(num("radius"), 1e-6), 1e-9);
        // deform/Shape.cpp's brush: radii growing by 1.5, weights cancelling the far field, Kelvin constants a and b.
        const double ratio = 1.5;
        const double eps[3] = {radius, radius * ratio, radius * ratio * ratio};
        double w[3] = {1.0, 0.0, 0.0};
        if (scales == 2)
            w[1] = -1.0;
        else if (scales == 3)
        {
            w[1] = -(1.0 + ratio * ratio) / (ratio * ratio);
            w[2] = 1.0 / (ratio * ratio);
        }
        const double nu = std::clamp(num("poissonRatio"), -0.99, 0.5);
        const double a = 1.0 / (4.0 * glm::pi<double>()), b = a / (4.0 * (1.0 - nu));
        double grabCentre = 0.0, pinchCentre = 0.0, den = 0.0;
        for (int k = 0; k < scales; ++k)
        {
            grabCentre += w[k] * (1.5 * a - b) / eps[k];
            pinchCentre += w[k] * (2.0 * b - 2.5 * a) / (eps[k] * eps[k] * eps[k]);
            den += w[k] * (1.0 / (eps[k] * eps[k] * eps[k]) + 1.5 / (eps[k] * eps[k] * eps[k]));
        }
        uint32_t flags = 0;
        if (glm::dot(m.grab, m.grab) > 0.0 && std::abs(grabCentre) >= 1e-300)
            flags |= 1;
        const double angle = glm::length(m.twist);
        if (angle >= 1e-15)
            flags |= 2;
        if (m.scale != 0.0)
            flags |= 4;
        bool pinch = false;
        for (int c = 0; c < 3; ++c)
            pinch = pinch || glm::dot(m.pinch[c], m.pinch[c]) > 1e-30;
        if (pinch && std::abs(pinchCentre) >= 1e-300)
            flags |= 8;
        out.kernel = deformEval::Kernel::Kelvinlet;
        out.push(geom);
        out.push(glm::dmat3(safeInverse(geom, "the mesh")));
        out.push(centre);
        out.push(uint32_t(scales));
        for (int k = 0; k < 3; ++k)
            out.push(float(eps[k]));
        for (int k = 0; k < 3; ++k)
            out.push(float(w[k]));
        out.push(float(a));
        out.push(float(b));
        out.push(float(den));
        out.push(flags);
        out.push((flags & 1) ? m.grab / grabCentre : glm::dvec3(0.0));
        out.push(angle >= 1e-15 ? m.twist / angle : glm::dvec3(0.0));
        out.push(float(angle));
        out.push(float(m.scale));
        out.push((flags & 8) ? m.pinch / pinchCentre : glm::dmat3(0.0));
        return true;
    }
};

// ------------------------------------------------------------------------------------------------ arap / laplacian

class HandleDeformNode : public DeformerNode
{
public:
    explicit HandleDeformNode(deform::HandleSolver::Mode m) : mMode(m) {}

    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const size_t n = points.size();
        DataPtr restData = as<DataPtr>(attr("rest"));
        const ShapeRest& rest = restOf(*this, n);
        DataPtr regionData = as<DataPtr>(attr("regions")), anchorData = as<DataPtr>(attr("anchors"));
        const auto* regions = dynamic_cast<const PointGroups*>(regionData.get());
        const auto* anchors = dynamic_cast<const PointGroups*>(anchorData.get());

        // Groups: handle k holds its region, anchors are the group after the last handle (no transform, so they stay).
        const int anchorGroup = regions ? int(regions->groups.size()) : 0;
        std::vector<int> group(n, -1);
        if (regions)
            for (size_t k = 0; k < regions->groups.size(); ++k)
                for (uint32_t p : regions->groups[k])
                    if (p < n)
                        group[p] = int(k);
        if (anchors)
            for (const auto& g : anchors->groups)
                for (uint32_t p : g)
                    if (p < n)
                        group[p] = anchorGroup;
        if (mRest != restData || mRegions != regionData || mAnchors != anchorData || mEdges != in.numEdges() || mPoints != n)
        {
            mSolver.prepare(in, rest.points, group, mMode);
            mRest = restData;
            mRegions = regionData;
            mAnchors = anchorData;
            mEdges = in.numEdges();
            mPoints = n;
        }
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dmat4> palette = handlePalette(*this);
        palette.resize(std::min<size_t>(palette.size(), size_t(anchorGroup)));
        for (glm::dmat4& m : palette)
            m = geomInv * m * geom;
        points = mSolver.solve(points, palette, int(std::clamp<int64_t>(integer("iterations"), 0, 200)));
    }

private:
    deform::HandleSolver::Mode mMode;
    mutable deform::HandleSolver mSolver;
    mutable DataPtr mRest, mRegions, mAnchors;
    mutable geo::Index mEdges = 0;
    mutable size_t mPoints = 0;
};

// ------------------------------------------------------------------------------------------------ bbw

class BbwNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const auto* b = dynamic_cast<const BbwBinding*>(as<DataPtr>(attr("binding")).get());
        if (!b)
            throw Unbound();
        if (b->points != points.size())
            throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dmat4> all = handlePalette(*this);
        const size_t h = b->indices.size();
        std::vector<glm::dmat4> palette(h, glm::dmat4(1.0));
        for (size_t k = 0; k < h; ++k)
            if (b->indices[k] < all.size())
                palette[k] = geomInv * all[b->indices[k]] * geom;
        parallelPoints(points.size(), [&](size_t i) {
            const float* row = &b->weights[i * h];
            glm::dvec3 out(0.0);
            double sum = 0.0;
            for (size_t k = 0; k < h; ++k)
                if (row[k] != 0.0f)
                {
                    out += double(row[k]) * xform(palette[k], points[i]);
                    sum += row[k];
                }
            if (sum > 1e-12)
                points[i] = out / sum;
        });
    }
};

// ------------------------------------------------------------------------------------------------ simple set

class ShearNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix"), handle = matrix("handleMatrix");
        const glm::dmat4 toHandle = safeInverse(handle, "the handle") * geom;
        const glm::dmat4 back = safeInverse(geom, "the mesh") * handle;
        const double radius = num("falloffRadius"), softness = num("falloffSoftness");
        const deform::ShearParams o{num("shearX"), num("shearZ"), num("lowBound"), num("highBound")};
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 h = xform(toHandle, points[i]);
            double w = deform::sphericalFalloff(glm::length(h), radius, softness);
            if (w > 0.0)
                points[i] = xform(back, glm::mix(h, deform::shear(h, o), w));
        });
    }
};

class InflateNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dvec3> world(points.size());
        parallelPoints(points.size(), [&](size_t i) { world[i] = xform(geom, points[i]); });
        std::vector<glm::dvec3> normals = deform::pointNormals(in, world);
        const double amount = num("amount");
        parallelPoints(points.size(), [&](size_t i) { points[i] = xform(geomInv, world[i] + normals[i] * amount); });
    }
};

class CastNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix"), handle = matrix("handleMatrix");
        const glm::dmat4 toHandle = safeInverse(handle, "the handle") * geom;
        const glm::dmat4 back = safeInverse(geom, "the mesh") * handle;
        std::vector<glm::dvec3> local;
        std::vector<size_t> ids;
        for (size_t i = 0; i < points.size(); ++i)
            if (in.pointValid(geo::Index(i)))
            {
                local.push_back(xform(toHandle, points[i]));
                ids.push_back(i);
            }
        auto shape = deform::CastShape(std::clamp<int64_t>(integer("shape"), 0, 2));
        deform::cast(local, shape, num("factor"), num("radius"));
        for (size_t k = 0; k < ids.size(); ++k)
            points[ids[k]] = xform(back, local[k]);
    }
};

class SmoothNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const int mode = int(integer("mode"));
        const int iterations = int(std::clamp<int64_t>(integer("iterations"), 0, 1000));
        const double step = std::clamp(num("smoothStep"), 0.0, 1.0);
        const bool pin = as<bool>(attr("pinBorders"));
        if (mEdges != in.numEdges() || mPoints != points.size())
        {
            mAdj = deform::pointAdjacency(in);
            mEdges = in.numEdges();
            mPoints = points.size();
            mRestKey.reset();
        }
        if (mode == 0)
            deform::smooth(points, mAdj, iterations, step, pin);
        else if (mode == 1)
            deform::smoothTaubin(points, mAdj, iterations, step, 0.1, pin);
        else
        {
            DataPtr restData = as<DataPtr>(attr("rest"));
            const ShapeRest& rest = restOf(*this, points.size());
            if (mRestKey != restData || mIterations != iterations || mStep != step || mPin != pin)
            {
                mBind = deform::deltaMushBind(in, rest.points, mAdj, iterations, step, pin);
                mRestKey = restData;
                mIterations = iterations;
                mStep = step;
                mPin = pin;
            }
            points = deform::deltaMushApply(in, points, mAdj, mBind, 1.0);
        }
    }

private:
    mutable geo::Index mEdges = ~geo::Index(0);
    mutable size_t mPoints = 0;
    mutable deform::Adjacency mAdj;
    mutable DataPtr mRestKey;
    mutable int mIterations = -1;
    mutable double mStep = -1.0;
    mutable bool mPin = false;
    mutable deform::DeltaMushBind mBind;
};

class WarpNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const glm::dmat4 from = matrix("fromMatrix");
        const glm::dmat4 move = matrix("toMatrix") * safeInverse(from, "the from handle");
        const glm::dvec3 centre(from[3]);
        const double radius = num("falloffRadius"), softness = num("falloffSoftness"), strength = num("strength");
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 w = xform(geom, points[i]);
            double f = deform::sphericalFalloff(glm::length(w - centre), radius, softness) * strength;
            if (f != 0.0)
                points[i] = xform(geomInv, glm::mix(w, xform(move, w), f));
        });
    }
};

class NoiseNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const double amplitude = num("amplitude"), frequency = num("frequency");
        const glm::dvec3 offset = as<glm::dvec3>(attr("offset"));
        const uint32_t seed = uint32_t(integer("seed"));
        const int octaves = int(std::clamp<int64_t>(integer("octaves"), 1, 12));
        const bool alongNormal = integer("direction") == 0;
        std::vector<glm::dvec3> normals;
        if (alongNormal)
            normals = deform::pointNormals(in, points);
        std::vector<glm::dvec3> src = points;
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 q = src[i] * frequency + offset;
            if (alongNormal)
                points[i] = src[i] + normals[i] * (deform::fractalNoise(q, seed, octaves) * amplitude);
            else
                points[i] = src[i] + deform::fractalNoise3(q, seed, octaves) * amplitude;
        });
    }
};

/** Leaves the points alone and writes how stretched each one is: a point float layer and a colour set. */
class TensionNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>&) const override {}

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        mError.clear();
        const MeshPtr& in = as<MeshPtr>(kInMesh);
        setOutput(attr("outStretch"), 0.0);
        setOutput(attr("outCompress"), 0.0);
        if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in))
        {
            setOutput(outAttr, in);
            return;
        }
        try
        {
            const ShapeRest& rest = restOf(*this, in->pointCapacity());
            std::vector<float> t = deform::tension(*in, rest.points, in->positions());
            auto out = std::make_shared<geo::Mesh>(*in);
            const double envelope = as<double>(kEnvelope), scale = num("scale");
            const std::vector<float> weight = deformWeights::effective(*this, *in);
            std::vector<glm::vec4> colours(t.size());
            double stretch = 0.0, compress = 0.0, total = 0.0;
            for (size_t p = 0; p < t.size(); ++p)
            {
                double w = envelope * (p < weight.size() ? double(weight[p]) : 1.0);
                t[p] = float(t[p] * w);
                double v = std::clamp(double(t[p]) * scale, -1.0, 1.0);
                colours[p] = glm::vec4(float(std::max(v, 0.0)), float(std::max(-v, 0.0)), 0.0f, 1.0f);
                if (in->pointValid(geo::Index(p)) && w > 0.0)
                {
                    stretch += std::max(v, 0.0);
                    compress += std::max(-v, 0.0);
                    total += w;
                }
            }
            if (total > 0.0)
            {
                setOutput(attr("outStretch"), std::min(stretch / total, 1.0));
                setOutput(attr("outCompress"), std::min(compress / total, 1.0));
            }
            std::span<float> layer = out->pointAttrs().getOrAdd<float>("tension");
            std::copy(t.begin(), t.begin() + std::min(t.size(), layer.size()), layer.begin());
            const std::string& set = as<std::string>(attr("colorSet"));
            if (!set.empty())
            {
                geo::colors::add(*out, set, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                geo::colors::setFromPoints(*out, set, colours);
            }
            setOutput(outAttr, MeshPtr(std::move(out)));
        }
        catch (const std::exception& e)
        {
            mError = e.what();
            setOutput(outAttr, in);
        }
    }
};

// ------------------------------------------------------------------------------------------------ cage coordinates

std::vector<glm::dvec3> cageWorld(const geo::Mesh& cage, const glm::dmat4& m)
{
    std::vector<glm::dvec3> out(cage.pointCapacity());
    for (geo::Index p = 0; p < cage.pointCapacity(); ++p)
        out[p] = xform(m, cage.position(p));
    return out;
}

// ------------------------------------------------------------------------------------------------ command helpers

Graph& g() { return Graph::current(); }

Node& meshNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!n.type().isA("mesh") || n.type().isA("lattice"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return n;
}

std::vector<NodeId> meshesOf(const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : names)
        out.push_back(meshNamed(name, cmd).id());
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give or select at least one mesh");
    return out;
}

DeformerNode& deformerNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!deformers::isDeformer(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a deformer");
    return static_cast<DeformerNode&>(n);
}

bool isHandleDeformer(const Node& n) { return n.type().name == "arap" || n.type().name == "laplacianDeform" || n.type().name == "bbw"; }

/** A handle deformer by name, or the topmost one in a mesh's stack when given the mesh. */
DeformerNode& handleDeformerNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (isHandleDeformer(n))
        return static_cast<DeformerNode&>(n);
    if (n.type().isA("mesh"))
    {
        std::vector<NodeId> list = deformers::list(g(), n.id());
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (isHandleDeformer(*g().node(*it)))
                return static_cast<DeformerNode&>(*g().node(*it));
    }
    throw CommandError(std::string(cmd) + ": " + name + " is not an arap, laplacianDeform or bbw deformer, nor a mesh with one");
}

geo::Bounds worldBounds(Graph& graph, const std::vector<NodeId>& meshes)
{
    graph.evaluate();
    geo::Bounds b;
    for (NodeId id : meshes)
    {
        Node* n = graph.node(id);
        MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        glm::dmat4 w = worldOf(*n);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                b.extend(xform(w, m->position(p)));
    }
    if (!b.valid())
        b.extend(glm::dvec3(0.0));
    return b;
}

DeformerNode& addOne(Graph& graph, const char* type, NodeId mesh, const Args& a, const char* label)
{
    std::vector<NodeId> made = deformers::addDeformer(graph, type, {mesh}, a.has("name") ? a.get<std::string>("name") : label);
    return static_cast<DeformerNode&>(*graph.node(made.at(0)));
}

void setPlug(Graph& graph, NodeId node, const char* attr, PlugValue v)
{
    int a = graph.node(node)->type().attrIndex(attr);
    if (a < 0)
        throw CommandError(std::string("no attribute ") + attr + " on " + graph.node(node)->name());
    guarded([&] {
        graph.setValue({node, a, -1}, std::move(v));
        return 0;
    });
}

void connectPlug(Graph& graph, NodeId src, const char* srcAttr, NodeId dst, const char* dstAttr)
{
    graph.connect({src, graph.node(src)->type().attrIndex(srcAttr), -1}, {dst, graph.node(dst)->type().attrIndex(dstAttr), -1}, true);
}

void applyParams(Graph& graph, Node& n, const json& params)
{
    if (!params.is_object())
        return;
    for (auto& [k, val] : params.items())
        guarded([&] {
            PlugRef p = graph.plug(n.name() + "." + k);
            AttrDef def = graph.attr(p);
            if (p.component >= 0)
                def.type = PlugType::Float;
            graph.setValue(p, fromJson(def, val));
            return 0;
        });
}

Node* makeLocator(Graph& graph, const std::string& name, const glm::dvec3& pos, double scale = 1.0)
{
    Node* h = guarded([&] { return graph.createNode(*types::find("locator"), name, kNoNode, true); });
    graph.setValue({h->id(), Transform::kTranslate, -1}, pos);
    if (scale != 1.0)
        graph.setValue({h->id(), Transform::kScale, -1}, glm::dvec3(scale));
    graph.evaluate();
    return h;
}

/** Locator handle sized to the bounds along axis (0 x, 1 y, 2 z); its +Y is the deformation axis. */
Node* makeAxisHandle(Graph& graph, const std::string& name, const geo::Bounds& b, int axis)
{
    glm::dvec3 size = b.size();
    double extent = size[axis] > 1e-9 ? size[axis] : std::max({size.x, size.y, size.z, 1.0});
    Node* h = makeLocator(graph, name, b.center(), extent * 0.5);
    glm::dvec3 rot(0.0);
    if (axis == 0)
        rot.z = -90.0;
    else if (axis == 2)
        rot.x = 90.0;
    graph.setValue({h->id(), Transform::kRotate, -1}, rot);
    graph.evaluate();
    return h;
}

int axisIndex(const std::string& a, const char* cmd)
{
    if (a == "x")
        return 0;
    if (a == "y")
        return 1;
    if (a == "z")
        return 2;
    throw CommandError(std::string(cmd) + ": axis must be x, y or z");
}

/** The deformer's input, evaluated now. */
MeshPtr inputOf(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
    if (!in)
        throw CommandError(d.name() + ": the mesh has no geometry");
    return in;
}

std::vector<glm::dvec3> worldPoints(const geo::Mesh& m, const glm::dmat4& geom)
{
    std::vector<glm::dvec3> out(m.pointCapacity());
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        out[p] = xform(geom, m.position(p));
    return out;
}

void bindRest(Graph& graph, DeformerNode& d)
{
    MeshPtr in = inputOf(graph, d);
    auto data = std::make_shared<ShapeRest>();
    data->points.assign(in->positions().begin(), in->positions().end());
    graph.setValue({d.id(), d.attr("rest"), -1}, DataPtr(std::move(data)));
}

struct HandleLink
{
    NodeId link = kNoNode;
    NodeId handle = kNoNode;
    uint32_t index = 0;
};

std::vector<HandleLink> handleLinks(Graph& graph, const DeformerNode& d)
{
    std::vector<HandleLink> out;
    PlugRef plug{d.id(), d.attr("handles"), -1};
    while (auto src = graph.source(plug))
    {
        Node* link = graph.node(src->node);
        if (!dynamic_cast<SkinInfluenceNode*>(link) || out.size() > 4096)
            break;
        HandleLink h;
        h.link = link->id();
        h.index = uint32_t(std::max<int64_t>(0, link->as<int64_t>(SkinInfluenceNode::kIndex)));
        if (auto m = graph.source({link->id(), SkinInfluenceNode::kMatrix, -1}))
            h.handle = m->node;
        out.push_back(h);
        plug = {link->id(), SkinInfluenceNode::kPrevious, -1};
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/** Links a transform as the deformer's next handle: its current pose is its rest. Returns the handle's index. */
uint32_t linkHandle(Graph& graph, DeformerNode& d, NodeId handle)
{
    uint32_t next = 0;
    for (const HandleLink& h : handleLinks(graph, d))
        next = std::max(next, h.index + 1);
    graph.evaluate();
    Node* link = graph.createNode(*types::find("skinInfluence"), d.name() + "Link", kNoNode, true);
    NodeId id = link->id();
    graph.setValue({id, SkinInfluenceNode::kIndex}, int64_t(next));
    graph.setValue({id, SkinInfluenceNode::kBindPreMatrix}, safeInverse(worldOf(*graph.node(handle)), "the handle"));
    graph.connect({handle, Transform::kWorldMatrix, -1}, {id, SkinInfluenceNode::kMatrix, -1});
    PlugRef end{d.id(), d.attr("handles"), -1};
    if (auto prev = graph.source(end))
    {
        graph.disconnect(end);
        graph.connect(*prev, {id, SkinInfluenceNode::kPrevious, -1});
    }
    graph.connect({id, SkinInfluenceNode::kLink, -1}, end);
    return next;
}

/** Each handle's current pose becomes its rest. */
void rebindHandleLinks(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    for (const HandleLink& h : handleLinks(graph, d))
        if (h.handle != kNoNode)
            graph.setValue({h.link, SkinInfluenceNode::kBindPreMatrix}, safeInverse(worldOf(*graph.node(h.handle)), "the handle"));
}

/**
 * Point ids on mesh from a list of items: point ids, component strings ("Body.vtx[0:9]", edges and faces become their
 * points) or object set names.
 */
std::vector<uint32_t> pointsFromItems(Graph& graph, NodeId mesh, const json& items, const char* cmd)
{
    std::vector<uint32_t> out;
    MeshPtr m = graph.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    auto take = [&](const objectSets::Member& mem) {
        if (mem.node != mesh)
            return;
        if (mem.kind == ComponentMode::Vertex)
            out.insert(out.end(), mem.ids.begin(), mem.ids.end());
        else if (mem.kind != ComponentMode::Object && m)
        {
            components::Ids pts = components::convert(*m, mem.kind, mem.ids, ComponentMode::Vertex);
            out.insert(out.end(), pts.begin(), pts.end());
        }
    };
    const json list = items.is_array() ? items : json::array({items});
    for (const json& item : list)
    {
        if (item.is_number_integer())
        {
            out.push_back(item.get<uint32_t>());
            continue;
        }
        if (!item.is_string())
            throw CommandError(std::string(cmd) + ": expected point ids, components or set names, got " + item.dump());
        const std::string s = item.get<std::string>();
        Node* set = graph.find(s);
        if (set && objectSets::isSet(*set))
        {
            for (const objectSets::Member& mem : objectSets::members(graph, set->id()))
                take(mem);
            continue;
        }
        try
        {
            for (const objectSets::Member& mem : objectSets::parse(graph, {s}))
                take(mem);
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(std::string(cmd) + ": " + e.what());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/** Point ids from a flag when given, else the selected components of the mesh. */
std::vector<uint32_t> pointsArg(Graph& graph, NodeId mesh, const Args& a, const char* flag, const char* cmd)
{
    if (a.has(flag))
        return pointsFromItems(graph, mesh, a.flags[flag], cmd);
    if (components::mode() == ComponentMode::Object)
        return {};
    return components::pointsOf(mesh);
}

std::shared_ptr<PointGroups> groupsOf(const DeformerNode& d, const char* attr)
{
    auto out = std::make_shared<PointGroups>();
    if (const auto* p = dynamic_cast<const PointGroups*>(d.as<DataPtr>(d.attr(attr)).get()))
        out->groups = p->groups;
    return out;
}

/** A new locator at the centroid of the points (world) linked as the next handle, its region set to the points. */
NodeId addRegionHandle(Graph& graph, DeformerNode& d, NodeId mesh, const std::vector<uint32_t>& pts, const char* cmd)
{
    if (pts.empty())
        throw CommandError(std::string(cmd) + ": the handle has no points; select vertices or give points=");
    MeshPtr in = inputOf(graph, d);
    const glm::dmat4 geom = d.as<glm::dmat4>(d.attr("geomMatrix"));
    glm::dvec3 c(0.0);
    size_t n = 0;
    for (uint32_t p : pts)
        if (p < in->pointCapacity())
        {
            c += xform(geom, in->position(p));
            ++n;
        }
    if (n == 0)
        throw CommandError(std::string(cmd) + ": the handle's points are not on " + graph.node(mesh)->name());
    Node* h = makeLocator(graph, d.name() + "Handle", c / double(n));
    uint32_t index = linkHandle(graph, d, h->id());
    auto regions = groupsOf(d, "regions");
    if (regions->groups.size() <= index)
        regions->groups.resize(index + 1);
    regions->groups[index] = pts;
    graph.setValue({d.id(), d.attr("regions"), -1}, DataPtr(std::move(regions)));
    return h->id();
}

/** Points a point handle holds: the nearest valid point to it plus any within radius (world). */
std::vector<uint32_t> pointsNear(const std::vector<glm::dvec3>& world, const geo::Mesh& m, const glm::dvec3& at, double radius)
{
    std::vector<uint32_t> out;
    double best = 1e300;
    uint32_t nearest = ~0u;
    for (size_t p = 0; p < world.size(); ++p)
    {
        if (!m.pointValid(geo::Index(p)))
            continue;
        double d = glm::length(world[p] - at);
        if (d < best)
        {
            best = d;
            nearest = uint32_t(p);
        }
        if (radius > 0.0 && d <= radius)
            out.push_back(uint32_t(p));
    }
    if (nearest != ~0u && std::find(out.begin(), out.end(), nearest) == out.end())
        out.push_back(nearest);
    return out;
}

void bindBbw(Graph& graph, DeformerNode& d)
{
    MeshPtr in = inputOf(graph, d);
    const std::vector<glm::dvec3> world = worldPoints(*in, d.as<glm::dmat4>(d.attr("geomMatrix")));
    std::vector<HandleLink> links = handleLinks(graph, d);
    if (links.empty())
        throw CommandError(d.name() + ": no handles");
    const double radius = d.num("handleRadius");
    std::vector<std::vector<uint32_t>> regions;
    auto data = std::make_shared<BbwBinding>();
    for (const HandleLink& h : links)
    {
        glm::dvec3 at = h.handle != kNoNode ? positionOf(*graph.node(h.handle)) : glm::dvec3(0.0);
        regions.push_back(pointsNear(world, *in, at, radius));
        data->indices.push_back(h.index);
    }
    data->points = uint32_t(world.size());
    data->weights = deform::boundedBiharmonicWeights(*in, world, regions);
    graph.setValue({d.id(), d.attr("binding"), -1}, DataPtr(std::move(data)));
}

// ------------------------------------------------------------------------------------------------ bbw skin weights

struct Bone
{
    uint32_t index;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> segments;
};

/** Distance from p to the bone, and where along its nearest segment (0..1, 0.5 for a point bone). */
double boneDistance(const Bone& b, const glm::dvec3& p, double* along)
{
    double best = 1e300;
    for (const auto& [a, c] : b.segments)
    {
        glm::dvec3 d = c - a;
        double len2 = glm::dot(d, d);
        double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.5;
        double dist = glm::length(p - (a + d * t));
        if (dist < best)
        {
            best = dist;
            if (along)
                *along = len2 > 0.0 ? t : 0.5;
        }
    }
    return best;
}

/**
 * The points each bone holds at weight 1: of the points nearest to it, those about as close as its closest one and
 * away from its ends, so the joints in between are left to the weights.
 */
std::vector<std::vector<uint32_t>> boneRegions(const std::vector<Bone>& bones, const std::vector<glm::dvec3>& world, const geo::Mesh& m)
{
    const size_t n = world.size();
    std::vector<int> nearest(n, -1);
    std::vector<double> dist(n, 0.0), along(n, 0.5);
    std::vector<double> closest(bones.size(), 1e300);
    for (size_t p = 0; p < n; ++p)
    {
        if (!m.pointValid(geo::Index(p)))
            continue;
        double best = 1e300;
        for (size_t b = 0; b < bones.size(); ++b)
        {
            double t = 0.5;
            double d = boneDistance(bones[b], world[p], &t);
            if (d < best)
            {
                best = d;
                nearest[p] = int(b);
                along[p] = t;
            }
        }
        dist[p] = best;
    }
    auto middle = [&](size_t p) { return along[p] > 0.15 && along[p] < 0.85; };
    for (size_t p = 0; p < n; ++p)
        if (nearest[p] >= 0 && middle(p))
            closest[size_t(nearest[p])] = std::min(closest[size_t(nearest[p])], dist[p]);
    std::vector<std::vector<uint32_t>> out(bones.size());
    for (size_t p = 0; p < n; ++p)
    {
        int b = nearest[p];
        if (b >= 0 && middle(p) && dist[p] <= closest[size_t(b)] * 1.25 + 1e-9)
            out[size_t(b)].push_back(uint32_t(p));
    }
    // A bone with no points along its middle holds the one point nearest to it.
    for (size_t b = 0; b < bones.size(); ++b)
    {
        if (!out[b].empty())
            continue;
        size_t pick = n;
        for (size_t p = 0; p < n; ++p)
            if (nearest[p] == int(b) && (pick == n || dist[p] < dist[pick]))
                pick = p;
        if (pick < n)
            out[b].push_back(uint32_t(pick));
    }
    return out;
}
} // namespace

namespace deformers
{
std::vector<TensionCurve> tensionCurves(Graph& graph)
{
    std::vector<TensionCurve> out;
    std::set<std::string> names;
    for (NodeId id : graph.nodes())
    {
        Node* n = graph.node(id);
        if (n->type().name != "tension" || !modifiers::exportEnabled(*n))
            continue;
        for (auto [curve, value] : {std::pair{"stretchCurve", "outStretch"}, std::pair{"compressCurve", "outCompress"}})
        {
            const std::string& name = n->as<std::string>(n->type().attrIndex(curve));
            if (!name.empty() && names.insert(name).second)
                out.push_back({id, modifiers::meshOf(graph, id), n->type().attrIndex(value), name});
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ cage

struct CageCache
{
    DataPtr key;
    CageMode mode = CageMode::MeanValue;
    double poisson = -1.0;
    int64_t resolution = 0;
    size_t nv = 0, nf = 0;
    std::vector<float> vertex, face; // per point: nv (x9 for Somigliana) then nf (x9)
};

std::vector<AttrDef> cageParams()
{
    return {enumAttr("coordinates", 0, {"meanValue", "green", "somigliana", "harmonic"},
                     "Mean value: smooth, fine for any closed cage. Green: shape preserving (conformal), the detail keeps its "
                     "proportions as the cage stretches. Somigliana: elastic, with poissonRatio and bulge for volume. Harmonic: "
                     "stays local in concave cages (a hand's fingers), for points inside the cage."),
            stored("resolution", PlugType::Int, int64_t(40), "Harmonic: grid cells across the cage's longest side, 8 to 128.", 8.0, 128.0),
            stored("poissonRatio", PlugType::Float, 0.3, "Somigliana: how incompressible the inside is, 0 to 0.49.", 0.0, 0.49),
            keyable("bulge", PlugType::Float, 0.0,
                    "Somigliana: 0 follows the cage's own stretch; toward 1 a cage face that shrinks pushes outward, keeping volume.")};
}

bool cageUsesCoordinates(const DeformerNode& cage)
{
    return cage.integer("coordinates") != 0 || dynamic_cast<const CageCoordinates*>(cage.as<DataPtr>(cage.attr("binding")).get());
}

namespace
{
/** The bound cage, with cache holding its coordinates for the current mode (computed when stale). Throws like cageDeform. */
const CageCoordinates& coordinatesFor(const DeformerNode& cage, size_t points, std::shared_ptr<CageCache>& cache)
{
    LILA_ZONE();

    DataPtr bindData = cage.as<DataPtr>(cage.attr("binding"));
    const auto* b = dynamic_cast<const CageCoordinates*>(bindData.get());
    if (!b || b->triangles.empty())
        throw Unbound();
    const MeshPtr& driver = cage.as<MeshPtr>(cage.attr("driver"));
    if (!driver)
        throw std::runtime_error("no cage connected");
    const size_t nv = b->rest.size(), nf = b->triangles.size() / 3;
    if (driver->pointCapacity() != nv)
        throw std::runtime_error("the cage's points changed since binding: run rebindDeformer");
    if (b->targets.size() != points)
        throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
    const auto mode = CageMode(std::clamp<int64_t>(cage.integer("coordinates"), 0, 3));
    const double nu = std::clamp(cage.num("poissonRatio"), 0.0, 0.49);
    const int64_t resolution = std::clamp<int64_t>(cage.integer("resolution"), 8, 128);

    if (!cache || cache->key != bindData || cache->mode != mode || (mode == CageMode::Somigliana && cache->poisson != nu) ||
        (mode == CageMode::Harmonic && cache->resolution != resolution))
    {
        auto c = std::make_shared<CageCache>();
        c->key = bindData;
        c->mode = mode;
        c->poisson = nu;
        c->resolution = resolution;
        c->nv = nv;
        c->nf = nf;
        const size_t n = b->targets.size();
        if (mode == CageMode::Harmonic)
            c->vertex = deform::harmonicCoordinates(b->rest, b->triangles, b->targets, int(resolution));
        else
        {
            const size_t vs = mode == CageMode::Somigliana ? 9 : 1;
            c->vertex.resize(n * nv * vs);
            if (mode != CageMode::MeanValue)
                c->face.resize(n * nf * vs);
            tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 16), [&](const tbb::blocked_range<size_t>& r) {
                std::vector<double> va(nv * vs), fa(nf * vs);
                for (size_t i = r.begin(); i != r.end(); ++i)
                {
                    if (mode == CageMode::MeanValue)
                        deform::meanValueCoordinates(b->rest, b->triangles, b->targets[i], va);
                    else if (mode == CageMode::Green)
                        deform::greenCoordinates(b->rest, b->triangles, b->targets[i], va, fa);
                    else
                        deform::somiglianaCoordinates(b->rest, b->triangles, b->targets[i], nu, va, fa);
                    std::transform(va.begin(), va.end(), c->vertex.begin() + i * nv * vs, [](double v) { return float(v); });
                    if (mode != CageMode::MeanValue)
                        std::transform(fa.begin(), fa.end(), c->face.begin() + i * nf * vs, [](double v) { return float(v); });
                }
            });
        }
        cache = std::move(c);
    }
    return *b;
}

/**
 * What one evaluation feeds the coordinates: the cage points' displacements (in the rigid fit's frame for Somigliana)
 * and the face terms (Green's scaled normals, Somigliana's tractions), with the fit.
 */
struct CageTerms
{
    std::vector<glm::dvec3> vertex, face;
    glm::dmat3 rotation{1.0};
    glm::dvec3 translation{0.0};
};

CageTerms cageTerms(const DeformerNode& cage, const CageCoordinates& b, CageMode mode)
{
    CageTerms t;
    const std::vector<glm::dvec3> def = cageWorld(*cage.as<MeshPtr>(cage.attr("driver")), cage.matrix("driverMatrix"));
    const size_t nv = b.rest.size();
    t.vertex.resize(nv);
    if (mode == CageMode::Somigliana)
    {
        const deform::RigidFit fit = deform::rigidFit(b.rest, def);
        t.rotation = fit.rotation;
        t.translation = fit.to - fit.rotation * fit.from;
        const glm::dmat3 rotT = glm::transpose(t.rotation);
        for (size_t j = 0; j < nv; ++j)
            t.vertex[j] = rotT * (def[j] - t.translation) - b.rest[j];
        const double nu = std::clamp(cage.num("poissonRatio"), 0.0, 0.49);
        t.face = deform::somiglianaTractions(b.rest, def, b.triangles, t.rotation, t.translation, {nu, cage.num("bulge")});
        return t;
    }
    for (size_t j = 0; j < nv; ++j)
        t.vertex[j] = def[j] - b.rest[j];
    if (mode == CageMode::Green)
        t.face = deform::greenFaceTerms(b.rest, def, b.triangles);
    return t;
}
} // namespace

void cageDeform(const DeformerNode& cage, std::vector<glm::dvec3>& points, std::shared_ptr<CageCache>& cache)
{
    LILA_ZONE();

    const CageCoordinates& b = coordinatesFor(cage, points.size(), cache);
    const CageCache& c = *cache;
    const size_t nv = c.nv, nf = c.nf;
    const CageTerms t = cageTerms(cage, b, c.mode);
    const glm::dmat4 geom = cage.matrix("geomMatrix");
    const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
    if (c.mode == CageMode::Somigliana)
    {
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 u(0.0);
            const float* vr = &c.vertex[i * nv * 9];
            for (size_t j = 0; j < nv; ++j, vr += 9)
                for (int r = 0; r < 3; ++r)
                    u[r] += vr[r * 3] * t.vertex[j].x + vr[r * 3 + 1] * t.vertex[j].y + vr[r * 3 + 2] * t.vertex[j].z;
            const float* fr = &c.face[i * nf * 9];
            for (size_t f = 0; f < nf; ++f, fr += 9)
                for (int r = 0; r < 3; ++r)
                    u[r] += fr[r * 3] * t.face[f].x + fr[r * 3 + 1] * t.face[f].y + fr[r * 3 + 2] * t.face[f].z;
            glm::dvec3 x = xform(geom, points[i]);
            points[i] = xform(geomInv, t.rotation * (x + u) + t.translation);
        });
        return;
    }
    parallelPoints(points.size(), [&](size_t i) {
        glm::dvec3 u(0.0);
        const float* vr = &c.vertex[i * nv];
        for (size_t j = 0; j < nv; ++j)
            u += t.vertex[j] * double(vr[j]);
        if (c.mode == CageMode::Green)
        {
            const float* fr = &c.face[i * nf];
            for (size_t f = 0; f < nf; ++f)
                u += t.face[f] * double(fr[f]);
        }
        points[i] = xform(geomInv, xform(geom, points[i]) + u);
    });
}

bool cageGpuProgram(const DeformerNode& cage, size_t points, deformEval::Program& out, std::shared_ptr<CageCache>& cache)
{
    LILA_ZONE();

    const CageCoordinates& b = coordinatesFor(cage, points, cache);
    std::shared_ptr<const CageCache> c = cache;
    const bool som = c->mode == CageMode::Somigliana;
    const bool faces = som || c->mode == CageMode::Green;
    const size_t vs = som ? 9 : 1;
    if (c->vertex.size() != points * c->nv * vs || (faces && c->face.size() != points * c->nf * vs) ||
        std::max(c->vertex.size(), c->face.size()) >= size_t(UINT32_MAX))
        return false;
    const CageTerms t = cageTerms(cage, b, c->mode);
    const glm::dmat4 geom = cage.matrix("geomMatrix");
    const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
    const uint64_t key = deformEval::identityKey(c);
    if (som)
    {
        out.kernel = deformEval::Kernel::CageSomigliana;
        out.push(geom);
        out.push(geomInv);
        out.push(t.rotation);
        out.push(t.translation);
    }
    else
    {
        out.kernel = deformEval::Kernel::CageLinear;
        out.push(glm::dmat3(geomInv));
    }
    out.push(uint32_t(c->nv));
    out.push(uint32_t(faces ? c->nf : 0));
    out.push(std::span<const glm::dvec3>(t.vertex));
    if (faces)
        out.push(std::span<const glm::dvec3>(t.face));
    out.blobs[0] = deformEval::blob(key ^ 1, c, c->vertex);
    if (faces)
        out.blobs[1] = deformEval::blob(key ^ 2, c, c->face);
    return true;
}

void bindCageCoordinates(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr cage = std::get<MeshPtr>(graph.value({d.id(), d.attr("driver"), -1}));
    if (!cage)
        throw CommandError(d.name() + ": no driver mesh connected");
    auto data = std::make_shared<CageCoordinates>();
    data->rest = cageWorld(*cage, std::get<glm::dmat4>(graph.value({d.id(), d.attr("driverMatrix"), -1})));
    data->triangles = deform::trianglePoints(*cage);
    if (data->triangles.empty())
        throw CommandError(d.name() + ": the cage has no faces");
    MeshPtr in = inputOf(graph, d);
    data->targets = worldPoints(*in, d.as<glm::dmat4>(d.attr("geomMatrix")));
    graph.setValue({d.id(), d.attr("binding"), -1}, DataPtr(std::move(data)));
}

// ------------------------------------------------------------------------------------------------ rebind

bool rebindShape(Graph& graph, DeformerNode& d)
{
    const std::string& t = d.type().name;
    if (t == "kelvinlet")
    {
        if (auto src = graph.source({d.id(), d.attr("handleMatrix"), -1}))
            graph.setValue({d.id(), d.attr("bindMatrix"), -1}, worldOf(*graph.node(src->node)));
    }
    else if (t == "arap" || t == "laplacianDeform")
    {
        rebindHandleLinks(graph, d);
        bindRest(graph, d);
    }
    else if (t == "bbw")
    {
        rebindHandleLinks(graph, d);
        bindBbw(graph, d);
    }
    else if (t == "smoothDeformer" || t == "tension")
        bindRest(graph, d);
    else if (t == "warp")
    {
        // The to handle's current offset from the from handle becomes the rest: snap from onto to.
        if (auto to = graph.source({d.id(), d.attr("toMatrix"), -1}))
            if (auto from = graph.source({d.id(), d.attr("fromMatrix"), -1}))
            {
                Node* fn = graph.node(from->node);
                Node* tn = graph.node(to->node);
                for (int a : {Transform::kTranslate, Transform::kRotate, Transform::kScale})
                    graph.setValue({fn->id(), a, -1}, graph.storedValue({tn->id(), a, -1}));
            }
    }
    else
        return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ types

void registerShapeTypes()
{
    data::registerKind("pointGroups", &PointGroups::fromJson);
    data::registerKind("shapeRest", &ShapeRest::fromJson);
    data::registerKind("bbwBinding", &BbwBinding::fromJson);
    data::registerKind("cageCoordinates", &CageCoordinates::fromJson);

    registerDeformer(
        {"kelvinlet", "hand-grab",
         "Moves the mesh like a block of rubber pushed by a handle (regularized Kelvinlets): move the handle to grab, rotate it "
         "to twist, scale it to grow or shrink, scale it unevenly to pinch. Volume behaves physically, set by poissonRatio.",
         {handleMatrixAttr(), hiddenStored("bindMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's world matrix when bound."), geomMatrixAttr(),
          keyable("radius", PlugType::Float, 10.0, "Size of the brush, cm: the region that moves almost rigidly with the handle.", 0.0),
          keyable("poissonRatio", PlugType::Float, 0.4, "0 squashes freely, 0.5 keeps volume (grabbing bulges the sides). Below 0.5.", -0.99, 0.5),
          enumAttr("falloff", 0, {"single", "biScale", "triScale"},
                   "Single fades slowly (1/r) like real rubber; bi- and tri-scale fade much faster, for local edits.", true),
          keyable("grab", PlugType::Float, 1.0, "How much of the handle's move applies."),
          keyable("twist", PlugType::Float, 1.0, "How much of the handle's rotation applies."),
          keyable("scale", PlugType::Float, 1.0, "How much of the handle's uniform scale applies."),
          keyable("pinch", PlugType::Float, 1.0, "How much of the handle's uneven scale applies.")},
         [] { return std::make_unique<KelvinletNode>(); }});

    auto handleParams = [](bool arap) {
        std::vector<AttrDef> a = {geomMatrixAttr(), handlesAttr(),
                                  hiddenStored("regions", PlugType::Data, DataPtr(), "Each handle's points, by handle index."),
                                  hiddenStored("anchors", PlugType::Data, DataPtr(), "Points that stay where the input puts them."), restAttr()};
        if (arap)
            a.push_back(keyable("iterations", PlugType::Int, int64_t(8), "Rigidity passes: more keeps detail stiffer across big bends.", 0.0, 200.0));
        return a;
    };
    registerDeformer({"arap", "hand-move",
                      "As-rigid-as-possible handle deformation: handle regions follow their locators, anchors stay, and the rest "
                      "bends while keeping its local shape, rotating detail with it.",
                      handleParams(true), [] { return std::make_unique<HandleDeformNode>(deform::HandleSolver::Mode::Arap); }});
    registerDeformer({"laplacianDeform", "hand-move",
                      "Laplacian handle deformation: handle regions follow their locators, anchors stay, and the surface between "
                      "moves smoothly, keeping its detail. Faster than arap; detail translates rather than rotates.",
                      handleParams(false), [] { return std::make_unique<HandleDeformNode>(deform::HandleSolver::Mode::Laplacian); }});

    registerDeformer({"bbw", "point",
                      "Point handles blended with bounded biharmonic weights: smooth, local, always between 0 and 1. Move, rotate "
                      "and scale the handles.",
                      {geomMatrixAttr(), handlesAttr(),
                       stored("handleRadius", PlugType::Float, 0.0, "Points this close to a handle (cm) move fully with it; 0 is just the nearest. Used when binding.", 0.0),
                       hiddenStored("binding", PlugType::Data, DataPtr(), "Each point's weight per handle.")},
                      [] { return std::make_unique<BbwNode>(); }});

    registerDeformer({"shearDeformer", "slashes",
                      "Shears the mesh along its handle's X and Z as Y rises, rigid past the bounds.",
                      {handleMatrixAttr(), geomMatrixAttr(),
                       keyable("falloffRadius", PlugType::Float, 0.0, "Spherical falloff around the handle, handle units. 0 turns it off.", 0.0),
                       keyable("falloffSoftness", PlugType::Float, 0.5, "How much of the falloff radius eases out, 0 to 1.", 0.0, 1.0),
                       keyable("shearX", PlugType::Float, 0.5, "X offset per unit of Y, in handle space."),
                       keyable("shearZ", PlugType::Float, 0.0, "Z offset per unit of Y, in handle space."),
                       keyable("lowBound", PlugType::Float, -1.0, "Where the shear starts along the handle's Y, handle units."),
                       keyable("highBound", PlugType::Float, 1.0, "Where the shear ends along the handle's Y, handle units.")},
                      [] { return std::make_unique<ShearNode>(); }});

    registerDeformer({"inflate", "inner-shadow-bottom",
                      "Pushes every point along its normal (inflate, or deflate with a negative amount).",
                      {geomMatrixAttr(), keyable("amount", PlugType::Float, 1.0, "Distance along the normal, cm.")},
                      [] { return std::make_unique<InflateNode>(); }});

    registerDeformer({"cast", "sphere",
                      "Pulls the mesh toward a sphere, a cylinder (around the handle's Y) or a box centred on its handle.",
                      {handleMatrixAttr(), geomMatrixAttr(),
                       enumAttr("shape", 0, {"sphere", "cylinder", "box"}, "The shape points are pulled toward."),
                       keyable("factor", PlugType::Float, 1.0, "0 leaves the mesh, 1 puts it on the shape; beyond pushes past."),
                       keyable("radius", PlugType::Float, 0.0, "Size of the shape, handle units; 0 uses the mesh's mean size.", 0.0)},
                      [] { return std::make_unique<CastNode>(); }});

    registerDeformer({"smoothDeformer", "blur",
                      "Smooths the mesh: plain Laplacian (shrinks), Taubin (keeps size) or corrective (smooths only what changed "
                      "since binding, keeping rest detail).",
                      {enumAttr("mode", 1, {"laplacian", "taubin", "corrective"}, "How to smooth."),
                       keyable("iterations", PlugType::Int, int64_t(5), "Smoothing passes.", 0.0, 1000.0),
                       keyable("smoothStep", PlugType::Float, 0.5, "How far each pass moves a point toward its neighbours, 0 to 1.", 0.0, 1.0),
                       stored("pinBorders", PlugType::Bool, true, "Keep open borders where they are."), restAttr()},
                      [] { return std::make_unique<SmoothNode>(); }});

    registerDeformer({"warp", "arrow-move-right",
                      "Moves points near the from handle by the from-to handle offset (move, rotate and scale the to handle), "
                      "fading out over the falloff radius.",
                      {connected("fromMatrix", PlugType::Matrix, glm::dmat4(1.0), "The from handle's worldMatrix."),
                       connected("toMatrix", PlugType::Matrix, glm::dmat4(1.0), "The to handle's worldMatrix."), geomMatrixAttr(),
                       keyable("falloffRadius", PlugType::Float, 10.0, "Distance from the from handle where the effect ends, cm. 0: everywhere.", 0.0),
                       keyable("falloffSoftness", PlugType::Float, 1.0, "How much of the radius eases out, 0 a hard edge to 1 all soft.", 0.0, 1.0),
                       keyable("strength", PlugType::Float, 1.0, "Scales the effect.")},
                      [] { return std::make_unique<WarpNode>(); }});

    registerDeformer({"noiseDeformer", "grain",
                      "Displaces points by seeded fractal noise, along the normal or in all directions. Key offset to animate it.",
                      {keyable("amplitude", PlugType::Float, 1.0, "Largest displacement, object units."),
                       keyable("frequency", PlugType::Float, 0.1, "Noise features per object unit.", 0.0),
                       keyable("offset", PlugType::Vec3, glm::dvec3(0.0), "Moves the noise through the mesh."),
                       stored("seed", PlugType::Int, int64_t(0), "Picks a different noise."),
                       stored("octaves", PlugType::Int, int64_t(3), "Layers of finer detail, 1 to 12.", 1.0, 12.0),
                       enumAttr("direction", 0, {"normal", "vector"}, "Along each point's normal, or a 3D offset.")},
                      [] { return std::make_unique<NoiseNode>(); }});

    registerDeformer({"tension", "arrows-diff",
                      "Measures stretch and compression against the shape at binding, without moving points: writes a 'tension' "
                      "point attribute (positive stretched) and a colour set (red stretch, green compression) for wrinkle masks.",
                      {keyable("scale", PlugType::Float, 4.0, "Colour gain: strain times scale is full colour.", 0.0),
                       stored("colorSet", PlugType::String, std::string("tension"), "Colour set written; empty writes none."), restAttr(),
                       stored("stretchCurve", PlugType::String, std::string(),
                              "Unreal material curve that carries outStretch in skeletal and animation exports; empty for none."),
                       stored("compressCurve", PlugType::String, std::string(),
                              "Unreal material curve that carries outCompress in skeletal and animation exports; empty for none."),
                       output("outStretch", PlugType::Float, 0.0,
                              "Mean stretch over the weighted points, 0 to 1 (strain times scale, clamped): a wrinkle mask's strength."),
                       output("outCompress", PlugType::Float, 0.0, "Mean compression over the weighted points, 0 to 1.")},
                      [] { return std::make_unique<TensionNode>(); }});
}

// ------------------------------------------------------------------------------------------------ commands

void registerShapeCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "kelvinlet",
        .doc = "Add a Kelvinlet handle to the meshes: moving, rotating or scaling the handle grabs, twists, scales or pinches "
               "the mesh around it with a physically based falloff. Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"position", "", T::Vec3, nullptr, "Handle position, world. Default: the meshes' centre."},
                  {"radius", "", T::Float, nullptr, "Brush radius, cm. Default: a quarter of the meshes' size.", {}, {}, 0.0},
                  {"poissonRatio", "", T::Float, 0.4, "0 squashes freely, 0.5 keeps volume.", {}, {}, -0.99, 0.5},
                  {"falloff", "", T::String, "single", "single | biScale | triScale", {"single", "biScale", "triScale"}},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "kelvinlet");
            geo::Bounds b = worldBounds(graph, meshes);
            glm::dvec3 pos = a.has("position") ? vec3Of(a.flags["position"]) : b.center();
            double radius = a.has("radius") ? a.get<double>("radius") : std::max(glm::length(b.size()) * 0.25, 1e-3);
            std::string fall = a.get<std::string>("falloff");
            int64_t falloff = fall == "single" ? 0 : fall == "biScale" ? 1 : fall == "triScale" ? 2 : -1;
            if (falloff < 0)
                throw CommandError("kelvinlet: falloff must be single, biScale or triScale");
            Node* h = makeLocator(graph, "KelvinletHandle", pos);
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "kelvinlet", mesh, a, "Kelvinlet");
                setPlug(graph, d.id(), "bindMatrix", worldOf(*h));
                setPlug(graph, d.id(), "radius", radius);
                setPlug(graph, d.id(), "poissonRatio", a.get<double>("poissonRatio"));
                setPlug(graph, d.id(), "falloff", falloff);
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    auto handleCommand = [](const char* cmd, const char* type, const char* label) {
        return [cmd, type, label](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), cmd);
            if (meshes.size() != 1)
                throw CommandError(std::string(cmd) + ": give one mesh");
            NodeId mesh = meshes[0];
            std::vector<std::vector<uint32_t>> handles;
            if (a.has("handles"))
            {
                for (const json& h : a.flags["handles"])
                    handles.push_back(pointsFromItems(graph, mesh, h, cmd));
            }
            else if (std::vector<uint32_t> sel = pointsArg(graph, mesh, a, "points", cmd); !sel.empty())
                handles.push_back(std::move(sel));
            std::vector<uint32_t> anchors = a.has("anchors") ? pointsFromItems(graph, mesh, a.flags["anchors"], cmd) : std::vector<uint32_t>{};

            DeformerNode& d = addOne(graph, type, mesh, a, label);
            if (a.has("iterations") && d.attr("iterations") >= 0)
                setPlug(graph, d.id(), "iterations", int64_t(a.get<int>("iterations")));
            auto anchorData = std::make_shared<PointGroups>();
            anchorData->groups.push_back(anchors);
            setPlug(graph, d.id(), "anchors", DataPtr(std::move(anchorData)));
            bindRest(graph, d);
            json out = json::array({d.name()});
            std::vector<NodeId> made;
            for (const std::vector<uint32_t>& pts : handles)
            {
                NodeId h = addRegionHandle(graph, d, mesh, pts, cmd);
                made.push_back(h);
                out.push_back(graph.node(h)->name());
            }
            graph.select(made.empty() ? std::vector<NodeId>{mesh} : std::vector<NodeId>{made.back()});
            return out;
        };
    };
    const std::vector<FlagDef> handleFlags = {
        {"handles", "", T::List, nullptr, "One entry per handle: its points as ids, components (\"Body.vtx[0:9]\") or set names. "
                                          "Default: the selected components as one handle."},
        {"points", "", T::List, nullptr, "The points of a single handle, as handles= takes them."},
        {"anchors", "", T::List, nullptr, "Points that stay put, as handles= takes them."},
        {"name", "", T::String, nullptr, "Name for the deformer."}};
    std::vector<FlagDef> arapFlags = handleFlags;
    arapFlags.push_back({"iterations", "", T::Int, nullptr, "Rigidity passes (default 8).", {}, {}, 0.0, 200.0});
    ops::registerCommand({
        .name = "arap",
        .doc = "Add an as-rigid-as-possible handle deformer to a mesh: each handle region gets a locator that drags it, "
               "anchors stay, and the surface bends rigidly between. Returns [deformer, handles...].",
        .positionalDoc = "mesh",
        .flags = arapFlags,
        .run = handleCommand("arap", "arap", "Arap"),
        .examples = {"arap(\"Arm\", handles=[\"Arm.vtx[120:139]\"], anchors=[\"Arm.vtx[0:19]\"])"},
    });
    ops::registerCommand({
        .name = "laplacianDeform",
        .doc = "Add a Laplacian handle deformer to a mesh: each handle region gets a locator that drags it, anchors stay, and the "
               "surface between follows smoothly with its detail. Returns [deformer, handles...].",
        .positionalDoc = "mesh",
        .flags = handleFlags,
        .run = handleCommand("laplacianDeform", "laplacianDeform", "LaplacianDeform"),
    });

    ops::registerCommand({
        .name = "addDeformHandle",
        .doc = "Add a handle to an arap or laplacianDeform deformer (or a mesh's topmost one) for the given or selected points; "
               "or, on a bbw deformer, link a transform as a new point handle and rebind. Returns the handle.",
        .positionalDoc = "deformer or mesh",
        .flags = {{"points", "", T::List, nullptr, "The handle's points: ids, components or set names. Default: the selected components."},
                  {"handle", "", T::String, nullptr, "bbw: an existing transform to use as the handle. Default: a new locator at position=."},
                  {"position", "", T::Vec3, nullptr, "bbw: where the new handle goes, world."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.positional.empty() ? a.objects() : std::vector<std::string>{a.positional[0].get<std::string>()};
            if (objs.empty())
                throw CommandError("addDeformHandle: no deformer");
            DeformerNode& d = handleDeformerNamed(objs[0], "addDeformHandle");
            NodeId mesh = modifiers::meshOf(graph, d.id());
            if (d.type().name == "bbw")
            {
                NodeId h = kNoNode;
                if (a.has("handle"))
                    h = nodeNamed(a.get<std::string>("handle"), "addDeformHandle").id();
                else if (a.has("position"))
                    h = makeLocator(graph, d.name() + "Handle", vec3Of(a.flags["position"]))->id();
                else
                    throw CommandError("addDeformHandle: give handle= or position= for a bbw deformer");
                linkHandle(graph, d, h);
                bindBbw(graph, d);
                return graph.node(h)->name();
            }
            if (mesh == kNoNode)
                throw CommandError("addDeformHandle: " + d.name() + " deforms no mesh");
            NodeId h = addRegionHandle(graph, d, mesh, pointsArg(graph, mesh, a, "points", "addDeformHandle"), "addDeformHandle");
            graph.select({h});
            return graph.node(h)->name();
        },
    });

    ops::registerCommand({
        .name = "setDeformAnchors",
        .doc = "Set the anchors of an arap or laplacianDeform deformer (or a mesh's topmost one): points that stay where the "
               "input puts them. With add, adds to them instead.",
        .positionalDoc = "deformer or mesh",
        .flags = {{"points", "", T::List, nullptr, "Ids, components or set names. Default: the selected components."},
                  {"add", "", T::Bool, false, "Add to the anchors instead of replacing them."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.positional.empty() ? a.objects() : std::vector<std::string>{a.positional[0].get<std::string>()};
            if (objs.empty())
                throw CommandError("setDeformAnchors: no deformer");
            DeformerNode& d = handleDeformerNamed(objs[0], "setDeformAnchors");
            if (d.attr("anchors") < 0)
                throw CommandError("setDeformAnchors: " + d.name() + " has no anchors");
            NodeId mesh = modifiers::meshOf(graph, d.id());
            if (mesh == kNoNode)
                throw CommandError("setDeformAnchors: " + d.name() + " deforms no mesh");
            std::vector<uint32_t> pts = pointsArg(graph, mesh, a, "points", "setDeformAnchors");
            auto anchors = std::make_shared<PointGroups>();
            if (a.get<bool>("add"))
                anchors = groupsOf(d, "anchors");
            if (anchors->groups.empty())
                anchors->groups.emplace_back();
            auto& list = anchors->groups[0];
            list.insert(list.end(), pts.begin(), pts.end());
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            graph.setValue({d.id(), d.attr("anchors"), -1}, DataPtr(std::move(anchors)));
            return int64_t(list.size());
        },
    });

    ops::registerCommand({
        .name = "deformHandles",
        .doc = "The handles of an arap, laplacianDeform or bbw deformer: [{handle, index, points}].",
        .positionalDoc = "deformer or mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("deformHandles: no deformer");
            DeformerNode& d = handleDeformerNamed(objs[0], "deformHandles");
            auto regions = d.attr("regions") >= 0 ? groupsOf(d, "regions") : std::make_shared<PointGroups>();
            json out = json::array();
            for (const HandleLink& h : handleLinks(graph, d))
            {
                json e = {{"handle", h.handle != kNoNode ? json(graph.node(h.handle)->name()) : json()}, {"index", h.index}};
                if (h.index < regions->groups.size())
                    e["points"] = regions->groups[h.index];
                out.push_back(e);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "bbw",
        .doc = "Deform meshes by point handles blended with bounded biharmonic weights. Give the meshes and the transforms "
               "(locators, joints) to use as handles, or positions= for new locators. Returns [deformers..., handles...].",
        .positionalDoc = "meshes and handles",
        .flags = {{"positions", "", T::List, nullptr, "New locator handles at these world positions."},
                  {"radius", "", T::Float, 0.0, "Points this close to a handle (cm) move fully with it; 0: just the nearest point.", {}, {}, 0.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes, handles;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "bbw");
                if (n.type().isA("mesh") && !n.type().isA("lattice"))
                    meshes.push_back(n.id());
                else if (n.type().isA("transform"))
                    handles.push_back(n.id());
                else
                    throw CommandError("bbw: " + name + " is neither a mesh nor a transform");
            }
            if (meshes.empty())
                throw CommandError("bbw: give or select at least one mesh");
            if (a.has("positions"))
                for (const json& p : a.flags["positions"])
                    handles.push_back(makeLocator(graph, "BbwHandle", vec3Of(p))->id());
            if (handles.empty())
                throw CommandError("bbw: give handles (transforms) or positions=");
            json out = json::array();
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "bbw", mesh, a, "Bbw");
                setPlug(graph, d.id(), "handleRadius", a.get<double>("radius"));
                for (NodeId h : handles)
                    linkHandle(graph, d, h);
                bindBbw(graph, d);
                out.push_back(d.name());
            }
            for (NodeId h : handles)
                out.push_back(graph.node(h)->name());
            graph.select({handles.back()});
            return out;
        },
    });

    ops::registerCommand({
        .name = "bbwSkinWeights",
        .doc = "Compute a skinCluster's weights as bounded biharmonic weights: each joint holds the points around the middle "
               "of its bone, and the weights blend smoothly between, kept to maxInfluences per point.",
        .positionalDoc = "skinCluster or mesh",
        .flags = {{"maxInfluences", "", T::Int, nullptr, "Influences kept per point. Default: the skinCluster's maxInfluences.", {}, {}, 1.0}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("bbwSkinWeights: no skinCluster");
            NodeId skinId = skin::resolve(graph, objs[0], "bbwSkinWeights");
            graph.evaluate();
            MeshPtr in = skin::inputMesh(graph, skinId);
            if (!in)
                throw CommandError("bbwSkinWeights: the mesh has no geometry");
            const glm::dmat4 geom = graph.node(skinId)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
            const std::vector<glm::dvec3> world = worldPoints(*in, geom);
            std::vector<skin::Influence> infs = skin::influences(graph, skinId);
            std::vector<Bone> bones;
            for (const skin::Influence& inf : infs)
            {
                if (inf.joint == kNoNode)
                    continue;
                Bone b{inf.index, {}};
                glm::dvec3 at = positionOf(*graph.node(inf.joint));
                for (NodeId c : graph.children(inf.joint))
                    if (rig::isJoint(*graph.node(c)))
                        b.segments.push_back({at, positionOf(*graph.node(c))});
                if (b.segments.empty())
                    b.segments.push_back({at, at});
                bones.push_back(std::move(b));
            }
            if (bones.empty())
                throw CommandError("bbwSkinWeights: the skinCluster has no joints");
            std::vector<float> w = deform::boundedBiharmonicWeights(*in, world, boneRegions(bones, world, *in));
            const int maxInf = a.has("maxInfluences") ? a.get<int>("maxInfluences")
                                                      : int(graph.node(skinId)->as<int64_t>(SkinClusterNode::kMaxInfluences));
            std::vector<char> unlocked;
            WeightTable t;
            t.points.resize(world.size());
            const size_t h = bones.size();
            for (size_t p = 0; p < world.size(); ++p)
            {
                if (!in->pointValid(geo::Index(p)))
                    continue;
                for (size_t k = 0; k < h; ++k)
                    if (w[p * h + k] > 1e-5f)
                        t.set(p, bones[k].index, w[p * h + k]);
                t.prune(p, std::max(maxInf, 1), 1e-4f, unlocked);
            }
            skin::setWeights(graph, skinId, t);
            return int64_t(world.size());
        },
    });

    auto simple = [](const char* cmd, const char* type, const char* label, std::function<void(Graph&, DeformerNode&, const Args&)> setup) {
        return [cmd, type, label, setup](const Args& a) -> json {
            Graph& graph = g();
            json out = json::array();
            for (NodeId mesh : meshesOf(a.objects(), cmd))
            {
                DeformerNode& d = addOne(graph, type, mesh, a, label);
                if (setup)
                    setup(graph, d, a);
                if (a.has("params"))
                    applyParams(graph, d, a.flags["params"]);
                out.push_back(d.name());
            }
            return out;
        };
    };
    const FlagDef nameFlag{"name", "", T::String, nullptr, "Name for the deformer."};
    const FlagDef paramsFlag{"params", "", T::Any, nullptr, "{param: value} to set on each deformer."};

    ops::registerCommand({
        .name = "inflate",
        .doc = "Add an inflate deformer: pushes points along their normals by amount (negative deflates). Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"amount", "", T::Float, 1.0, "Distance along the normal, cm."}, paramsFlag, nameFlag},
        .run = simple("inflate", "inflate", "Inflate", [](Graph& graph, DeformerNode& d, const Args& a) { setPlug(graph, d.id(), "amount", a.get<double>("amount")); }),
    });

    ops::registerCommand({
        .name = "smoothDeformer",
        .doc = "Add a smooth deformer: Laplacian, Taubin (no shrinking) or corrective (smooths only what changed since binding). "
               "Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"mode", "", T::String, "taubin", "laplacian | taubin | corrective", {"laplacian", "taubin", "corrective"}},
                  {"iterations", "", T::Int, 5, "Smoothing passes.", {}, {}, 0.0, 1000.0},
                  {"smoothStep", "", T::Float, 0.5, "How far each pass moves a point, 0 to 1.", {}, {}, 0.0, 1.0},
                  paramsFlag, nameFlag},
        .run = simple("smoothDeformer", "smoothDeformer", "Smooth",
                      [](Graph& graph, DeformerNode& d, const Args& a) {
                          std::string m = a.get<std::string>("mode");
                          int64_t mode = m == "laplacian" ? 0 : m == "taubin" ? 1 : m == "corrective" ? 2 : -1;
                          if (mode < 0)
                              throw CommandError("smoothDeformer: mode must be laplacian, taubin or corrective");
                          setPlug(graph, d.id(), "mode", mode);
                          setPlug(graph, d.id(), "iterations", int64_t(a.get<int>("iterations")));
                          setPlug(graph, d.id(), "smoothStep", a.get<double>("smoothStep"));
                          bindRest(graph, d);
                      }),
    });

    ops::registerCommand({
        .name = "noiseDeform",
        .doc = "Add a noise deformer: seeded fractal noise displacement along normals or in 3D. Key offset to animate it. "
               "Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"amplitude", "", T::Float, 1.0, "Largest displacement, object units."},
                  {"frequency", "", T::Float, 0.1, "Noise features per object unit.", {}, {}, 0.0},
                  {"seed", "", T::Int, 0, "Picks a different noise."},
                  {"octaves", "", T::Int, 3, "Layers of finer detail.", {}, {}, 1.0, 12.0},
                  {"direction", "", T::String, "normal", "normal | vector", {"normal", "vector"}},
                  paramsFlag, nameFlag},
        .run = simple("noiseDeform", "noiseDeformer", "Noise",
                      [](Graph& graph, DeformerNode& d, const Args& a) {
                          setPlug(graph, d.id(), "amplitude", a.get<double>("amplitude"));
                          setPlug(graph, d.id(), "frequency", a.get<double>("frequency"));
                          setPlug(graph, d.id(), "seed", int64_t(a.get<int>("seed")));
                          setPlug(graph, d.id(), "octaves", int64_t(a.get<int>("octaves")));
                          std::string dir = a.get<std::string>("direction");
                          if (dir != "normal" && dir != "vector")
                              throw CommandError("noiseDeform: direction must be normal or vector");
                          setPlug(graph, d.id(), "direction", int64_t(dir == "vector"));
                      }),
    });

    ops::registerCommand({
        .name = "tension",
        .doc = "Add a tension deformer: measures stretch and compression against the current shape, as a 'tension' point "
               "attribute and a colour set, for wrinkle maps. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"scale", "", T::Float, 4.0, "Colour gain: strain times scale is full colour.", {}, {}, 0.0},
                  {"colorSet", "", T::String, "tension", "Colour set to write; empty for none."},
                  nameFlag},
        .run = simple("tension", "tension", "Tension",
                      [](Graph& graph, DeformerNode& d, const Args& a) {
                          setPlug(graph, d.id(), "scale", a.get<double>("scale"));
                          setPlug(graph, d.id(), "colorSet", a.get<std::string>("colorSet"));
                          bindRest(graph, d);
                      }),
    });

    ops::registerCommand({
        .name = "shear",
        .doc = "Add a shear deformer driven by a new handle sized to the meshes: X and Z offset grow with the handle's Y. "
               "Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"axis", "", T::String, "y", "World axis the handle's Y lies along.", {"x", "y", "z"}}, paramsFlag, nameFlag},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "shear");
            Node* h = makeAxisHandle(graph, "ShearHandle", worldBounds(graph, meshes), axisIndex(a.get<std::string>("axis"), "shear"));
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "shearDeformer", mesh, a, "Shear");
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                if (a.has("params"))
                    applyParams(graph, d, a.flags["params"]);
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    ops::registerCommand({
        .name = "cast",
        .doc = "Add a cast (spherify) deformer with a handle at the meshes' centre: pulls them toward a sphere, cylinder or box. "
               "Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"shape", "", T::String, "sphere", "sphere | cylinder | box", {"sphere", "cylinder", "box"}},
                  {"factor", "", T::Float, 1.0, "0 none, 1 onto the shape."},
                  {"radius", "", T::Float, 0.0, "Shape size, cm; 0 uses the mesh's mean size.", {}, {}, 0.0},
                  paramsFlag, nameFlag},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "cast");
            std::string s = a.get<std::string>("shape");
            int64_t shape = s == "sphere" ? 0 : s == "cylinder" ? 1 : s == "box" ? 2 : -1;
            if (shape < 0)
                throw CommandError("cast: shape must be sphere, cylinder or box");
            Node* h = makeLocator(graph, "CastHandle", worldBounds(graph, meshes).center());
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "cast", mesh, a, "Cast");
                setPlug(graph, d.id(), "shape", shape);
                setPlug(graph, d.id(), "factor", a.get<double>("factor"));
                setPlug(graph, d.id(), "radius", a.get<double>("radius"));
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                if (a.has("params"))
                    applyParams(graph, d, a.flags["params"]);
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    ops::registerCommand({
        .name = "warp",
        .doc = "Add a warp deformer with a from and a to handle at the meshes' centre: moving, rotating or scaling the to handle "
               "moves the points near the from handle. Returns [from, to, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"position", "", T::Vec3, nullptr, "Where both handles start, world. Default: the meshes' centre."},
                  {"radius", "", T::Float, nullptr, "Falloff radius, cm. Default: a quarter of the meshes' size.", {}, {}, 0.0},
                  paramsFlag, nameFlag},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "warp");
            geo::Bounds b = worldBounds(graph, meshes);
            glm::dvec3 pos = a.has("position") ? vec3Of(a.flags["position"]) : b.center();
            double radius = a.has("radius") ? a.get<double>("radius") : std::max(glm::length(b.size()) * 0.25, 1e-3);
            Node* from = makeLocator(graph, "WarpFrom", pos);
            Node* to = makeLocator(graph, "WarpTo", pos);
            json out = json::array({from->name(), to->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "warp", mesh, a, "Warp");
                setPlug(graph, d.id(), "falloffRadius", radius);
                connectPlug(graph, from->id(), "worldMatrix", d.id(), "fromMatrix");
                connectPlug(graph, to->id(), "worldMatrix", d.id(), "toMatrix");
                if (a.has("params"))
                    applyParams(graph, d, a.flags["params"]);
                out.push_back(d.name());
            }
            graph.select({to->id()});
            return out;
        },
    });
}
} // namespace deformers
} // namespace lila::scene
