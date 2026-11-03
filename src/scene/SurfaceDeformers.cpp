#include "scene/SurfaceDeformers.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "deform/Deform.h"
#include "deform/Surface.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "scene/Components.h"
#include "scene/DeformEval.h"
#include "scene/DeformWeights.h"
#include "scene/DeformerUtil.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/ObjectSets.h"
#include "scene/Repeat.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <optional>

namespace lila::scene::deformers
{
using namespace rigutil;
using namespace deformutil;

namespace
{

// ------------------------------------------------------------------------------------------------ data kinds

/** Offsets as CSR with every entry clamped to count and non-decreasing, so a damaged file can't index out of range. */
void sanitizeOffsets(std::vector<uint32_t>& offsets, size_t count)
{
    if (offsets.empty())
        offsets = {0};
    offsets[0] = 0;
    for (size_t i = 1; i < offsets.size(); ++i)
        offsets[i] = std::max(uint32_t(std::min<size_t>(offsets[i], count)), offsets[i - 1]);
}

/** The deformer's input points at bind time, object space. */
struct RestShape final : Data
{
    std::vector<glm::dvec3> points;
    const char* kind() const override { return "restShape"; }
    size_t bytes() const override { return points.size() * sizeof(glm::dvec3); }
    json toJson() const override { return {{"kind", kind()}, {"points", flatten(points)}}; }
    static DataPtr fromJson(const json& j)
    {
        auto r = std::make_shared<RestShape>();
        std::vector<double> f;
        readArray(j, "points", f);
        r->points = unflatten(f);
        return r;
    }
};

/** Rigid region per point, -1 for none. */
struct RegionIds final : Data
{
    std::vector<int32_t> ids;
    const char* kind() const override { return "rigidRegions"; }
    size_t bytes() const override { return ids.size() * sizeof(int32_t); }
    json toJson() const override { return {{"kind", kind()}, {"ids", ids}}; }
    static DataPtr fromJson(const json& j)
    {
        auto r = std::make_shared<RegionIds>();
        readArray(j, "ids", r->ids);
        return r;
    }
};

struct ProximityData final : Data
{
    deform::ProximityBinding bind;
    const char* kind() const override { return "proximityBinding"; }
    size_t bytes() const override { return bind.offsets.size() * 4 + bind.influences.size() * sizeof(deform::ProximityBinding::Influence); }
    json toJson() const override
    {
        std::vector<uint32_t> drivers, tris;
        std::vector<double> bary, offset, weight, area, anchor;
        for (const auto& inf : bind.influences)
        {
            drivers.push_back(inf.driver);
            tris.insert(tris.end(), {inf.tri[0], inf.tri[1], inf.tri[2]});
            bary.insert(bary.end(), {inf.bary.x, inf.bary.y, inf.bary.z});
            offset.insert(offset.end(), {inf.offset.x, inf.offset.y, inf.offset.z});
            anchor.insert(anchor.end(), {inf.anchor.x, inf.anchor.y, inf.anchor.z});
            weight.push_back(inf.weight);
            area.push_back(inf.restArea);
        }
        return {{"kind", kind()}, {"offsets", bind.offsets}, {"drivers", drivers}, {"tris", tris},       {"bary", bary},
                {"offset", offset}, {"weight", weight},      {"area", area},       {"anchor", anchor}, {"driverPoints", bind.driverPoints}};
    }
    /** Reads wrap 2 bindings and the single-driver "wrapBinding" ones scenes made before it. */
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<ProximityData>();
        std::vector<uint32_t> drivers, tris;
        std::vector<double> bary, offset, weight, area, anchor;
        readArray(j, "offsets", d->bind.offsets);
        readArray(j, "drivers", drivers);
        readArray(j, "tris", tris);
        readArray(j, "bary", bary);
        readArray(j, "offset", offset);
        readArray(j, "weight", weight);
        readArray(j, "area", area);
        readArray(j, "anchor", anchor);
        if (j.contains("driverPoints") && j["driverPoints"].is_array())
            readArray(j, "driverPoints", d->bind.driverPoints);
        else
            d->bind.driverPoints = {j.value("driverPoints", 0u)};
        size_t n = std::min({tris.size() / 3, bary.size() / 3, offset.size() / 3, weight.size()});
        for (size_t i = 0; i < n; ++i)
        {
            deform::ProximityBinding::Influence inf;
            inf.driver = i < drivers.size() ? drivers[i] : 0;
            std::copy(&tris[i * 3], &tris[i * 3] + 3, inf.tri);
            inf.bary = {bary[i * 3], bary[i * 3 + 1], bary[i * 3 + 2]};
            inf.offset = {offset[i * 3], offset[i * 3 + 1], offset[i * 3 + 2]};
            inf.weight = weight[i];
            inf.restArea = i < area.size() ? area[i] : 0.0;
            if (anchor.size() >= i * 3 + 3)
                inf.anchor = {anchor[i * 3], anchor[i * 3 + 1], anchor[i * 3 + 2]};
            else
                inf.restArea = 0.0;
            d->bind.influences.push_back(inf);
        }
        sanitizeOffsets(d->bind.offsets, n);
        return d;
    }
};

struct SurfaceData final : Data
{
    deform::SurfaceBinding bind;
    const char* kind() const override { return "surfaceBinding"; }
    size_t bytes() const override
    {
        return bind.offsets.size() * 4 + bind.influences.size() * sizeof(deform::SurfaceBinding::Influence) + bind.ids.size() * 4 + bind.coords.size() * 8;
    }
    json toJson() const override
    {
        std::vector<uint32_t> first, count;
        std::vector<double> weight, offset, uv, uvOffset;
        for (const auto& inf : bind.influences)
        {
            first.push_back(inf.first);
            count.push_back(inf.count);
            weight.push_back(inf.weight);
            offset.insert(offset.end(), {inf.offset.x, inf.offset.y, inf.offset.z});
            uv.insert(uv.end(), {inf.uv.x, inf.uv.y});
            uvOffset.insert(uvOffset.end(), {inf.uvOffset.x, inf.uvOffset.y, inf.uvOffset.z});
        }
        return {{"kind", kind()},     {"offsets", bind.offsets}, {"first", first},   {"count", count},   {"weight", weight},
                {"offset", offset},   {"uv", uv},                {"uvOffset", uvOffset}, {"ids", bind.ids}, {"coords", bind.coords},
                {"driverPoints", bind.driverPoints}, {"hasUv", bind.hasUv}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<SurfaceData>();
        std::vector<uint32_t> first, count;
        std::vector<double> weight, offset, uv, uvOffset;
        readArray(j, "offsets", d->bind.offsets);
        readArray(j, "first", first);
        readArray(j, "count", count);
        readArray(j, "weight", weight);
        readArray(j, "offset", offset);
        readArray(j, "uv", uv);
        readArray(j, "uvOffset", uvOffset);
        readArray(j, "ids", d->bind.ids);
        readArray(j, "coords", d->bind.coords);
        d->bind.driverPoints = j.value("driverPoints", 0u);
        d->bind.hasUv = j.value("hasUv", false);
        size_t n = std::min({first.size(), count.size(), weight.size(), offset.size() / 3});
        const size_t idCount = std::min(d->bind.ids.size(), d->bind.coords.size());
        for (size_t i = 0; i < n; ++i)
        {
            deform::SurfaceBinding::Influence inf;
            inf.first = first[i];
            inf.count = count[i];
            if (size_t(inf.first) + inf.count > idCount)
                inf.first = inf.count = 0;
            inf.weight = weight[i];
            inf.offset = {offset[i * 3], offset[i * 3 + 1], offset[i * 3 + 2]};
            if (uv.size() >= i * 2 + 2)
                inf.uv = {uv[i * 2], uv[i * 2 + 1]};
            if (uvOffset.size() >= i * 3 + 3)
                inf.uvOffset = {uvOffset[i * 3], uvOffset[i * 3 + 1], uvOffset[i * 3 + 2]};
            d->bind.influences.push_back(inf);
        }
        sanitizeOffsets(d->bind.offsets, n);
        return d;
    }
};

struct CaptureData final : Data
{
    deform::PointCapture capture;
    const char* kind() const override { return "pointCapture"; }
    size_t bytes() const override
    {
        return capture.offsets.size() * 4 + capture.ids.size() * 12 + capture.rest.size() * sizeof(glm::dvec3) + capture.nb.size() * 4;
    }
    json toJson() const override
    {
        return {{"kind", kind()},          {"offsets", capture.offsets}, {"ids", capture.ids},          {"weights", capture.weights},
                {"rest", flatten(capture.rest)}, {"nbOffsets", capture.nbOffsets}, {"nb", capture.nb}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<CaptureData>();
        deform::PointCapture& c = d->capture;
        std::vector<double> rest;
        readArray(j, "offsets", c.offsets);
        readArray(j, "ids", c.ids);
        readArray(j, "weights", c.weights);
        readArray(j, "rest", rest);
        readArray(j, "nbOffsets", c.nbOffsets);
        readArray(j, "nb", c.nb);
        c.rest = unflatten(rest);
        const size_t n = std::min(c.ids.size(), c.weights.size());
        c.ids.resize(n);
        c.weights.resize(n);
        sanitizeOffsets(c.offsets, n);
        sanitizeOffsets(c.nbOffsets, c.nb.size());
        auto badId = [&](uint32_t id) { return id >= c.rest.size(); };
        if (std::any_of(c.ids.begin(), c.ids.end(), badId) || std::any_of(c.nb.begin(), c.nb.end(), badId))
            c = {};
        return d;
    }
};

/** A pin's place on the surface when it pins to the nearest point. */
struct PinData final : Data
{
    deform::SurfacePoint point;
    const char* kind() const override { return "surfacePoint"; }
    size_t bytes() const override { return sizeof(deform::SurfacePoint); }
    json toJson() const override
    {
        const auto& p = point;
        return {{"kind", kind()},
                {"points", {p.points[0], p.points[1], p.points[2]}},
                {"bary", {p.bary.x, p.bary.y, p.bary.z}},
                {"uv", {p.uv[0].x, p.uv[0].y, p.uv[1].x, p.uv[1].y, p.uv[2].x, p.uv[2].y}},
                {"hasUv", p.hasUv}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto d = std::make_shared<PinData>();
        std::vector<uint32_t> pts;
        std::vector<double> bary, uv;
        readArray(j, "points", pts);
        readArray(j, "bary", bary);
        readArray(j, "uv", uv);
        if (pts.size() == 3 && bary.size() == 3)
        {
            std::copy(pts.begin(), pts.end(), d->point.points);
            d->point.bary = {bary[0], bary[1], bary[2]};
        }
        if (uv.size() == 6)
            for (int k = 0; k < 3; ++k)
                d->point.uv[k] = {uv[size_t(k) * 2], uv[size_t(k) * 2 + 1]};
        d->point.hasUv = j.value("hasUv", false) && uv.size() == 6;
        return d;
    }
};

// ------------------------------------------------------------------------------------------------ attr helpers

AttrDef output(std::string name, PlugType type, PlugValue def, std::string doc)
{
    AttrDef a{.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrOutput, .doc = std::move(doc)};
    if (type == PlugType::Vec3)
        for (int k = 0; k < 3; ++k)
            a.childNames[k] = a.name + char('X' + k);
    return a;
}

std::vector<glm::dvec3> toWorld(const glm::dmat4& m, std::span<const glm::dvec3> pts)
{
    std::vector<glm::dvec3> out(pts.size());
    parallelPoints(pts.size(), [&](size_t i) { out[i] = xform(m, pts[i]); });
    return out;
}

void toLocal(const glm::dmat4& inv, std::span<const glm::dvec3> world, std::vector<glm::dvec3>& out)
{
    parallelPoints(out.size(), [&](size_t i) { out[i] = xform(inv, world[i]); });
}

geo::Mesh worldMesh(const geo::Mesh& m, const glm::dmat4& w)
{
    geo::Mesh out = m;
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        out.setPosition(p, xform(w, m.position(p)));
    return out;
}

/** A SurfaceTarget rebuilt only when its mesh or placement changes. */
struct TargetCache
{
    MeshPtr mesh;
    glm::dmat4 matrix{0.0};
    std::shared_ptr<deform::SurfaceTarget> target;

