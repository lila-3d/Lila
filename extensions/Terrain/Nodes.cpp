// The Terrain geometry nodes: heightfield sources, edits that read fields per sample, sampling, and conversion to and
// from meshes. Every terrain command builds a group out of these.

#include "Terrain.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Profile.h>
#include <lila/Workspace.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

using namespace lila;

namespace terrain
{
namespace
{
using gn::ExecContext;
using gn::FieldPtr;
using gn::Geometry;
using gn::GeometryPtr;
using gn::Implicit;
using gn::PropDecl;
using gn::SocketDecl;
using T = gn::SocketType;

SocketDecl in(std::string id, T t, nlohmann::json def, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}

SocketDecl fin(std::string id, T t, nlohmann::json def, Implicit imp, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .field = true, .implicit = imp, .doc = std::move(doc)};
}

SocketDecl geoIn(std::string id = "geometry") { return {.id = std::move(id), .type = T::Geometry}; }
SocketDecl geoOut(std::string id = "geometry") { return {.id = std::move(id), .type = T::Geometry}; }
SocketDecl out(std::string id, T t) { return {.id = std::move(id), .type = t}; }
SocketDecl fout(std::string id, T t, std::string doc = {}) { return {.id = std::move(id), .type = t, .field = true, .doc = std::move(doc)}; }

PropDecl prop(std::string id, std::vector<std::string> options, std::string def, std::string doc)
{
    return {.id = std::move(id), .options = std::move(options), .defaultValue = std::move(def), .doc = std::move(doc)};
}

nlohmann::json vec(double x, double y, double z) { return nlohmann::json::array({x, y, z}); }
using D3 = glm::dvec3;

constexpr const char* kCategory = "Terrain";

std::string blendList()
{
    std::string s;
    for (const std::string& n : blendNames())
        s += (s.empty() ? "" : ", ") + n;
    return s;
}

Blend blendOf(const ExecContext& c, const char* id)
{
    Blend b = Blend::Replace;
    if (!parseBlend(c.prop(id), &b))
        throw gn::ExecError(c.node.name + ": unknown blend '" + c.prop(id) + "' (" + blendList() + ")");
    return b;
}

PropDecl blendProp(std::string def) { return prop("blend", blendNames(), std::move(def), "How the new value combines with the one there."); }

/** The input's heightfield; throws when it has none. */
const Heightfield& requireHeightfield(const ExecContext& c, const GeometryPtr& g)
{
    if (const Heightfield* h = heightfieldOf(*g))
        return *h;
    throw gn::ExecError(c.node.name + ": the geometry has no heightfield (start from Heightfield Grid or Mesh to Heightfield)");
}

/** g with its heightfield replaced, every other component kept. */
GeometryPtr withHeightfield(const GeometryPtr& g, Heightfield h)
{
    Geometry out = *g;
    out.heightfield = std::make_shared<const Heightfield>(std::move(h));
    return gn::makeGeometry(std::move(out));
}

class GridField final : public gn::Field
{
public:
    using Fn = std::function<gn::Column(const gn::FieldContext&)>;
    GridField(gn::SocketType t, Fn fn) : Field(t), mFn(std::move(fn)) {}
    gn::Column evaluate(const gn::FieldContext& ctx) const override { return mFn(ctx); }

private:
    Fn mFn;
};

gn::NodeDecl gridDecl()
{
    return {
        .type = "HeightfieldGrid",
        .label = "Heightfield Grid",
        .category = kCategory,
        .doc = "A flat heightfield: size along X and Z, centred on center, with resolution samples along the longer side and "
               "square cells. Lengths are cm.",
        .sockets = {.inputs = {in("sizeX", T::Float, 100000.0, "Width along X."), in("sizeZ", T::Float, 100000.0, "Depth along Z."),
                               in("resolution", T::Int, 513, "Samples along the longer side (2 to 16385)."),
                               in("center", T::Vector, vec(0, 0, 0), "Centre; its Y is the starting height.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                const D3 center = c.v("center");
                const int res = int(std::clamp<int64_t>(c.i("resolution"), 2, 16385));
                c.out("heightfield", gn::heightfieldGeometry(geo::makeHeightfieldCovering({center.x, center.z}, {c.f("sizeX"), c.f("sizeZ")}, res,
                                                                                           float(center.y))));
            },
    };
}

gn::NodeDecl noiseDecl()
{
    const NoiseParams p;
    return {
        .type = "TerrainNoise",
        .label = "Terrain Noise",
        .category = kCategory,
        .doc = "Fractal noise over world X and Z, from 0 to amplitude, combined into the heightfield's heights. Ridged turns rolling "
               "hills into sharp crests with detail gathered on them; warp bends the shapes so they don't line up. Works in world "
               "space, so neighbouring heightfields with the same settings join without a seam. Lengths are cm.",
        .props = {blendProp("add")},
        .sockets = {.inputs = {geoIn("heightfield"), in("amplitude", T::Float, p.amplitude, "Height of the tallest features."),
                               in("featureSize", T::Float, p.featureSize, "Size of the largest features."),
                               in("octaves", T::Int, p.octaves, "Layers of detail, each smaller than the last."),
                               in("roughness", T::Float, p.roughness, "0 to 1: how strong each smaller layer is."),
                               in("lacunarity", T::Float, p.lacunarity, "How much smaller each layer is."),
                               in("ridged", T::Float, p.ridged, "0 rolling, 1 sharp ridges."),
                               in("warp", T::Float, p.warp, "Domain warp, as a fraction of feature size."),
                               in("seed", T::Int, 0, "Another seed gives other shapes."),
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the noise is applied.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                NoiseParams p;
                p.amplitude = c.f("amplitude");
                p.featureSize = std::max(1.0, c.f("featureSize"));
                p.octaves = int(std::clamp<int64_t>(c.i("octaves"), 1, 16));
                p.roughness = std::clamp(c.f("roughness"), 0.0, 1.0);
                p.lacunarity = std::clamp(c.f("lacunarity"), 1.0, 4.0);
                p.ridged = std::clamp(c.f("ridged"), 0.0, 1.0);
                p.warp = c.f("warp");
                p.seed = uint32_t(c.i("seed"));
                const Blend mode = blendOf(c, "blend");
                const std::vector<double> factor = evaluateOnGrid(h, c.field("factor"));
                tbb::parallel_for(0, h.nz, [&](int z) {
                    for (int x = 0; x < h.nx; ++x)
                    {
                        const size_t k = h.index(x, z);
                        const glm::dvec2 w = h.samplePos(x, z);
                        h.heights[k] = blend(mode, h.heights[k], float(noiseHeight(p, w.x, w.y)), float(factor[k]));
                    }
                });
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl setHeightDecl()
{
    return {
        .type = "SetHeight",
        .label = "Set Height",
        .category = kCategory,
        .doc = "Sets every sample's height from a field, combined by blend. Fields see each sample as a point at its position, "
               "with height and every mask as attributes, so Noise Texture, Math and Named Attribute all work per sample.",
        .props = {blendProp("replace")},
        .sockets = {.inputs = {geoIn("heightfield"), fin("height", T::Float, 0.0, Implicit::None, "The value, cm."),
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                const Blend mode = blendOf(c, "blend");
                const std::vector<double> v = evaluateOnGrid(h, c.field("height")), f = evaluateOnGrid(h, c.field("factor"));
                tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { h.heights[k] = blend(mode, h.heights[k], float(v[k]), float(f[k])); });
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl setMaskDecl()
{
    return {
        .type = "SetHeightfieldMask",
        .label = "Set Heightfield Mask",
        .category = kCategory,
        .doc = "Writes a named mask (made when missing) from a field, combined by blend. Masks ride along with the heightfield, "
               "reach fields as attributes, and become point attributes when it turns into a mesh.",
        .props = {blendProp("replace")},
        .sockets = {.inputs = {geoIn("heightfield"), in("name", T::String, "mask", "Mask name."),
                               fin("value", T::Float, 1.0, Implicit::None, "The value, usually 0 to 1."),
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                const std::string name = c.s("name");
                if (name.empty() || name == "height")
                    throw gn::ExecError(c.node.name + ": a mask needs a name other than height");
                const Blend mode = blendOf(c, "blend");
                const std::vector<double> v = evaluateOnGrid(h, c.field("value")), f = evaluateOnGrid(h, c.field("factor"));
                std::vector<float>& m = h.mask(name);
                tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { m[k] = blend(mode, m[k], float(v[k]), float(f[k])); });
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl sampleDecl()
{
    return {
        .type = "SampleHeightfield",
        .label = "Sample Heightfield",
        .category = kCategory,
        .doc = "Reads a heightfield under each element: height, normal and slope at its X and Z, and a mask by name. Positions "
               "outside the heightfield read its nearest edge; inside says which are over it.",
        .sockets = {.inputs = {geoIn("heightfield"), fin("position", T::Vector, nullptr, Implicit::Position, "Where to read; only X and Z count."),
                               in("mask", T::String, "", "Mask to read into the mask output.")},
                    .outputs = {fout("height", T::Float), fout("normal", T::Vector), fout("slope", T::Float, "Degrees from flat."),
                                fout("mask", T::Float), fout("inside", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("heightfield");
                const auto h = g->heightfield;
                const FieldPtr pos = c.field("position");
                const std::string mask = c.s("mask");
                auto per = [h, pos](gn::SocketType t, auto fn) {
                    return std::make_shared<GridField>(t, [h, pos, fn](const gn::FieldContext& ctx) {
                        const std::vector<D3> p = gn::evaluateAll<D3>(pos, ctx);
                        using R = decltype(fn(*h, D3()));
                        std::vector<R> out(ctx.size, R{});
                        if (h && !h->empty())
                            tbb::parallel_for(size_t(0), ctx.size, [&](size_t k) { out[k] = fn(*h, p[k]); });
                        return gn::Column::of(std::move(out));
                    });
                };
                c.out("height", per(T::Float, [](const Heightfield& hf, const D3& p) { return hf.heightAt(p.x, p.z); }));
                c.out("normal", per(T::Vector, [](const Heightfield& hf, const D3& p) { return hf.normalAt(p.x, p.z); }));
                c.out("slope", per(T::Float, [](const Heightfield& hf, const D3& p) {
                          return std::acos(std::clamp(hf.normalAt(p.x, p.z).y, -1.0, 1.0)) * 57.29577951308232;
                      }));
                c.out("mask", per(T::Float, [mask](const Heightfield& hf, const D3& p) { return hf.maskAt(mask, p.x, p.z); }));
                c.out("inside", per(T::Bool, [](const Heightfield& hf, const D3& p) { return uint8_t(hf.contains(p.x, p.z) ? 1 : 0); }));
            },
    };
}

gn::NodeDecl toMeshDecl()
{
    return {
        .type = "HeightfieldToMesh",
        .label = "Heightfield to Mesh",
        .category = kCategory,
        .doc = "Turns the heightfield into a grid of quads facing up, keeping every step-th sample (the edges always stay), with "
               "uvs over the whole grid and masks as point attributes. Other components pass through.",
        .sockets = {.inputs = {geoIn(), in("step", T::Int, 1, "Keep every step-th sample."), in("uvs", T::Bool, true, "Add uv set 0.")},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("geometry");
                const Heightfield* h = heightfieldOf(*g);
                if (!h)
                {
                    c.out("geometry", g);
                    return;
                }
                geo::HeightfieldMeshOptions o;
                o.step = int(std::clamp<int64_t>(c.i("step"), 1, 1024));
                o.uvs = c.b("uvs");
                Geometry out = *g;
                out.heightfield = nullptr;
                geo::Mesh m = geo::heightfieldToMesh(*h, o);
                if (out.mesh)
                {
                    geo::Mesh joined = *out.mesh;
                    joined.append(m);
                    m = std::move(joined);
                }
                out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
                c.out("geometry", gn::makeGeometry(std::move(out)));
            },
    };
}

gn::NodeDecl fromMeshDecl()
{
    return {
        .type = "MeshToHeightfield",
        .label = "Mesh to Heightfield",
        .category = kCategory,
        .doc = "Bakes the highest surface of a mesh, seen from above, into a heightfield covering its footprint plus padding. Any "
               "mesh works: a blockout, a scan, a sculpt. Samples nothing covers take fill, or the mesh's lowest point when "
               "fillLowest is on.",
        .sockets = {.inputs = {geoIn("mesh"), in("resolution", T::Int, 513, "Samples along the longer side."),
                               in("padding", T::Float, 0.0, "Extra border around the footprint, cm."),
                               in("fill", T::Float, 0.0, "Height where the mesh doesn't reach."),
                               in("fillLowest", T::Bool, true, "Fill with the mesh's lowest point instead.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("mesh");
                Geometry r = gn::realize(*g);
                if (!r.mesh || r.mesh->numPoints() == 0)
                    throw gn::ExecError(c.node.name + ": no mesh to bake");
                geo::Bounds b;
                for (geo::Index p = 0; p < r.mesh->pointCapacity(); ++p)
                    if (r.mesh->pointValid(p))
                        b.extend(r.mesh->position(p));
                const double pad = std::max(0.0, c.f("padding"));
                const glm::dvec2 lo(b.min.x - pad, b.min.z - pad), hi(b.max.x + pad, b.max.z + pad);
                Heightfield grid = geo::makeHeightfieldCovering((lo + hi) * 0.5, hi - lo, int(std::clamp<int64_t>(c.i("resolution"), 2, 16385)));
                const float fill = c.b("fillLowest") ? float(b.min.y) : float(c.f("fill"));
                c.out("heightfield", gn::heightfieldGeometry(geo::meshToHeightfield(*r.mesh, grid.nx, grid.nz, grid.cell, grid.origin, fill)));
            },
    };
}

gn::NodeDecl blendDecl()
{
    return {
        .type = "BlendHeightfields",
        .label = "Blend Heightfields",
        .category = kCategory,
        .doc = "Combines B into A on A's grid: B is read bilinearly at each of A's samples and combined by blend, its masks "
               "too. Where B doesn't reach, A is left alone.",
        .props = {blendProp("max")},
        .sockets = {.inputs = {geoIn("a"), geoIn("b"), fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of B is applied.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr ga = c.geometry("a"), gb = c.geometry("b");
                Heightfield a = requireHeightfield(c, ga);
                const Heightfield& b = requireHeightfield(c, gb);
                const Blend mode = blendOf(c, "blend");
                const std::vector<double> f = evaluateOnGrid(a, c.field("factor"));
                std::vector<std::pair<std::vector<float>*, std::string>> masks;
                for (const auto& [name, values] : b.masks)
                    if (values.size() == b.size())
                        masks.emplace_back(&a.mask(name), name);
                tbb::parallel_for(0, a.nz, [&](int z) {
                    for (int x = 0; x < a.nx; ++x)
                    {
                        const glm::dvec2 w = a.samplePos(x, z);
                        if (!b.contains(w.x, w.y))
                            continue;
                        const size_t k = a.index(x, z);
                        a.heights[k] = blend(mode, a.heights[k], float(b.heightAt(w.x, w.y)), float(f[k]));
                        for (auto& [dst, name] : masks)
                            (*dst)[k] = blend(mode, (*dst)[k], float(b.maskAt(name, w.x, w.y)), float(f[k]));
                    }
                });
                c.out("heightfield", withHeightfield(ga, std::move(a)));
            },
    };
}

gn::NodeDecl resampleDecl()
{
    return {
        .type = "ResampleHeightfield",
        .label = "Resample Heightfield",
        .category = kCategory,
        .doc = "The same area at another resolution (samples along the longer side), masks included, read bilinearly.",
        .sockets = {.inputs = {geoIn("heightfield"), in("resolution", T::Int, 1025, "Samples along the longer side.")}, .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                const Heightfield& h = requireHeightfield(c, g);
                const glm::dvec2 e = h.extent();
                Heightfield grid = geo::makeHeightfieldCovering(h.origin + e * 0.5, e, int(std::clamp<int64_t>(c.i("resolution"), 2, 16385)));
                c.out("heightfield", withHeightfield(g, geo::resampled(h, grid.nx, grid.nz, grid.cell, grid.origin)));
            },
    };
}

gn::NodeDecl infoDecl()
{
    return {
        .type = "HeightfieldInfo",
        .label = "Heightfield Info",
        .category = kCategory,
        .doc = "Size and height range of a heightfield: lowest and highest height, the area it covers (X and Z in the vector), "
               "its sample counts and cell size. All zero without one.",
        .sockets = {.inputs = {geoIn("heightfield")},
                    .outputs = {out("minHeight", T::Float), out("maxHeight", T::Float), out("size", T::Vector), out("resolutionX", T::Int),
                                out("resolutionZ", T::Int), out("cellSize", T::Float), out("center", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                const Heightfield* h = heightfieldOf(*c.geometry("heightfield"));
                const glm::dvec2 r = h ? h->range() : glm::dvec2(0.0), e = h ? h->extent() : glm::dvec2(0.0);
                const glm::dvec2 mid = h ? h->origin + e * 0.5 : glm::dvec2(0.0);
                c.out("minHeight", gn::constantFloat(r.x));
                c.out("maxHeight", gn::constantFloat(r.y));
                c.out("size", gn::constantVector({e.x, r.y - r.x, e.y}));
                c.out("resolutionX", gn::constantInt(h ? h->nx : 0));
                c.out("resolutionZ", gn::constantInt(h ? h->nz : 0));
                c.out("cellSize", gn::constantFloat(h ? h->cell : 0.0));
                c.out("center", gn::constantVector({mid.x, (r.x + r.y) * 0.5, mid.y}));
            },
    };
}

gn::NodeDecl readDecl()
{
    return {
        .type = "ReadHeightfield",
        .label = "Read Heightfield",
        .category = kCategory,
        .doc = "Loads a heightfield file. A .lilahf (what terrainBake writes) comes back exactly, masks and placement included. An "
               "image (png, exr, tif, psd, tga) is read as height from its red channel: 0 to 1 maps to low..high, or float EXR and "
               "TIFF values are taken as heights when raw is on. Images are width wide along X with square cells, centred on "
               "center; the top row is the -Z edge as seen from above.",
        .sockets = {.inputs = {in("file", T::String, "", "Path; relative paths are under the workspace."),
                               in("width", T::Float, 100000.0, "Images: width along X, cm."), in("low", T::Float, 0.0, "Images: height of black, cm."),
                               in("high", T::Float, 50000.0, "Images: height of white, cm."),
                               in("raw", T::Bool, false, "Float images: values are heights, cm."), in("center", T::Vector, vec(0, 0, 0), "Images: centre.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::filesystem::path file = c.s("file");
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no file");
                if (file.is_relative())
                    if (const auto& root = Workspace::get().root(); !root.empty())
                        file = root / file;
                const D3 center = c.v("center");
                try
                {
                    c.out("heightfield", gn::heightfieldGeometry(loadHeightfield(file, {center.x, center.z}, c.f("width"), c.f("low"), c.f("high"), c.b("raw"))));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
            },
    };
}
} // namespace

bool registerNodes(ext::Context& ctx)
{
    for (gn::NodeDecl d : {gridDecl(), noiseDecl(), setHeightDecl(), setMaskDecl(), sampleDecl(), toMeshDecl(), fromMeshDecl(), blendDecl(),
                           resampleDecl(), infoDecl(), readDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace terrain
