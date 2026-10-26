// Masonry: the Masonry Wall geometry node, stones laid in courses along footprint lines as instances, plus the mortar
// core they sit in. Every tool and command builds a group around this node.

#include "Masonry.h"
#include "MasonryCore.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Profile.h>

#include <algorithm>

using namespace lila;

namespace masonry
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;
using T = gn::SocketType;

gn::SocketDecl in(std::string id, T t, gn::json def, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}

gn::SocketDecl geoSocket(std::string id, std::string doc) { return {.id = std::move(id), .type = T::Geometry, .doc = std::move(doc)}; }

void wallExec(ExecContext& c)
{
    LILA_ZONE();

    const std::vector<Footprint> fps = footprintsOf(*c.geometry("footprint"));
    const BuildOptions o = optionsFrom([&](const char* id) { return c.f(id); }, [&](const char* id) { return c.i(id); }, c.s("bond"));
    Built b = buildWalls(fps, o);

    auto inst = std::make_shared<gn::Instances>();
    for (geo::Mesh& m : b.meshes)
        inst->references.push_back(gn::meshGeometry(std::move(m)));
    for (size_t k = 0; k < b.stones.size(); ++k)
        inst->add(stoneMatrix(b.stones[k]), b.reference[k]);
    writeStoneAttrs(inst->attrs, b);

    Geometry stones;
    stones.instances = std::move(inst);
    GeometryPtr stonesPtr = gn::makeGeometry(std::move(stones));
    GeometryPtr corePtr = gn::meshGeometry(std::move(b.core));
    c.out("stones", stonesPtr);
    c.out("core", corePtr);
    const GeometryPtr parts[] = {stonesPtr, corePtr};
    c.out("geometry", gn::makeGeometry(gn::join(parts)));
}

gn::NodeDecl wallDecl()
{
    const Units u;
    const WallParams w;
    const BuildOptions o;
    std::string bonds;
    for (const std::string& n : bondNames())
        bonds += (bonds.empty() ? "" : ", ") + n;
    return {
        .type = "MasonryWall",
        .label = "Masonry Wall",
        .category = "Masonry",
        .doc = "Lays stones in courses along footprint lines (curves, or a mesh's loose edges) and builds the mortar core they "
               "sit in. Corners turn by alternating which wall runs through on each course; cut stones land mid wall. Stones are "
               "instances sharing a few meshes, with attributes stone, wall, course, role (0 stretcher, 1 header, 2 bat, 3 closer, "
               "4 filler), face (0 outer, 1 inner, 2 through), random, variant and size. Lengths are cm.",
        .sockets = {.inputs = {geoSocket("footprint", "Lines the wall stands on: curves, or a mesh's loose edges. Y is up."),
                               in("bond", T::String, "running", "How stones are arranged: " + bonds + "."),
                               in("height", T::Float, w.height, "Wall height."),
                               in("thickness", T::Float, w.thickness, "Wall thickness. Under 1.5 stone depths the wall is one stone thick."),
                               in("stoneLength", T::Float, u.length, "Stone length along the face."),
                               in("stoneHeight", T::Float, u.height, "Stone height; courses are fitted to the wall height."),
                               in("stoneDepth", T::Float, u.depth, "Stone depth into the wall, and a header's width on the face."),
                               in("joint", T::Float, u.joint, "Mortar joint."),
                               in("recess", T::Float, w.recess, "How far the mortar sits behind the stone faces."),
                               in("bevel", T::Float, o.bevel, "Stone edge chamfer."),
                               in("wear", T::Float, o.wear, "0 to 1: how knocked about stone corners are."),
                               in("jitter", T::Float, w.jitter, "0 to 1: how far stones wander and tilt inside their joints."),
                               in("protrude", T::Float, w.protrude, "Stones pushed out of the face by up to this."),
                               in("cornerAngle", T::Float, w.cornerAngle, "Turns sharper than this, degrees, are corners; gentler ones curve."),
                               in("variants", T::Int, o.variants, "How many differently worn versions of each stone size."),
                               in("detail", T::Int, 0, "0 worn, 1 clean chamfer, 2 plain boxes (fast preview)."),
                               in("seed", T::Int, w.seed, "Another seed gives other wander, wear and cut positions.")},
                    .outputs = {geoSocket("geometry", "Stones and core together."), geoSocket("stones", "The stones, as instances."),
                                geoSocket("core", "The mortar core, a mesh.")}},
        .exec = wallExec,
    };
}
} // namespace

