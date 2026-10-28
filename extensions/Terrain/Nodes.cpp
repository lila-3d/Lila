// The Terrain geometry nodes: heightfield sources, edits that read fields per sample, sampling, and conversion to and
// from meshes. Every terrain command builds a group out of these.

#include "Erosion.h"
#include "Inputs.h"
#include "Terrain.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Profile.h>
#include <lila/TextureGraph.h>
#include <lila/Workspace.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <fstream>

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

SocketDecl in(std::string id, T t, nlohmann::json def, std::string doc, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    SocketDecl s = in(std::move(id), t, std::move(def), std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
}

SocketDecl fin(std::string id, T t, nlohmann::json def, Implicit imp, std::string doc, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    SocketDecl s = fin(std::move(id), t, std::move(def), imp, std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
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
        .sockets = {.inputs = {in("sizeX", T::Float, 100000.0, "Width along X.", 0.0), in("sizeZ", T::Float, 100000.0, "Depth along Z.", 0.0),
                               in("resolution", T::Int, 513, "Samples along the longer side (2 to 16385).", 2, 16385),
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
                               in("featureSize", T::Float, p.featureSize, "Size of the largest features.", 1.0),
                               in("octaves", T::Int, p.octaves, "Layers of detail, each smaller than the last.", 1, 16),
                               in("roughness", T::Float, p.roughness, "0 to 1: how strong each smaller layer is.", 0.0, 1.0),
                               in("lacunarity", T::Float, p.lacunarity, "How much smaller each layer is.", 1.0, 4.0),
                               in("ridged", T::Float, p.ridged, "0 rolling, 1 sharp ridges.", 0.0, 1.0),
                               in("warp", T::Float, p.warp, "Domain warp, as a fraction of feature size."),
                               in("seed", T::Int, 0, "Another seed gives other shapes."),
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the noise is applied.", 0.0, 1.0)},
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
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.", 0.0, 1.0)},
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
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.", 0.0, 1.0)},
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
        .sockets = {.inputs = {geoIn(), in("step", T::Int, 1, "Keep every step-th sample.", 1, 1024), in("uvs", T::Bool, true, "Add uv set 0.")},
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
        .sockets = {.inputs = {geoIn("mesh"), in("resolution", T::Int, 513, "Samples along the longer side.", 2, 16385),
                               in("padding", T::Float, 0.0, "Extra border around the footprint, cm.", 0.0),
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
        .sockets = {.inputs = {geoIn("a"), geoIn("b"), fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of B is applied.", 0.0, 1.0)},
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
        .sockets = {.inputs = {geoIn("heightfield"), in("resolution", T::Int, 1025, "Samples along the longer side.", 2, 16385)}, .outputs = {geoOut("heightfield")}},
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

/** A file path as given, else under the workspace. */
std::filesystem::path resolved(std::filesystem::path file)
{
    if (!file.empty() && file.is_relative())
        if (const auto& root = Workspace::get().root(); !root.empty())
            file = root / file;
    return file;
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
                               in("width", T::Float, 100000.0, "Images: width along X, cm.", 0.0), in("low", T::Float, 0.0, "Images: height of black, cm."),
                               in("high", T::Float, 50000.0, "Images: height of white, cm."),
                               in("raw", T::Bool, false, "Float images: values are heights, cm."), in("center", T::Vector, vec(0, 0, 0), "Images: centre.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                const std::filesystem::path file = resolved(c.s("file"));
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no file");
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


gn::NodeDecl textureGraphDecl()
{
    return {
        .type = "TerrainTextureGraph",
        .label = "Evaluate Texture Graph",
        .category = kCategory,
        .doc = "Runs a texture graph (a .lilamat from texExportGraph) over the heightfield, one texel per sample, and writes one "
               "of its channels into the height or a mask: 0 to 1 maps to low..high. The texture's uv covers the whole grid, its "
               "top row on the -Z edge, so every texture node is a terrain node. Runs on the GPU when there is one unless backend "
               "says cpu.",
        .props = {blendProp("replace"), prop("backend", {"auto", "cpu", "gpu"}, "auto", "Where the graph runs.")},
        .sockets = {.inputs = {geoIn("heightfield"), in("file", T::String, "", "The .lilamat; relative paths are under the workspace."),
                               in("channel", T::String, "height", "Channel to read: height, roughness, ao, baseColor (its red) ..."),
                               in("target", T::String, "", "Mask to write; empty writes the height."),
                               in("low", T::Float, 0.0, "What 0 becomes."), in("high", T::Float, 10000.0, "What 1 becomes."),
                               fin("factor", T::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.", 0.0, 1.0)},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                const std::filesystem::path file = resolved(c.s("file"));
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no texture graph file");
                if (h.nx > 16384 || h.nz > 16384)
                    throw gn::ExecError(c.node.name + ": the grid is over 16384 samples a side");
                tex::Graph graph;
                try
                {
                    std::ifstream in(file);
                    if (!in)
                        throw std::runtime_error("can't open " + file.string());
                    nlohmann::json j = nlohmann::json::parse(in);
                    graph = tex::Graph::fromJson(j.contains("graph") ? j["graph"] : j);
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                const std::string channel = c.s("channel");
                tex::PlanRequest req;
                req.width = h.nx;
                req.height = h.nz;
                req.bufferFormat = "rgba32f";
                for (tex::OutputRequest& r : tex::materialRequests(graph, tex::standardChannels()))
                    if (r.name == channel)
                        req.outputs.push_back(std::move(r));
                if (req.outputs.empty())
                    throw gn::ExecError(c.node.name + ": the graph has no channel '" + channel + "' (it needs a Material Output)");
                const tex::Plan plan = tex::buildPlan(graph, req);
                if (!plan.error.empty())
                    throw gn::ExecError(c.node.name + ": " + plan.error);
                tex::EvalOptions opts;
                opts.imageRoot = file.parent_path();
                const std::string backend = c.prop("backend");
                tex::EvalResult res;
                try
                {
                    res = tex::evaluate(plan, opts, backend == "auto" ? std::string() : backend);
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                auto it = res.outputs.find(channel);
                if (it == res.outputs.end() || it->second.width != h.nx || it->second.height != h.nz)
                    throw gn::ExecError(c.node.name + ": the graph gave no " + channel + " image");
                const image::Image& img = it->second;
                const Blend mode = blendOf(c, "blend");
                const double low = c.f("low"), high = c.f("high");
                const std::string target = c.s("target");
                const std::vector<double> f = evaluateOnGrid(h, c.field("factor"));
                std::vector<float>& dst = target.empty() ? h.heights : h.mask(target);
                tbb::parallel_for(0, h.nz, [&](int z) {
                    for (int x = 0; x < h.nx; ++x)
                    {
                        const size_t k = h.index(x, z);
                        const double v = low + (high - low) * double(img.rgba[k * 4]);
                        dst[k] = blend(mode, dst[k], float(v), float(f[k]));
                    }
                });
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl erodeDecl()
{
    const ErosionParams p;
    return {
        .type = "ErodeTerrain",
        .label = "Erode Terrain",
        .category = kCategory,
        .doc = "Rivers cut valleys: stream-power erosion on the drainage network, with optional uplift building the land as it "
               "wears, and slopes over the talus angle shedding downhill. Every valley drains to the edge, which stays put. "
               "Writes masks flow (drainage), wear (removed), deposit (laid down) and lakes. Iterations is age: more is older and "
               "more carved. Lengths are cm.",
        .sockets = {.inputs = {geoIn("heightfield"), in("iterations", T::Int, p.iterations, "Steps; more is an older landscape.", 0, 10000),
                               in("strength", T::Float, p.strength, "How fast rivers cut.", 0.0),
                               in("areaExponent", T::Float, p.areaExponent, "How much bigger rivers cut faster (0.3 to 0.7).", 0.1, 1.0),
                               in("uplift", T::Float, p.uplift, "Rise per step, cm."),
                               in("upliftMask", T::String, "", "Mask scaling uplift; empty is uniform."),
                               in("hardnessMask", T::String, "", "Mask of hard rock (1) that erodes slower."),
                               in("talusAngle", T::Float, p.talusAngle, "Slopes steeper than this shed material, degrees.", 1.0, 89.0),
                               in("thermalIterations", T::Int, p.thermalIterations, "Thermal steps per erosion step.", 0, 100)},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                ErosionParams p;
                p.iterations = int(std::clamp<int64_t>(c.i("iterations"), 0, 10000));
                p.strength = std::max(0.0, c.f("strength"));
                p.areaExponent = std::clamp(c.f("areaExponent"), 0.1, 1.0);
                p.uplift = c.f("uplift");
                p.upliftMask = c.s("upliftMask");
                p.hardnessMask = c.s("hardnessMask");
                p.talusAngle = c.f("talusAngle");
                p.thermalIterations = int(std::clamp<int64_t>(c.i("thermalIterations"), 0, 100));
                if (p.iterations > 0)
                    erode(h, p);
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl flowDecl()
{
    return {
        .type = "HeightfieldFlow",
        .label = "Heightfield Flow",
        .category = kCategory,
        .doc = "Where water would go, without changing the ground: writes masks flow (0 to 1, how much drains through each "
               "sample, on a log scale: rivers are near 1) and lakes (how deep the pits are that water would fill). Fill Pits "
               "also raises the heights to those lake levels, so everything drains.",
        .sockets = {.inputs = {geoIn("heightfield"), in("fillPits", T::Bool, false, "Raise pits to their spill level.")}, .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                const Drainage d = drain(h);
                writeFlowMasks(h, d);
                if (c.b("fillPits"))
                    h.heights = d.filled;
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl thermalDecl()
{
    return {
        .type = "ThermalErosion",
        .label = "Thermal Erosion",
        .category = kCategory,
        .doc = "Slopes steeper than the talus angle shed material downhill until they settle: scree below cliffs, softened "
               "crests.",
        .sockets = {.inputs = {geoIn("heightfield"), in("talusAngle", T::Float, 38.0, "Steepest stable slope, degrees.", 1.0, 89.0),
                               in("iterations", T::Int, 20, "Steps.", 0, 10000)},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                thermal(h, c.f("talusAngle"), int(std::clamp<int64_t>(c.i("iterations"), 0, 10000)));
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl elevationDecl()
{
    const DemOptions o;
    return {
        .type = "ReadElevationData",
        .label = "Read Elevation Data",
        .category = kCategory,
        .doc = "Loads real-world elevation: GeoTIFF (DEM tiles such as Copernicus GLO-30, national lidar), SRTM .hgt, ESRI ASCII "
               "grid (.asc), XYZ points, or Terrarium and Terrain-RGB PNG map tiles. Heights become cm, centred on center with "
               "north toward -Z; data in degrees is flattened around its middle. Voids are filled from the data around them and "
               "marked in the mask voids. Data without spacing (PNG tiles, bare XYZ) uses spacing.",
        .props = {prop("format", demFormatNames(), "auto", "File type; auto goes by extension, and .png is Terrarium.")},
        .sockets = {.inputs = {in("file", T::String, "", "Path; relative paths are under the workspace."),
                               in("center", T::Vector, vec(0, 0, 0), "Centre of the area."),
                               in("cellSize", T::Float, o.cell, "Grid spacing, cm; 0 keeps the data's own.", 0.0),
                               in("spacing", T::Float, o.spacing, "Sample spacing, metres, for files that don't carry one.", 0.0),
                               in("exaggeration", T::Float, o.exaggeration, "Multiplies heights.", 0.0),
                               in("fromLowest", T::Bool, o.fromLowest, "Lowest point at 0 instead of sea level."),
                               in("fillVoids", T::Bool, true, "Fill voids; off leaves them at 0.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                const std::filesystem::path file = resolved(c.s("file"));
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no file");
                DemFormat format = DemFormat::Auto;
                if (!parseDemFormat(c.prop("format"), &format))
                    throw gn::ExecError(c.node.name + ": unknown format '" + c.prop("format") + "'");
                const D3 center = c.v("center");
                DemOptions o;
                o.center = {center.x, center.z};
                o.cell = std::max(0.0, c.f("cellSize"));
                o.spacing = c.f("spacing");
                o.exaggeration = c.f("exaggeration");
                o.fromLowest = c.b("fromLowest");
                try
                {
                    Dem d = readDem(file, format);
                    if (c.b("fillVoids"))
                        fillVoids(d);
                    c.out("heightfield", gn::heightfieldGeometry(demToHeightfield(d, o)));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
            },
    };
}

gn::NodeDecl imageDecl()
{
    const ImageOptions o;
    return {
        .type = "ImportTerrainImage",
        .label = "Import Terrain Image",
        .category = kCategory,
        .doc = "Turns a painted height image (png, psd, tif, exr, tga from Photoshop, Krita or GIMP) into terrain. Mode guide keeps "
               "the painting's big shapes and lets erosion add natural detail; height takes it literally; uplift reads brightness "
               "as how fast the land rises, so ranges grow where it is bright and rivers carve them; mask leaves flat ground and "
               "puts the image in a mask. The image is width wide along X, its top row on the -Z edge. Erosion is slow on big "
               "grids: seconds at 1K.",
        .props = {prop("mode", imageModeNames(), "guide", "How brightness is read.")},
        .sockets = {.inputs = {in("file", T::String, "", "Path; relative paths are under the workspace."),
                               in("width", T::Float, 100000.0, "Width along X, cm.", 0.0), in("low", T::Float, o.low, "Height of black, cm."),
                               in("high", T::Float, o.high, "Height of white, cm."), in("center", T::Vector, vec(0, 0, 0), "Centre."),
                               in("smooth", T::Float, o.smooth, "Blur radius, samples: removes 8-bit steps and brush noise.", 0.0, 64.0),
                               in("shape", T::Float, o.shape, "Exponent on brightness: above 1 widens valleys and sharpens peaks.", 0.0),
                               in("erosion", T::Float, o.erosion, "0 to 1: how far erosion may move the painting.", 0.0, 1.0),
                               in("detail", T::Float, o.detail, "Noise added before erosion, a fraction of the height range.", 0.0),
                               in("seed", T::Int, int64_t(o.seed), "Seed for that noise."),
                               in("mask", T::String, o.mask, "Mask mode: the mask written.")},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                const std::filesystem::path file = resolved(c.s("file"));
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no file");
                ImageOptions o;
                if (!parseImageMode(c.prop("mode"), &o.mode))
                    throw gn::ExecError(c.node.name + ": unknown mode '" + c.prop("mode") + "'");
                o.low = c.f("low");
                o.high = c.f("high");
                o.smooth = std::clamp(c.f("smooth"), 0.0, 64.0);
                o.shape = c.f("shape");
                o.erosion = c.f("erosion");
                o.detail = std::max(0.0, c.f("detail"));
                o.seed = uint32_t(c.i("seed"));
                o.mask = c.s("mask");
                const D3 center = c.v("center");
                try
                {
                    Heightfield h = loadHeightfield(file, {center.x, center.z}, c.f("width"), 0.0, 1.0, false);
                    interpretImage(h, o);
                    c.out("heightfield", gn::heightfieldGeometry(std::move(h)));
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
                           resampleDecl(), infoDecl(), readDecl(), textureGraphDecl(), erodeDecl(), flowDecl(), thermalDecl(), elevationDecl(), imageDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace terrain