    const deform::SurfaceTarget& get(const MeshPtr& m, const glm::dmat4& w)
    {
        if (!target || m != mesh || w != matrix)
        {
            target = std::make_shared<deform::SurfaceTarget>(worldMesh(*m, w));
            mesh = m;
            matrix = w;
        }
        return *target;
    }
};

/** Point adjacency rebuilt when the topology changes. */
struct AdjacencyCache
{
    geo::Index edges = ~geo::Index(0), points = ~geo::Index(0);
    deform::Adjacency adj;

    const deform::Adjacency& get(const geo::Mesh& m)
    {
        if (m.numEdges() != edges || m.pointCapacity() != points)
        {
            adj = deform::pointAdjacency(m);
            edges = m.numEdges();
            points = m.pointCapacity();
        }
        return adj;
    }
};

// ------------------------------------------------------------------------------------------------ nodes

class ShrinkwrapNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const MeshPtr& target = as<MeshPtr>(attr("target"));
        if (!target)
            throw std::runtime_error("no target connected");
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        deform::ShrinkwrapParams o;
        o.mode = deform::ShrinkwrapMode(std::clamp<int64_t>(integer("mode"), 0, 3));
        o.place = deform::ShrinkwrapPlace(std::clamp<int64_t>(integer("placement"), 0, 4));
        o.offset = num("offset");
        if (int64_t axis = integer("projectAxis"); axis >= 1 && axis <= 3)
            o.axis = glm::dmat3(geom)[int(axis - 1)];
        o.positive = num("positive") != 0.0;
        o.negative = num("negative") != 0.0;
        o.cull = deform::CullFaces(std::clamp<int64_t>(integer("cullFaces"), 0, 2));
        o.limit = num("projectLimit");
        o.smoothIterations = int(std::clamp<int64_t>(integer("smoothIterations"), 0, 1000));
        o.smoothStep = num("smoothStep");
        o.keepDetail = num("keepDetail") != 0.0;
        const deform::SurfaceTarget& t = mTarget.get(target, matrix("targetMatrix"));
        std::vector<glm::dvec3> moved = deform::shrinkwrap(in, mAdj.get(in), toWorld(geom, points), t, o);
        toLocal(geomInv, moved, points);
    }

private:
    mutable TargetCache mTarget;
    mutable AdjacencyCache mAdj;
};

class SurfaceDeformNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        DataPtr data = as<DataPtr>(attr("binding"));
        const MeshPtr& driver = as<MeshPtr>(attr("driver"));
        if (!data)
            throw Unbound();
        if (!driver)
            throw std::runtime_error("no driver connected");
        const glm::dmat4 dm = matrix("driverMatrix"), geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dvec3> drv = toWorld(dm, driver->positions());
        std::vector<glm::dvec3> world = toWorld(geom, points);
        const int64_t rebind = integer("rebind");
        const std::string& uvSet = as<std::string>(attr("uvSet"));
        std::vector<glm::dvec3> moved;
        if (const auto* s = dynamic_cast<const SurfaceData*>(data.get()))
        {
            const deform::SurfaceBinding* b = &s->bind;
            if (b->driverPoints != driver->pointCapacity())
                b = &rebound(*driver, dm, data, world, rebind, uvSet);
            moved = deform::surfaceApply(*b, drv, world);
        }
        else if (const auto* c = dynamic_cast<const CaptureData*>(data.get()))
        {
            const deform::PointCapture* cap = &c->capture;
            if (cap->rest.size() != drv.size())
            {
                if (rebind != 1)
                    throw std::runtime_error(rebind == 2 ? "rebinding by uv needs polygons mode" : "the driver's points changed since binding: run rebindDeformer");
                cap = &capturedFromRest(*driver, dm, data, world);
            }
            moved = deform::pointDeform(*cap, drv, world);
        }
        else
            throw Unbound();
        toLocal(geomInv, moved, points);
    }

private:
    /** The binding carried onto a driver whose topology changed, by rest attribute or uv; kept until the topology changes again. */
    const deform::SurfaceBinding& rebound(const geo::Mesh& driver, const glm::dmat4& dm, const DataPtr& key, std::span<const glm::dvec3> world,
                                          int64_t rebind, const std::string& uvSet) const
    {
        if (mKey == key && mKeyPoints == driver.pointCapacity() && mKeyFaces == driver.numFaces() && mKeyMode == rebind)
            return mRebound;
        if (rebind == 1)
        {
            std::span<const glm::vec3> rest = driver.pointAttrs().get<glm::vec3>("rest");
            if (rest.size() != driver.pointCapacity())
                throw std::runtime_error("the driver's topology changed and it has no rest point attribute: run rebindDeformer");
            geo::Mesh m = driver;
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                m.setPosition(p, xform(dm, glm::dvec3(rest[p])));
            mRebound = deform::surfaceBind(m, world, int(integer("maxFaces")), num("falloff"), uvSet);
        }
        else if (rebind == 2)
        {
            const auto& s = static_cast<const SurfaceData&>(*key);
            if (!s.bind.hasUv)
                throw std::runtime_error("the driver had no uvs when bound, so it can't be rebound by uv: run rebindDeformer");
            mRebound = deform::surfaceRebindByUv(s.bind, worldMesh(driver, dm), uvSet);
        }
        else
            throw std::runtime_error("the driver's points changed since binding: run rebindDeformer");
        mKey = key;
        mKeyPoints = driver.pointCapacity();
        mKeyFaces = driver.numFaces();
        mKeyMode = rebind;
        return mRebound;
    }

    const deform::PointCapture& capturedFromRest(const geo::Mesh& driver, const glm::dmat4& dm, const DataPtr& key, std::span<const glm::dvec3> world) const
    {
        if (mKey == key && mKeyPoints == driver.pointCapacity() && mKeyFaces == driver.numFaces() && mKeyMode == 1)
            return mRecaptured;
        std::span<const glm::vec3> rest = driver.pointAttrs().get<glm::vec3>("rest");
        if (rest.size() != driver.pointCapacity())
            throw std::runtime_error("the driver's topology changed and it has no rest point attribute: run rebindDeformer");
        std::vector<glm::dvec3> r(rest.size());
        for (size_t p = 0; p < r.size(); ++p)
            r[p] = xform(dm, glm::dvec3(rest[p]));
        mRecaptured = deform::pointCapture(r, world, int(integer("maxPoints")), num("radius"), int(integer("neighbours")));
        mKey = key;
        mKeyPoints = driver.pointCapacity();
        mKeyFaces = driver.numFaces();
        mKeyMode = 1;
        return mRecaptured;
    }

