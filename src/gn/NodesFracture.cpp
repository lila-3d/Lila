#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "scene/Fracture.h"

#include <algorithm>
#include <cmath>

namespace lila::gn::nodes
{
namespace
{
namespace fr = scene::fracture;

/** The axis the bounds are thinnest along, for a glass pane's normal. */
glm::dvec3 thinnestAxis(const geo::Bounds& b)
{
    const glm::dvec3 s = b.size();
    if (s.x <= s.y && s.x <= s.z)
        return {1, 0, 0};
    return s.y <= s.z ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
}

int longestAxis(const geo::Bounds& b)
{
    const glm::dvec3 s = b.size();
    if (s.x >= s.y && s.x >= s.z)
        return 0;
    return s.y >= s.z ? 1 : 2;
}
} // namespace

void registerFractureNode()
{
    registerDecl({
        .type = "Fracture",
        .label = "Fracture",
        .category = "Destruction",
        .doc = "Breaks a closed mesh into pieces the way a material cracks, and links the pieces that touch so the Rigid Body "
               "Solver holds them together until they break. Concrete gives chunky rough pieces, glass shards radiating from "
               "the impact, wood long splinters along the grain. The same inputs always give the same pieces.",
        .props = {prop("material", {"concrete", "glass", "wood"}, "concrete", "How it cracks."),
                  prop("grain", {"auto", "x", "y", "z"}, "auto", "Wood: the axis the grain runs along; auto takes the longest."),
                  prop("linkType", pieceLinkTypes(), "glue", "Links between touching pieces: glue holds until it breaks, spring bends first, hard never breaks.")},
        .sockets = {.inputs = {geoIn("mesh", "A closed mesh."), bounded(in("pieces", T::Int, 30, "Roughly how many pieces."), 1, 20000),
                               in("seed", T::Int, 1, "Another seed gives another pattern."),
                               in("impact", T::Vector, vec(0, 0, 0), "Where it's hit, in the mesh's space, cm. Pieces are smaller near it."),
                               bounded(in("impactFocus", T::Float, 0.0, "0 spreads pieces evenly; toward 1 packs small pieces around the impact."), 0.0, 1.0),
                               bounded(in("impactRadius", T::Float, 50.0, "How far the small pieces reach from the impact, cm."), 1e-3),
                               bounded(in("roughness", T::Float, 1.0, "How jagged the cracks are, cm. Glass ignores it."), 0.0),
                               bounded(in("roughnessSize", T::Float, 10.0, "Size of the bumps along a crack, cm."), 0.1),
                               bounded(in("crackDetail", T::Float, 0.0,
                                          "Edge length of the faces along a rough crack, cm: smaller is smoother and heavier. 0 picks half the "
                                          "bump size."),
                                       0.0),
                               bounded(in("maxCrackFaces", T::Int, 400, "Most faces on one piece's cracks, keeping the result light for games."), 16, 100000),
                               bounded(in("chunks", T::Float, 1.0, "Pieces glued into each chunk that never breaks apart; 1 keeps every piece separate."), 1.0),
                               in("interiorMaterial", T::String, "", "Material for the inside faces; empty names it after the mesh's material."),
                               bounded(in("interiorUvScale", T::Float, 100.0, "cm per UV tile on the inside faces."), 1e-3),
                               bounded(in("linkStrength", T::Float, 20.0, "Strength of the links, N per cm2 of the area two pieces share."), 0.0)},
                    .outputs = {geoOut("pieces", "The pieces, as instances with their origins at their centres of mass."),
                                geoOut("links", "A link between each pair of touching pieces, as two-point curves.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                const Geometry src = realize(*c.geometry("mesh"));
                if (!src.mesh || src.mesh->numFaces() == 0)
                {
                    c.out("pieces", makeGeometry({}));
                    c.out("links", makeGeometry({}));
                    return;
                }
                const geo::Mesh& mesh = *src.mesh;
                const geo::Bounds bounds = mesh.bounds();
                const std::string material = c.prop("material");
                const uint64_t seed = uint64_t(c.i("seed"));
                const int count = int(std::clamp<int64_t>(c.i("pieces"), 1, 20000));
                const glm::dvec3 impact = c.v("impact");
                const double focus = std::clamp(c.f("impactFocus"), 0.0, 1.0);

                std::vector<glm::dvec3> points;
                fr::FractureOptions fo;
                fo.seed = seed;
                fo.noiseScale = std::max(0.1, c.f("roughnessSize"));
                fo.detailSize = c.f("crackDetail") > 0.0 ? c.f("crackDetail") : fo.noiseScale * 0.5;
                fo.maxInteriorFacesPerPiece = int(std::clamp<int64_t>(c.i("maxCrackFaces"), 16, 100000));
                fo.interiorMaterial = c.s("interiorMaterial");
                fo.interiorUvScale = std::max(1e-3, c.f("interiorUvScale"));
                const double roughness = std::max(0.0, c.f("roughness"));
                if (material == "glass")
                {
                    const int rings = std::max(2, int(std::lround(std::sqrt(count / 6.0))));
                    const int spokes = std::max(3, count / rings);
                    points = fr::radialPoints(impact, thinnestAxis(bounds), glm::length(bounds.size()) * 0.5, rings, spokes, 0.35, seed);
                }
                else
                {
                    fr::ScatterOptions so;
                    so.count = count;
                    so.seed = seed;
                    if (focus > 0.0)
                    {
                        so.impacts = {impact};
                        so.impactRadius = std::max(1e-3, c.f("impactRadius"));
                        so.impactBias = focus;
                    }
                    points = fr::scatterPoints(mesh, so);
                    fo.noiseAmplitude = material == "wood" ? roughness * 0.3 : roughness;
                    if (material == "wood")
                    {
                        const std::string g = c.prop("grain");
                        const int axis = g == "x" ? 0 : g == "y" ? 1 : g == "z" ? 2 : longestAxis(bounds);
                        fo.stretch[axis] = 0.25;
                    }
                }
                fr::FractureResult r = fr::voronoiFracture(mesh, points, fo);
                if (!r.ok())
                    throw ExecError(r.error);
                const double chunks = c.f("chunks");
                if (chunks > 1.0)
                    r = fr::clusterPieces(r, chunks, seed);

                std::vector<geo::Mesh> meshes;
                std::vector<glm::dvec3> origins;
                meshes.reserve(r.pieces.size());
                for (fr::Piece& p : r.pieces)
                {
                    origins.push_back(p.centroid);
                    meshes.push_back(std::move(p.mesh));
                }
                c.out("pieces", piecesFromMeshes(std::move(meshes), origins));
                const double strength = std::max(0.0, c.f("linkStrength"));
                std::vector<PieceLink> links;
                links.reserve(r.contacts.size());
                for (const fr::Contact& k : r.contacts)
                    links.push_back({k.a, k.b, k.centroid, strength * k.area});
                c.out("links", linkGeometry(links, c.prop("linkType")));
            },
    });
}
} // namespace lila::gn::nodes
