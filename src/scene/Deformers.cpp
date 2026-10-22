#include "scene/Deformers.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/SoftSelect.h"

#include <tbb/parallel_for.h>

#include <cctype>
#include <cmath>
#include <format>

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

std::vector<double> flatten(const std::vector<glm::dvec3>& v)
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

/** Points captured at bind time (delta mush rest shape). */
struct RestPoints final : Data
{
    std::vector<glm::dvec3> points;
    const char* kind() const override { return "restPoints"; }
    size_t bytes() const override { return points.size() * sizeof(glm::dvec3); }
    json toJson() const override { return {{"kind", kind()}, {"points", flatten(points)}}; }
    static DataPtr fromJson(const json& j)
    {
        auto r = std::make_shared<RestPoints>();
        std::vector<double> f;
        readArray(j, "points", f);
        r->points = unflatten(f);
        return r;
    }
};

struct WrapData final : Data
{
    deform::WrapBinding bind;
    const char* kind() const override { return "wrapBinding"; }
    size_t bytes() const override { return bind.offsets.size() * 4 + bind.influences.size() * sizeof(deform::WrapBinding::Influence); }
    json toJson() const override
    {
        std::vector<uint32_t> tris;
        std::vector<double> bary, offset, weight;
        for (const auto& inf : bind.influences)
        {
            tris.insert(tris.end(), {inf.tri[0], inf.tri[1], inf.tri[2]});
            bary.insert(bary.end(), {inf.bary.x, inf.bary.y, inf.bary.z});
            offset.insert(offset.end(), {inf.offset.x, inf.offset.y, inf.offset.z});
            weight.push_back(inf.weight);
        }
        return {{"kind", kind()}, {"offsets", bind.offsets}, {"tris", tris}, {"bary", bary}, {"offset", offset}, {"weight", weight},
                {"driverPoints", bind.driverPoints}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<WrapData>();
        std::vector<uint32_t> tris;
        std::vector<double> bary, offset, weight;
        readArray(j, "offsets", d->bind.offsets);
        readArray(j, "tris", tris);
        readArray(j, "bary", bary);
        readArray(j, "offset", offset);
        readArray(j, "weight", weight);
        d->bind.driverPoints = j.value("driverPoints", 0u);
        size_t n = std::min({tris.size() / 3, bary.size() / 3, offset.size() / 3, weight.size()});
        for (size_t i = 0; i < n; ++i)
            d->bind.influences.push_back({{tris[i * 3], tris[i * 3 + 1], tris[i * 3 + 2]},
                                          {bary[i * 3], bary[i * 3 + 1], bary[i * 3 + 2]},
                                          {offset[i * 3], offset[i * 3 + 1], offset[i * 3 + 2]},
                                          weight[i]});
        if (d->bind.offsets.empty())
            d->bind.offsets = {0};
        for (uint32_t& o : d->bind.offsets)
            o = uint32_t(std::min<size_t>(o, n));
        for (size_t i = 1; i < d->bind.offsets.size(); ++i)
            d->bind.offsets[i] = std::max(d->bind.offsets[i], d->bind.offsets[i - 1]);
        return d;
    }
};

/** Dense mean value coordinates of every target point against the cage as it was at bind time. */
struct CageData final : Data
{
    std::vector<glm::dvec3> rest; // cage points, world
    std::vector<float> weights;   // points x rest.size()
    uint32_t points = 0;
    const char* kind() const override { return "cageBinding"; }
    size_t bytes() const override { return rest.size() * sizeof(glm::dvec3) + weights.size() * sizeof(float); }
    json toJson() const override { return {{"kind", kind()}, {"rest", flatten(rest)}, {"weights", weights}, {"points", points}}; }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<CageData>();
        std::vector<double> f;
        readArray(j, "rest", f);
        d->rest = unflatten(f);
        readArray(j, "weights", d->weights);
        d->points = j.value("points", 0u);
        if (d->weights.size() != size_t(d->points) * d->rest.size())
            d->points = d->rest.empty() ? 0 : uint32_t(d->weights.size() / d->rest.size());
        return d;
    }
};

/** What one blendTarget passes down its chain. */
struct BlendTargetLink final : Data
{
    DataPtr prev;
    double weight = 0.0;
    DataPtr deltas;
    MeshPtr live;
    const char* kind() const override { return "blendTargetLink"; }
    json toJson() const override { return {{"kind", kind()}}; }
};

// ------------------------------------------------------------------------------------------------ attr helpers

AttrDef keyable(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}
AttrDef stored(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable, .doc = std::move(doc)};
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
AttrDef enumAttr(std::string name, int64_t def, std::vector<std::string> names, std::string doc)
{
    AttrDef a = stored(std::move(name), PlugType::Enum, def, std::move(doc));
    a.enumNames = std::move(names);
    return a;
}

AttrDef geomMatrixAttr() { return hiddenStored("geomMatrix", PlugType::Matrix, glm::dmat4(1.0), "The mesh's world matrix when the deformer was made."); }

AttrDef boundsAttr(const char* name, double def, const char* doc) { return keyable(name, PlugType::Float, def, doc); }

AttrDef falloffRadiusAttr()
{
    return keyable("falloffRadius", PlugType::Float, 0.0,
                   "Spherical falloff around the handle, in handle units: points beyond it are untouched. 0 turns it off.");
}
AttrDef falloffSoftnessAttr()
{
    return keyable("falloffSoftness", PlugType::Float, 0.5, "How much of the falloff radius eases out, 0 a hard edge to 1 all soft.");
}

template <class F> void parallelPoints(size_t n, F&& fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 512), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
            fn(i);
    });
}

/** A deformer whose binding is not captured yet: reported on the node, not logged, as it happens while binding. */
struct Unbound : std::runtime_error
{
    Unbound() : std::runtime_error("not bound: run rebindDeformer") {}
};

glm::dmat4 safeInverse(const glm::dmat4& m, const char* what)
{
    if (std::abs(glm::determinant(m)) < 1e-18)
        throw std::runtime_error(std::string(what) + " has zero scale");
    return glm::inverse(m);
}

glm::dvec3 xform(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

// ------------------------------------------------------------------------------------------------ nodes

enum class NonlinearKind
{
    Bend,
    Twist,
    Taper,
    Squash,
    Sine,
    Wave,
};

class NonlinearNode : public DeformerNode
{
public:
    explicit NonlinearNode(NonlinearKind k) : mKind(k) {}

    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 handle = matrix("handleMatrix");
        const glm::dmat4 toHandle = safeInverse(handle, "the handle") * geom;
        const glm::dmat4 back = safeInverse(geom, "the mesh") * handle;
        const double radius = num("falloffRadius"), softness = num("falloffSoftness");
        std::function<glm::dvec3(const glm::dvec3&)> f;
        switch (mKind)
        {
        case NonlinearKind::Bend:
            f = [o = deform::BendParams{num("angle"), num("lowBound"), num("highBound")}](const glm::dvec3& p) { return deform::bend(p, o); };
            break;
        case NonlinearKind::Twist:
            f = [o = deform::TwistParams{num("startAngle"), num("endAngle"), num("lowBound"), num("highBound")}](const glm::dvec3& p) {
                return deform::twist(p, o);
            };
            break;
        case NonlinearKind::Taper:
            f = [o = deform::TaperParams{num("startScale"), num("endScale"), num("curve"), num("lowBound"), num("highBound")}](const glm::dvec3& p) {
                return deform::taper(p, o);
            };
            break;
        case NonlinearKind::Squash:
            f = [o = deform::SquashParams{num("factor"), num("expand"), num("maxExpandPos"), num("lowBound"), num("highBound")}](
                    const glm::dvec3& p) { return deform::squash(p, o); };
            break;
        case NonlinearKind::Sine:
            f = [o = deform::SineParams{num("amplitude"), num("wavelength"), num("offset"), num("dropoff"), num("lowBound"), num("highBound")}](
                    const glm::dvec3& p) { return deform::sine(p, o); };
            break;
        case NonlinearKind::Wave:
            f = [o = deform::WaveParams{num("amplitude"), num("wavelength"), num("offset"), num("dropoff"), num("minRadius"), num("maxRadius")}](
                    const glm::dvec3& p) { return deform::wave(p, o); };
            break;
        }
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 h = xform(toHandle, points[i]);
            double w = deform::sphericalFalloff(glm::length(h), radius, softness);
            if (w <= 0.0)
                return;
            points[i] = xform(back, glm::mix(h, f(h), w));
        });
    }