    mutable DataPtr mKey;
    mutable geo::Index mKeyPoints = 0, mKeyFaces = 0;
    mutable int64_t mKeyMode = -1;
    mutable deform::SurfaceBinding mRebound;
    mutable deform::PointCapture mRecaptured;
};

/** A wrap binding as deform.slang's wrap kernel reads it: 16 words per influence, triangles indexing every driver's points in turn. */
struct WrapGpu
{
    std::vector<uint32_t> offsets, influences;
    size_t bytes() const { return (offsets.size() + influences.size()) * 4; }
};

class WrapNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const ProximityData& b = binding();
        const std::vector<std::vector<glm::dvec3>> drv = drivers(b);
        const std::vector<std::span<const glm::dvec3>> spans(drv.begin(), drv.end());
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        auto mode = deform::WrapMode(std::clamp<int64_t>(integer("wrapMode"), 0, 2));
        std::vector<glm::dvec3> moved = deform::proximityApply(b.bind, spans, toWorld(geom, points), mode, num("scaleCompensation"));
        toLocal(geomInv, moved, points);
    }

    bool gpuProgram(const geo::Mesh& in, deformEval::Program& out) const override
    {
        const ProximityData& b = binding();
        if (b.bind.offsets.size() != size_t(in.pointCapacity()) + 1)
            return false;
        const std::vector<std::vector<glm::dvec3>> drv = drivers(b);
        DataPtr bindData = as<DataPtr>(attr("binding"));
        const uint64_t key = deformEval::identityKey(bindData);
        std::shared_ptr<const WrapGpu> packed = deformEval::cachedBind<WrapGpu>(key, [&] { return pack(b); });
        const glm::dmat4 geom = matrix("geomMatrix");
        out.kernel = deformEval::Kernel::Wrap;
        out.push(geom);
        out.push(safeInverse(geom, "the mesh"));
        out.push(uint32_t(std::clamp<int64_t>(integer("wrapMode"), 0, 2)));
        out.push(float(num("scaleCompensation")));
        uint32_t total = 0;
        for (const auto& d : drv)
            total += uint32_t(d.size());
        out.push(total);
        for (const auto& d : drv)
            out.push(std::span<const glm::dvec3>(d));
        out.blobs[0] = deformEval::blob(key ^ 1, packed, packed->offsets);
        out.blobs[1] = deformEval::blob(key ^ 2, packed, packed->influences);
        return true;
    }

private:
    const ProximityData& binding() const
    {
        const auto* b = dynamic_cast<const ProximityData*>(as<DataPtr>(attr("binding")).get());
        if (!b)
            throw Unbound();
        if (b->bind.driverPoints.empty())
            throw Unbound();
        return *b;
    }

    /** Every driver's points in world space, by slot. */
    std::vector<std::vector<glm::dvec3>> drivers(const ProximityData& b) const
    {
        const size_t slots = b.bind.driverPoints.size();
        std::vector<std::vector<glm::dvec3>> drv(slots);
        for (size_t s = 0; s < slots; ++s)
        {
            const MeshPtr& m = elementOr<MeshPtr>(attr("drivers.mesh"), int(s));
            if (!m)
                throw std::runtime_error(s == 0 ? std::string("no driver connected") : std::format("driver {} is not connected", s));
            if (m->pointCapacity() != b.bind.driverPoints[s])
                throw std::runtime_error("a driver's points changed since binding: run rebindDeformer");
            drv[s] = toWorld(elementOr<glm::dmat4>(attr("drivers.matrix"), int(s)), m->positions());
        }
        return drv;
    }

    static std::shared_ptr<const WrapGpu> pack(const ProximityData& b)
    {
        LILA_ZONE();

        auto g = std::make_shared<WrapGpu>();
        g->offsets = b.bind.offsets;
        std::vector<uint32_t> base(b.bind.driverPoints.size() + 1, 0);
        for (size_t s = 0; s < b.bind.driverPoints.size(); ++s)
            base[s + 1] = base[s] + b.bind.driverPoints[s];
        g->influences.resize(b.bind.influences.size() * 16, 0);
        tbb::parallel_for(size_t(0), b.bind.influences.size(), [&](size_t k) {
            const deform::ProximityBinding::Influence& inf = b.bind.influences[k];
            uint32_t* w = &g->influences[k * 16];
            const bool valid = inf.driver < b.bind.driverPoints.size() && inf.tri[0] < b.bind.driverPoints[inf.driver] &&
                               inf.tri[1] < b.bind.driverPoints[inf.driver] && inf.tri[2] < b.bind.driverPoints[inf.driver];
            auto f = [](double v) { return std::bit_cast<uint32_t>(float(v)); };
            for (int c = 0; c < 3; ++c)
            {
                w[c] = valid ? base[inf.driver] + inf.tri[c] : 0;
                w[4 + c] = f(inf.bary[c]);
                w[7 + c] = f(inf.offset[c]);
                w[12 + c] = f(inf.anchor[c]);
            }
            w[3] = valid ? 1 : 0;
            w[10] = f(inf.weight);
            w[11] = f(inf.restArea);
        });
        return g;
    }
};

class CollisionNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<const deform::SurfaceTarget*> meshes;
        std::vector<deform::Capsule> capsules;
        for (const auto& [s, v] : elements(attr("colliders.mesh")))
            if (const MeshPtr& m = std::get<MeshPtr>(v))
                meshes.push_back(&mTargets[s].get(m, elementOr<glm::dmat4>(attr("colliders.matrix"), s)));
        for (const auto& [s, v] : elements(attr("capsules.matrix")))
        {
            const glm::dmat4& cm = std::get<glm::dmat4>(v);
            double half = elementOr<double>(attr("capsules.length"), s) * 0.5;
            double scale = (glm::length(glm::dvec3(cm[0])) + glm::length(glm::dvec3(cm[2]))) * 0.5;
            capsules.push_back({xform(cm, {0, -half, 0}), xform(cm, {0, half, 0}), elementOr<double>(attr("capsules.radius"), s) * scale});
        }
        if (meshes.empty() && capsules.empty())
            throw std::runtime_error("no colliders connected");
        deform::CollisionParams o;
        o.offset = num("offset");
        o.falloff = num("falloff");
        o.volumeKeep = num("volumeKeep");
        o.bulgeRadius = num("bulgeRadius");
        o.keepInside = integer("side") == 1;
        std::vector<glm::dvec3> moved = deform::collide(in, toWorld(geom, points), meshes, capsules, o);
        toLocal(geomInv, moved, points);
    }

private:
    mutable std::map<int, TargetCache> mTargets; // by colliders element
};

const RestShape& restOf(const DeformerNode& d, size_t points)
{
    const auto* rest = dynamic_cast<const RestShape*>(d.as<DataPtr>(d.attr("rest")).get());
    if (!rest)
        throw Unbound();
    if (rest->points.size() != points)
        throw std::runtime_error("the mesh's points changed since binding: run rebindDeformer");
    return *rest;
}

class RigidifyNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const RestShape& rest = restOf(*this, points.size());
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        const deform::Adjacency& adj = mAdj.get(in);
        std::vector<int32_t> regions;
        switch (integer("regionMode"))
        {
        case 0:
            regions = deform::connectedRegions(adj);
            break;
        case 1:
        {
            const auto* r = dynamic_cast<const RegionIds*>(as<DataPtr>(attr("regions")).get());
            if (!r || r->ids.empty())
                throw std::runtime_error("no regions stored: run rigidify with regions=selection or sets");
            regions = r->ids;
            regions.resize(points.size(), -1);
            break;
        }
        default:
        {
            std::vector<float> w = deformWeights::effective(*this, in);
            std::vector<uint8_t> mask(points.size(), 0);
            for (size_t i = 0; i < mask.size() && i < w.size(); ++i)
                mask[i] = w[i] > 0.0f;
            regions = deform::connectedRegions(adj, mask);
            break;
        }
        }
        std::vector<glm::dvec3> moved =
            deform::rigidify(adj, toWorld(geom, rest.points), toWorld(geom, points), regions, int(std::clamp<int64_t>(integer("blendRings"), 0, 100)));
        toLocal(geomInv, moved, points);
    }

private:
    mutable AdjacencyCache mAdj;
};

class SkinSlideNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        DataPtr restData = as<DataPtr>(attr("rest"));
        const RestShape& rest = restOf(*this, points.size());
        const glm::dmat4 geom = matrix("geomMatrix");
        const glm::dmat4 geomInv = safeInverse(geom, "the mesh");
        std::vector<glm::dvec3> restWorld = toWorld(geom, rest.points);
        if (mRestKey != restData || mGeom != geom || mFaces != in.numFaces())
        {
            geo::Mesh m = in;
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                m.setPosition(p, restWorld[p]);
            mSurface = std::make_shared<deform::SurfaceTarget>(m);
            mRestKey = restData;
            mGeom = geom;
            mFaces = in.numFaces();
        }
        deform::SlideParams o;
        o.relaxIterations = int(std::clamp<int64_t>(integer("relaxIterations"), 0, 1000));
        o.relaxStep = num("relaxStep");
        o.normalAmount = num("normalAmount");
        std::vector<glm::dvec3> moved = deform::skinSlide(mAdj.get(in), restWorld, *mSurface, toWorld(geom, points), o);
        toLocal(geomInv, moved, points);
    }

