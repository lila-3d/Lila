// One of each scene-side extension point, each registered through the Context so unloading removes it.

#include <lila/AnimCurve.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/FileTranslator.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/IkSolver.h>
#include <lila/LinkExporter.h>
#include <lila/Mcp.h>
#include <lila/SceneFile.h>

#include <glm/gtx/quaternion.hpp>

#include <fstream>
#include <memory>
#include <sstream>

using namespace lila;

namespace
{
std::filesystem::path utf8Path(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

/** .pts: one "name x y z" line per locator. */
json importPoints(const Args& a)
{
    if (a.positional.size() != 1)
        throw CommandError("scenePointsImport: give one path");
    std::ifstream in(utf8Path(a.positional[0].get<std::string>()));
    if (!in)
        throw CommandError("scenePointsImport: can't read the file");
    scene::Graph& g = scene::Graph::current();
    json made = json::array();
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream row(line);
        std::string name;
        glm::dvec3 p{0.0};
        if (!(row >> name >> p.x >> p.y >> p.z))
            continue;
        scene::Node* n = g.createNode(*scene::types::find("locator"), name);
        g.setValue(g.plug(n->name() + ".translate"), p);
        made.push_back(n->name());
    }
    return made;
}

json exportPoints(const Args& a)
{
    if (a.positional.size() != 1)
        throw CommandError("scenePointsExport: give one path");
    scene::Graph& g = scene::Graph::current();
    std::ofstream out(utf8Path(a.positional[0].get<std::string>()));
    for (scene::NodeId id : g.selection())
    {
        const scene::Node* n = g.node(id);
        int t = n->type().attrIndex("translate");
        if (t < 0)
            continue;
        const glm::dvec3& p = n->as<glm::dvec3>(t);
        out << n->name() << ' ' << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    return int64_t(g.selection().size());
}

/** Aims the start joint at the target; the rest keep their local rotations. */
scene::ik::Solution aimSolve(const scene::ik::Problem& p)
{
    auto rotationOf = [](const glm::dmat4& m) {
        return glm::quat_cast(glm::dmat3(glm::normalize(glm::dvec3(m[0])), glm::normalize(glm::dvec3(m[1])), glm::normalize(glm::dvec3(m[2]))));
    };
    scene::ik::Solution local;
    glm::dquat parent = rotationOf(p.parentWorld);
    const glm::dvec3 start(p.world.front()[3]);
    const glm::dvec3 end(p.world.back()[3]);
    for (size_t i = 0; i + 1 < p.world.size(); ++i)
    {
        glm::dquat world = rotationOf(p.world[i]);
        if (i == 0 && glm::length(end - start) > 1e-9 && glm::length(p.target - start) > 1e-9)
            world = glm::rotation(glm::normalize(end - start), glm::normalize(p.target - start)) * world;
        local.push_back(glm::inverse(parent) * world);
        parent = world;
    }
    return local;
}

/** Moves every point by offset times amount. */
class LiftDeformer final : public scene::DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        const glm::dvec3 offset = as<glm::dvec3>(attr("offset")) * num("amount");
        for (glm::dvec3& p : points)
            p += offset;
    }
};

/** A note a plug can carry and scene files can store. */
struct Note final : scene::Data
{
    std::string text;
    const char* kind() const override { return "scenePointsNote"; }
    json toJson() const override { return {{"kind", "scenePointsNote"}, {"text", text}}; }
};

json gUiSection = nullptr;
int gScriptChanges = 0;
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    using T = ArgType;
    if (!ctx.registerCommand({.name = "scenePointsImport",
                              .doc = "Import a .pts file: a locator per \"name x y z\" line.",
                              .positionalDoc = "path",
                              .run = importPoints}) ||
        !ctx.registerCommand({.name = "scenePointsExport",
                              .doc = "Write the selection's translates to a .pts file.",
                              .positionalDoc = "path",
                              .undoable = false,
                              .run = exportPoints}))
        return 2;

    if (!io::translators::add(ctx, {.id = "scenePoints",
                                    .label = "Point lists",
                                    .doc = "Locators as \"name x y z\" lines.",
                                    .extensions = {".pts"},
                                    .import = {.command = "scenePointsImport"},
                                    .exportSelection = {.command = "scenePointsExport"}}))
        return 3;

    scene::anim::Interpolator smooth{
        .name = "smoothStep",
        .doc = "Eases in and out between the two keys' values, ignoring tangents.",
        .evaluate = [](const scene::anim::Curve& c, size_t i, double t) {
            const scene::anim::Key& a = c.keys[i];
            const scene::anim::Key& b = c.keys[i + 1];
            double u = b.time > a.time ? (t - a.time) / (b.time - a.time) : 1.0;
            return a.value + (b.value - a.value) * u * u * (3.0 - 2.0 * u);
        },
    };
    if (!scene::anim::registerInterpolator(ctx, std::move(smooth)))
        return 4;

    if (!scene::ik::registerSolver(ctx, {.name = "scenePointsAim", .doc = "Aims the chain's start joint at the target.", .solve = aimSolve}))
        return 5;

    gn::NodeDecl golden{
        .type = "ScenePointsGolden",
        .label = "Golden Ratio",
        .category = "Input",
        .doc = "The golden ratio times scale.",
        .sockets = {.inputs = {{.id = "scale", .type = gn::SocketType::Float, .defaultValue = 1.0}},
                    .outputs = {{.id = "value", .type = gn::SocketType::Float, .field = true}}},
        .exec = [](gn::ExecContext& c) { c.out("value", gn::constantFloat(1.6180339887 * c.f("scale"))); },
    };
    if (!gn::registerDecl(ctx, std::move(golden)))
        return 6;

    if (!scene::data::registerKind(ctx, "scenePointsNote", [](const json& j) -> scene::DataPtr {
            auto n = std::make_shared<Note>();
            n->text = j.value("text", "");
            return n;
        }))
        return 7;

    if (!mcp::Server::get().addTool(ctx, {.name = "scene_points_count",
                                          .description = "How many nodes the scene has.",
                                          .inputSchema = {{"type", "object"}, {"properties", json::object()}},
                                          .run = [](const json&) { return std::to_string(scene::Graph::current().nodes().size()); }}))
        return 8;

    if (!link::registerExporter(ctx, "scenePoints:pts", [](const std::vector<scene::NodeId>& nodes, const std::filesystem::path& path, std::string*) {
            std::ofstream(path) << nodes.size() << " nodes\n";
            return true;
        }))
        return 9;

    if (!io::registerUiSection(ctx, "scenePoints", [] { return gUiSection; }, [](const json& j) { gUiSection = j; }))
        return 10;
    scene::scripts::addListener(ctx, [] { ++gScriptChanges; });

    if (!scene::deformers::registerDeformer(ctx, {.name = "liftDeformer",
                                                  .icon = "arrow-bar-up",
                                                  .doc = "Moves the whole mesh by an offset, scaled by amount.",
                                                  .params = {{.name = "offset", .type = scene::PlugType::Vec3, .defaultValue = glm::dvec3(0.0, 1.0, 0.0),
                                                              .flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable,
                                                              .doc = "Distance moved at amount 1."},
                                                             {.name = "amount", .type = scene::PlugType::Float, .defaultValue = 1.0,
                                                              .flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable,
                                                              .doc = "Scales the offset."}},
                                                  .make = [] { return std::make_unique<LiftDeformer>(); }}))
        return 11;

    ctx.log(LogLevel::Info, "scene points extension loaded");
    return 0;
}