private:
    NonlinearKind mKind;
};

class FfdNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const MeshPtr& lattice = as<MeshPtr>(attr("lattice"));
        if (!lattice)
            throw std::runtime_error("no lattice connected");
        glm::dvec3 dv = as<glm::dvec3>(attr("divisions"));
        glm::ivec3 n(std::max(2, int(dv.x)), std::max(2, int(dv.y)), std::max(2, int(dv.z)));
        size_t count = size_t(n.x) * size_t(n.y) * size_t(n.z);
        if (lattice->pointCapacity() != count)
            throw std::runtime_error("the lattice's points no longer match its divisions");
        std::vector<glm::dvec3> disp(count);
        for (int k = 0; k < n.z; ++k)
            for (int j = 0; j < n.y; ++j)
                for (int i = 0; i < n.x; ++i)
                {
                    size_t idx = deform::latticeIndex(n, i, j, k);
                    disp[idx] = lattice->position(geo::Index(idx)) - deform::latticeRest(n, i, j, k);
                }
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const glm::dmat4 baseInv = safeInverse(matrix("baseMatrix"), "the lattice base");
        const glm::dmat4 lat = matrix("latticeMatrix");
        const auto basis = deform::FfdBasis(std::clamp<int64_t>(integer("interpolation"), 0, 2));
        const double outside = num("outsideFalloff");
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 w = xform(geom, points[i]);
            glm::dvec3 q = xform(baseInv, w);
            double dist = glm::length(glm::max(glm::abs(q) - glm::dvec3(0.5), glm::dvec3(0.0)));
            double fall = outside > 0.0 ? deform::sphericalFalloff(dist, outside, 1.0) : (dist > 1e-9 ? 0.0 : 1.0);
            if (fall <= 0.0)
                return;
            glm::dvec3 stu = glm::clamp(q + glm::dvec3(0.5), glm::dvec3(0.0), glm::dvec3(1.0));
            glm::dvec3 moved = xform(lat, q + deform::ffdDisplacement(n, disp, stu, basis));
            points[i] = xform(geomInv, glm::mix(w, moved, fall));
        });
    }
};

class DeltaMushNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        DataPtr restData = as<DataPtr>(attr("rest"));
        const auto* rest = dynamic_cast<const RestPoints*>(restData.get());
        if (!rest)
            throw Unbound();
        if (rest->points.size() != points.size())
            throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
        int iterations = int(std::clamp<int64_t>(integer("iterations"), 0, 1000));
        double step = std::clamp(num("smoothStep"), 0.0, 1.0);
        bool pin = as<bool>(attr("pinBorders"));
        // Rebinding is the costly half, so it is kept until the rest shape, settings or topology change.
        if (mRestKey != restData || mIterations != iterations || mStep != step || mPin != pin || mEdges != in.numEdges())
        {
            mAdj = deform::pointAdjacency(in);
            mBind = deform::deltaMushBind(in, rest->points, mAdj, iterations, step, pin);
            mRestKey = restData;
            mIterations = iterations;
            mStep = step;
            mPin = pin;
            mEdges = in.numEdges();
        }
        points = deform::deltaMushApply(in, points, mAdj, mBind, num("displacement"));
    }

private:
    mutable DataPtr mRestKey;
    mutable int mIterations = -1;
    mutable double mStep = -1.0;
    mutable bool mPin = false;
    mutable geo::Index mEdges = 0;
    mutable deform::Adjacency mAdj;
    mutable deform::DeltaMushBind mBind;
};

class WrapNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const auto* b = dynamic_cast<const WrapData*>(as<DataPtr>(attr("binding")).get());
        const MeshPtr& driver = as<MeshPtr>(attr("driver"));
        if (!b)
            throw Unbound();
        if (!driver)
            throw std::runtime_error("no driver connected");
        if (driver->pointCapacity() != b->bind.driverPoints)
            throw std::runtime_error("the driver's points changed since binding: run rebindDeformer");
        const glm::dmat4 dm = matrix("driverMatrix"), geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dvec3> drv(driver->pointCapacity());
        for (geo::Index p = 0; p < driver->pointCapacity(); ++p)
            drv[p] = xform(dm, driver->position(p));
        std::vector<glm::dvec3> world(points.size());
        parallelPoints(points.size(), [&](size_t i) { world[i] = xform(geom, points[i]); });
        std::vector<glm::dvec3> moved = deform::wrapApply(b->bind, drv, world);
        parallelPoints(points.size(), [&](size_t i) { points[i] = xform(geomInv, moved[i]); });
    }
};

class CageNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const auto* b = dynamic_cast<const CageData*>(as<DataPtr>(attr("binding")).get());
        const MeshPtr& cage = as<MeshPtr>(attr("driver"));
        if (!b)
            throw Unbound();
        if (!cage)
            throw std::runtime_error("no cage connected");
        const size_t nv = b->rest.size();
        if (cage->pointCapacity() != nv)
            throw std::runtime_error("the cage's points changed since binding: run rebindDeformer");
        if (b->points != points.size())
            throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
        const glm::dmat4 dm = matrix("driverMatrix"), geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dvec3> disp(nv);
        for (size_t j = 0; j < nv; ++j)
            disp[j] = xform(dm, cage->position(geo::Index(j))) - b->rest[j];
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 d(0.0);
            const float* row = &b->weights[i * nv];
            for (size_t j = 0; j < nv; ++j)
                d += disp[j] * double(row[j]);
            points[i] = xform(geomInv, xform(geom, points[i]) + d);
        });
    }
};

class SoftModNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const glm::dmat4 move = matrix("handleMatrix") * safeInverse(matrix("bindMatrix"), "the bind matrix");
        const glm::dvec3 center = as<glm::dvec3>(attr("falloffCenter"));
        const double radius = num("falloffRadius");
        const std::string& curveText = as<std::string>(attr("falloffCurve"));
        softSelect::Curve curve;
        if (!curveText.empty())
            curve = softSelect::Curve::fromJson(json::parse(curveText, nullptr, false));
        parallelPoints(points.size(), [&](size_t i) {
            glm::dvec3 w = xform(geom, points[i]);
            double f = 1.0;
            if (radius > 0.0)
            {
                double d = glm::length(w - center) / radius;
                f = d >= 1.0 ? 0.0 : curve.evaluate(d);
            }
            if (f <= 0.0)
                return;
            points[i] = xform(geomInv, glm::mix(w, xform(move, w), f));
        });
    }
};

class BlendShapeNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        for (DataPtr d = as<DataPtr>(attr("targets")); d;)
        {
            const auto* link = dynamic_cast<const BlendTargetLink*>(d.get());
            if (!link)
                break;
            d = link->prev;
            const double w = link->weight;
            if (w == 0.0)
                continue;
            if (link->live)
            {
                if (link->live->pointCapacity() != in.pointCapacity())
                    throw std::runtime_error("a live target's point count differs from the mesh");
                parallelPoints(points.size(), [&](size_t p) { points[p] += (link->live->position(geo::Index(p)) - in.position(geo::Index(p))) * w; });
            }
            else if (const auto* deltas = dynamic_cast<const BlendDeltas*>(link->deltas.get()))
            {
                const size_t n = std::min(deltas->indices.size(), deltas->deltas.size());
                parallelPoints(n, [&](size_t k) {
                    uint32_t p = deltas->indices[k];
                    if (p < points.size())
                        points[p] += deltas->deltas[k] * w;
                });
            }
        }
    }
};

