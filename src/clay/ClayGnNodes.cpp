#include "clay/ClayNodes.h"

#include "core/Profile.h"
#include "gn/NodesCommon.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace lila::clay
{
namespace
{
using namespace gn;
using namespace gn::nodes;
using D3 = glm::dvec3;

SocketDecl blendIn() { return bounded(in("blend", SocketType::Float, 2.0, "Radius of the smooth joins, cm; 0 is a hard edge."), 0.0); }
SocketDecl clayVoxelIn() { return bounded(in("voxelSize", SocketType::Float, 0.5, "Lattice spacing, cm. Smaller is finer and slower."), 0.01); }

MeshOptions meshOptions(const ExecContext& c) { return {.voxelSize = std::max(c.f("voxelSize"), 0.01), .sharp = c.b("sharpEdges")}; }

void output(ExecContext& c, const Item& root)
{
    try
    {
        c.out("mesh", meshGeometry(mesh(Field(root), meshOptions(c))));
    }
    catch (const std::exception& e)
    {
        throw ExecError(c.node.name + ": " + e.what());
    }
}

/** A round cone from a (radius ra) to b (radius rb), as a clay capsule. */
Item span(const D3& a, const D3& b, double ra, double rb)
{
    const double len = glm::length(b - a);
    const D3 dir = len > 1e-12 ? (b - a) / len : D3(0, 1, 0);
    return {.matrix = glm::translate(glm::dmat4(1.0), 0.5 * (a + b)) * glm::toMat4(glm::rotation(D3(0, 1, 0), dir)),
            .shape = Primitive{.shape = Shape::Capsule, .radius = ra, .endRadius = rb, .length = len}};
}

/** Spheres at the points (the points component, or the mesh's vertices), radius from the field. */
void addPoints(ExecContext& c, const GeometryPtr& g, Op op, double blend, std::vector<Item>& out)
{
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
        return;
    std::vector<double> radii = evaluateAll<double>(c.field("pointRadius"), ctx);
    size_t k = 0;
    for (Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
        {
            const double r = radii.empty() ? 0.0 : radii[std::min(k++, radii.size() - 1)];
            out.push_back({.op = op, .blend = blend, .matrix = glm::translate(glm::dmat4(1.0), m->position(p)),
                           .shape = Primitive{.shape = Shape::Sphere, .radius = std::max(r, 0.0)}});
        }
}

/** A tube along each curve, radius from the field point by point. */
void addCurves(ExecContext& c, const GeometryPtr& g, Op op, double blend, std::vector<Item>& out)
{
    if (!g->curves || g->curves->numCurves() == 0)
        return;
    const CurveSet curves = g->curves->evaluated();
    std::vector<double> radii = evaluateAll<double>(c.field("curveRadius"), FieldContext::forCurves(curves, Domain::Point));
    auto radius = [&](size_t i) { return std::max(radii.empty() ? 0.0 : radii[std::min(i, radii.size() - 1)], 0.0); };
    for (size_t cv = 0; cv < curves.numCurves(); ++cv)
    {
        const size_t first = curves.offsets[cv], n = curves.curveSize(cv);
        // A curve's spans share their end spheres, so they join without a blend; the tube blends with everything else.
        Item tube{.op = op, .blend = blend};
        if (n == 1)
            tube.children.push_back({.matrix = glm::translate(glm::dmat4(1.0), curves.positions[first]),
                                     .shape = Primitive{.shape = Shape::Sphere, .radius = radius(first)}});
        const size_t spans = curves.cyclic[cv] && n > 2 ? n : n - 1;
        for (size_t s = 0; s < spans && n > 1; ++s)
        {
            const size_t a = first + s, b = first + (s + 1) % n;
            tube.children.push_back(span(curves.positions[a], curves.positions[b], radius(a), radius(b)));
        }
        out.push_back(std::move(tube));
    }
}

void clayNode()
{
    registerDecl({
        .type = "Clay",
        .label = "Clay",
        .category = "Mesh",
        .doc = "Melts spheres at points and tubes along curves into one surface with smooth joins: blobby creatures, vines, roots, "
               "horns. Either input can cut instead, carving into the other.",
        .sockets = {.inputs = {geoIn("points"), bounded(fin("pointRadius", SocketType::Float, 5.0), 0.0),
                               in("cutWithPoints", SocketType::Bool, false, "The spheres carve into the tubes instead of adding."),
                               geoIn("curves"), bounded(fin("curveRadius", SocketType::Float, 2.0), 0.0),
                               in("cutWithCurves", SocketType::Bool, false, "The tubes carve into the spheres instead of adding."),
                               blendIn(), clayVoxelIn(), in("sharpEdges", SocketType::Bool, false, "Keep hard joins sharp (dual contouring).")},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE_N("Clay");
                const double blend = std::max(c.f("blend"), 0.0);
                const bool cutPoints = c.b("cutWithPoints"), cutCurves = c.b("cutWithCurves");
                // Cuts go last so they carve whatever the other input added.
                std::vector<Item> adds, cuts;
                addPoints(c, c.geometry("points"), cutPoints ? Op::Subtract : Op::Add, blend, cutPoints ? cuts : adds);
                addCurves(c, c.geometry("curves"), cutCurves ? Op::Subtract : Op::Add, blend, cutCurves ? cuts : adds);
                Item root;
                root.children = std::move(adds);
                root.children.insert(root.children.end(), std::make_move_iterator(cuts.begin()), std::make_move_iterator(cuts.end()));
                output(c, root);
            },
    });
}
} // namespace

void registerGnNodes()
{
    clayNode();
}
} // namespace lila::clay
