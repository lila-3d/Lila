#include "seq/Imposter.h"

#include "core/ImageIO.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "fbx/Fbx.h"
#include "geo/Bvh.h"
#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "gn/Bake.h"
#include "gn/Gn.h"
#include "gn/NodesCommon.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "seq/RigidPieces.h"

#include <tbb/parallel_for.h>

#include <fstream>

namespace lila::seq
{
namespace
{
using json = nlohmann::json;
using namespace gn::nodes;

/** The bounding sphere of the mesh's points around its bounds centre. */
std::pair<glm::dvec3, double> sphereOf(const geo::Mesh& m)
{
    geo::Bounds b = m.bounds();
    glm::dvec3 c = b.valid() ? b.center() : glm::dvec3(0.0);
    double r = 0.0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            r = std::max(r, glm::length(m.position(p) - c));
    return {c, std::max(r, 1e-6)};
}

/** A square facing +Z through the centre, 2r across, with uv 0..1: the card an imposter material draws on. */
geo::Mesh cardMesh(const glm::dvec3& c, double r)
{
    std::vector<glm::dvec3> pts = {c + glm::dvec3(-r, -r, 0), c + glm::dvec3(r, -r, 0), c + glm::dvec3(r, r, 0), c + glm::dvec3(-r, r, 0)};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, std::vector<geo::Index>{4}, std::vector<geo::Index>{0, 1, 2, 3});
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    const glm::vec2 corner[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (geo::Index k = 0; k < m.cornerCapacity() && k < 4; ++k)
        uv[k] = corner[m.cornerPoint(k)];
    return m;
}

void imposterNode()
{
    gn::registerDecl({
        .type = "Imposter",
        .label = "Imposter",
        .category = "Game",
        .doc = "Bakes what reaches it into an octahedral imposter for Unreal: Bake in the side panel views it from Frames x Frames "
               "directions and writes a base colour atlas (alpha is coverage), a normal atlas (alpha is depth), a card mesh FBX and a "
               "JSON of the material inputs into imposters/ next to the scene. Base colour comes from the mesh's current colour set, "
               "else light grey. Geometry passes through; Card is the square the imposter draws on.",
        .props = {prop("layout", {"octahedral", "hemi-octahedral"}, "hemi-octahedral",
                       "octahedral: views from every side; hemi-octahedral: only from above, twice the detail for things seen from the ground.")},
        .sockets = {.inputs = {geoIn("geometry"), bounded(in("frames", gn::SocketType::Int, 12, "Views per side of the atlas."), 2, 32),
                               bounded(in("resolution", gn::SocketType::Int, 256, "Pixels per view."), 16, 2048)},
                    .outputs = {geoOut("geometry"), geoOut("card", "A square through the centre, as wide as the bounding sphere, facing +Z.")}},
        .exec =
            [](gn::ExecContext& c) {
                gn::recordCapture(c);
                gn::GeometryPtr g = c.geometry("geometry");
                c.out("geometry", g);
                geo::Mesh m = gn::toOutputMesh(*g);
                auto [centre, radius] = sphereOf(m);
                c.out("card", gn::meshGeometry(cardMesh(centre, radius)));
            },
        .actions = {{"Bake", "BakeImposter", "View what reaches this node from every frame's direction and write the imposter atlases."}},
    });
}

/** Unit direction toward the viewer for an octahedral uv in [-1, 1], z up. */
glm::dvec3 octDirection(double u, double v, bool hemi)
{
    glm::dvec3 d;
    if (hemi)
    {
        double x = (u + v) * 0.5, y = (u - v) * 0.5;
        d = {x, y, 1.0 - std::abs(x) - std::abs(y)};
    }
    else
    {
        d = {u, v, 1.0 - std::abs(u) - std::abs(v)};
        if (d.z < 0.0)
        {
            double x = (1.0 - std::abs(v)) * (u >= 0.0 ? 1.0 : -1.0), y = (1.0 - std::abs(u)) * (v >= 0.0 ? 1.0 : -1.0);
            d.x = x;
            d.y = y;
        }
    }
    return glm::normalize(d);
}
} // namespace

ImposterResult bakeImposter(const geo::Mesh& source, const ImposterOptions& o)
{
    LILA_ZONE();

    const glm::dmat3 toUe = unrealBasis(o.profile);
    geo::Mesh m = source;
    gn::transformMesh(m, glm::dmat4(toUe));
    if (m.numFaces() == 0)
        throw std::runtime_error("nothing to bake: no faces");
    geo::MeshBvh bvh(m);
    std::vector<glm::vec3> normals = geo::cornerNormals(m);
    std::string set = geo::colors::current(m);
    std::span<const glm::vec4> colors = set.empty() ? std::span<const glm::vec4>{} : geo::colors::values(m, set);
    auto [centre, radius] = sphereOf(m);
    const bool hemi = o.layout == "hemi-octahedral";
    const int n = o.frames, res = o.resolution, size = n * res;
    std::vector<float> color(size_t(size) * size_t(size) * 4, 0.0f), normal(size_t(size) * size_t(size) * 4, 0.0f);
    tbb::parallel_for(0, n * n, [&](int cell) {
        const int cx = cell % n, cy = cell / n;
        const glm::dvec3 d = octDirection(double(cx) / (n - 1) * 2.0 - 1.0, double(cy) / (n - 1) * 2.0 - 1.0, hemi);
        const glm::dvec3 upWorld = std::abs(d.z) > 0.999 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
        const glm::dvec3 right = glm::normalize(glm::cross(upWorld, d)), up = glm::cross(d, right);
        for (int j = 0; j < res; ++j)
            for (int i = 0; i < res; ++i)
            {
                const double px = ((i + 0.5) / res * 2.0 - 1.0) * radius, py = (1.0 - (j + 0.5) / res * 2.0) * radius;
                const glm::dvec3 origin = centre + d * (radius * 2.0) + right * px + up * py;
                geo::BvhHit hit;
                const size_t at = (size_t(cy * res + j) * size_t(size) + size_t(cx * res + i)) * 4;
                if (!bvh.raycast(origin, -d, hit, radius * 4.0))
                    continue;
                glm::vec3 nrm = glm::normalize(bvh.interpolate<glm::vec3>(normals, hit.triangle, hit.bary));
                if (glm::dot(glm::dvec3(nrm), d) < 0.0)
                    nrm = -nrm;
                glm::vec4 col = colors.empty() ? glm::vec4(0.8f, 0.8f, 0.8f, 1.0f) : bvh.interpolate<glm::vec4>(colors, hit.triangle, hit.bary);
                color[at] = col.r;
                color[at + 1] = col.g;
                color[at + 2] = col.b;
                color[at + 3] = 1.0f;
                normal[at] = nrm.x * 0.5f + 0.5f;
                normal[at + 1] = nrm.y * 0.5f + 0.5f;
                normal[at + 2] = nrm.z * 0.5f + 0.5f;
                // Depth: 0 at the near side of the bounding sphere, 1 at the far side.
                normal[at + 3] = float(std::clamp((hit.t - radius) / (2.0 * radius), 0.0, 1.0));
            }
    });

    ImposterResult r;
    const std::filesystem::path folder = paths::fromUtf8(o.folder);
    std::filesystem::create_directories(folder);
    r.baseColor = folder / (o.name + "_BaseColor.png");
    r.normal = folder / (o.name + "_Normal.png");
    r.mesh = folder / (o.name + "_Card.fbx");
    r.meta = folder / (o.name + ".json");
    image::writePng(r.baseColor, size, size, color, 8);
    image::writePng(r.normal, size, size, normal, 8);

    fbx::ExportScene es;
    es.upAxis = math::upAxis();
    fbx::ExportNode node;
    node.name = o.name + "_Card";
    auto [lilaCentre, lilaRadius] = sphereOf(source);
    node.mesh = std::make_shared<const geo::Mesh>(cardMesh(lilaCentre, lilaRadius));
    es.nodes.push_back(std::move(node));
    fbx::ExportOptions fo;
    fbx::parseProfile(o.profile, &fo.profile);
    std::string err;
    if (!fbx::writeFbx(es, r.mesh, fo, &err))
        throw std::runtime_error(err);

    r.metadata = {{"version", 1},
                  {"name", o.name},
                  {"layout", o.layout},
                  {"frames", n},
                  {"resolution", res},
                  {"atlasSize", size},
                  {"space", "unreal"},
                  {"centre", {centre.x, centre.y, centre.z}},
                  {"radius", radius},
                  {"encoding", "cell (x, y), y from the top row, has octahedral uv (x, y) / (frames - 1) * 2 - 1; "
                               "the view looks from the decoded direction toward the centre, z up"},
                  {"files", {{"baseColor", r.baseColor.filename().generic_string()}, {"normal", r.normal.filename().generic_string()},
                             {"mesh", r.mesh.filename().generic_string()}}},
                  {"materialParams", {{"FramesXY", n}, {"UpperHemisphereOnlyImpostor", hemi}, {"Default Mesh Size", radius * 2.0},
                                      {"BaseColor", r.baseColor.filename().generic_string()},
                                      {"Normal", r.normal.filename().generic_string()}}}};
    std::ofstream(r.meta) << r.metadata.dump(2);
    return r;
}

void registerImposterNode()
{
    imposterNode();
    using T = ArgType;
    ops::registerCommand({
        .name = "gnBakeImposter",
        .doc = "Bake an Imposter node: evaluate a mesh running its group at the current frame, view what reaches the node from every "
               "atlas direction and write the base colour and normal-depth atlases, a card mesh FBX and a JSON of the material inputs. "
               "Returns {files, materialParams}.",
        .positionalDoc = "group node",
        .flags = {{"folder", "", T::String, nullptr, "Where to write; defaults to imposters/ next to the scene."},
                  {"name", "", T::String, nullptr, "File base name; defaults to the mesh's."},
                  {"modifier", "", T::String, nullptr, "The modifier to evaluate when several meshes run the group; defaults to the first."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            if (a.positional.size() < 2 || !a.positional[0].is_string() || !a.positional[1].is_string())
                throw CommandError("gnBakeImposter: group and node required");
            std::string groupName = a.positional[0].get<std::string>(), nodeName = a.positional[1].get<std::string>();
            scene::Node* groupNode = gn::findGroupNode(g, groupName);
            if (!groupNode)
                throw CommandError("gnBakeImposter: no geometry-nodes group named '" + groupName + "'");
            gn::Group grp = gn::readGroup(*groupNode);
            const gn::GNode* node = grp.node(nodeName);
            if (!node || node->type != "Imposter")
                throw CommandError("gnBakeImposter: " + nodeName + " isn't an Imposter node in " + groupName);
            std::vector<scene::NodeId> mods = gn::instancesOf(g, groupNode->id());
            scene::NodeId mod = mods.empty() ? scene::kNoNode : mods.front();
            if (a.has("modifier"))
            {
                scene::Node* m = g.find(a.get<std::string>("modifier"));
                mod = m && std::find(mods.begin(), mods.end(), m->id()) != mods.end() ? m->id() : scene::kNoNode;
            }
            if (mod == scene::kNoNode)
                throw CommandError("gnBakeImposter: no mesh runs " + groupName + "; apply it to one first");
            scene::NodeId mesh = scene::modifiers::meshOf(g, mod);

            gn::Capture capture{groupName, nodeName};
            {
                gn::CaptureScope scope(capture);
                g.touch(mod);
                g.evaluateUpstream({mesh});
            }
            if (!capture.got || !capture.geometry)
                throw CommandError("gnBakeImposter: nothing reached " + nodeName + "; is it connected to the output and the modifier enabled?");

            ImposterOptions o;
            const gn::NodeDecl* d = gn::findDecl(node->type);
            o.layout = d->propValue(*node, "layout");
            auto num = [&](const char* id, int def) { return node->values.contains(id) ? int(node->values[id].get<double>()) : def; };
            o.frames = std::clamp(num("frames", 12), 2, 32);
            o.resolution = std::clamp(num("resolution", 256), 16, 2048);
            std::filesystem::path base = paths::currentSceneDir();
            if (base.empty())
                base = paths::userConfigDir() / "untitled";
            o.folder = paths::toUtf8(a.has("folder") ? paths::fromUtf8(a.get<std::string>("folder")) : base / "imposters");
            o.name = a.get<std::string>("name", g.node(mesh) ? g.node(mesh)->name() : std::string("imposter"));
            try
            {
                ImposterResult r = bakeImposter(gn::toOutputMesh(*capture.geometry), o);
                return {{"files", {{"baseColor", paths::toUtf8(r.baseColor)}, {"normal", paths::toUtf8(r.normal)}, {"mesh", paths::toUtf8(r.mesh)},
                                   {"meta", paths::toUtf8(r.meta)}}},
                        {"materialParams", r.metadata["materialParams"]}};
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnBakeImposter: ") + e.what());
            }
        },
    });
}
} // namespace lila::seq