class BlendTargetNode : public Node
{
public:
    enum Attr
    {
        kWeight,
        kMorphName,
        kDeltas,
        kTargetMesh,
        kPrevious,
        kLink,
    };
    void compute(const EvalContext&) override
    {
        auto link = std::make_shared<BlendTargetLink>();
        link->prev = as<DataPtr>(kPrevious);
        link->weight = as<double>(kWeight);
        link->deltas = as<DataPtr>(kDeltas);
        link->live = as<MeshPtr>(kTargetMesh);
        setOutput(kLink, DataPtr(std::move(link)));
    }
};

// ------------------------------------------------------------------------------------------------ registration

struct Spec
{
    std::string name;
    std::string icon;
    std::string doc;
    std::vector<AttrDef> params;
    std::function<std::unique_ptr<DeformerNode>()> make;
};

void registerDeformer(Spec s)
{
    NodeType t;
    t.name = s.name;
    t.icon = s.icon;
    t.doc = s.doc;
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        keyable("enabled", PlugType::Bool, true, "Turn the deformer off without removing it."),
        stored("exportEnabled", PlugType::Bool, true, "Apply the deformer when exporting."),
        keyable("envelope", PlugType::Float, 1.0, "How much of the deformation applies: 0 none, 1 all."),
        hiddenStored("weights", PlugType::Data, DataPtr(), "Per-point weight map; points past its end weigh 1."),
    };
    for (AttrDef& p : s.params)
    {
        if (p.type == PlugType::Vec3)
            for (int k = 0; k < 3; ++k)
                p.childNames[k] = p.name + char('X' + k);
        t.ownAttrs.push_back(std::move(p));
    }
    t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden});
    const int out = int(t.ownAttrs.size()) - 1;
    auto make = s.make;
    t.create = [make, out]() -> std::unique_ptr<Node> {
        std::unique_ptr<DeformerNode> n = make();
        n->inAttr = DeformerNode::kInMesh;
        n->enabledAttr = DeformerNode::kEnabled;
        n->outAttr = out;
        n->apply = [](const geo::Mesh& m, const Node& self) { return static_cast<const DeformerNode&>(self).run(m); };
        return n;
    };
    types::registerType(std::move(t));
}

std::vector<AttrDef> nonlinearCommon(std::vector<AttrDef> own)
{
    std::vector<AttrDef> a = {connected("handleMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's worldMatrix."), geomMatrixAttr(),
                              falloffRadiusAttr(), falloffSoftnessAttr()};
    a.insert(a.end(), own.begin(), own.end());
    return a;
}

AttrDef lowBound(double def = -1.0) { return boundsAttr("lowBound", def, "Where the effect starts along the handle's Y axis, handle units."); }
AttrDef highBound(double def = 1.0) { return boundsAttr("highBound", def, "Where the effect ends along the handle's Y axis, handle units."); }

template <NonlinearKind K> std::unique_ptr<DeformerNode> makeNonlinear() { return std::make_unique<NonlinearNode>(K); }

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
    auto* d = dynamic_cast<DeformerNode*>(&nodeNamed(name, cmd));
    if (!d)
        throw CommandError(std::string(cmd) + ": " + name + " is not a deformer");
    return *d;
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

/** Splices the deformer into the mesh's stack: at the top, or just under the skinCluster when belowSkin. */
void insertDeformer(Graph& graph, NodeId mesh, DeformerNode& d, bool belowSkin)
{
    PlugRef top{mesh, MeshNode::kInMesh, -1};
    PlugRef at = top;
    if (belowSkin)
        if (NodeId s = skin::find(graph, mesh); s != kNoNode)
            at = {s, SkinClusterNode::kInMesh, -1};
    PlugRef in{d.id(), DeformerNode::kInMesh, -1};
    if (auto src = graph.source(at))
    {
        graph.disconnect(at);
        graph.connect(*src, in);
    }
    else
    {
        Node* source = graph.createNode(*types::find("meshSource"), {}, kNoNode, true);
        graph.setValue({source->id(), MeshSourceNode::kMesh, -1}, graph.storedValue(top));
        graph.setValue(top, MeshPtr());
        graph.connect({source->id(), MeshSourceNode::kOutMesh, -1}, in);
    }
    graph.connect({d.id(), d.outAttr, -1}, at);
}

