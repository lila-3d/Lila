#include "seq/Vat.h"

#include "core/Paths.h"
#include "gn/Bake.h"
#include "gn/Gn.h"
#include "gn/NodesCommon.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

namespace lila::seq
{
namespace
{
using json = nlohmann::json;
using namespace gn::nodes;

void vatNode()
{
    gn::registerDecl({
        .type = "VertexAnimationTexture",
        .label = "Vertex Animation Texture",
        .category = "Game",
        .doc = "Encodes the animation of what reaches it (a simulation, anything that changes over time) into Vertex Animation "
               "Textures for Unreal: Encode in the side panel samples this node's input over the playback range and writes a rest "
               "mesh FBX, position and normal textures and a JSON of the material inputs into vat/ next to the scene. The geometry "
               "passes through unchanged.",
        .props = {prop("mode", {"auto", "constant", "soup", "rigid"}, "auto",
                       "auto: soup when the topology changes, rigid when every point has a piece attribute and pieces only move, else "
                       "constant. constant: a column per point. soup: a triangle soup padded to the largest frame. rigid: a transform "
                       "per piece."),
                  prop("format", {"exr", "png16", "png8"}, "exr", "Position texture format: EXR keeps exact values; PNG is normalised by bounds."),
                  prop("positions", {"offset", "absolute"}, "offset", "offset from the rest mesh (for World Position Offset), or absolute.")},
        .sockets = {.inputs = {geoIn("geometry")}, .outputs = {geoOut("geometry")}},
        .exec =
            [](gn::ExecContext& c) {
                gn::recordCapture(c);
                c.out("geometry", c.geometry("geometry"));
            },
        .actions = {{"Encode", "EncodeVat", "Sample this node's input over the playback range and write the vertex animation textures."}},
    });
}

std::string arg(const Args& a, size_t i, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string("gnEncodeVat: ") + what + " required");
    return a.positional[i].get<std::string>();
}
} // namespace

void registerVatNode()
{
    vatNode();
    using A = ArgType;
    ops::registerCommand({
        .name = "gnEncodeVat",
        .doc = "Encode a Vertex Animation Texture node: sample what reaches it on a mesh running its group, frame by frame, and write the "
               "vertex animation textures (see exportVertexAnimation for the files). Returns {files, materialParams}.",
        .positionalDoc = "group node",
        .flags = {{"start", "", A::Float, nullptr, "First frame; defaults to the playback range's."},
                  {"end", "", A::Float, nullptr, "Last frame; defaults to the playback range's."},
                  {"folder", "", A::String, nullptr, "Where to write; defaults to vat/ next to the scene."},
                  {"name", "", A::String, "{name}", "File base name; {name} is the mesh."},
                  {"modifier", "", A::String, nullptr, "The modifier to sample when several meshes run the group; defaults to the first."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::string groupName = arg(a, 0, "group"), nodeName = arg(a, 1, "node");
            scene::Node* groupNode = gn::findGroupNode(g, groupName);
            if (!groupNode)
                throw CommandError("gnEncodeVat: no geometry-nodes group named '" + groupName + "'");
            gn::Group grp = gn::readGroup(*groupNode);
            const gn::GNode* node = grp.node(nodeName);
            if (!node || node->type != "VertexAnimationTexture")
                throw CommandError("gnEncodeVat: " + nodeName + " isn't a Vertex Animation Texture node in " + groupName);
            std::vector<scene::NodeId> mods = gn::instancesOf(g, groupNode->id());
            scene::NodeId mod = mods.empty() ? scene::kNoNode : mods.front();
            if (a.has("modifier"))
            {
                scene::Node* m = g.find(a.get<std::string>("modifier"));
                mod = m && std::find(mods.begin(), mods.end(), m->id()) != mods.end() ? m->id() : scene::kNoNode;
            }
            if (mod == scene::kNoNode)
                throw CommandError("gnEncodeVat: no mesh runs " + groupName + "; apply it to one first");
            scene::NodeId mesh = scene::modifiers::meshOf(g, mod);

            VatOptions o;
            const gn::NodeDecl* d = gn::findDecl(node->type);
            o.mode = d->propValue(*node, "mode");
            o.format = d->propValue(*node, "format");
            o.positions = d->propValue(*node, "positions");
            o.name = a.get<std::string>("name", "{name}");
            o.maxWidth = vatDefaultMaxWidth();
            o.sampling.playbackRange = !a.has("start") && !a.has("end");
            if (!o.sampling.playbackRange)
            {
                o.sampling.start = a.get<double>("start", o.sampling.start);
                o.sampling.end = a.get<double>("end", o.sampling.end);
            }
            std::filesystem::path base = paths::currentSceneDir();
            if (base.empty())
                base = paths::userConfigDir() / "untitled";
            o.folder = paths::toUtf8(a.has("folder") ? paths::fromUtf8(a.get<std::string>("folder")) : base / "vat");

            gn::Capture capture{groupName, nodeName};
            gn::CaptureScope scope(capture);
            g.touch(mod);
            MeshGetter getter = [&](scene::Node&) -> std::shared_ptr<const geo::Mesh> {
                if (!capture.got || !capture.geometry)
                    return nullptr;
                return std::make_shared<const geo::Mesh>(gn::toOutputMesh(*capture.geometry));
            };
            VatResult r;
            try
            {
                r = exportVat(g, {mesh}, o, getter);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnEncodeVat: ") + e.what());
            }
            json out = {{"files", r.files()}};
            if (r.metadata.contains("materialParams"))
                out["materialParams"] = r.metadata["materialParams"];
            return out;
        },
    });
}
} // namespace lila::seq