private:
    mutable DataPtr mRestKey;
    mutable glm::dmat4 mGeom{0.0};
    mutable geo::Index mFaces = 0;
    mutable std::shared_ptr<deform::SurfaceTarget> mSurface;
    mutable AdjacencyCache mAdj;
};

/** Rides a mesh after its stack: outputs a world frame at a uv or a bound surface point, which drives a pin locator. */
class SurfacePinNode : public Node
{
public:
    enum Attr
    {
        kInMesh,
        kMeshMatrix,
        kMode,
        kUvSet,
        kU,
        kV,
        kBinding,
        kOutMatrix,
        kOutTranslate,
        kOutRotate,
    };

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const MeshPtr& mesh = as<MeshPtr>(kInMesh);
        glm::dmat4 frame = as<glm::dmat4>(kOutMatrix);
        if (mesh)
        {
            deform::SurfacePoint sp;
            bool found = false;
            if (as<int64_t>(kMode) == 0)
            {
                glm::dvec2 uv(as<double>(kU), as<double>(kV));
                const std::string& set = as<std::string>(kUvSet);
                if (mCacheMesh != mesh->pointCapacity() || mCacheFaces != mesh->numFaces() || mCacheUv != uv || mCacheSet != set)
                {
                    mCacheFound = deform::locateUv(*mesh, set, uv, mCache);
                    mCacheMesh = mesh->pointCapacity();
                    mCacheFaces = mesh->numFaces();
                    mCacheUv = uv;
                    mCacheSet = set;
                }
                sp = mCache;
                found = mCacheFound;
            }
            else if (const auto* b = dynamic_cast<const PinData*>(as<DataPtr>(kBinding).get()))
            {
                sp = b->point;
                found = true;
            }
            if (found)
            {
                std::vector<glm::dvec3> world = toWorld(as<glm::dmat4>(kMeshMatrix), mesh->positions());
                frame = deform::surfaceFrame(world, sp);
            }
        }
        setOutput(kOutMatrix, frame);
        setOutput(kOutTranslate, glm::dvec3(frame[3]));
        setOutput(kOutRotate, matrixToEuler(glm::dmat3(frame), RotateOrder::XYZ));
    }

private:
    geo::Index mCacheMesh = ~geo::Index(0), mCacheFaces = 0;
    glm::dvec2 mCacheUv{-1e300};
    std::string mCacheSet;
    deform::SurfacePoint mCache;
    bool mCacheFound = false;
};

// ------------------------------------------------------------------------------------------------ command helpers

DeformerNode& deformerOfType(const std::string& name, const char* type, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!isDeformer(n) || n.type().name != type)
        throw CommandError(std::string(cmd) + ": " + name + " is not a " + type + " deformer");
    return static_cast<DeformerNode&>(n);
}

/** Sets an enum attr from a flag's name, throwing with the choices on a typo. */
void setEnum(Graph& graph, DeformerNode& d, const char* attr, const std::string& value, const char* cmd)
{
    const AttrDef& def = d.type().attrs[size_t(d.attr(attr))];
    for (size_t k = 0; k < def.enumNames.size(); ++k)
        if (def.enumNames[k] == value)
            return setPlug(graph, d.id(), attr, int64_t(k));
    std::string all;
    for (const std::string& s : def.enumNames)
        all += (all.empty() ? "" : ", ") + s;
    throw CommandError(std::format("{}: {} must be one of {}", cmd, attr, all));
}

/** The deformer's input points in world space, evaluated now. */
std::vector<glm::dvec3> inputWorld(Graph& graph, DeformerNode& d, MeshPtr* meshOut = nullptr)
{
    graph.evaluate();
    MeshPtr in = std::get<MeshPtr>(graph.value({d.id(), DeformerNode::kInMesh, -1}));
    if (!in)
        throw CommandError(d.name() + ": the mesh has no geometry");
    if (meshOut)
        *meshOut = in;
    return toWorld(d.as<glm::dmat4>(d.attr("geomMatrix")), in->positions());
}

/** A connected mesh input and its world matrix, as a world-space mesh. */
std::optional<geo::Mesh> connectedWorld(Graph& graph, DeformerNode& d, const std::string& meshAttr, const std::string& matrixAttr)
{
    graph.evaluate();
    MeshPtr m = std::get<MeshPtr>(graph.value({d.id(), d.attr(meshAttr.c_str()), -1}));
    if (!m)
        return std::nullopt;
    return worldMesh(*m, std::get<glm::dmat4>(graph.value({d.id(), d.attr(matrixAttr.c_str()), -1})));
}

/** The rest shape for a deformer bound now: the bind shape when it sits above a skinCluster, else its input. */
void captureRest(Graph& graph, DeformerNode& d)
{
    graph.evaluate();
    MeshPtr rest;
    NodeId mesh = modifiers::meshOf(graph, d.id());
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
    auto data = std::make_shared<RestShape>();
    data->points.assign(rest->positions().begin(), rest->positions().end());
    graph.setValue({d.id(), d.attr("rest"), -1}, DataPtr(std::move(data)));
}

void bindSurface(Graph& graph, DeformerNode& d)
{
    std::optional<geo::Mesh> driver = connectedWorld(graph, d, "driver", "driverMatrix");
    if (!driver)
        throw CommandError(d.name() + ": no driver mesh connected");
    std::vector<glm::dvec3> targets = inputWorld(graph, d);
    DataPtr data;
    if (d.integer("mode") == 1)
    {
        auto c = std::make_shared<CaptureData>();
        c->capture = deform::pointCapture(driver->positions(), targets, int(d.integer("maxPoints")), d.num("radius"), int(d.integer("neighbours")));
        data = std::move(c);
    }
    else
    {
        if (driver->numFaces() == 0)
            throw CommandError(d.name() + ": the driver has no faces; use mode points");
        auto s = std::make_shared<SurfaceData>();
        s->bind = deform::surfaceBind(*driver, targets, int(d.integer("maxFaces")), d.num("falloff"), d.as<std::string>(d.attr("uvSet")));
        data = std::move(s);
    }
    graph.setValue({d.id(), d.attr("binding"), -1}, std::move(data));
}

/** Connected wrap drivers, from element 0; binding stops at the first empty one. */
int wrapSlotsUsed(Graph& graph, const DeformerNode& d)
{
    int n = 0;
    while (graph.source({d.id(), d.attr("drivers.mesh"), -1, n}))
        ++n;
    return n;
}

/** Connects src's srcAttr into element `element` of dst's array dstAttr, replacing what was there. */
void connectElement(Graph& graph, NodeId src, const std::string& srcAttr, NodeId dst, const std::string& dstAttr, int element)
{
    guarded([&] {
        graph.connect({src, graph.node(src)->type().attrIndex(srcAttr), -1}, {dst, graph.node(dst)->type().attrIndex(dstAttr), -1, element}, true);
        return 0;
    });
}

void bindWrapImpl(Graph& graph, DeformerNode& d)
{
    int slots = wrapSlotsUsed(graph, d);
    if (slots == 0)
        throw CommandError(d.name() + ": no driver mesh connected");
    std::vector<geo::Mesh> worlds;
    for (int s = 0; s < slots; ++s)
    {
        graph.evaluate();
        const MeshPtr& m = d.elementOr<MeshPtr>(d.attr("drivers.mesh"), s);
        worlds.push_back(m ? worldMesh(*m, d.elementOr<glm::dmat4>(d.attr("drivers.matrix"), s)) : geo::Mesh());
    }
    std::vector<const geo::Mesh*> ptrs;
    for (const geo::Mesh& m : worlds)
        ptrs.push_back(&m);
    MeshPtr in;
    std::vector<glm::dvec3> targets = inputWorld(graph, d, &in);
    deform::ProximityParams o;
    o.maxDrivers = int(d.integer("maxDrivers"));
    o.falloff = d.num("falloff");
    o.smoothIterations = int(std::clamp<int64_t>(d.integer("smoothInfluences"), 0, 100));
    o.smoothAmount = d.num("smoothAmount");
    deform::Adjacency adj;
    if (o.smoothIterations > 0)
        adj = deform::pointAdjacency(*in);
    auto data = std::make_shared<ProximityData>();
    data->bind = deform::proximityBind(ptrs, targets, o, o.smoothIterations > 0 ? &adj : nullptr);
    graph.setValue({d.id(), d.attr("binding"), -1}, DataPtr(std::move(data)));
}

/** Point ids of mesh's members in a component list, mapped from edges and faces through the mesh. */
std::vector<geo::Index> pointsOfMember(const geo::Mesh& m, ComponentMode kind, const components::Ids& ids)
{
    std::vector<geo::Index> v(ids.begin(), ids.end());
    switch (kind)
    {
    case ComponentMode::Vertex:
        return geo::uniqueSorted(v);
    case ComponentMode::Edge:
        return geo::pointsOfEdges(m, v);
    case ComponentMode::Face:
        return geo::pointsOfFaces(m, v);
    default:
        return {};
    }
}