DeformerNode& createDeformer(Graph& graph, const std::string& type, NodeId mesh, const std::string& name, bool belowSkin)
{
    const NodeType* t = types::find(type);
    Node* n = guarded([&] { return graph.createNode(*t, name, kNoNode, true); });
    auto& d = static_cast<DeformerNode&>(*n);
    insertDeformer(graph, mesh, d, belowSkin);
    if (int a = d.attr("geomMatrix"); a >= 0)
        graph.setValue({d.id(), a, -1}, worldOf(*graph.node(mesh)));
    return d;
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

/** Applies {param: value} from a command's params flag, as addModifier does. */
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

NodeId meshOfDeformer(Graph& graph, NodeId d) { return modifiers::meshOf(graph, d); }

/** The deformer's input points in world space, evaluated now. */
std::vector<glm::dvec3> inputWorld(Graph& graph, DeformerNode& d, MeshPtr* meshOut = nullptr)
{
    graph.evaluate();
    MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
    if (!in)
        throw CommandError(d.name() + ": the mesh has no geometry");
    glm::dmat4 geom = d.as<glm::dmat4>(d.attr("geomMatrix"));
    std::vector<glm::dvec3> out(in->pointCapacity());
    for (geo::Index p = 0; p < in->pointCapacity(); ++p)
        out[p] = xform(geom, in->position(p));
    if (meshOut)
        *meshOut = in;
    return out;
}

/** The driver mesh feeding a wrap or cage, with its points in world space. */
std::pair<MeshPtr, glm::dmat4> driverOf(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr m = std::get<MeshPtr>(graph.value({d.id(), d.attr("driver"), -1}));
    if (!m)
        throw CommandError(d.name() + ": no driver mesh connected");
    return {m, std::get<glm::dmat4>(graph.value({d.id(), d.attr("driverMatrix"), -1}))};
}

void bindWrap(Graph& graph, DeformerNode& d)
{
    auto [driver, dm] = driverOf(graph, d);
    geo::Mesh world = *driver;
    for (geo::Index p = 0; p < world.pointCapacity(); ++p)
        world.setPosition(p, xform(dm, driver->position(p)));
    std::vector<glm::dvec3> targets = inputWorld(graph, d);
    auto data = std::make_shared<WrapData>();
    data->bind = deform::wrapBind(world, targets, int(d.integer("maxDrivers")), d.num("falloff"));
    graph.setValue({d.id(), d.attr("binding"), -1}, DataPtr(std::move(data)));
}

void bindCage(Graph& graph, DeformerNode& d)
{
    auto [cage, dm] = driverOf(graph, d);
    auto data = std::make_shared<CageData>();
    data->rest.resize(cage->pointCapacity());
    for (geo::Index p = 0; p < cage->pointCapacity(); ++p)
        data->rest[p] = xform(dm, cage->position(p));
    std::vector<uint32_t> tris = deform::trianglePoints(*cage);
    if (tris.empty())
        throw CommandError(d.name() + ": the cage has no faces");
    std::vector<glm::dvec3> targets = inputWorld(graph, d);
    data->points = uint32_t(targets.size());
    data->weights = deform::meanValueCoordinates(data->rest, tris, targets);
    graph.setValue({d.id(), d.attr("binding"), -1}, DataPtr(std::move(data)));
}

void bindDeltaMush(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr rest;
    NodeId mesh = meshOfDeformer(graph, d.id());
    // Above a skinCluster the rest shape is the bind shape, not whatever pose the joints are in now.
    if (NodeId s = mesh != kNoNode ? skin::find(graph, mesh) : kNoNode; s != kNoNode)
    {
        std::vector<NodeId> st = modifiers::stack(graph, mesh);
        if (std::find(st.begin(), st.end(), s) < std::find(st.begin(), st.end(), d.id()))
            rest = skin::inputMesh(graph, s);
    }
    MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
    if (!rest || !in || rest->pointCapacity() != in->pointCapacity())
        rest = in;
    if (!rest)
        throw CommandError(d.name() + ": the mesh has no geometry");
    auto data = std::make_shared<RestPoints>();
    data->points.assign(rest->positions().begin(), rest->positions().end());
    graph.setValue({d.id(), d.attr("rest"), -1}, DataPtr(std::move(data)));
}

/** Locator handle sized to the bounds along `axis` (0 x, 1 y, 2 z); its +Y is the deformation axis. */
Node* makeHandle(Graph& graph, const std::string& name, const geo::Bounds& b, int axis)
{
    Node* h = guarded([&] { return graph.createNode(*types::find("locator"), name, kNoNode, true); });
    glm::dvec3 size = b.size();
    double extent = size[axis] > 1e-9 ? size[axis] : std::max({size.x, size.y, size.z, 1.0});
    double s = extent * 0.5;
    graph.setValue({h->id(), Transform::kTranslate, -1}, b.center());
    glm::dvec3 rot(0.0);
    if (axis == 0)
        rot.z = -90.0;
    else if (axis == 2)
        rot.x = 90.0;
    graph.setValue({h->id(), Transform::kRotate, -1}, rot);
    graph.setValue({h->id(), Transform::kScale, -1}, glm::dvec3(s));
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

// ------------------------------------------------------------------------------------------------ blend shape helpers

const NodeType& blendTargetType() { return *types::find("blendTarget"); }

NodeId resolveBlendShape(Graph& graph, const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (n.type().isA("blendShape"))
        return n.id();
    if (n.type().isA("mesh"))
        if (NodeId b = deformers::blendShapeOf(graph, n.id()); b != kNoNode)
            return b;
    throw CommandError(std::string(cmd) + ": " + name + " has no blendShape");
}

/** Target nodes of a blendShape, first added first. */
std::vector<NodeId> targetNodes(Graph& graph, NodeId bs)
{
    std::vector<NodeId> out;
    PlugRef plug{bs, graph.node(bs)->type().attrIndex("targets"), -1};
    while (auto src = graph.source(plug))
    {
        Node* t = graph.node(src->node);
        if (!dynamic_cast<BlendTargetNode*>(t) || out.size() > 100000)
            break;
        out.push_back(t->id());
        plug = {t->id(), BlendTargetNode::kPrevious, -1};
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::shared_ptr<BlendDeltas> deltasBetween(const geo::Mesh& base, const geo::Mesh& target)
{
    auto d = std::make_shared<BlendDeltas>();
    for (geo::Index p = 0; p < base.pointCapacity(); ++p)
    {
        if (!base.pointValid(p))
            continue;
        glm::dvec3 delta = target.position(p) - base.position(p);
        if (glm::length(delta) > 1e-7)
        {
            d->indices.push_back(uint32_t(p));
            d->deltas.push_back(delta);
        }
    }
    return d;
}

/** Hooks a target node onto the end of the blendShape's chain. */
void appendTarget(Graph& graph, NodeId bs, NodeId t)
{
    PlugRef targets{bs, graph.node(bs)->type().attrIndex("targets"), -1};
    if (auto last = graph.source(targets))
    {
        graph.disconnect(targets);
        graph.connect(*last, {t, BlendTargetNode::kPrevious, -1});
    }
    graph.connect({t, BlendTargetNode::kLink, -1}, targets);
}

NodeId addTarget(Graph& graph, NodeId bs, NodeId targetMesh, const std::string& morphName, bool live)
{
    graph.evaluate();
    MeshPtr base = std::get<MeshPtr>(graph.value({bs, DeformerNode::kInMesh, -1}));
    MeshPtr target = graph.node(targetMesh)->as<MeshPtr>(MeshNode::kOutMesh);
    if (!base || !target)
        throw CommandError("blendShape: " + graph.node(targetMesh)->name() + " or the base has no geometry");
    if (base->pointCapacity() != target->pointCapacity())
        throw CommandError(std::format("blendShape: {} has {} points, the base {}", graph.node(targetMesh)->name(), target->pointCapacity(),
                                       base->pointCapacity()));
    Node* t = guarded([&] { return graph.createNode(blendTargetType(), morphName + "Target", kNoNode, true); });
    graph.setValue({t->id(), BlendTargetNode::kMorphName, -1}, morphName);
    if (live)
        graph.connect({targetMesh, MeshNode::kOutMesh, -1}, {t->id(), BlendTargetNode::kTargetMesh, -1});
    else
        graph.setValue({t->id(), BlendTargetNode::kDeltas, -1}, DataPtr(deltasBetween(*base, *target)));
    appendTarget(graph, bs, t->id());
    return t->id();
}

void removeTarget(Graph& graph, NodeId t)
{
    auto prev = graph.source({t, BlendTargetNode::kPrevious, -1});
    std::vector<PlugRef> dsts = graph.destinations({t, BlendTargetNode::kLink, -1});
    for (const PlugRef& d : dsts)
        graph.disconnect(d);
    if (prev)
        for (const PlugRef& d : dsts)
            graph.connect(*prev, d);
    graph.deleteNode(t);
}

json targetInfo(Graph& graph, NodeId t)
{
    Node* n = graph.node(t);
    return {{"node", n->name()},
            {"name", n->as<std::string>(BlendTargetNode::kMorphName)},
            {"weight", n->as<double>(BlendTargetNode::kWeight)},
            {"live", graph.source({t, BlendTargetNode::kTargetMesh, -1}).has_value()}};
}
} // namespace

// ------------------------------------------------------------------------------------------------ data kinds

json PointWeights::toJson() const { return {{"kind", kind()}, {"values", values}}; }

DataPtr PointWeights::fromJson(const json& j)
{
    auto w = std::make_shared<PointWeights>();
    readArray(j, "values", w->values);
    return w;
}

json BlendDeltas::toJson() const { return {{"kind", kind()}, {"indices", indices}, {"deltas", flatten(deltas)}}; }

DataPtr BlendDeltas::fromJson(const json& j)
{
    auto d = std::make_shared<BlendDeltas>();
    readArray(j, "indices", d->indices);
    std::vector<double> f;
    readArray(j, "deltas", f);
    d->deltas = unflatten(f);
    d->indices.resize(std::min(d->indices.size(), d->deltas.size()));
    d->deltas.resize(d->indices.size());
    return d;
}

// ------------------------------------------------------------------------------------------------ DeformerNode

double DeformerNode::num(const char* name) const
{
    int a = attr(name);
    if (a < 0)
        return 0.0;
    const PlugValue& v = value(a);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

int64_t DeformerNode::integer(const char* name) const { return int64_t(std::llround(num(name))); }

glm::dmat4 DeformerNode::matrix(const char* name) const
{
    int a = attr(name);
    return a < 0 ? glm::dmat4(1.0) : as<glm::dmat4>(a);
}

geo::Mesh DeformerNode::run(const geo::Mesh& in) const
{
    LILA_ZONE();

    std::vector<glm::dvec3> points(in.positions().begin(), in.positions().end());
    deform(in, points);
    const auto* map = dynamic_cast<const PointWeights*>(as<DataPtr>(kWeights).get());
    const double envelope = as<double>(kEnvelope);
    geo::Mesh out = in;
    parallelPoints(points.size(), [&](size_t p) {
        if (!in.pointValid(geo::Index(p)))
            return;
        double w = envelope * (map && p < map->values.size() ? double(map->values[p]) : 1.0);
        const glm::dvec3& rest = in.position(geo::Index(p));
        glm::dvec3 moved = rest + (points[p] - rest) * w;
        if (std::isfinite(moved.x) && std::isfinite(moved.y) && std::isfinite(moved.z))
            out.setPosition(geo::Index(p), moved);
    });
    if (out.cornerAttrs().has(geo::attr::kNormal))
        geo::updateCornerNormals(out);
    if (out.cornerAttrs().has(geo::attr::kTangent))
        geo::updateTangents(out);
    return out;
}

void DeformerNode::compute(const EvalContext&)
{
    LILA_ZONE();

    mError.clear();
    const MeshPtr& in = as<MeshPtr>(kInMesh);
    if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in) || as<double>(kEnvelope) == 0.0)
    {
        setOutput(outAttr, in);
        return;
    }
    try
    {
        setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(run(*in))));
    }
    catch (const Unbound& e)
    {
        mError = e.what();
        setOutput(outAttr, in);
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        mError = e.what();
        setOutput(outAttr, in);
    }
}

