#include "uv/UvCommands.h"

#include "cmds/Command.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/Align.h"
#include "uv/Bff.h"
#include "uv/OptCuts.h"
#include "uv/Optimize.h"
#include "uv/SymmetryUv.h"
#include "uv/Uv.h"
#include "uv/UvEdit.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lila::uv
{
namespace
{
using scene::ComponentMode;
using scene::Node;
using Ids = scene::components::Ids;

std::string uvSetOf(const Args& a)
{
    return a.has("uvSet") ? a.get<std::string>("uvSet") : currentUvSet();
}

FlagDef uvSetFlag()
{
    return {"uvSet", "", ArgType::String, nullptr, "UV set; the current one (uvSetCurrent) when not given."};
}

std::vector<Chart> uvIslands(const geo::Mesh& m, std::span<const Index> faces, const std::string& set)
{
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = set;
    return buildCharts(m, faces, co);
}

std::vector<uint8_t> edgeMask(const geo::Mesh& m, const Ids& edges)
{
    std::vector<uint8_t> mask(m.edgeCapacity(), 0);
    for (Index e : edges)
        if (e < mask.size())
            mask[e] = 1;
    return mask;
}

/** Faces of every uv island that has one of the edges. */
std::vector<Index> islandFacesOfEdges(const geo::Mesh& m, const Ids& edges)
{
    std::vector<Index> faces;
    for (Index e : edges)
        if (m.edgeValid(e))
            m.forEachEdgeCorner(e, [&](Index c) { faces.push_back(m.cornerFace(c)); });
    return faces;
}

OptimizeMode modeArg(const Args& a)
{
    std::string mode = a.get<std::string>("mode", "blend");
    if (mode == "conformal")
        return OptimizeMode::Conformal;
    if (mode == "authalic")
        return OptimizeMode::Authalic;
    if (mode == "isometric")
        return OptimizeMode::Isometric;
    if (mode == "blend")
        return OptimizeMode::Blend;
    throw CommandError("mode must be conformal, authalic, blend or isometric");
}

std::vector<FlagDef> modeFlags()
{
    using T = ArgType;
    return {{"mode", "", T::String, "blend",
             "conformal keeps angles, authalic keeps areas, blend mixes them by mix, isometric is SLIM (symmetric Dirichlet).",
             {"conformal", "authalic", "blend", "isometric"}},
            {"mix", "", T::Float, 0.5, "Blend only: 0 keeps angles, 1 keeps areas."},
            {"iterations", "", T::Int, 20, "Solver iterations."}};
}

OptimizeOptions optimizeArgs(const Args& a)
{
    OptimizeOptions o;
    o.mode = modeArg(a);
    o.mix = std::clamp(a.get<double>("mix", 0.5), 0.0, 1.0);
    o.iterations = std::clamp(a.get<int>("iterations", 20), 1, 500);
    o.usePins = a.get<bool>("pins", true);
    return o;
}

/** The selected points of a mesh node when vertices are being picked (Polygon corners). */
std::vector<Index> selectedPoints(const Node& n)
{
    if (scene::components::mode() != ComponentMode::Vertex)
        return {};
    const Ids& ids = scene::components::selected(n.id(), ComponentMode::Vertex);
    return {ids.begin(), ids.end()};
}

bool usableUvs(const Chart& ch)
{
    double a = std::abs(ch.areaUv());
    return std::isfinite(a) && a > 1e-14 * std::max(1.0, ch.area3d()) && foldedTriangles(ch) == 0;
}
} // namespace

void registerOptimizeCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    cmds::registerCommand({
        .name = "uvBff",
        .doc = "Boundary First Flattening of the selected faces' islands: conformal, with the boundary chosen first. free keeps the boundary "
               "lengths (least area distortion), disk makes a circle, rectangle straight sides between four corners (the selected vertices, "
               "else the sharpest), polygon one straight side per selected corner. cones adds automatic cone points that cut the island "
               "open to cut area distortion. Islands keep their place unless pack=True or they had no uvs.",
        .flags = {uvSetFlag(),
                  {"boundary", "", T::String, "free", "free, disk, rectangle or polygon.", {"free", "disk", "rectangle", "polygon"}},
                  {"cones", "", T::Int, 0, "Cone points placed automatically (free boundary only); each adds a short seam."},
                  {"exactLengths", "", T::Bool, false, "free boundary: bend rather than stretch to close it, so its lengths are exact."},
                  {"pack", "", T::Bool, false, "Pack the islands into 0-1 afterwards."}},
        .run = [](const Args& a) -> json {
            std::string boundary = a.get<std::string>("boundary", "free");
            BffOptions bo;
            if (boundary == "free")
                bo.boundary = BffBoundary::MinimalArea;
            else if (boundary == "disk")
                bo.boundary = BffBoundary::Disk;
            else if (boundary == "rectangle" || boundary == "polygon")
                bo.boundary = BffBoundary::Polygon;
            else
                throw CommandError("uvBff: boundary must be free, disk, rectangle or polygon");
            bo.autoCones = std::clamp(a.get<int>("cones", 0), 0, 64);
            bo.exactLengths = a.get<bool>("exactLengths", false);
            const bool pack = a.get<bool>("pack", false), rectangle = boundary == "rectangle";
            std::string set = uvSetOf(a);
            return editUvMeshes("UV BFF", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                LILA_ZONE_N("uvBff");
                std::vector<Index> faces = liveFaces(m, f);
                Index opened = cutToDisks(m, faces);
                ChartOptions co;
                co.uvSet = set;
                std::vector<Chart> charts = buildCharts(m, faces, co);
                std::vector<Index> corners = selectedPoints(n);
                std::atomic<size_t> failed{0}, cones{0};
                std::vector<uint8_t> place(charts.size(), 0);
                tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
                    Chart& ch = charts[i];
                    const bool had = usableUvs(ch);
                    const glm::dvec2 c0 = ch.uvCentroid();
                    const double a0 = std::abs(ch.areaUv());
                    BffOptions o = bo;
                    for (Index v = 0; v < ch.vertexCount(); ++v)
                        if (std::find(corners.begin(), corners.end(), ch.vertexPoint[v]) != corners.end())
                            o.corners.push_back(v);
                    if (rectangle && o.corners.size() != 4)
                        o.corners.clear();
                    Chart trial = ch;
                    BffResult r = bff(trial, o);
                    if (!r.ok)
                    {
                        ++failed;
                        return;
                    }
                    ch = std::move(trial);
                    cones += r.cones.size();
                    if (had && !pack)
                        fitChart(ch, c0, a0);
                    else
                        place[i] = 1;
                });
                std::vector<Chart> toPack;
                for (size_t i = 0; i < charts.size(); ++i)
                    if (place[i])
                        toPack.push_back(std::move(charts[i]));
                if (!toPack.empty())
                    packCharts(toPack, {});
                for (size_t i = 0, k = 0; i < charts.size(); ++i)
                    if (place[i])
                        charts[i] = std::move(toPack[k++]);
                writeCharts(m, charts, set);
                if (cones > 0)
                    markSeamsFromUvs(m, faces, set);
                return {{"islands", charts.size()}, {"failed", failed.load()}, {"cones", cones.load()}, {"seamsAdded", opened}};
            });
        },
    });

    cmds::registerCommand({
        .name = "uvOptimize",
        .doc = "Optimise the selected uv islands from where they are: conformal (keep angles), authalic (keep areas), blend (both, weighted by "
               "mix) or isometric (SLIM). Islands keep their place, size and pins, and never fold.",
        .flags = [] {
            std::vector<FlagDef> f{uvSetFlag()};
            for (FlagDef& d : modeFlags())
                f.push_back(std::move(d));
            f.push_back({"pins", "", ArgType::Bool, true, "Respect pinned uvs (corner attribute uvPin)."});
            return f;
        }(),
        .run = [](const Args& a) -> json {
            OptimizeOptions o = optimizeArgs(a);
            std::string set = uvSetOf(a);
            return editUvMeshes("UV Optimize", K::Face, [&](geo::Mesh& m, const Ids& f, Node&) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvOptimize: no " + set + " uvs; unwrap first");
                std::vector<Chart> charts = uvIslands(m, f, set);
                std::vector<OptimizeResult> res(charts.size());
                tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) { res[i] = optimizeChart(charts[i], o); });
                writeCharts(m, charts, set);
                double area[2] = {0.0, 0.0}, angle[2] = {0.0, 0.0}, w = 0.0;
                size_t failed = 0;
                for (size_t i = 0; i < charts.size(); ++i)
                {
                    if (!res[i].ok)
                    {
                        ++failed;
                        continue;
                    }
                    double a3 = charts[i].area3d();
                    area[0] += a3 * res[i].before.area;
                    area[1] += a3 * res[i].after.area;
                    angle[0] += a3 * res[i].before.angle;
                    angle[1] += a3 * res[i].after.angle;
                    w += a3;
                }
                w = std::max(w, 1e-300);
                return {{"islands", charts.size()},
                        {"failed", failed},
                        {"areaDistortion", {area[0] / w, area[1] / w}},
                        {"angleDistortion", {angle[0] / w, angle[1] / w}}};
            });
        },
    });

    cmds::registerCommand({
        .name = "uvMosaic",
        .doc = "Split the selected uv islands that cannot flatten below threshold along short, hidden, creased cuts, then re-solve every piece with "
               "the optimise mode. Pieces keep the island's scale; the largest stays in place, the rest line up beside it.",
        .flags = [] {
            std::vector<FlagDef> f{uvSetFlag(),
                                   {"threshold", "", ArgType::Float, 0.08, "Largest stretch (symmetric Dirichlet excess) an island may keep."},
                                   {"visibility", "", ArgType::Bool, true, "Hide cuts where they are least visible."}};
            for (FlagDef& d : modeFlags())
                f.push_back(std::move(d));
            return f;
        }(),
        .run = [](const Args& a) -> json {
            MosaicOptions o;
            o.threshold = std::max(0.0, a.get<double>("threshold", 0.08));
            o.visibility = a.get<bool>("visibility", true);
            o.optimize = optimizeArgs(a);
            std::string set = uvSetOf(a);
            return editUvMeshes("UV Mosaic", K::Face, [&](geo::Mesh& m, const Ids& f, Node&) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvMosaic: no " + set + " uvs; unwrap first");
                MosaicResult r = mosaicFaces(m, f, set, o);
                return {{"islandsBefore", r.islandsBefore}, {"islandsAfter", r.islandsAfter}, {"split", r.split}};
            });
        },
    });

    cmds::registerCommand({
        .name = "uvUnwrapSymmetric",
        .doc = "Symmetry-aware unwrap of the selected faces using the mesh's symmetry pairs: islands on one side are solved and mirrored onto "
               "their partners (flip lays the mirror image beside, stack shares the same uvs), islands across the plane come out exactly "
               "mirror symmetric. Seams are mirrored first so islands pair up.",
        .flags = {uvSetFlag(),
                  {"axis", "", T::String, "auto", "x, y, z, or auto: the mesh's symmetry axis, else x.", {"auto", "x", "y", "z"}},
                  {"mode", "", T::String, "flip", "flip or stack.", {"flip", "stack"}},
                  {"method", "", T::String, "slim", "slim, conformal or bff.", {"slim", "conformal", "bff"}},
                  {"mirrorSeams", "", T::Bool, true, "Copy seams across the plane first."},
                  {"pack", "", T::Bool, false, "Pack into 0-1 afterwards."}},
        .run = [](const Args& a) -> json {
            SymmetryUvOptions o;
            std::string axis = a.get<std::string>("axis", "auto");
            std::string mode = a.get<std::string>("mode", "flip");
            std::string method = a.get<std::string>("method", "slim");
            o.mode = mode == "stack" ? MirrorUvMode::Stack : MirrorUvMode::Flip;
            o.unwrap.method = method == "conformal" ? UnwrapMethod::Conformal : method == "bff" ? UnwrapMethod::Bff : UnwrapMethod::Slim;
            o.mirrorSeams = a.get<bool>("mirrorSeams", true);
            o.pack = a.get<bool>("pack", false);
            std::string set = uvSetOf(a);
            return editUvMeshes("UV Unwrap Symmetric", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                SymmetryUvOptions oo = o;
                if (axis == "x" || axis == "y" || axis == "z")
                    oo.axis = axis[0] - 'x';
                else
                {
                    int64_t stored = n.as<int64_t>(scene::MeshNode::kSymmetry);
                    oo.axis = stored >= 1 && stored <= 3 ? int(stored - 1) : 0;
                }
                SymmetryUvResult r = unwrapSymmetric(m, f, set, oo);
                return {{"islands", r.islands},
                        {"pairs", r.pairs},
                        {"acrossPlane", r.selfSymmetric},
                        {"unpaired", r.asymmetric},
                        {"seamsAdded", r.seamsAdded},
                        {"axis", std::string(1, char('x' + oo.axis))}};
            });
        },
    });

    cmds::registerCommand({
        .name = "uvStraightenEdges",
        .doc = "Straighten the selected edge chains in uv (edge loops, borders, seam sides): each goes onto its best-fit line with its uv length "
               "kept, and the rest of its island is re-solved around it.",
        .flags = {uvSetFlag(), {"iterations", "", T::Int, 12, "SLIM iterations for the re-solve."}},
        .run = [](const Args& a) -> json {
            std::string set = uvSetOf(a);
            int iterations = std::clamp(a.get<int>("iterations", 12), 0, 200);
            return editUvMeshes("UV Straighten Edges", K::Edge, [&](geo::Mesh& m, const Ids& e, Node&) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvStraightenEdges: no " + set + " uvs; unwrap first");
                std::vector<uint8_t> sel = edgeMask(m, e);
                std::vector<Chart> charts = uvIslands(m, islandFacesOfEdges(m, e), set);
                std::atomic<size_t> chains{0};
                tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) { chains += straightenEdges(charts[i], m, sel, iterations); });
                writeCharts(m, charts, set);
                return {{"chains", chains.load()}};
            }, false);
        },
    });

    cmds::registerCommand({
        .name = "uvAlignToEdge",
        .doc = "Rotate each island with selected edges so their direction lies along U or V: axis=u or v aligns an island side, nearest "
               "orients the island by the edge.",
        .flags = {uvSetFlag(), {"axis", "", T::String, "nearest", "u, v, or nearest.", {"nearest", "u", "v"}}},
        .run = [](const Args& a) -> json {
            std::string set = uvSetOf(a);
            std::string axis = a.get<std::string>("axis", "nearest");
            AlignAxis ax = axis == "u" ? AlignAxis::U : axis == "v" ? AlignAxis::V : AlignAxis::Nearest;
            return editUvMeshes("UV Align To Edge", K::Edge, [&](geo::Mesh& m, const Ids& e, Node&) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvAlignToEdge: no " + set + " uvs; unwrap first");
                std::vector<uint8_t> sel = edgeMask(m, e);
                std::vector<Chart> charts = uvIslands(m, islandFacesOfEdges(m, e), set);
                size_t turned = 0;
                for (Chart& ch : charts)
                    turned += alignToEdges(ch, m, sel, ax) ? 1 : 0;
                writeCharts(m, charts, set);
                return {{"islands", turned}};
            }, false);
        },
    });
}
} // namespace lila::uv