/** Selects or adds a collider to a collision deformer's first free slot of the right kind. */
void addColliderTo(Graph& graph, DeformerNode& d, Node& collider, double radius, double length, const char* cmd)
{
    bool mesh = collider.type().isA("mesh");
    if (!mesh && !collider.type().isA("transform"))
        throw CommandError(std::string(cmd) + ": " + collider.name() + " is neither a mesh nor a transform");
    std::vector<int> used = graph.groupIndices(d.id(), mesh ? "colliders" : "capsules");
    const int s = used.empty() ? 0 : used.back() + 1;
    if (mesh)
    {
        connectElement(graph, collider.id(), "outMesh", d.id(), "colliders.mesh", s);
        connectElement(graph, collider.id(), "worldMatrix", d.id(), "colliders.matrix", s);
        return;
    }
    connectElement(graph, collider.id(), "worldMatrix", d.id(), "capsules.matrix", s);
    guarded([&] {
        graph.setValue({d.id(), d.attr("capsules.radius"), -1, s}, radius);
        graph.setValue({d.id(), d.attr("capsules.length"), -1, s}, length);
        return 0;
    });
}

/** The meshes before the last positional object, the last one apart (driver, target or collider). */
std::pair<std::vector<NodeId>, Node*> meshesAndLast(const std::vector<std::string>& objs, const char* cmd, const char* lastWhat)
{
    if (objs.size() < 2)
        throw CommandError(std::format("{}: give the meshes to deform, then the {} last", cmd, lastWhat));
    Node& last = nodeNamed(objs.back(), cmd);
    std::vector<NodeId> meshes;
    for (size_t i = 0; i + 1 < objs.size(); ++i)
    {
        NodeId m = meshNamed(objs[i], cmd).id();
        if (m == last.id())
            throw CommandError(std::format("{}: a mesh can't be its own {}", cmd, lastWhat));
        meshes.push_back(m);
    }
    return {meshes, &last};
}

DeformerNode& addOne(Graph& graph, const char* type, NodeId mesh, const Args& a, const char* label)
{
    std::vector<NodeId> made = addDeformer(graph, type, {mesh}, a.has("name") ? a.get<std::string>("name") : label);
    auto& d = static_cast<DeformerNode&>(*graph.node(made.front()));
    if (a.has("params"))
        applyParams(graph, d, a.flags["params"]);
    return d;
}

// ------------------------------------------------------------------------------------------------ pins

void registerPinType()
{
    NodeType t;
    t.name = "surfacePin";
    t.icon = "pin";
    t.doc = "A frame riding a mesh's final surface at a uv or a bound surface point; drives a pin locator that objects and joints are "
            "constrained to.";
    t.ownAttrs = {
        connected("inMesh", PlugType::Mesh, MeshPtr(), "The mesh's outMesh."),
        connected("meshMatrix", PlugType::Matrix, glm::dmat4(1.0), "The mesh's worldMatrix."),
        enumAttr("mode", 0, {"uv", "nearest"}, "uv finds the point by uv every evaluation (survives topology changes); nearest keeps the triangle bound when made."),
        stored("uvSet", PlugType::String, std::string("uv0"), "The uv set the pin reads."),
        keyable("u", PlugType::Float, 0.5, "U of the pin, uv mode."),
        keyable("v", PlugType::Float, 0.5, "V of the pin, uv mode."),
        hiddenStored("binding", PlugType::Data, DataPtr(), "The bound triangle and weights, nearest mode."),
        output("outMatrix", PlugType::Matrix, glm::dmat4(1.0), "The pin's world frame: z along the surface normal, x along u."),
        output("outTranslate", PlugType::Vec3, glm::dvec3(0.0), "Drives the pin locator's translate."),
        output("outRotate", PlugType::Vec3, glm::dvec3(0.0), "Drives the pin locator's rotate."),
    };
    t.create = [] { return std::make_unique<SurfacePinNode>(); };
    types::registerType(std::move(t));
}

/** A pin node (what surfacePin makes) or the locator one drives. */
bool isPinOrLocator(Graph& graph, const Node& n)
{
    if (n.type().name == "pin")
        return true;
    auto src = graph.source({n.id(), Transform::kTranslate, -1});
    return src && graph.node(src->node)->type().name == "pin";
}

/** A surfacePin node, given it or the locator it drives. */
Node& pinNamed(Graph& graph, const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (n.type().name == "surfacePin")
        return n;
    if (auto src = graph.source({n.id(), Transform::kTranslate, -1}))
        if (Node* p = graph.node(src->node); p && p->type().name == "surfacePin")
            return *p;
    throw CommandError(std::format("{}: {} is neither a surface pin nor its locator", cmd, name));
}

/** The influence with the most weight at a surface point, blending its triangle's weights by the point's barycentrics. */
std::optional<uint32_t> dominantAt(const WeightTable& t, const deform::SurfacePoint& sp)
{
    std::map<uint32_t, double> sum;
    for (int k = 0; k < 3; ++k)
        if (sp.points[k] < t.points.size())
            for (const auto& [inf, w] : t.points[sp.points[k]])
                sum[inf] += double(w) * sp.bary[k];
    std::optional<uint32_t> best;
    double most = 0.0;
    for (const auto& [inf, w] : sum)
        if (w > most)
        {
            most = w;
            best = inf;
        }
    return best;
}

json bakePins(const Args& a)
{
    const char* cmd = "bakeSurfacePin";
    Graph& graph = Graph::current();
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError("bakeSurfacePin: give the pins or their locators");
    if (a.has("name") && objs.size() > 1)
        throw CommandError("bakeSurfacePin: name takes one pin");
    graph.evaluate();
    json out = json::array();
    for (const std::string& name : objs)
    {
        if (isPinOrLocator(graph, nodeNamed(name, cmd)))
        {
            json call = {{"args", json::array({name})}};
            if (a.has("name"))
                call["name"] = a.flags["name"];
            for (const json& j : ops::execute("bakePin", call))
                out.push_back(j);
            continue;
        }
        Node& pin = pinNamed(graph, name, cmd);
        auto meshSrc = graph.source({pin.id(), SurfacePinNode::kInMesh, -1});
        MeshPtr m = pin.as<MeshPtr>(SurfacePinNode::kInMesh);
        if (!meshSrc || !m)
            throw CommandError("bakeSurfacePin: " + pin.name() + " has no mesh");
        const NodeId mesh = meshSrc->node;
        const NodeId skinId = skin::find(graph, mesh);
        if (skinId == kNoNode)
            throw CommandError("bakeSurfacePin: " + graph.node(mesh)->name() + " isn't skinned, so no joint carries the pin");
        deform::SurfacePoint sp;
        bool found = false;
        if (pin.as<int64_t>(SurfacePinNode::kMode) == 0)
            found = deform::locateUv(*m, pin.as<std::string>(SurfacePinNode::kUvSet), {pin.as<double>(SurfacePinNode::kU), pin.as<double>(SurfacePinNode::kV)}, sp);
        else if (const auto* b = dynamic_cast<const PinData*>(pin.as<DataPtr>(SurfacePinNode::kBinding).get()))
        {
            sp = b->point;
            found = true;
        }
        if (!found)
            throw CommandError("bakeSurfacePin: " + pin.name() + " isn't on the surface");
        std::optional<uint32_t> inf = dominantAt(skin::table(graph, skinId), sp);
        NodeId parent = kNoNode;
        for (const skin::Influence& i : skin::influences(graph, skinId))
            if (inf && i.index == *inf)
                parent = i.joint;
        if (parent == kNoNode)
            throw CommandError("bakeSurfacePin: no joint weights the surface under " + pin.name());

        std::string base = pin.name();
        for (const PlugRef& dst : graph.destinations({pin.id(), SurfacePinNode::kOutTranslate, -1}))
            base = graph.node(dst.node)->name();
        const std::string jointName = a.has("name") ? a.get<std::string>("name") : base + "_socket";
        Node* j = guarded([&] { return graph.createNode(*types::find("joint"), jointName, parent); });
        graph.setValue({j->id(), Joint::kRadius}, graph.node(parent)->value(Joint::kRadius));
        repeat::setWorldMatrix(graph, j->id(), pin.as<glm::dmat4>(SurfacePinNode::kOutMatrix));
        out.push_back(j->path());
    }
    graph.evaluate();
    return out;
}
} // namespace

// ------------------------------------------------------------------------------------------------ registration

bool rebindSurface(Graph& graph, DeformerNode& d)
{
    const std::string& t = d.type().name;
    if (t == "wrap")
        bindWrapImpl(graph, d);
    else if (t == "surfaceDeform")
        bindSurface(graph, d);
    else if (t == "rigidify" || t == "skinSlide")
        captureRest(graph, d);
    else if (t != "shrinkwrap" && t != "collision")
        return false;
    return true;
}