// ------------------------------------------------------------------------------------------------ api

namespace deformers
{
bool isDeformer(const Node& n) { return dynamic_cast<const DeformerNode*>(&n) != nullptr; }

std::vector<NodeId> list(Graph& graph, NodeId mesh)
{
    std::vector<NodeId> out;
    for (NodeId id : modifiers::stack(graph, mesh))
        if (isDeformer(*graph.node(id)))
            out.push_back(id);
    return out;
}

NodeId blendShapeOf(Graph& graph, NodeId mesh)
{
    for (NodeId id : modifiers::stack(graph, mesh))
        if (graph.node(id)->type().isA("blendShape"))
            return id;
    return kNoNode;
}

std::vector<MorphTarget> morphTargets(Graph& graph, NodeId bs)
{
    graph.evaluate();
    std::vector<MorphTarget> out;
    Node* b = graph.node(bs);
    if (!b)
        return out;
    MeshPtr base = std::get<MeshPtr>(graph.value({bs, DeformerNode::kInMesh, -1}));
    double envelope = b->as<double>(DeformerNode::kEnvelope);
    for (NodeId t : targetNodes(graph, bs))
    {
        Node* n = graph.node(t);
        MorphTarget m;
        m.node = t;
        m.name = n->as<std::string>(BlendTargetNode::kMorphName);
        if (m.name.empty())
            m.name = n->name();
        m.weight = n->as<double>(BlendTargetNode::kWeight) * envelope;
        MeshPtr live = n->as<MeshPtr>(BlendTargetNode::kTargetMesh);
        std::shared_ptr<const BlendDeltas> deltas;
        if (live && base && live->pointCapacity() == base->pointCapacity())
            deltas = deltasBetween(*base, *live);
        else
            deltas = std::dynamic_pointer_cast<const BlendDeltas>(n->as<DataPtr>(BlendTargetNode::kDeltas));
        if (deltas)
        {
            m.indices = deltas->indices;
            m.deltas = deltas->deltas;
        }
        out.push_back(std::move(m));
    }
    return out;
}

NodeId addBlendShape(Graph& graph, NodeId mesh, const std::string& name)
{
    return createDeformer(graph, "blendShape", mesh, name, true).id();
}

NodeId addBlendTarget(Graph& graph, NodeId bs, const std::string& morphName, std::shared_ptr<const BlendDeltas> deltas, double weight)
{
    Node* t = guarded([&] { return graph.createNode(blendTargetType(), morphName + "Target", kNoNode, true); });
    graph.setValue({t->id(), BlendTargetNode::kMorphName, -1}, morphName);
    graph.setValue({t->id(), BlendTargetNode::kDeltas, -1}, DataPtr(std::move(deltas)));
    graph.setValue({t->id(), BlendTargetNode::kWeight, -1}, weight);
    appendTarget(graph, bs, t->id());
    return t->id();
}

std::vector<double> morphWeights(Graph& graph, NodeId bs)
{
    graph.evaluate();
    std::vector<double> out;
    Node* b = graph.node(bs);
    if (!b)
        return out;
    double envelope = b->as<double>(DeformerNode::kEnvelope);
    for (NodeId t : targetNodes(graph, bs))
        out.push_back(graph.node(t)->as<double>(BlendTargetNode::kWeight) * envelope);
    return out;
}

std::vector<float> pointWeights(Graph& graph, NodeId deformer, size_t n)
{
    std::vector<float> out(n, 1.0f);
    const auto* w = dynamic_cast<const PointWeights*>(std::get<DataPtr>(graph.storedValue({deformer, DeformerNode::kWeights, -1})).get());
    if (w)
        for (size_t i = 0; i < std::min(n, w->values.size()); ++i)
            out[i] = w->values[i];
    return out;
}

void registerTypes()
{
    if (types::find("blendShape"))
        return;
    data::registerKind("pointWeights", &PointWeights::fromJson);
    data::registerKind("blendDeltas", &BlendDeltas::fromJson);
    data::registerKind("restPoints", &RestPoints::fromJson);
    data::registerKind("wrapBinding", &WrapData::fromJson);
    data::registerKind("cageBinding", &CageData::fromJson);

    registerDeformer({"bendDeformer", "rainbow",
                      "Bends the mesh along an arc around its handle's Y axis, rigid past the bounds. Use it for curling, drooping and "
                      "arching shapes.",
                      nonlinearCommon({keyable("angle", PlugType::Float, 90.0, "Degrees bent across the bounds."), lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Bend>});
    registerDeformer({"twistDeformer", "rotate-clockwise",
                      "Twists the mesh around its handle's Y axis, the angle ramping from the low bound to the high one.",
                      nonlinearCommon({keyable("startAngle", PlugType::Float, 0.0, "Twist at the low bound, degrees."),
                                       keyable("endAngle", PlugType::Float, 90.0, "Twist at the high bound, degrees."), lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Twist>});
    registerDeformer({"taperDeformer", "triangle-inverted",
                      "Scales the mesh across its handle's Y axis, ramping from the low bound to the high one, with an optional bulge "
                      "(flare) in between.",
                      nonlinearCommon({keyable("startScale", PlugType::Float, 1.0, "Cross-section scale at the low bound."),
                                       keyable("endScale", PlugType::Float, 0.5, "Cross-section scale at the high bound."),
                                       keyable("curve", PlugType::Float, 0.0, "Bulge added midway, in scale units (negative pinches)."),
                                       lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Taper>});
    registerDeformer({"squashDeformer", "arrows-vertical",
                      "Squashes or stretches the mesh along its handle's Y axis while the sides bulge to keep the volume. The cartoon "
                      "squash and stretch.",
                      nonlinearCommon({keyable("factor", PlugType::Float, -0.5, "Length change along Y: -0.5 halves it, 1 doubles it."),
                                       keyable("expand", PlugType::Float, 1.0, "How much the sides bulge to keep the volume, 0 not at all."),
                                       keyable("maxExpandPos", PlugType::Float, 0.5, "Where between the bounds the bulge peaks, 0 to 1."),
                                       lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Squash>});
    registerDeformer({"sineDeformer", "wave-sine",
                      "Offsets the mesh along the handle's X axis by a sine wave running along its Y axis. Use it for flags, tails and "
                      "wiggles.",
                      nonlinearCommon({keyable("amplitude", PlugType::Float, 0.2, "Wave height, handle units."),
                                       keyable("wavelength", PlugType::Float, 1.0, "Length of one wave, handle units."),
                                       keyable("offset", PlugType::Float, 0.0, "Phase, in waves; key it to animate the wave."),
                                       keyable("dropoff", PlugType::Float, 0.0, "-1 to 1: fades the amplitude toward the ends (positive) or the middle (negative)."),
                                       lowBound(), highBound()}),
                      &makeNonlinear<NonlinearKind::Sine>});
    registerDeformer({"waveDeformer", "ripple",
                      "Ripples the mesh along the handle's Y axis in rings spreading from it. Use it for water rings and shock waves.",
                      nonlinearCommon({keyable("amplitude", PlugType::Float, 0.1, "Ripple height, handle units."),
                                       keyable("wavelength", PlugType::Float, 0.5, "Distance between rings, handle units."),
                                       keyable("offset", PlugType::Float, 0.0, "Phase, in waves; key it to make the rings travel."),
                                       keyable("dropoff", PlugType::Float, 0.0, "-1 to 1: fades the rings outward (positive) or inward (negative)."),
                                       keyable("minRadius", PlugType::Float, 0.0, "Rings start this far from the axis, handle units."),
                                       keyable("maxRadius", PlugType::Float, 1.0, "Rings stop this far from the axis, handle units.")}),
                      &makeNonlinear<NonlinearKind::Wave>});

    AttrDef divisions = stored("divisions", PlugType::Vec3, glm::dvec3(4.0), "Lattice points per axis, set when the lattice is made.");
    registerDeformer({"ffd", "grid-dots",
                      "Free-form deformation: the mesh follows a lattice whose points are moved with the usual component tools. Use it "
                      "for broad shape changes and squash and stretch.",
                      {connected("lattice", PlugType::Mesh, MeshPtr(), "The lattice's outMesh."),
                       connected("latticeMatrix", PlugType::Matrix, glm::dmat4(1.0), "The lattice's worldMatrix."),
                       hiddenStored("baseMatrix", PlugType::Matrix, glm::dmat4(1.0), "The lattice's rest box: its world matrix when bound."),
                       geomMatrixAttr(), divisions,
                       enumAttr("interpolation", 1, {"trilinear", "bernstein", "bspline"},
                                "Trilinear is exact per cell, Bernstein smooth and global, bspline smooth and local (best for dense lattices)."),
                       keyable("outsideFalloff", PlugType::Float, 0.0,
                               "Points outside the lattice box are affected up to this far (lattice units), fading out. 0: only points inside.")},
                      [] { return std::make_unique<FfdNode>(); }});

    registerDeformer({"deltaMush", "droplet",
                      "Smooths away deformation artefacts while keeping the surface detail of the rest shape. Put it on top of a "
                      "skinCluster to fix collapsing elbows and candy-wrapped twists.",
                      {keyable("iterations", PlugType::Int, int64_t(10), "Smoothing passes; more spreads the correction further."),
                       keyable("smoothStep", PlugType::Float, 0.5, "How far each pass moves a point toward its neighbours, 0 to 1."),
                       keyable("displacement", PlugType::Float, 1.0, "How much of the rest detail is put back: 0 gives the smoothed mesh."),
                       stored("pinBorders", PlugType::Bool, true, "Keep open borders from shrinking while smoothing."),
                       hiddenStored("rest", PlugType::Data, DataPtr(), "The rest shape captured at bind time.")},
                      [] { return std::make_unique<DeltaMushNode>(); }});

    registerDeformer({"wrap", "layers-intersect",
                      "Makes the mesh follow another mesh (a low-res proxy or cage), each point riding its nearest driver triangles. "
                      "Use it to drive a detailed mesh from a simple one.",
                      {connected("driver", PlugType::Mesh, MeshPtr(), "The driver's outMesh."),
                       connected("driverMatrix", PlugType::Matrix, glm::dmat4(1.0), "The driver's worldMatrix."), geomMatrixAttr(),
                       stored("maxDrivers", PlugType::Int, int64_t(4), "Driver triangles each point follows, 1 to 32. Used when binding."),
                       stored("falloff", PlugType::Float, 2.0,
                              "Triangles within this many times the nearest distance count; higher is smoother. Used when binding."),
                       hiddenStored("binding", PlugType::Data, DataPtr(), "Each point's driver triangles and offsets.")},
                      [] { return std::make_unique<WrapNode>(); }});

    registerDeformer({"cage", "box",
                      "Deforms the mesh by a closed low-poly cage around it (mean value coordinates): edit the cage, the mesh inside "
                      "follows smoothly.",
                      {connected("driver", PlugType::Mesh, MeshPtr(), "The cage's outMesh."),
                       connected("driverMatrix", PlugType::Matrix, glm::dmat4(1.0), "The cage's worldMatrix."), geomMatrixAttr(),
                       hiddenStored("binding", PlugType::Data, DataPtr(), "Each point's coordinates against the rest cage.")},
                      [] { return std::make_unique<CageNode>(); }});

    registerDeformer({"softMod", "focus-2",
                      "Moves points with a handle, fading out with distance through the soft select falloff curve. Radius 0 moves "
                      "every point (weighted by the weight map), like a cluster.",
                      {connected("handleMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's worldMatrix."),
                       hiddenStored("bindMatrix", PlugType::Matrix, glm::dmat4(1.0), "The handle's world matrix when bound."), geomMatrixAttr(),
                       keyable("falloffCenter", PlugType::Vec3, glm::dvec3(0.0), "Centre of the falloff, world space."),
                       keyable("falloffRadius", PlugType::Float, 10.0, "Falloff radius, cm. 0 affects every point fully."),
                       stored("falloffCurve", PlugType::String, std::string(),
                              "Falloff shape as soft select curve JSON ([[pos, value, interp], ...]); empty for the default smooth curve.")},
                      [] { return std::make_unique<SoftModNode>(); }});

    registerDeformer({"blendShape", "mood-smile",
                      "Morph targets: blends the mesh toward target shapes by their weights. Exported to Unreal as morph targets.",
                      {connected("targets", PlugType::Data, DataPtr(), "The last blendTarget in the chain.")},
                      [] { return std::make_unique<BlendShapeNode>(); }});

    NodeType target;
    target.name = "blendTarget";
    target.icon = "mood-smile";
    target.doc = "One morph target of a blendShape: its weight, and the shape as offsets or a live target mesh.";
    target.ownAttrs = {
        keyable("weight", PlugType::Float, 0.0, "How much of the target applies, usually 0 to 1."),
        stored("morphName", PlugType::String, std::string(), "The morph target's name in exports."),
        hiddenStored("deltas", PlugType::Data, DataPtr(), "Offsets from the base, object space."),
        connected("targetMesh", PlugType::Mesh, MeshPtr(), "A live target's outMesh; overrides the stored offsets."),
        connected("previous", PlugType::Data, DataPtr(), "The target before this one."),
        {.name = "link", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "This target plus everything before it."},
    };
    target.create = [] { return std::make_unique<BlendTargetNode>(); };
    types::registerType(std::move(target));

    // The lattice is a mesh, so its points are edited with every component tool; the viewport draws it as wire.
    NodeType lattice;
    lattice.name = "lattice";
    lattice.icon = "grid-dots";
    lattice.base = types::find("mesh");
    lattice.dag = true;
    lattice.doc = "The control lattice of an ffd deformer. Move its points (vertex mode) to reshape the meshes it deforms.";
    lattice.create = [] { return std::make_unique<MeshNode>(); };
    types::registerType(std::move(lattice));
}

void registerCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "deformers",
        .doc = "A mesh's deformers (blendShape, ffd, deltaMush, ...), applied first first.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            for (NodeId mesh : meshesOf(a.objects(), "deformers"))
                for (NodeId id : deformers::list(g(), mesh))
                    out.push_back(g().node(id)->name());
            return out;
        },
    });

    cmds::registerCommand({
        .name = "nonlinear",
        .doc = "Add a nonlinear deformer (bend, twist, taper, squash, sine or wave) to the meshes, driven by a new handle sized to "
               "them. Move, rotate and scale the handle to place the effect. Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"type", "", T::String, "bend", "bend | twist | taper | squash | sine | wave", {"bend", "twist", "taper", "squash", "sine", "wave"}},
                  {"axis", "", T::String, "y", "World axis the handle's deformation axis lies along: x, y or z.", {"x", "y", "z"}},
                  {"params", "", T::Any, nullptr, "{param: value} to set on each deformer, e.g. {\"angle\": 45}."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "nonlinear");
            std::string kind = a.get<std::string>("type");
            const NodeType* t = types::find(kind + "Deformer");
            if (!t || !t->isA(kind + "Deformer") || kind.empty())
                throw CommandError("nonlinear: unknown type '" + kind + "'");
            std::string label = std::string(1, char(std::toupper(static_cast<unsigned char>(kind[0])))) + kind.substr(1);
            Node* h = makeHandle(graph, label + "Handle", worldBounds(graph, meshes), axisIndex(a.get<std::string>("axis"), "nonlinear"));
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = createDeformer(graph, kind + "Deformer", mesh, a.has("name") ? a.get<std::string>("name") : label, false);
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                if (a.has("params"))
                    applyParams(graph, d, a.flags["params"]);
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    cmds::registerCommand({
        .name = "lattice",
        .doc = "Add an ffd deformer to the meshes with a new lattice around them. Edit the lattice's points in vertex mode (soft select "
               "and symmetry work) or transform the whole lattice. Returns [lattice, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"divisions", "", T::Vec3, json::array({4, 4, 4}), "Lattice points along x, y and z, each 2 to 32."},
                  {"interpolation", "", T::String, "bernstein", "trilinear | bernstein | bspline", {"trilinear", "bernstein", "bspline"}},
                  {"name", "", T::String, nullptr, "Name for the lattice."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "lattice");
            glm::dvec3 div = vec3Of(a.flags["divisions"]);
            glm::ivec3 n(std::clamp(int(div.x), 2, 32), std::clamp(int(div.y), 2, 32), std::clamp(int(div.z), 2, 32));
            std::string interp = a.get<std::string>("interpolation");
            int64_t basis = interp == "trilinear" ? 0 : interp == "bspline" ? 2 : interp == "bernstein" ? 1 : -1;
            if (basis < 0)
                throw CommandError("lattice: interpolation must be trilinear, bernstein or bspline");
            geo::Bounds b = worldBounds(graph, meshes);
            glm::dvec3 size = b.size();
            double pad = std::max(glm::length(size) * 0.005, 1e-3);
            size = glm::max(size + glm::dvec3(pad * 2.0), glm::dvec3(pad));

            Node* lat = guarded([&] { return graph.createNode(*types::find("lattice"), a.has("name") ? a.get<std::string>("name") : "Lattice", kNoNode, true); });
            graph.setValue({lat->id(), Transform::kTranslate, -1}, b.center());
            graph.setValue({lat->id(), Transform::kScale, -1}, size);
            graph.setValue({lat->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(deform::latticeMesh(n))));
            graph.evaluate();
            glm::dmat4 latW = worldOf(*lat);
            json out = json::array({lat->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = createDeformer(graph, "ffd", mesh, "Ffd", false);
                setPlug(graph, d.id(), "divisions", glm::dvec3(n));
                setPlug(graph, d.id(), "interpolation", basis);
                setPlug(graph, d.id(), "baseMatrix", latW);
                connectPlug(graph, lat->id(), "outMesh", d.id(), "lattice");
                connectPlug(graph, lat->id(), "worldMatrix", d.id(), "latticeMatrix");
                out.push_back(d.name());
            }
            graph.select({lat->id()});
            return out;
        },
    });

    cmds::registerCommand({
        .name = "deltaMush",
        .doc = "Add a delta mush to the meshes: smooths out deformation artefacts (from skinning or other deformers) while keeping "
               "the rest shape's detail. Above a skinCluster the rest shape is the bind shape. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"iterations", "", T::Int, 10, "Smoothing passes."},
                  {"smoothStep", "", T::Float, 0.5, "How far each pass moves a point, 0 to 1."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            json out = json::array();
            for (NodeId mesh : meshesOf(a.objects(), "deltaMush"))
            {
                DeformerNode& d = createDeformer(graph, "deltaMush", mesh, a.has("name") ? a.get<std::string>("name") : "DeltaMush", false);
                setPlug(graph, d.id(), "iterations", int64_t(a.get<int>("iterations")));
                setPlug(graph, d.id(), "smoothStep", a.get<double>("smoothStep"));
                bindDeltaMush(graph, d);
                out.push_back(d.name());
            }
            return out;
        },
    });

    auto driverCommand = [](const char* cmd, const char* type, const char* label, bool isWrap) {
        return [cmd, type, label, isWrap](const Args& a) -> json {
            Graph& graph = g();
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError(std::string(cmd) + ": give the meshes to deform, then the driver last");
            Node& driver = meshNamed(objs.back(), cmd);
            objs.pop_back();
            json out = json::array();
            for (NodeId mesh : meshesOf(objs, cmd))
            {
                if (mesh == driver.id())
                    throw CommandError(std::string(cmd) + ": a mesh can't drive itself");
                DeformerNode& d = createDeformer(graph, type, mesh, a.has("name") ? a.get<std::string>("name") : label, false);
                guarded([&] {
                    connectPlug(graph, driver.id(), "outMesh", d.id(), "driver");
                    connectPlug(graph, driver.id(), "worldMatrix", d.id(), "driverMatrix");
                    return 0;
                });
                if (isWrap)
                {
                    setPlug(graph, d.id(), "maxDrivers", int64_t(a.get<int>("maxDrivers")));
                    setPlug(graph, d.id(), "falloff", a.get<double>("falloff"));
                    bindWrap(graph, d);
                }
                else
                    bindCage(graph, d);
                out.push_back(d.name());
            }
            return out;
        };
    };

    cmds::registerCommand({
        .name = "wrap",
        .doc = "Make meshes follow a driver mesh (proximity wrap): each point binds to its nearest driver triangles. Give the "
               "meshes to deform first and the driver last. Returns the deformers.",
        .positionalDoc = "meshes..., driver",
        .flags = {{"maxDrivers", "", T::Int, 4, "Driver triangles each point follows, 1 to 32."},
                  {"falloff", "", T::Float, 2.0, "Triangles within this many times the nearest distance count; higher is smoother."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = driverCommand("wrap", "wrap", "Wrap", true),
    });

    cmds::registerCommand({
        .name = "cage",
        .doc = "Deform meshes by a closed cage mesh around them (mean value coordinates). Give the meshes first and the cage last; "
               "then edit the cage's points. Returns the deformers.",
        .positionalDoc = "meshes..., cage",
        .flags = {{"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = driverCommand("cage", "cage", "Cage", false),
    });

    cmds::registerCommand({
        .name = "softMod",
        .doc = "Add a soft modification handle: moving, rotating or scaling the handle moves nearby points, fading out over the "
               "radius with the soft select curve. Returns [handle, deformers...].",
        .positionalDoc = "meshes",
        .flags = {{"position", "", T::Vec3, nullptr, "Handle and falloff centre, world space. Default: the meshes' centre."},
                  {"radius", "", T::Float, nullptr, "Falloff radius, cm. Default: a quarter of the meshes' size."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<NodeId> meshes = meshesOf(a.objects(), "softMod");
            geo::Bounds b = worldBounds(graph, meshes);
            glm::dvec3 pos = a.has("position") ? vec3Of(a.flags["position"]) : b.center();
            double radius = a.has("radius") ? a.get<double>("radius") : std::max(glm::length(b.size()) * 0.25, 1e-3);
            Node* h = guarded([&] { return graph.createNode(*types::find("locator"), "SoftModHandle", kNoNode, true); });
            graph.setValue({h->id(), Transform::kTranslate, -1}, pos);
            graph.evaluate();
            json out = json::array({h->name()});
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = createDeformer(graph, "softMod", mesh, a.has("name") ? a.get<std::string>("name") : "SoftMod", false);
                setPlug(graph, d.id(), "bindMatrix", worldOf(*h));
                setPlug(graph, d.id(), "falloffCenter", pos);
                setPlug(graph, d.id(), "falloffRadius", radius);
                connectPlug(graph, h->id(), "worldMatrix", d.id(), "handleMatrix");
                out.push_back(d.name());
            }
            graph.select({h->id()});
            return out;
        },
    });

    cmds::registerCommand({
        .name = "rebindDeformer",
        .doc = "Re-capture a deformer's bind-time state from the scene as it is now: the mesh's placement, the delta mush rest "
               "shape, wrap and cage bindings, the softMod handle's rest, or the lattice's rest box.",
        .positionalDoc = "deformers",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            for (const std::string& name : a.objects())
            {
                DeformerNode& d = deformerNamed(name, "rebindDeformer");
                NodeId mesh = meshOfDeformer(graph, d.id());
                graph.evaluate();
                if (int ga = d.attr("geomMatrix"); ga >= 0 && mesh != kNoNode)
                    graph.setValue({d.id(), ga, -1}, worldOf(*graph.node(mesh)));
                const std::string& t = d.type().name;
                if (t == "deltaMush")
                    bindDeltaMush(graph, d);
                else if (t == "wrap")
                    bindWrap(graph, d);
                else if (t == "cage")
                    bindCage(graph, d);
                else if (t == "softMod")
                {
                    if (auto src = graph.source({d.id(), d.attr("handleMatrix"), -1}))
                    {
                        glm::dmat4 hw = worldOf(*graph.node(src->node));
                        graph.setValue({d.id(), d.attr("bindMatrix"), -1}, hw);
                        graph.setValue({d.id(), d.attr("falloffCenter"), -1}, glm::dvec3(hw[3]));
                    }
                }
                else if (t == "ffd")
                {
                    if (auto src = graph.source({d.id(), d.attr("latticeMatrix"), -1}))
                        graph.setValue({d.id(), d.attr("baseMatrix"), -1}, worldOf(*graph.node(src->node)));
                }
            }
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "setDeformerWeights",
        .doc = "Set a deformer's per-point weight map: weights= the whole map (one value per point), or points= ids with value=. "
               "Weights scale the envelope per point; 1 is full effect.",
        .positionalDoc = "deformer",
        .flags = {{"weights", "", T::List, nullptr, "One weight per point."},
                  {"points", "", T::List, nullptr, "Point ids to set to value."},
                  {"value", "", T::Float, 1.0, "Weight for points=."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("setDeformerWeights: no deformer");
            DeformerNode& d = deformerNamed(objs.back(), "setDeformerWeights");
            graph.evaluate();
            MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
            size_t n = in ? in->pointCapacity() : 0;
            auto w = std::make_shared<PointWeights>();
            w->values = deformers::pointWeights(graph, d.id(), n);
            if (a.has("weights"))
            {
                w->values.clear();
                for (const json& v : a.flags["weights"])
                    w->values.push_back(v.get<float>());
            }
            if (a.has("points"))
                for (const json& p : a.flags["points"])
                {
                    size_t i = p.get<size_t>();
                    if (i >= w->values.size())
                        w->values.resize(i + 1, 1.0f);
                    w->values[i] = float(a.get<double>("value"));
                }
            graph.setValue({d.id(), DeformerNode::kWeights, -1}, DataPtr(std::move(w)));
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "deformerWeights",
        .doc = "A deformer's per-point weight map, one value per point of its input (1 where unpainted).",
        .positionalDoc = "deformer",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("deformerWeights: no deformer");
            DeformerNode& d = deformerNamed(objs.back(), "deformerWeights");
            graph.evaluate();
            MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
            return deformers::pointWeights(graph, d.id(), in ? in->pointCapacity() : 0);
        },
    });

    cmds::registerCommand({
        .name = "blendShape",
        .doc = "Add a blendShape (morph targets) to the base mesh, the last argument, with a target per other mesh. Targets need "
               "the base's point count. It sits under any skinCluster, so morphs apply before skinning, as in Unreal. Returns "
               "the blendShape.",
        .positionalDoc = "targets..., base",
        .flags = {{"live", "", T::Bool, false, "Keep targets connected so editing a target mesh updates the morph."},
                  {"name", "", T::String, nullptr, "Name for the blendShape."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("blendShape: give targets and the base mesh last");
            NodeId base = meshNamed(objs.back(), "blendShape").id();
            objs.pop_back();
            if (deformers::blendShapeOf(graph, base) != kNoNode)
                throw CommandError("blendShape: " + graph.node(base)->name() + " already has a blendShape; use addBlendTarget");
            DeformerNode& bs = createDeformer(graph, "blendShape", base, a.has("name") ? a.get<std::string>("name") : "BlendShape", true);
            for (const std::string& name : objs)
            {
                NodeId t = meshNamed(name, "blendShape").id();
                addTarget(graph, bs.id(), t, name, a.get<bool>("live"));
            }
            return bs.name();
        },
    });

    cmds::registerCommand({
        .name = "addBlendTarget",
        .doc = "Add target meshes to a blendShape (or the blendShape on a mesh): the first argument, then the targets. Returns the "
               "new target nodes, whose weight attrs drive the morphs.",
        .positionalDoc = "blendShape, targets...",
        .flags = {{"live", "", T::Bool, false, "Keep the targets connected so editing them updates the morphs."},
                  {"morphName", "", T::String, nullptr, "Export name when adding one target. Default: the target mesh's name."}},
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("addBlendTarget: give the blendShape (or its mesh), then the targets");
            NodeId bs = resolveBlendShape(graph, objs.front(), "addBlendTarget");
            json out = json::array();
            for (size_t i = 1; i < objs.size(); ++i)
            {
                NodeId t = meshNamed(objs[i], "addBlendTarget").id();
                std::string morph = a.has("morphName") && objs.size() == 2 ? a.get<std::string>("morphName") : objs[i];
                out.push_back(graph.node(addTarget(graph, bs, t, morph, a.get<bool>("live")))->name());
            }
            return out;
        },
    });

    cmds::registerCommand({
        .name = "removeBlendTarget",
        .doc = "Remove morph targets (blendTarget nodes) from their blendShape.",
        .positionalDoc = "targets",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "removeBlendTarget");
                if (!dynamic_cast<BlendTargetNode*>(&n))
                    throw CommandError("removeBlendTarget: " + name + " is not a blendTarget");
                removeTarget(graph, n.id());
            }
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "blendTargets",
        .doc = "A blendShape's targets in order: [{node, name, weight, live}]. Set a weight with setAttr(node + \".weight\", w).",
        .positionalDoc = "blendShape or mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("blendTargets: no blendShape");
            json out = json::array();
            for (NodeId t : targetNodes(graph, resolveBlendShape(graph, objs.back(), "blendTargets")))
                out.push_back(targetInfo(graph, t));
            return out;
        },
    });

    cmds::registerCommand({
        .name = "extractBlendTarget",
        .doc = "Build a new mesh in a morph target's full shape (base plus offsets), to sculpt it and add it back with addBlendTarget "
               "or keep it live. Returns the new mesh.",
        .positionalDoc = "target",
        .run = [](const Args& a) -> json {
            Graph& graph = g();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("extractBlendTarget: no target");
            Node& n = nodeNamed(objs.back(), "extractBlendTarget");
            if (!dynamic_cast<BlendTargetNode*>(&n))
                throw CommandError("extractBlendTarget: " + n.name() + " is not a blendTarget");
            auto dsts = graph.destinations({n.id(), BlendTargetNode::kLink, -1});
            NodeId bs = kNoNode;
            for (NodeId cur = n.id(); !dsts.empty();)
            {
                cur = dsts[0].node;
                if (graph.node(cur)->type().isA("blendShape"))
                {
                    bs = cur;
                    break;
                }
                dsts = graph.destinations({cur, BlendTargetNode::kLink, -1});
            }
            if (bs == kNoNode)
                throw CommandError("extractBlendTarget: " + n.name() + " is not connected to a blendShape");
            graph.evaluate();
            MeshPtr base = std::get<MeshPtr>(graph.value({bs, DeformerNode::kInMesh, -1}));
            if (!base)
                throw CommandError("extractBlendTarget: the base has no geometry");
            auto shape = std::make_shared<geo::Mesh>(*base);
            for (const deformers::MorphTarget& m : deformers::morphTargets(graph, bs))
                if (m.node == n.id())
                    for (size_t k = 0; k < m.indices.size(); ++k)
                        if (m.indices[k] < shape->pointCapacity())
                            shape->setPosition(m.indices[k], shape->position(m.indices[k]) + m.deltas[k]);
            std::string morph = n.as<std::string>(BlendTargetNode::kMorphName);
            Node* mesh = guarded([&] { return graph.createNode(*types::find("mesh"), morph.empty() ? n.name() : morph, kNoNode, true); });
            graph.setValue({mesh->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(shape)));
            NodeId baseMesh = modifiers::meshOf(graph, bs);
            if (baseMesh != kNoNode)
                Transform::setLocalMatrix(graph, mesh->id(), worldOf(*graph.node(baseMesh)));
            return mesh->name();
        },
    });
}
} // namespace deformers
} // namespace lila::scene
