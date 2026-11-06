// The Terrain geometry nodes: heightfield sources, edits that read fields per sample, sampling, and conversion to and
// from meshes. Every terrain command builds a group out of these.

#include "Erosion.h"
#include "Analysis.h"
#include "ErosionGpu.h"
#include "Inputs.h"
#include "LandformCommands.h"
#include "Painted.h"
#include "Projection.h"
#include "Stamps.h"
#include "Terrain.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Paths.h>
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
using Socket = gn::SocketType;

SocketDecl in(std::string id, Socket t, nlohmann::json def, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}

SocketDecl fin(std::string id, Socket t, nlohmann::json def, Implicit imp, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .field = true, .implicit = imp, .doc = std::move(doc)};
}

SocketDecl in(std::string id, Socket t, nlohmann::json def, std::string doc, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    SocketDecl s = in(std::move(id), t, std::move(def), std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
}

SocketDecl fin(std::string id, Socket t, nlohmann::json def, Implicit imp, std::string doc, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    SocketDecl s = fin(std::move(id), t, std::move(def), imp, std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
}

SocketDecl geoIn(std::string id = "geometry") { return {.id = std::move(id), .type = Socket::Geometry}; }
SocketDecl geoOut(std::string id = "geometry") { return {.id = std::move(id), .type = Socket::Geometry}; }
SocketDecl out(std::string id, Socket t) { return {.id = std::move(id), .type = t}; }
SocketDecl fout(std::string id, Socket t, std::string doc = {}) { return {.id = std::move(id), .type = t, .field = true, .doc = std::move(doc)}; }

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
        .sockets = {.inputs = {in("sizeX", Socket::Float, 100000.0, "Width along X.", 0.0), in("sizeZ", Socket::Float, 100000.0, "Depth along Z.", 0.0),
                               in("resolution", Socket::Int, 513, "Samples along the longer side (2 to 16385).", 2, 16385),
                               in("center", Socket::Vector, vec(0, 0, 0), "Centre; its Y is the starting height.")},
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
        .sockets = {.inputs = {geoIn("heightfield"), in("amplitude", Socket::Float, p.amplitude, "Height of the tallest features."),
                               in("featureSize", Socket::Float, p.featureSize, "Size of the largest features.", 1.0),
                               in("octaves", Socket::Int, p.octaves, "Layers of detail, each smaller than the last.", 1, 16),
                               in("roughness", Socket::Float, p.roughness, "0 to 1: how strong each smaller layer is.", 0.0, 1.0),
                               in("lacunarity", Socket::Float, p.lacunarity, "How much smaller each layer is.", 1.0, 4.0),
                               in("ridged", Socket::Float, p.ridged, "0 rolling, 1 sharp ridges.", 0.0, 1.0),
                               in("warp", Socket::Float, p.warp, "Domain warp, as a fraction of feature size."),
                               in("seed", Socket::Int, 0, "Another seed gives other shapes."),
                               fin("factor", Socket::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the noise is applied.", 0.0, 1.0)},
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
        .sockets = {.inputs = {geoIn("heightfield"), fin("height", Socket::Float, 0.0, Implicit::None, "The value, cm."),
                               fin("factor", Socket::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.", 0.0, 1.0)},
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
        .sockets = {.inputs = {geoIn("heightfield"), in("name", Socket::String, "mask", "Mask name."),
                               fin("value", Socket::Float, 1.0, Implicit::None, "The value, usually 0 to 1."),
                               fin("factor", Socket::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.", 0.0, 1.0)},
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
        .sockets = {.inputs = {geoIn("heightfield"), fin("position", Socket::Vector, nullptr, Implicit::Position, "Where to read; only X and Z count."),
                               in("mask", Socket::String, "", "Mask to read into the mask output.")},
                    .outputs = {fout("height", Socket::Float), fout("normal", Socket::Vector), fout("slope", Socket::Float, "Degrees from flat."),
                                fout("mask", Socket::Float), fout("inside", Socket::Bool)}},
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
                c.out("height", per(Socket::Float, [](const Heightfield& hf, const D3& p) { return hf.heightAt(p.x, p.z); }));
                c.out("normal", per(Socket::Vector, [](const Heightfield& hf, const D3& p) { return hf.normalAt(p.x, p.z); }));
                c.out("slope", per(Socket::Float, [](const Heightfield& hf, const D3& p) {
                          return std::acos(std::clamp(hf.normalAt(p.x, p.z).y, -1.0, 1.0)) * 57.29577951308232;
                      }));
                c.out("mask", per(Socket::Float, [mask](const Heightfield& hf, const D3& p) { return hf.maskAt(mask, p.x, p.z); }));
                c.out("inside", per(Socket::Bool, [](const Heightfield& hf, const D3& p) { return uint8_t(hf.contains(p.x, p.z) ? 1 : 0); }));
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
        .sockets = {.inputs = {geoIn(), in("step", Socket::Int, 1, "Keep every step-th sample.", 1, 1024), in("uvs", Socket::Bool, true, "Add uv set 0.")},
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
        .sockets = {.inputs = {geoIn("mesh"), in("resolution", Socket::Int, 513, "Samples along the longer side.", 2, 16385),
                               in("padding", Socket::Float, 0.0, "Extra border around the footprint, cm.", 0.0),
                               in("fill", Socket::Float, 0.0, "Height where the mesh doesn't reach."),
                               in("fillLowest", Socket::Bool, true, "Fill with the mesh's lowest point instead.")},
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
        .sockets = {.inputs = {geoIn("a"), geoIn("b"), fin("factor", Socket::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of B is applied.", 0.0, 1.0)},
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
        .sockets = {.inputs = {geoIn("heightfield"), in("resolution", Socket::Int, 1025, "Samples along the longer side.", 2, 16385)}, .outputs = {geoOut("heightfield")}},
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
                    .outputs = {out("minHeight", Socket::Float), out("maxHeight", Socket::Float), out("size", Socket::Vector), out("resolutionX", Socket::Int),
                                out("resolutionZ", Socket::Int), out("cellSize", Socket::Float), out("center", Socket::Vector)}},
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

/** A file path as stored: absolute as given, relative under the workspace, then next to the scene. */
std::filesystem::path resolved(const std::string& file)
{
    return file.empty() ? std::filesystem::path() : paths::resolvePath(file);
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
        .sockets = {.inputs = {in("file", Socket::String, "", "Path; relative paths are under the workspace."),
                               in("width", Socket::Float, 100000.0, "Images: width along X, cm.", 0.0), in("low", Socket::Float, 0.0, "Images: height of black, cm."),
                               in("high", Socket::Float, 50000.0, "Images: height of white, cm."),
                               in("raw", Socket::Bool, false, "Float images: values are heights, cm."), in("center", Socket::Vector, vec(0, 0, 0), "Images: centre.")},
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
        .sockets = {.inputs = {geoIn("heightfield"), in("file", Socket::String, "", "The .lilamat; relative paths are under the workspace."),
                               in("channel", Socket::String, "height", "Channel to read: height, roughness, ao, baseColor (its red) ..."),
                               in("target", Socket::String, "", "Mask to write; empty writes the height."),
                               in("low", Socket::Float, 0.0, "What 0 becomes."), in("high", Socket::Float, 10000.0, "What 1 becomes."),
                               fin("factor", Socket::Float, 1.0, Implicit::None, "0 to 1 per sample: how much of the change is applied.", 0.0, 1.0)},
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
               "more carved. Lengths are cm. Device gpu runs it on the graphics card (in the app), within float tolerance of cpu, "
               "which is exact and the same everywhere; auto takes the GPU when there is one.",
        .props = {prop("device", {"cpu", "gpu", "auto"}, "cpu", "Where it runs.")},
        .sockets = {.inputs = {geoIn("heightfield"), in("iterations", Socket::Int, p.iterations, "Steps; more is an older landscape.", 0, 10000),
                               in("strength", Socket::Float, p.strength, "How fast rivers cut.", 0.0),
                               in("areaExponent", Socket::Float, p.areaExponent, "How much bigger rivers cut faster (0.3 to 0.7).", 0.1, 1.0),
                               in("uplift", Socket::Float, p.uplift, "Rise per step, cm."),
                               in("upliftMask", Socket::String, "", "Mask scaling uplift; empty is uniform."),
                               in("hardnessMask", Socket::String, "", "Mask of hard rock (1) that erodes slower."),
                               in("talusAngle", Socket::Float, p.talusAngle, "Slopes steeper than this shed material, degrees.", 1.0, 89.0),
                               in("thermalIterations", Socket::Int, p.thermalIterations, "Thermal steps per erosion step.", 0, 100)},
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
                const std::string device = c.prop("device");
                if (p.iterations > 0)
                {
                    std::string err;
                    bool gpu = device == "gpu" || (device == "auto" && gpuErosionAvailable());
                    if (gpu && h.findMask(p.constraintMask) && h.findMask(p.constraintHeightMask))
                    {
                        log::warn("{}: drawn constraints erode on the CPU (the GPU path doesn't hold them)", c.node.name);
                        gpu = false;
                    }
                    const std::vector<float> uneroded = h.findMask("erosionProtect") ? h.heights : std::vector<float>{};
                    if (!gpu)
                        erode(h, p);
                    else if (!erodeGpu(h, p, &err))
                        throw gn::ExecError(c.node.name + ": " + err);
                    // Roads laid before erosion keep their shape where erosionProtect is 1.
                    if (const std::vector<float>* keep = h.findMask("erosionProtect"); keep && keep->size() == h.size() && uneroded.size() == h.size())
                        for (size_t i = 0; i < h.size(); ++i)
                            h.heights[i] += (*keep)[i] * (uneroded[i] - h.heights[i]);
                }
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
        .sockets = {.inputs = {geoIn("heightfield"), in("fillPits", Socket::Bool, false, "Raise pits to their spill level.")}, .outputs = {geoOut("heightfield")}},
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
               "crests. Where Material Mask is set, its angle takes over (hard rock stands steeper, sand slumps lower). Writes "
               "talus (where scree landed).",
        .props = {prop("device", {"cpu", "gpu", "auto"}, "cpu", "Where it runs: cpu is exact and the same everywhere.")},
        .sockets = {.inputs = {geoIn("heightfield"), in("talusAngle", Socket::Float, 38.0, "Steepest stable slope, degrees.", 1.0, 89.0),
                               in("iterations", Socket::Int, 20, "Steps.", 0, 10000),
                               in("materialMask", Socket::String, "", "Mask of a material with its own talus angle (such as a hardness mask)."),
                               in("materialAngle", Socket::Float, 60.0, "The material's talus angle, degrees.", 1.0, 89.0)},
                    .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                runThermalNode(c, h);
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
               "marked in the mask voids. Data without spacing (PNG tiles, bare XYZ) uses spacing. Projection utm or enu "
               "reprojects data in degrees to UTM metres or a tangent plane at its centre (with the earth's curvature).",
        .props = {prop("format", demFormatNames(), "auto", "File type; auto goes by extension, and .png is Terrarium."),
                  prop("projection", projectionNames(), "local", "local flattens degrees around the middle; utm and enu reproject.")},
        .sockets = {.inputs = {in("file", Socket::String, "", "Path; relative paths are under the workspace."),
                               in("center", Socket::Vector, vec(0, 0, 0), "Centre of the area."),
                               in("cellSize", Socket::Float, o.cell, "Grid spacing, cm; 0 keeps the data's own.", 0.0),
                               in("spacing", Socket::Float, o.spacing, "Sample spacing, metres, for files that don't carry one.", 0.0),
                               in("exaggeration", Socket::Float, o.exaggeration, "Multiplies heights.", 0.0),
                               in("fromLowest", Socket::Bool, o.fromLowest, "Lowest point at 0 instead of sea level."),
                               in("fillVoids", Socket::Bool, true, "Fill voids; off leaves them at 0."),
                               in("zone", Socket::Int, int64_t(0), "UTM zone; 0 picks the data's.", 0.0, 60.0)},
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
                    ProjectionKind projection = ProjectionKind::Local;
                    if (!parseProjection(c.prop("projection"), &projection))
                        throw std::runtime_error("unknown projection '" + c.prop("projection") + "'");
                    Georef geo;
                    Heightfield h = demToTerrain(readDem(file, format), o, projection, int(c.i("zone")), c.b("fillVoids"), &geo);
                    publishGeoref(c.env.owner, geo);
                    c.out("heightfield", gn::heightfieldGeometry(std::move(h)));
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
               "puts the image in a mask; sketch reads red strokes as ridges, green as valleys, blue as rivers and yellow as flat "
               "ground over a grey base, and erosion keeps them. The image is width wide along X, its top row on the -Z edge. "
               "Erosion is slow on big grids: seconds at 1K.",
        .props = {prop("mode", imageModeNames(), "guide", "How brightness is read.")},
        .sockets = {.inputs = {in("file", Socket::String, "", "Path; relative paths are under the workspace."),
                               in("width", Socket::Float, 100000.0, "Width along X, cm.", 0.0), in("low", Socket::Float, o.low, "Height of black, cm."),
                               in("high", Socket::Float, o.high, "Height of white, cm."), in("center", Socket::Vector, vec(0, 0, 0), "Centre."),
                               in("smooth", Socket::Float, o.smooth, "Blur radius, samples: removes 8-bit steps and brush noise.", 0.0, 64.0),
                               in("shape", Socket::Float, o.shape, "Exponent on brightness: above 1 widens valleys and sharpens peaks.", 0.0),
                               in("erosion", Socket::Float, o.erosion, "0 to 1: how far erosion may move the painting.", 0.0, 1.0),
                               in("detail", Socket::Float, o.detail, "Noise added before erosion, a fraction of the height range.", 0.0),
                               in("seed", Socket::Int, int64_t(o.seed), "Seed for that noise."),
                               in("mask", Socket::String, o.mask, "Mask mode: the mask written.")},
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
                    Heightfield h;
                    if (o.mode == ImageMode::Sketch)
                    {
                        std::string err;
                        const image::Image img = image::read(file, &err);
                        if (img.width < 2 || img.height < 2)
                            throw std::runtime_error(file.string() + ": " + (err.empty() ? std::string("no image") : err));
                        h = sketchTerrain(img, {center.x, center.z}, c.f("width"), o);
                    }
                    else
                    {
                        h = loadHeightfield(file, {center.x, center.z}, c.f("width"), 0.0, 1.0, false);
                        interpretImage(h, o);
                    }
                    c.out("heightfield", gn::heightfieldGeometry(std::move(h)));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
            },
    };
}

/** A JSON list prop parsed, or an empty list. */
nlohmann::json listProp(const ExecContext& c, const char* id)
{
    const std::string text = c.prop(id);
    if (text.empty())
        return nlohmann::json::array();
    try
    {
        nlohmann::json j = nlohmann::json::parse(text);
        return j.is_array() ? j : nlohmann::json::array();
    }
    catch (const std::exception& e)
    {
        throw gn::ExecError(c.node.name + ": " + id + " is not a JSON list: " + e.what());
    }
}

gn::NodeDecl stampsDecl()
{
    return {
        .type = "TerrainStamps",
        .label = "Terrain Stamps",
        .category = kCategory,
        .doc = "Footprint stamps: building pads and plazas, parks and yards flattened, carved, raised, blended, terraced or left on "
               "plinths and stilts, each with a skirt back to the ground, and their coverage written to masks such as cityFootprint. "
               "Each stamp's footprint is a polygon or follows a mesh or curve through an Object Info node in this group, so moving "
               "that object moves the stamp. terrainStampPad and terrainStampRegion write the list; put this after Erode Terrain so "
               "erosion never re-cuts a pad.",
        .props = {prop("stamps", {}, "[]", "The stamps, a JSON list in the heightfield's own space (see terrainStampPad).")},
        .sockets = {.inputs = {geoIn("heightfield")}, .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                std::map<std::string, StampResult> results;
                for (const nlohmann::json& j : listProp(c, "stamps"))
                {
                    Stamp s;
                    try
                    {
                        s = Stamp::fromJson(j);
                    }
                    catch (const std::exception& e)
                    {
                        throw gn::ExecError(c.node.name + ": " + e.what());
                    }
                    Footprint f;
                    if (!s.object.empty())
                    {
                        auto it = c.env.objects.find(s.object);
                        if (it == c.env.objects.end())
                            continue;
                        const glm::dmat4 toLocal = glm::inverse(c.env.ownerMatrix) * it->second.matrix;
                        if (it->second.curves)
                            f = footprintFromCurves(*it->second.curves, toLocal);
                        else if (it->second.mesh)
                            f = footprintFromMesh(*it->second.mesh, toLocal);
                    }
                    else
                        f = footprintFromPolygon(s.polygon);
                    results[s.id] = applyStamp(h, s, f);
                }
                publishStampResults(c.env.owner, c.node.name, std::move(results));
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl sculptDecl()
{
    return {
        .type = "TerrainSculpt",
        .label = "Terrain Sculpt",
        .category = kCategory,
        .doc = "Sculpt strokes replayed on the heightfield in order: raise, lower, smooth, flatten and noise, at points or along "
               "paths, with a radius and strength. terrainSculpt adds strokes; before Erode Terrain they are shaped by erosion, "
               "after it they stay as drawn.",
        .props = {prop("strokes", {}, "[]", "The strokes, a JSON list in the heightfield's own space (see terrainSculpt).")},
        .sockets = {.inputs = {geoIn("heightfield")}, .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = requireHeightfield(c, g);
                for (const nlohmann::json& j : listProp(c, "strokes"))
                {
                    try
                    {
                        applyStroke(h, SculptStroke::fromJson(j));
                    }
                    catch (const std::exception& e)
                    {
                        throw gn::ExecError(c.node.name + ": " + e.what());
                    }
                }
                c.out("heightfield", withHeightfield(g, std::move(h)));
            },
    };
}

/** Curves from JSON point lists ([[x, y, z], ...]), with named float attributes per point taken from the same rows. */
struct CurveBuilder
{
    gn::CurveSet set;
    std::map<std::string, std::vector<float>> point, curve;

    void add(const nlohmann::json& pts, bool closed, const nlohmann::json& rows = nullptr, const std::vector<std::string>& keys = {},
             const std::map<std::string, float>& perCurve = {})
    {
        std::vector<glm::dvec3> p;
        for (const nlohmann::json& q : pts)
            p.push_back({q[0].get<double>(), q[1].get<double>(), q[2].get<double>()});
        if (p.size() < 2)
            return;
        set.addCurve(p, closed);
        for (const std::string& k : keys)
            for (size_t i = 0; i < p.size(); ++i)
                point[k].push_back(rows.is_array() && i < rows.size() && rows[i].contains(k) ? rows[i][k].get<float>() : 0.0f);
        for (const auto& [k, v] : perCurve)
            curve[k].push_back(v);
    }

    GeometryPtr finish()
    {
        set.syncSizes();
        for (const auto& [k, v] : point)
            if (v.size() == set.numPoints())
                std::copy(v.begin(), v.end(), set.pointAttrs.getOrAdd<float>(k).begin());
        for (const auto& [k, v] : curve)
            if (v.size() == set.numCurves())
                std::copy(v.begin(), v.end(), set.curveAttrs.getOrAdd<float>(k).begin());
        return gn::curveGeometry(std::move(set));
    }
};

GeometryPtr scoredPoints(const nlohmann::json& list)
{
    geo::Mesh m;
    std::vector<float> score;
    for (const nlohmann::json& s : list)
    {
        const nlohmann::json& p = s["position"];
        m.addPoint({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
        score.push_back(s.value("score", 1.0f));
    }
    auto layer = m.pointAttrs().getOrAdd<float>("score");
    std::copy(score.begin(), score.end(), layer.begin());
    return gn::pointGeometry(std::move(m));
}

AnalysisParams analysisFrom(const ExecContext& c)
{
    AnalysisParams p;
    p.riverArea = std::max(1.0, c.f("riverArea"));
    if (c.b("hasSea"))
        p.seaLevel = c.f("seaLevel");
    return p;
}

std::vector<SocketDecl> analysisInputs()
{
    return {in("riverArea", Socket::Float, 250000.0, "Drainage area, m2, past which water is a river.", 1.0),
            in("hasSea", Socket::Bool, false, "There is a sea."), in("seaLevel", Socket::Float, 0.0, "Sea level, cm.")};
}

gn::NodeDecl fieldDecl()
{
    std::vector<SocketDecl> inputs = {geoIn("heightfield")};
    for (SocketDecl& s : analysisInputs())
        inputs.push_back(std::move(s));
    return {
        .type = "TerrainField",
        .label = "Terrain Field",
        .category = kCategory,
        .doc = "Writes an analysis field into a mask of the heightfield: slope, aspect, curvature, roughness, flow, flowDir, "
               "basinId, streamOrder, wet, hand, floodRisk, distanceToWater, nearestWaterId, waterDepth, rockHardness, soilDepth, "
               "excavationCost, slopeClass, buildability, prominence, viewshedScore, solar or windExposure (see terrainField). "
               "Sample Heightfield reads it back per point.",
        .props = {prop("field", Analysis::fieldNames(), "buildability", "Which field."), prop("mask", {}, "", "Mask to write; empty is the field's name.")},
        .sockets = {.inputs = inputs, .outputs = {geoOut("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                auto h = std::make_shared<const Heightfield>(requireHeightfield(c, g));
                const std::string field = c.prop("field");
                const auto an = analyse(h, analysisFrom(c));
                Heightfield out = *h;
                const std::string name = c.prop("mask").empty() ? field : c.prop("mask");
                out.mask(name) = an->field(field);
                c.out("heightfield", withHeightfield(g, std::move(out)));
            },
    };
}

gn::NodeDecl waterDecl()
{
    std::vector<SocketDecl> inputs = {geoIn("heightfield")};
    for (SocketDecl& s : analysisInputs())
        inputs.push_back(std::move(s));
    return {
        .type = "TerrainWater",
        .label = "Terrain Water",
        .category = kCategory,
        .doc = "The water on a heightfield as geometry: rivers as curves with width, depth, waterLevel, bedHeight, discharge and "
               "flowSpeed per point and streamOrder per curve; lake and sea shorelines as closed curves with waterLevel; and the "
               "sites (springs, fords, bridge sites, confluences, rapids, waterfalls) as points with a score. See terrainWaterInfo.",
        .sockets = {.inputs = inputs, .outputs = {geoOut("rivers"), geoOut("shorelines"), geoOut("sites")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                auto h = std::make_shared<const Heightfield>(requireHeightfield(c, c.geometry("heightfield")));
                const nlohmann::json w = waterGraph(*analyse(h, analysisFrom(c)), WorldMap{});
                CurveBuilder rivers, shores;
                const std::vector<std::string> keys = {"width", "depth", "waterLevel", "bedHeight", "discharge", "flowSpeed"};
                for (const nlohmann::json& r : w["rivers"])
                {
                    nlohmann::json pts = nlohmann::json::array();
                    for (const nlohmann::json& p : r["points"])
                        pts.push_back(p["position"]);
                    rivers.add(pts, false, r["points"], keys, {{"streamOrder", r["streamOrder"].get<float>()}});
                }
                auto addShores = [&](const nlohmann::json& lake) {
                    for (const nlohmann::json& ring : lake["shoreline"])
                        shores.add(ring, true, nullptr, {}, {{"waterLevel", lake["waterLevel"].get<float>()}});
                };
                for (const nlohmann::json& l : w["lakes"])
                    addShores(l);
                if (w["sea"].is_object())
                    addShores(w["sea"]);
                nlohmann::json all = nlohmann::json::array();
                for (const char* k : {"springs", "fords", "bridgeSites", "confluences", "rapids", "waterfalls"})
                    for (const nlohmann::json& p : w[k])
                        all.push_back(p);
                c.out("rivers", rivers.finish());
                c.out("shorelines", shores.finish());
                c.out("sites", scoredPoints(all));
            },
    };
}

gn::NodeDecl buildableDecl()
{
    std::vector<SocketDecl> inputs = {geoIn("heightfield"), in("minArea", Socket::Float, 2000.0, "Smallest region, m2.", 0.0),
                                      in("maxSlope", Socket::Float, 15.0, "Steepest ground, degrees.", 0.0, 90.0), in("avoidFlood", Socket::Bool, true, "Leave out ground that floods."),
                                      in("floodTolerance", Socket::Float, 0.3, "Flood risk counted as flooding.", 0.0, 1.0)};
    for (SocketDecl& s : analysisInputs())
        inputs.push_back(std::move(s));
    return {
        .type = "BuildableRegions",
        .label = "Buildable Regions",
        .category = kCategory,
        .doc = "Outlines of contiguous buildable land (see terrainBuildable) as closed curves, with area (m2), meanSlope and "
               "meanBuildability per curve; holes are their own curves with hole = 1.",
        .sockets = {.inputs = inputs, .outputs = {geoOut("regions")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                auto h = std::make_shared<const Heightfield>(requireHeightfield(c, c.geometry("heightfield")));
                const nlohmann::json regions =
                    buildableRegions(*analyse(h, analysisFrom(c)), WorldMap{}, c.f("minArea"), c.f("maxSlope"), c.b("avoidFlood"), c.f("floodTolerance"));
                CurveBuilder b;
                for (const nlohmann::json& r : regions)
                {
                    const std::map<std::string, float> attrs = {{"area", r["area"].get<float>()}, {"meanSlope", r["meanSlope"].get<float>()},
                                                                {"meanBuildability", r["meanBuildability"].get<float>()}, {"hole", 0.0f}};
                    b.add(r["outline"], true, nullptr, {}, attrs);
                    for (const nlohmann::json& hole : r["holes"])
                    {
                        std::map<std::string, float> ha = attrs;
                        ha["hole"] = 1.0f;
                        b.add(hole, true, nullptr, {}, ha);
                    }
                }
                c.out("regions", b.finish());
            },
    };
}

gn::NodeDecl sitesDecl()
{
    std::vector<SocketDecl> inputs = {geoIn("heightfield"), in("count", Socket::Int, 5, "How many.", 1, 1000), in("spacing", Socket::Float, 20000.0, "Least distance between them, cm.", 0.0)};
    for (SocketDecl& s : analysisInputs())
        inputs.push_back(std::move(s));
    return {
        .type = "TerrainSites",
        .label = "Terrain Sites",
        .category = kCategory,
        .doc = "Scored places for a kind of site (see terrainSites) as points with a score attribute, best first.",
        .props = {prop("kind", siteKinds(), "townCentre", "Kind of site.")},
        .sockets = {.inputs = inputs, .outputs = {geoOut("sites")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                auto h = std::make_shared<const Heightfield>(requireHeightfield(c, c.geometry("heightfield")));
                c.out("sites", scoredPoints(sites(*analyse(h, analysisFrom(c)), WorldMap{}, c.prop("kind"), int(c.i("count")), c.f("spacing"))));
            },
    };
}
} // namespace

bool registerNodes(ext::Context& ctx)
{
    for (gn::NodeDecl d : {gridDecl(), noiseDecl(), setHeightDecl(), setMaskDecl(), sampleDecl(), toMeshDecl(), fromMeshDecl(), blendDecl(),
                           resampleDecl(), infoDecl(), readDecl(), textureGraphDecl(), erodeDecl(), flowDecl(), thermalDecl(), elevationDecl(), imageDecl(),
                           stampsDecl(), sculptDecl(), fieldDecl(), waterDecl(), buildableDecl(), sitesDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace terrain
