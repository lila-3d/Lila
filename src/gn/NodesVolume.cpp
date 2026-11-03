#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "gn/Volume.h"

#include <tbb/parallel_for.h>

#include <algorithm>

namespace lila::gn::nodes
{
namespace volume_nodes
{
using VolumePtr = std::shared_ptr<const Volume>;

constexpr size_t kMaxPoints = size_t(50) << 20;

GeometryPtr withVolume(const GeometryPtr& g, Volume v)
{
    Geometry out = *g;
    out.volume = std::make_shared<const Volume>(std::move(v));
    return makeGeometry(std::move(out));
}

VolumePtr sdfOf(const ExecContext& c, std::string_view id)
{
    VolumePtr v = c.geometry(id)->volume;
    if (v && !v->isSdf())
        throw ExecError(c.node.name + ": needs a signed distance volume (Mesh to SDF, Points to SDF), not a density");
    return v;
}

SocketDecl voxelSizeIn() { return bounded(in("voxelSize", T::Float, 5.0, "Edge length of a voxel."), 0.01); }
SocketDecl bandWidthIn() { return bounded(in("bandWidth", T::Int, 3, "Voxels kept either side of the surface; further ones hold the band distance."), 1, 100); }

/** Polynomial smooth minimum: min(a, b) with the crease rounded over k. */
double smoothMin(double a, double b, double k)
{
    if (k <= 0.0)
        return std::min(a, b);
    double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

class SampleField final : public Field
{
public:
    SampleField(VolumePtr v, FieldPtr position, bool nearest) : Field(SocketType::Float), mVolume(std::move(v)), mPosition(std::move(position)), mNearest(nearest) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        std::vector<D3> pos = evaluateAll<D3>(mPosition, ctx);
        std::vector<double> out(pos.size());
        tbb::parallel_for(size_t(0), pos.size(), [&](size_t k) { out[k] = mNearest ? mVolume->sampleNearest(pos[k]) : mVolume->sample(pos[k]); });
        return Column::of(std::move(out));
    }

private:
    VolumePtr mVolume;
    FieldPtr mPosition;
    bool mNearest;
};

void volumeCube()
{
    registerDecl({
        .type = "VolumeCube",
        .label = "Volume Cube",
        .category = "Volume",
        .doc = "A density grid filling a box, with voxel centres at both corners. The density field reads Position as the voxel's centre and "
               "Index as its voxel.",
        .sockets = {.inputs = {fin("density", T::Float, 1.0), in("background", T::Float, 0.0, "Density outside the grid."),
                               in("min", T::Vector, vec(-50, -50, -50)), in("max", T::Vector, vec(50, 50, 50)),
                               bounded(in("resolutionX", T::Int, 32), 2, 1024), bounded(in("resolutionY", T::Int, 32), 2, 1024),
                               bounded(in("resolutionZ", T::Int, 32), 2, 1024)},
                    .outputs = {geoOut("volume")}},
        .exec =
            [](ExecContext& c) {
                glm::ivec3 res(int(std::clamp<int64_t>(c.i("resolutionX"), 2, 1024)), int(std::clamp<int64_t>(c.i("resolutionY"), 2, 1024)),
                               int(std::clamp<int64_t>(c.i("resolutionZ"), 2, 1024)));
                if (double(res.x) * res.y * res.z > double(size_t(64) << 20))
                    throw ExecError(c.node.name + ": the volume would have over 64 million voxels");
                D3 lo = c.v("min"), hi = c.v("max");
                Volume v;
                v.kind = Volume::Kind::Density;
                v.dims = res;
                v.background = float(c.f("background"));
                v.indexToWorld = glm::dmat4(1.0);
                for (int a = 0; a < 3; ++a)
                    v.indexToWorld[a][a] = std::max(hi[a] - lo[a], 1e-9) / double(res[a] - 1);
                v.indexToWorld[3] = glm::dvec4(lo, 1.0);

                geo::Mesh centres;
                centres.reserve(size_t(res.x) * res.y * res.z, 0, 0, 0);
                for (int k = 0; k < res.z; ++k)
                    for (int j = 0; j < res.y; ++j)
                        for (int i = 0; i < res.x; ++i)
                            centres.addPoint(v.worldOf({i, j, k}));
                std::vector<double> d = evaluateAll<double>(c.field("density"), FieldContext::forPoints(centres));
                v.values.resize(d.size());
                std::transform(d.begin(), d.end(), v.values.begin(), [](double x) { return float(x); });
                c.out("volume", volumeGeometry(std::move(v)));
            },
    });
}

void meshToSdfNode()
{
    registerDecl({
        .type = "MeshToSDF",
        .label = "Mesh to SDF",
        .category = "Volume",
        .doc = "Signed distance to a closed mesh: negative inside, positive outside, kept accurate within the band. Open meshes give "
               "streaks where rows pass through their holes.",
        .sockets = {.inputs = {geoIn("mesh"), voxelSizeIn(), bandWidthIn()}, .outputs = {geoOut("volume")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("mesh");
                std::shared_ptr<const geo::Mesh> m = g->instances ? realize(*g).mesh : g->mesh;
                if (!m || m->numFaces() == 0)
                {
                    c.out("volume", makeGeometry({}));
                    return;
                }
                c.out("volume", volumeGeometry(meshToSdf(*m, std::max(c.f("voxelSize"), 0.01), int(std::clamp<int64_t>(c.i("bandWidth"), 1, 100)))));
            },
    });
}

void pointsToSdfNode()
{
    registerDecl({
        .type = "PointsToSDF",
        .label = "Points to SDF",
        .category = "Volume",
        .doc = "Signed distance to a sphere around each point (the points component, or the mesh's vertices when there are none).",
        .sockets = {.inputs = {geoIn("points"), bounded(fin("radius", T::Float, 10.0), 0.0), voxelSizeIn(), bandWidthIn()}, .outputs = {geoOut("volume")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("points");
                std::shared_ptr<const geo::Mesh> m = g->points;
                FieldContext ctx;
                if (m)
                    ctx = FieldContext::forPoints(*m);
                else
                {
                    m = g->instances ? realize(*g).mesh : g->mesh;
                    if (m)
                        ctx = FieldContext::forMesh(*m, Domain::Point);
                }
                if (!m || m->numPoints() == 0)
                {
                    c.out("volume", makeGeometry({}));
                    return;
                }
                std::vector<D3> pts;
                pts.reserve(m->numPoints());
                for (Index p = 0; p < m->pointCapacity(); ++p)
                    if (m->pointValid(p))
                        pts.push_back(m->position(p));
                std::vector<double> radii = evaluateAll<double>(c.field("radius"), ctx);
                if (radii.size() != pts.size())
                    radii.resize(1, radii.empty() ? 0.0 : radii[0]);
                c.out("volume", volumeGeometry(pointsToSdf(pts, radii, std::max(c.f("voxelSize"), 0.01), int(std::clamp<int64_t>(c.i("bandWidth"), 1, 100)))));
            },
    });
}

void sdfBoolean()
{
    registerDecl({
        .type = "SDFBoolean",
        .label = "SDF Boolean",
        .category = "Volume",
        .doc = "Combines signed distance volumes. Union grows the grid to cover every input at a's voxel size; intersect and difference "
               "keep a's grid. Smooth rounds the seam over that distance.",
        .props = {prop("operation", {"union", "intersect", "difference"}, "union")},
        .sockets = {.inputs = {geoIn("a"), multiIn("b", "Volumes combined with a; difference subtracts each."), bounded(in("smooth", T::Float, 0.0), 0.0)},
                    .outputs = {geoOut("volume")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                VolumePtr a = sdfOf(c, "a");
                std::vector<VolumePtr> others;
                for (const GeometryPtr& g : c.geometries("b"))
                    if (g->volume)
                    {
                        if (!g->volume->isSdf())
                            throw ExecError(c.node.name + ": needs signed distance volumes, not densities");
                        others.push_back(g->volume);
                    }
                const std::string op = c.prop("operation");
                if (!a)
                {
                    if (op != "union" || others.empty())
                    {
                        c.out("volume", c.geometry("a"));
                        return;
                    }
                    a = others.front();
                    others.erase(others.begin());
                }
                const double k = std::max(c.f("smooth"), 0.0);
                Volume target = *a;
                if (op == "union")
                {
                    geo::Bounds b = a->bounds();
                    float background = a->background;
                    for (const VolumePtr& o : others)
                    {
                        b.extend(o->bounds());
                        background = std::max(background, o->background);
                    }
                    if (!others.empty())
                    {
                        target = Volume::grid(b, a->voxelSize(), Volume::Kind::Sdf, background);
                        target.name = a->name;
                        target.values = a->resampled(target);
                    }
                }
                for (const VolumePtr& o : others)
                {
                    std::vector<float> bv = o->resampled(target);
                    tbb::parallel_for(size_t(0), target.values.size(), [&](size_t n) {
                        double x = target.values[n], y = bv[n];
                        if (op == "union")
                            x = smoothMin(x, y, k);
                        else if (op == "intersect")
                            x = -smoothMin(-x, -y, k);
                        else
                            x = -smoothMin(-x, y, k);
                        target.values[n] = float(x);
                    });
                }
                c.out("volume", withVolume(c.geometry("a"), std::move(target)));
            },
    });
}

void offsetSdf()
{
    registerDecl({
        .type = "OffsetSDF",
        .label = "Offset SDF",
        .category = "Volume",
        .doc = "Moves a signed distance surface outward by distance (inward when negative). Accurate while the distance stays within the band.",
        .sockets = {.inputs = {geoIn("volume"), in("distance", T::Float, 1.0)}, .outputs = {geoOut("volume")}},
        .exec =
            [](ExecContext& c) {
                VolumePtr v = sdfOf(c, "volume");
                if (!v)
                {
                    c.out("volume", c.geometry("volume"));
                    return;
                }
                Volume out = *v;
                const float d = float(c.f("distance"));
                for (float& x : out.values)
                    x -= d;
                c.out("volume", withVolume(c.geometry("volume"), std::move(out)));
            },
    });
}

/** One separable pass along an axis, edges clamped. */
void blurAxis(Volume& v, int axis, const std::vector<double>& weights)
{
    const int r = int(weights.size() / 2);
    std::vector<float> src = v.values;
    tbb::parallel_for(0, v.dims.z, [&](int k) {
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
            {
                glm::ivec3 at(i, j, k);
                double sum = 0.0;
                for (int o = -r; o <= r; ++o)
                {
                    glm::ivec3 q = at;
                    q[axis] = std::clamp(q[axis] + o, 0, v.dims[axis] - 1);
                    sum += weights[size_t(o + r)] * src[v.index(q.x, q.y, q.z)];
                }
                v.values[v.index(i, j, k)] = float(sum);
            }
    });
}

void sdfSmooth()
{
    registerDecl({
        .type = "SDFSmooth",
        .label = "Smooth Volume",
        .category = "Volume",
        .doc = "Blurs a volume's values, rounding a signed distance surface or softening a density. Width is the filter's reach in voxels.",
        .props = {prop("filter", {"mean", "gaussian"}, "gaussian")},
        .sockets = {.inputs = {geoIn("volume"), bounded(in("iterations", T::Int, 1), 0, 100), bounded(in("width", T::Int, 1), 1, 32)},
                    .outputs = {geoOut("volume")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                VolumePtr v = c.geometry("volume")->volume;
                if (!v || v->empty())
                {
                    c.out("volume", c.geometry("volume"));
                    return;
                }
                const int width = int(std::clamp<int64_t>(c.i("width"), 1, 32));
                std::vector<double> w(size_t(2 * width + 1), 1.0);
                if (c.prop("filter") == "gaussian")
                {
                    const double sigma = std::max(width * 0.5, 0.5);
                    for (int o = -width; o <= width; ++o)
                        w[size_t(o + width)] = std::exp(-double(o * o) / (2.0 * sigma * sigma));
                }
                double total = 0.0;
                for (double x : w)
                    total += x;
                for (double& x : w)
                    x /= total;
                Volume out = *v;
                for (int64_t n = std::clamp<int64_t>(c.i("iterations"), 0, 100); n > 0; --n)
                    for (int axis = 0; axis < 3; ++axis)
                        blurAxis(out, axis, w);
                c.out("volume", withVolume(c.geometry("volume"), std::move(out)));
            },
    });
}

void sampleVolume()
{
    registerDecl({
        .type = "SampleVolume",
        .label = "Sample Volume",
        .category = "Volume",
        .doc = "The volume's value at each position; outside the grid it reads the background.",
        .props = {prop("interpolation", {"trilinear", "nearest"}, "trilinear")},
        .sockets = {.inputs = {geoIn("volume"), fin("position", T::Vector, nullptr, Implicit::Position)}, .outputs = {fout("value", T::Float)}},
        .exec =
            [](ExecContext& c) {
                VolumePtr v = c.geometry("volume")->volume;
                if (!v || v->empty())
                {
                    c.out("value", constantFloat(0.0));
                    return;
                }
                c.out("value", std::make_shared<SampleField>(v, c.field("position"), c.prop("interpolation") == "nearest"));
            },
    });
}

void volumeToMeshNode()
{
    registerDecl({
        .type = "VolumeToMesh",
        .label = "Volume to Mesh",
        .category = "Volume",
        .doc = "The volume's surface as quads, joined with any mesh the geometry already has. Density above threshold is solid; for a signed "
               "distance, distance below it, so 0 is the surface itself.",
        .sockets = {.inputs = {geoIn("volume"), in("threshold", T::Float, 0.1)}, .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("volume");
                if (!g->volume)
                {
                    c.out("mesh", g);
                    return;
                }
                Geometry rest = *g;
                rest.volume = nullptr;
                GeometryPtr parts[2] = {makeGeometry(std::move(rest)), meshGeometry(volumeToMesh(*g->volume, c.f("threshold")))};
                c.out("mesh", makeGeometry(join(parts)));
            },
    });
}

void distributeInVolume()
{
    registerDecl({
        .type = "DistributePointsInVolume",
        .label = "Distribute Points in Volume",
        .category = "Volume",
        .doc = "Points inside a volume. Random scatters density points per 100-unit cube, scaled by a density volume's value; a signed "
               "distance counts as 1 inside. Grid places a point every spacing where the density is above threshold, or the distance below 0.",
        .props = {prop("mode", {"random", "grid"}, "random")},
        .sockets = {.inputs = {geoIn("volume"), bounded(in("density", T::Float, 100.0), 0.0), in("seed", T::Int, 0),
                               in("spacing", T::Vector, vec(10, 10, 10)), in("threshold", T::Float, 0.1)},
                    .outputs = {geoOut("points")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                VolumePtr v = c.geometry("volume")->volume;
                geo::Mesh pts;
                if (!v || v->empty())
                {
                    c.out("points", pointGeometry(std::move(pts)));
                    return;
                }
                const bool sdf = v->isSdf();
                if (c.prop("mode") == "grid")
                {
                    D3 sp = glm::max(c.v("spacing"), D3(0.01));
                    geo::Bounds b = v->bounds();
                    glm::dvec3 n = glm::floor(b.size() / sp) + 1.0;
                    if (n.x * n.y * n.z > double(kMaxPoints))
                        throw ExecError(c.node.name + ": over 50 million grid points; raise the spacing");
                    const double threshold = c.f("threshold");
                    for (int k = 0; k < int(n.z); ++k)
                        for (int j = 0; j < int(n.y); ++j)
                            for (int i = 0; i < int(n.x); ++i)
                            {
                                D3 p = b.min + D3(i, j, k) * sp;
                                double s = v->sample(p);
                                if (sdf ? s < 0.0 : s > threshold)
                                    pts.addPoint(p);
                            }
                    c.out("points", pointGeometry(std::move(pts)));
                    return;
                }

                const double perVoxel = std::max(c.f("density"), 0.0) * std::abs(glm::determinant(glm::dmat3(v->indexToWorld))) / 1e6;
                const uint32_t seed = uint32_t(c.i("seed"));
                auto weight = [&](float x) { return sdf ? (x < 0.0f ? 1.0 : 0.0) : std::max(double(x), 0.0); };
                double expected = 0.0;
                for (float x : v->values)
                    expected += weight(x) * perVoxel;
                if (expected > double(kMaxPoints))
                    throw ExecError(c.node.name + ": over 50 million points; lower the density");
                std::vector<std::vector<D3>> slices(size_t(v->dims.z));
                tbb::parallel_for(0, v->dims.z, [&](int k) {
                    for (int j = 0; j < v->dims.y; ++j)
                        for (int i = 0; i < v->dims.x; ++i)
                        {
                            const uint32_t id = uint32_t(v->index(i, j, k));
                            double e = weight(v->values[id]) * perVoxel;
                            if (e <= 0.0)
                                continue;
                            int count = int(e) + (hashToUnit(hash32(id, seed, 0)) < e - std::floor(e) ? 1 : 0);
                            for (int m = 0; m < count; ++m)
                            {
                                D3 jitter(hashToUnit(hash32(id, seed, uint32_t(3 * m + 1))), hashToUnit(hash32(id, seed, uint32_t(3 * m + 2))),
                                          hashToUnit(hash32(id, seed, uint32_t(3 * m + 3))));
                                slices[size_t(k)].push_back(v->worldOf(D3(i, j, k) + jitter - 0.5));
                            }
                        }
                });
                for (const auto& s : slices)
                    for (const D3& p : s)
                        pts.addPoint(p);
                c.out("points", pointGeometry(std::move(pts)));
            },
    });
}

void volumeInfo()
{
    registerDecl({
        .type = "VolumeInfo",
        .label = "Volume Info",
        .category = "Volume",
        .doc = "A volume's grid: world bounds of its voxel centres, voxel size, voxel counts per axis and kind. Zero without a volume.",
        .sockets = {.inputs = {geoIn("volume")},
                    .outputs = {out("min", T::Vector), out("max", T::Vector), out("voxelSize", T::Float), out("dimensions", T::Vector),
                                out("voxelCount", T::Int), out("isSDF", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                VolumePtr v = c.geometry("volume")->volume;
                bool has = v && !v->empty();
                geo::Bounds b = has ? v->bounds() : geo::Bounds{};
                c.out("min", constantVector(has ? b.min : D3(0.0)));
                c.out("max", constantVector(has ? b.max : D3(0.0)));
                c.out("voxelSize", constantFloat(has ? v->voxelSize() : 0.0));
                c.out("dimensions", constantVector(has ? D3(v->dims) : D3(0.0)));
                c.out("voxelCount", constantInt(has ? int64_t(v->size()) : 0));
                c.out("isSDF", constantBool(has && v->isSdf()));
            },
    });
}
} // namespace volume_nodes

void registerVolumeNodes()
{
    volume_nodes::volumeCube();
    volume_nodes::meshToSdfNode();
    volume_nodes::pointsToSdfNode();
    volume_nodes::sdfBoolean();
    volume_nodes::offsetSdf();
    volume_nodes::sdfSmooth();
    volume_nodes::sampleVolume();
    volume_nodes::volumeToMeshNode();
    volume_nodes::distributeInVolume();
    volume_nodes::volumeInfo();
}
} // namespace lila::gn::nodes