void registerSurfaceTypes()
{
    if (types::find("shrinkwrap"))
        return;
    data::registerKind("restShape", &RestShape::fromJson);
    data::registerKind("rigidRegions", &RegionIds::fromJson);
    data::registerKind("proximityBinding", &ProximityData::fromJson);
    data::registerKind("wrapBinding", &ProximityData::fromJson);
    data::registerKind("surfaceBinding", &SurfaceData::fromJson);
    data::registerKind("pointCapture", &CaptureData::fromJson);
    data::registerKind("surfacePoint", &PinData::fromJson);

    registerDeformer({"shrinkwrap", "magnet",
                      "Moves the mesh onto another mesh's surface: nearest point, projection along an axis or the normals, nearest "
                      "vertex or along the target's normals; on, inside, outside or above it by an offset. Smoothing with keep detail "
                      "wraps the broad shape and keeps the fine detail.",
                      {connected("target", PlugType::Mesh, MeshPtr(), "The target's outMesh."),
                       connected("targetMatrix", PlugType::Matrix, glm::dmat4(1.0), "The target's worldMatrix."), geomMatrixAttr(),
                       enumAttr("mode", 0, {"nearestSurface", "project", "nearestVertex", "targetNormal"},
                                "How each point finds its place on the target."),
                       enumAttr("placement", 0, {"onSurface", "inside", "outside", "outsideSurface", "aboveSurface"},
                                "Where points end up: on the surface, kept inside or outside it, on it pushed outward, or above it along its normal."),
                       keyable("offset", PlugType::Float, 0.0, "Distance kept from the surface, cm."),
                       enumAttr("projectAxis", 0, {"normal", "x", "y", "z"}, "Project mode: along each point's normal or one of the mesh's own axes."),
                       stored("positive", PlugType::Bool, true, "Project mode: cast along the axis."),
                       stored("negative", PlugType::Bool, false, "Project mode: cast against the axis too; the nearer hit wins."),
                       enumAttr("cullFaces", 0, {"off", "front", "back"}, "Project mode: ignore target faces facing the ray (front) or facing away (back)."),
                       keyable("projectLimit", PlugType::Float, 0.0, "Project mode: farthest a point moves, cm; 0 for no limit.", 0.0),
                       keyable("smoothIterations", PlugType::Int, int64_t(0), "Smooth the mesh this many passes before wrapping it.", 0.0, 1000.0),
                       keyable("smoothStep", PlugType::Float, 0.5, "How far each smoothing pass moves a point, 0 to 1.", 0.0, 1.0),
                       stored("keepDetail", PlugType::Bool, false, "Put the detail smoothing removed back on after wrapping, so it survives.")},
                      [] { return std::make_unique<ShrinkwrapNode>(); }});

    registerDeformer({"surfaceDeform", "topology-star-ring-3",
                      "Makes the mesh follow a driver of any topology: each point binds to nearby driver polygons (or, in points mode, "
                      "to driver points, like Houdini's Point Deform) and rides them. Use it to carry detail meshes on a simulated or "
                      "sculpted driver.",
                      {connected("driver", PlugType::Mesh, MeshPtr(), "The driver's outMesh."),
                       connected("driverMatrix", PlugType::Matrix, glm::dmat4(1.0), "The driver's worldMatrix."), geomMatrixAttr(),
                       enumAttr("mode", 0, {"polygons", "points"}, "Bind to driver polygons, or capture by driver points (works without faces). Used when binding."),
                       stored("maxFaces", PlugType::Int, int64_t(4), "Polygons mode: driver polygons each point follows, 1 to 16. Used when binding.", 1.0, 16.0),
                       stored("falloff", PlugType::Float, 4.0, "Polygons mode: how sharply nearer polygons win, as a distance exponent. Used when binding.", 0.0, 16.0),
                       stored("maxPoints", PlugType::Int, int64_t(8), "Points mode: driver points capturing each point. Used when binding.", 1.0, 64.0),
                       stored("radius", PlugType::Float, 0.0, "Points mode: capture radius, cm; 0 sizes it from the nearest points. Used when binding.", 0.0),
                       stored("neighbours", PlugType::Int, int64_t(8), "Points mode: neighbours each driver point fits its rotation from. Used when binding.", 2.0, 64.0),
                       enumAttr("rebind", 0, {"none", "restAttribute", "uv"},
                                "When the driver's topology changes: stop with an error, rebind against its 'rest' point attribute, or "
                                "carry the binding over by uv (polygons mode)."),
                       stored("uvSet", PlugType::String, std::string("uv0"), "The driver uv set rebinding by uv reads."),
                       hiddenStored("binding", PlugType::Data, DataPtr(), "Each point's driver polygons or capture points.")},
                      [] { return std::make_unique<SurfaceDeformNode>(); }});

    auto element = [](std::string name, PlugType type, PlugValue def, bool keyed, std::string doc, std::string alias) {
        AttrDef a = keyed ? keyable(std::move(name), type, std::move(def), std::move(doc)) : connected(std::move(name), type, std::move(def), std::move(doc));
        a.flags |= AttrArray;
        a.fileAlias = std::move(alias);
        return a;
    };
    // Drivers are a grouped array: drivers[n].mesh with drivers[n].matrix. Older scenes' driver, driver1 ... load into it.
    std::vector<AttrDef> wrap = {
        element("drivers.mesh", PlugType::Mesh, MeshPtr(), false, "Each driver's outMesh, from element 0.", "driver#"),
        element("drivers.matrix", PlugType::Matrix, glm::dmat4(1.0), false, "Each driver's worldMatrix.", "driverMatrix#"),
        geomMatrixAttr(),
        stored("maxDrivers", PlugType::Int, int64_t(4), "Driver triangles each point follows, 1 to 32, across all drivers. Used when binding.", 1.0, 32.0),
        stored("falloff", PlugType::Float, 2.0, "Triangles within this many times the nearest distance count; higher is smoother. Used when binding.", 1.0),
        stored("smoothInfluences", PlugType::Int, int64_t(0), "Passes spreading each point's influences to its neighbours, for smoother results from coarse drivers. Used when binding.", 0.0, 100.0),
        stored("smoothAmount", PlugType::Float, 0.5, "How much each spreading pass takes from the neighbours, 0 to 1. Used when binding.", 0.0, 1.0),
        enumAttr("wrapMode", 0, {"offset", "surface", "snap"},
                 "offset: points keep their offset from the driver, turning with it. surface: the driver's movement is added without "
                 "turning the offset. snap: points land on the driver."),
        keyable("scaleCompensation", PlugType::Float, 0.0, "0 to 1: offsets grow and shrink with the driver's local scale.", 0.0, 1.0),
        hiddenStored("binding", PlugType::Data, DataPtr(), "Each point's driver triangles and offsets."),
    };
    registerDeformer({"wrap", "layers-intersect",
                      "Makes the mesh follow one or more driver meshes (proximity wrap), each point riding its nearest driver triangles. "
                      "Use it to drive a detailed mesh from simple proxies.",
                      std::move(wrap), [] { return std::make_unique<WrapNode>(); }});

    std::vector<AttrDef> collision = {
        geomMatrixAttr(),
        keyable("offset", PlugType::Float, 0.0, "Distance points keep from each collider, cm.", 0.0),
        keyable("falloff", PlugType::Float, 0.0, "Points within this much more than the offset ease out softly instead of stopping hard, cm.", 0.0),
        keyable("volumeKeep", PlugType::Float, 0.0, "0 to 1: the pushed amount bulges out around the contact.", 0.0, 1.0),
        keyable("bulgeRadius", PlugType::Float, 0.0, "How far the bulge spreads, cm; 0 is four times the deepest push.", 0.0),
        enumAttr("side", 0, {"outside", "inside"}, "Push points out of the colliders, or hold them inside."),
    };
    // Grouped arrays, one element per collider; older scenes' collider0, capsuleMatrix0 ... load into them.
    collision.push_back(element("colliders.mesh", PlugType::Mesh, MeshPtr(), false, "A collider mesh's outMesh.", "collider#"));
    collision.push_back(element("colliders.matrix", PlugType::Matrix, glm::dmat4(1.0), false, "That collider's worldMatrix.", "colliderMatrix#"));
    collision.push_back(element("capsules.matrix", PlugType::Matrix, glm::dmat4(1.0), false, "A capsule's transform: its axis is local Y.", "capsuleMatrix#"));
    collision.push_back(element("capsules.radius", PlugType::Float, 5.0, true, "Capsule radius, in its transform's units.", "capsuleRadius#"));
    collision.push_back(element("capsules.length", PlugType::Float, 10.0, true, "Capsule length between the sphere centres, along its Y.", "capsuleLength#"));
    for (AttrDef& capsule : std::span(collision).last(2))
        capsule.minValue = 0.0;
    registerDeformer({"collision", "shield",
                      "Pushes the mesh out of collider meshes and capsules (or holds it inside them) with an offset, a soft falloff and "
                      "optional volume keeping. The fix for layered clothing passing through the body.",
                      std::move(collision), [] { return std::make_unique<CollisionNode>(); }, {"capsuleOn0", "capsuleOn1", "capsuleOn2", "capsuleOn3"}});

    registerDeformer({"rigidify", "box-multiple",
                      "Keeps regions rigid on a deforming mesh: each moves by the best rigid fit to its deformation, eased into the "
                      "surrounding surface. Buttons, badges and armour plates.",
                      {geomMatrixAttr(),
                       enumAttr("regionMode", 0, {"islands", "stored", "weights"},
                                "Regions: each connected piece of the mesh, the stored ones (from a selection or sets), or the pieces painted above 0 in the weight map."),
                       keyable("blendRings", PlugType::Int, int64_t(2), "Edge rings around each region easing from rigid into the deformation.", 0.0, 100.0),
                       hiddenStored("regions", PlugType::Data, DataPtr(), "Region per point, for stored mode."),
                       hiddenStored("rest", PlugType::Data, DataPtr(), "The rest shape captured at bind time.")},
                      [] { return std::make_unique<RigidifyNode>(); }});

    registerDeformer({"skinSlide", "arrows-move",
                      "Makes skin slide over the rest form instead of stretching: the deformation is moved along the rest surface and "
                      "relaxed on it.",
                      {geomMatrixAttr(), keyable("relaxIterations", PlugType::Int, int64_t(4), "Relax passes on the rest surface.", 0.0, 1000.0),
                       keyable("relaxStep", PlugType::Float, 0.5, "How far each relax pass moves a point, 0 to 1.", 0.0, 1.0),
                       keyable("normalAmount", PlugType::Float, 0.0, "0 to 1: how much of the movement off the rest surface is kept.", 0.0, 1.0),
                       hiddenStored("rest", PlugType::Data, DataPtr(), "The rest shape captured at bind time.")},
                      [] { return std::make_unique<SkinSlideNode>(); }});

    registerPinType();
}

void registerSurfaceCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "shrinkwrapDeformer",
        .doc = "Add a shrinkwrap deformer: the meshes move onto the target's surface, live. Give the meshes first and the target last. "
               "Returns the deformers.",
        .positionalDoc = "meshes..., target",
        .flags = {{"mode", "", T::String, "nearestSurface", "nearestSurface | project | nearestVertex | targetNormal",
                   {"nearestSurface", "project", "nearestVertex", "targetNormal"}},
                  {"placement", "", T::String, "onSurface", "onSurface | inside | outside | outsideSurface | aboveSurface",
                   {"onSurface", "inside", "outside", "outsideSurface", "aboveSurface"}},
                  {"offset", "", T::Float, 0.0, "Distance kept from the surface, cm."},
                  {"projectAxis", "", T::String, "normal", "Project mode: normal | x | y | z (the mesh's own axes).", {"normal", "x", "y", "z"}},
                  {"negative", "", T::Bool, false, "Project mode: also cast against the axis."},
                  {"smoothIterations", "", T::Int, 0, "Smooth the mesh this many passes before wrapping it.", {}, {}, 0.0, 1000.0},
                  {"keepDetail", "", T::Bool, false, "Put the smoothed-away detail back after wrapping."},
                  {"params", "", T::Any, nullptr, "{param: value} for any other attribute."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto [meshes, target] = meshesAndLast(a.objects(), "shrinkwrapDeformer", "target");
            if (!target->type().isA("mesh"))
                throw CommandError("shrinkwrapDeformer: " + target->name() + " is not a mesh");
            json out = json::array();
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "shrinkwrap", mesh, a, "Shrinkwrap");
                connectPlug(graph, target->id(), "outMesh", d.id(), "target");
                connectPlug(graph, target->id(), "worldMatrix", d.id(), "targetMatrix");
                setEnum(graph, d, "mode", a.get<std::string>("mode"), "shrinkwrapDeformer");
                setEnum(graph, d, "placement", a.get<std::string>("placement"), "shrinkwrapDeformer");
                setEnum(graph, d, "projectAxis", a.get<std::string>("projectAxis"), "shrinkwrapDeformer");
                setPlug(graph, d.id(), "offset", a.get<double>("offset"));
                setPlug(graph, d.id(), "negative", a.get<bool>("negative"));
                setPlug(graph, d.id(), "smoothIterations", int64_t(a.get<int>("smoothIterations")));
                setPlug(graph, d.id(), "keepDetail", a.get<bool>("keepDetail"));
                out.push_back(d.name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "surfaceDeform",
        .doc = "Make meshes follow a driver of any topology: each point binds to the driver's nearby polygons, or to its points in "
               "points mode. Give the meshes first and the driver last. Returns the deformers.",
        .positionalDoc = "meshes..., driver",
        .flags = {{"mode", "", T::String, "polygons", "polygons | points", {"polygons", "points"}},
                  {"maxFaces", "", T::Int, 4, "Polygons mode: driver polygons each point follows.", {}, {}, 1.0, 16.0},
                  {"falloff", "", T::Float, 4.0, "Polygons mode: distance exponent between a point's polygons.", {}, {}, 0.0, 16.0},
                  {"maxPoints", "", T::Int, 8, "Points mode: driver points capturing each point.", {}, {}, 1.0, 64.0},
                  {"radius", "", T::Float, 0.0, "Points mode: capture radius, cm; 0 sizes it from the nearest points.", {}, {}, 0.0},
                  {"rebind", "", T::String, "none", "When the driver's topology changes: none | restAttribute | uv", {"none", "restAttribute", "uv"}},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto [meshes, driver] = meshesAndLast(a.objects(), "surfaceDeform", "driver");
            if (!driver->type().isA("mesh"))
                throw CommandError("surfaceDeform: " + driver->name() + " is not a mesh");
            json out = json::array();
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "surfaceDeform", mesh, a, "SurfaceDeform");
                connectPlug(graph, driver->id(), "outMesh", d.id(), "driver");
                connectPlug(graph, driver->id(), "worldMatrix", d.id(), "driverMatrix");
                setEnum(graph, d, "mode", a.get<std::string>("mode"), "surfaceDeform");
                setEnum(graph, d, "rebind", a.get<std::string>("rebind"), "surfaceDeform");
                setPlug(graph, d.id(), "maxFaces", int64_t(a.get<int>("maxFaces")));
                setPlug(graph, d.id(), "falloff", a.get<double>("falloff"));
                setPlug(graph, d.id(), "maxPoints", int64_t(a.get<int>("maxPoints")));
                setPlug(graph, d.id(), "radius", a.get<double>("radius"));
                bindSurface(graph, d);
                out.push_back(d.name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "wrap",
        .doc = "Make meshes follow one or more driver meshes (proximity wrap): each point binds to its nearest driver triangles. Give "
               "the meshes to deform first and the driver last; more drivers with drivers=. Returns the deformers.",
        .positionalDoc = "meshes..., driver",
        .flags = {{"drivers", "", T::List, nullptr, "More driver meshes, up to 8 in all."},
                  {"maxDrivers", "", T::Int, 4, "Driver triangles each point follows, 1 to 32.", {}, {}, 1.0, 32.0},
                  {"falloff", "", T::Float, 2.0, "Triangles within this many times the nearest distance count; higher is smoother.", {}, {}, 1.0},
                  {"smoothInfluences", "", T::Int, 0, "Passes spreading influences to neighbouring points.", {}, {}, 0.0, 100.0},
                  {"wrapMode", "", T::String, "offset", "offset | surface | snap", {"offset", "surface", "snap"}},
                  {"scaleCompensation", "", T::Float, 0.0, "0 to 1: offsets follow the driver's local scale.", {}, {}, 0.0, 1.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto [meshes, driver] = meshesAndLast(a.objects(), "wrap", "driver");
            std::vector<Node*> drivers = {driver};
            if (a.has("drivers"))
                for (const json& j : a.flags["drivers"])
                    drivers.push_back(&nodeNamed(j.get<std::string>(), "wrap"));
            for (Node* dn : drivers)
                if (!dn->type().isA("mesh") || std::find(meshes.begin(), meshes.end(), dn->id()) != meshes.end())
                    throw CommandError("wrap: " + dn->name() + " can't drive: it is not a mesh, or it is one of the deformed meshes");
            json out = json::array();
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "wrap", mesh, a, "Wrap");
                for (size_t s = 0; s < drivers.size(); ++s)
                {
                    connectElement(graph, drivers[s]->id(), "outMesh", d.id(), "drivers.mesh", int(s));
                    connectElement(graph, drivers[s]->id(), "worldMatrix", d.id(), "drivers.matrix", int(s));
                }
                setPlug(graph, d.id(), "maxDrivers", int64_t(a.get<int>("maxDrivers")));
                setPlug(graph, d.id(), "falloff", a.get<double>("falloff"));
                setPlug(graph, d.id(), "smoothInfluences", int64_t(a.get<int>("smoothInfluences")));
                setPlug(graph, d.id(), "scaleCompensation", a.get<double>("scaleCompensation"));
                setEnum(graph, d, "wrapMode", a.get<std::string>("wrapMode"), "wrap");
                bindWrapImpl(graph, d);
                out.push_back(d.name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "addWrapDriver",
        .doc = "Add driver meshes to a wrap deformer and rebind it.",
        .positionalDoc = "wrap, drivers...",
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("addWrapDriver: give the wrap deformer, then the drivers");
            DeformerNode& d = deformerOfType(objs.front(), "wrap", "addWrapDriver");
            NodeId mesh = modifiers::meshOf(graph, d.id());
            for (size_t i = 1; i < objs.size(); ++i)
            {
                Node& drv = meshNamed(objs[i], "addWrapDriver");
                if (drv.id() == mesh)
                    throw CommandError("addWrapDriver: a mesh can't drive itself");
                int slot = wrapSlotsUsed(graph, d);
                connectElement(graph, drv.id(), "outMesh", d.id(), "drivers.mesh", slot);
                connectElement(graph, drv.id(), "worldMatrix", d.id(), "drivers.matrix", slot);
            }
            bindWrapImpl(graph, d);
            return d.name();
        },
    });

    ops::registerCommand({
        .name = "collision",
        .doc = "Add a collision deformer: the meshes are pushed out of the colliders (meshes, or transforms as capsules along their "
               "Y). Give the meshes first and a collider last; more with colliders=. Returns the deformers.",
        .positionalDoc = "meshes..., collider",
        .flags = {{"colliders", "", T::List, nullptr, "More colliders, up to 4 meshes and 4 capsules in all."},
                  {"offset", "", T::Float, 0.0, "Distance kept from each collider, cm.", {}, {}, 0.0},
                  {"falloff", "", T::Float, 0.0, "Soft easing width beyond the offset, cm.", {}, {}, 0.0},
                  {"volumeKeep", "", T::Float, 0.0, "0 to 1: bulge around the contact.", {}, {}, 0.0, 1.0},
                  {"side", "", T::String, "outside", "outside | inside", {"outside", "inside"}},
                  {"capsuleRadius", "", T::Float, 5.0, "Radius for transform colliders, in their units.", {}, {}, 0.0},
                  {"capsuleLength", "", T::Float, 10.0, "Length for transform colliders, along their Y.", {}, {}, 0.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto [meshes, first] = meshesAndLast(a.objects(), "collision", "collider");
            std::vector<Node*> colliders = {first};
            if (a.has("colliders"))
                for (const json& j : a.flags["colliders"])
                    colliders.push_back(&nodeNamed(j.get<std::string>(), "collision"));
            json out = json::array();
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "collision", mesh, a, "Collision");
                for (Node* c : colliders)
                {
                    if (c->id() == mesh)
                        throw CommandError("collision: a mesh can't collide with itself");
                    addColliderTo(graph, d, *c, a.get<double>("capsuleRadius"), a.get<double>("capsuleLength"), "collision");
                }
                setPlug(graph, d.id(), "offset", a.get<double>("offset"));
                setPlug(graph, d.id(), "falloff", a.get<double>("falloff"));
                setPlug(graph, d.id(), "volumeKeep", a.get<double>("volumeKeep"));
                setEnum(graph, d, "side", a.get<std::string>("side"), "collision");
                out.push_back(d.name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "addCollider",
        .doc = "Add colliders to a collision deformer: meshes, or transforms as capsules along their Y.",
        .positionalDoc = "collision, colliders...",
        .flags = {{"capsuleRadius", "", T::Float, 5.0, "Radius for transform colliders.", {}, {}, 0.0},
                  {"capsuleLength", "", T::Float, 10.0, "Length for transform colliders, along their Y.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("addCollider: give the collision deformer, then the colliders");
            DeformerNode& d = deformerOfType(objs.front(), "collision", "addCollider");
            for (size_t i = 1; i < objs.size(); ++i)
                addColliderTo(graph, d, nodeNamed(objs[i], "addCollider"), a.get<double>("capsuleRadius"), a.get<double>("capsuleLength"), "addCollider");
            return d.name();
        },
    });

    ops::registerCommand({
        .name = "rigidify",
        .doc = "Add a rigidify deformer: regions of the meshes keep their rest shape and move rigidly with the deformation under them. "
               "Regions are the meshes' islands, the selected components (each connected patch one region), sets, or the painted "
               "weights. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"regions", "", T::String, "islands", "islands | selection | sets | weights", {"islands", "selection", "sets", "weights"}},
                  {"sets", "", T::List, nullptr, "regions=sets: the sets, each one region."},
                  {"blendRings", "", T::Int, 2, "Edge rings easing from rigid into the deformation.", {}, {}, 0.0, 100.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::string mode = a.get<std::string>("regions");
            std::vector<objectSets::Member> members;
            if (mode == "selection")
                members = objectSets::fromSelection(graph);
            std::vector<NodeId> meshes;
            if (mode == "selection")
            {
                for (const objectSets::Member& m : members)
                    if (m.kind != ComponentMode::Object && std::find(meshes.begin(), meshes.end(), m.node) == meshes.end())
                        meshes.push_back(m.node);
                if (meshes.empty())
                    throw CommandError("rigidify: select the components of each rigid region");
            }
            else
                for (const std::string& name : a.objects())
                    meshes.push_back(meshNamed(name, "rigidify").id());
            if (meshes.empty())
                throw CommandError("rigidify: give or select at least one mesh");
            std::vector<NodeId> sets;
            if (mode == "sets")
            {
                if (!a.has("sets"))
                    throw CommandError("rigidify: regions=sets needs sets=");
                for (const json& j : a.flags["sets"])
                {
                    Node& s = nodeNamed(j.get<std::string>(), "rigidify");
                    if (!objectSets::isSet(s))
                        throw CommandError("rigidify: " + s.name() + " is not a set");
                    sets.push_back(s.id());
                }
            }
            json out = json::array();
            for (NodeId mesh : meshes)
            {
                DeformerNode& d = addOne(graph, "rigidify", mesh, a, "Rigidify");
                setPlug(graph, d.id(), "blendRings", int64_t(a.get<int>("blendRings")));
                setPlug(graph, d.id(), "regionMode", int64_t(mode == "islands" ? 0 : mode == "weights" ? 2 : 1));
                if (mode == "selection" || mode == "sets")
                {
                    MeshPtr in;
                    inputWorld(graph, d, &in);
                    auto regions = std::make_shared<RegionIds>();
                    regions->ids.assign(in->pointCapacity(), -1);
                    int32_t next = 0;
                    auto mark = [&](const std::vector<objectSets::Member>& list, bool perPatch) {
                        std::vector<uint8_t> mask(in->pointCapacity(), 0);
                        for (const objectSets::Member& m : list)
                            if (m.node == mesh)
                                for (geo::Index p : pointsOfMember(*in, m.kind, m.ids))
                                    if (p < mask.size())
                                        mask[p] = 1;
                        if (!perPatch)
                        {
                            bool any = false;
                            for (size_t p = 0; p < mask.size(); ++p)
                                if (mask[p])
                                    regions->ids[p] = next, any = true;
                            next += any ? 1 : 0;
                            return;
                        }
                        std::vector<int32_t> patch = deform::connectedRegions(deform::pointAdjacency(*in), mask);
                        int32_t most = -1;
                        for (size_t p = 0; p < patch.size(); ++p)
                            if (patch[p] >= 0)
                            {
                                regions->ids[p] = next + patch[p];
                                most = std::max(most, patch[p]);
                            }
                        next += most + 1;
                    };
                    if (mode == "selection")
                        mark(members, true);
                    else
                        for (NodeId s : sets)
                            mark(objectSets::members(graph, s), false);
                    if (next == 0)
                        throw CommandError("rigidify: no regions on " + graph.node(mesh)->name());
                    setPlug(graph, d.id(), "regions", DataPtr(std::move(regions)));
                }
                captureRest(graph, d);
                out.push_back(d.name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "skinSlide",
        .doc = "Add a skin slide deformer: the deformation below it slides along the rest surface and is relaxed there, so skin moves "
               "over the form instead of stretching. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"relaxIterations", "", T::Int, 4, "Relax passes on the rest surface.", {}, {}, 0.0, 1000.0},
                  {"normalAmount", "", T::Float, 0.0, "0 to 1: how much movement off the rest surface is kept.", {}, {}, 0.0, 1.0},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            json out = json::array();
            std::vector<std::string> names = a.objects();
            if (names.empty())
                throw CommandError("skinSlide: give or select at least one mesh");
            for (const std::string& name : names)
            {
                DeformerNode& d = addOne(graph, "skinSlide", meshNamed(name, "skinSlide").id(), a, "SkinSlide");
                setPlug(graph, d.id(), "relaxIterations", int64_t(a.get<int>("relaxIterations")));
                setPlug(graph, d.id(), "normalAmount", a.get<double>("normalAmount"));
                captureRest(graph, d);
                out.push_back(d.name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "surfacePin",
        .doc = "Pin objects or joints to a mesh's final surface (after its whole stack): each gets a pin locator riding the surface at a "
               "uv (uvPin) or its nearest point (proximityPin) and is parent-constrained to it, keeping its offset. Give the objects "
               "first and the mesh last; with no objects one pin is made. Returns the pin locators. Short for pin with source "
               "surface, whose other options it leaves at their defaults.",
        .positionalDoc = "objects..., mesh",
        .flags = {{"mode", "", T::String, "uv", "uv | nearest", {"uv", "nearest"}},
                  {"uv", "", T::List, nullptr, "uv mode: [u, v] of the pin. Default: the uv nearest each object."},
                  {"uvSet", "", T::String, "uv0", "The uv set read."},
                  {"position", "", T::Vec3, nullptr, "With no objects: where the pin goes, world space."},
                  {"name", "", T::String, nullptr, "Name for the pin locator."}},
        .run = [](const Args& a) -> json {
            json call = {{"args", a.positional}, {"source", "surface"}, {"surfaceMode", a.get<std::string>("mode")}, {"uvSet", a.get<std::string>("uvSet")}};
            for (const char* f : {"uv", "position", "name"})
                if (a.has(f))
                    call[f] = a.flags[f];
            return ops::execute("pin", call);
        },
    });

    ops::registerCommand({
        .name = "bakeSurfacePin",
        .doc = "Bake surface pins to joints for Unreal: each becomes a joint at the pin's current frame, parented to the joint "
               "with the most skin weight under the pin, so it exports as a bone that sockets and attachments use. Give the "
               "pins or their locators. Pins made by pin or surfacePin go through bakePin. Returns the joints.",
        .positionalDoc = "pins or pin locators",
        .flags = {{"name", "", T::String, nullptr, "Name for the joint (one pin). Default: <locator>_socket."}},
        .run = [](const Args& a) -> json { return bakePins(a); },
    });
}
} // namespace lila::scene::deformers