std::vector<Footprint> footprintsOf(const gn::Geometry& g)
{
    std::vector<Footprint> out;
    if (g.curves)
    {
        const gn::CurveSet ev = g.curves->evaluated();
        for (size_t k = 0; k < ev.numCurves(); ++k)
        {
            Footprint f;
            const auto pts = ev.curvePoints(k);
            f.points.assign(pts.begin(), pts.end());
            f.closed = ev.cyclic[k] != 0;
            if (f.points.size() >= 2)
                out.push_back(std::move(f));
        }
    }
    if (g.mesh)
        for (Footprint& f : footprintsFromEdges(*g.mesh))
            out.push_back(std::move(f));
    return out;
}

BuildOptions optionsFrom(const std::function<double(const char*)>& f, const std::function<int64_t(const char*)>& i,
                         const std::string& bond)
{
    BuildOptions o;
    if (!parseBond(bond, &o.wall.bond))
    {
        std::string names;
        for (const std::string& n : bondNames())
            names += (names.empty() ? "" : ", ") + n;
        throw gn::ExecError("Masonry Wall: unknown bond '" + bond + "' (" + names + ")");
    }
    o.wall.height = std::max(0.0, f("height"));
    o.wall.thickness = std::max(1.0, f("thickness"));
    o.wall.stone.length = std::max(1.0, f("stoneLength"));
    o.wall.stone.height = std::max(0.5, f("stoneHeight"));
    o.wall.stone.depth = std::max(0.5, f("stoneDepth"));
    o.wall.stone.joint = std::max(0.0, f("joint"));
    o.wall.recess = std::max(0.0, f("recess"));
    o.wall.jitter = std::clamp(f("jitter"), 0.0, 1.0);
    o.wall.protrude = std::max(0.0, f("protrude"));
    o.wall.cornerAngle = std::clamp(f("cornerAngle"), 1.0, 179.0);
    o.wall.seed = uint32_t(i("seed"));
    o.bevel = std::max(0.0, f("bevel"));
    o.wear = std::clamp(f("wear"), 0.0, 1.0);
    o.variants = int(std::clamp<int64_t>(i("variants"), 1, 64));
    o.detail = Detail(std::clamp<int64_t>(i("detail"), 0, 2));
    return o;
}

void writeStoneAttrs(geo::AttributeSet& attrs, const Built& b)
{
    // Every layer first: adding one may move the others, which would leave earlier spans dangling.
    for (const char* name : {"stone", "wall", "course", "role", "face", "variant"})
        attrs.add(name, geo::AttrType::Int);
    attrs.add("random", geo::AttrType::Float);
    attrs.add("size", geo::AttrType::Float3);
    std::span<int32_t> stone = attrs.get<int32_t>("stone");
    std::span<int32_t> wall = attrs.get<int32_t>("wall");
    std::span<int32_t> course = attrs.get<int32_t>("course");
    std::span<int32_t> role = attrs.get<int32_t>("role");
    std::span<int32_t> face = attrs.get<int32_t>("face");
    std::span<int32_t> variant = attrs.get<int32_t>("variant");
    std::span<float> random = attrs.get<float>("random");
    std::span<glm::vec3> size = attrs.get<glm::vec3>("size");
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const Stone& s = b.stones[k];
        stone[k] = int32_t(k);
        wall[k] = s.wall;
        course[k] = s.course;
        role[k] = int32_t(s.role);
        face[k] = int32_t(s.face);
        variant[k] = b.variant[k];
        random[k] = s.random;
        size[k] = glm::vec3(s.size);
    }
}

bool registerNodes(ext::Context& ctx) { return gn::registerDecl(ctx, wallDecl()) != nullptr; }
} // namespace masonry

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!masonry::registerNodes(ctx))
        return 1;
    masonry::registerCommands(ctx);
    masonry::registerUi(ctx);
    return 0;
}
