#include "collada/Collada.h"

#include "collada/ColladaInternal.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "fbx/GameExport.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"

namespace lila::collada
{
namespace
{
using scene::Graph;
using scene::NodeId;

std::vector<std::string> names(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        if (const scene::Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "importCollada",
        .doc = "Import a COLLADA 1.4.1 file (.dae): the visual scene's nodes as transforms, joints and meshes, converted from the "
               "file's up axis and unit to the scene's axes and cm.\n\n"
               "Transform stacks (matrix, translate, rotate, scale, lookat, skew) are read in order. Polylists, triangles and "
               "polygons become meshes with normals (hard edges where they split), every UV set and every color set; material "
               "symbols become slots named after the materials they bind to. Skin controllers become skinClusters with their bind "
               "shape, inverse bind matrices and weights; a joint's rest rotation goes into its jointOrient. Animations of whole "
               "matrices or single transform elements and their components (LINEAR, STEP and BEZIER) are sampled once per frame "
               "onto translate, rotate and scale; with several animation clips each becomes a take over its range, the first "
               "active. Materials become material nodes with diffuse, emission, transparency and bump.\n\n"
               "Returns the new node names, or with report=True a dict of nodes, roots, joints, skinClusters, materials, clips, "
               "fps, start, end, curves, upAxis, unitMeters, authoringTool, skipped and warnings.",
        .positionalDoc = "path",
        .flags = {nodeFlag({"parent", "p", T::String, nullptr, "Parent the imported roots under this node."}, "transform"),
                  {"skins", "", T::Bool, true, "Rebuild skin controllers as joints and skinClusters with their weights and bind matrices."},
                  {"animation", "anim", T::Bool, true, "Import the animation, one key per frame."},
                  {"fps", "", T::Float, 0.0, "Frames per second to sample at; 0 guesses from the key spacing.", {}, {}, 0.0},
                  {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the animation."},
                  {"materials", "", T::Bool, true, "Create material nodes; off keeps only the slot names on the faces."},
                  {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                  {"select", "", T::Bool, true, "Select the imported roots."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importCollada: path required");
            const std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            ImportOptions opts;
            if (a.has("parent"))
            {
                scene::Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("importCollada: no node named '" + a.get<std::string>("parent") + "'");
                opts.parent = p->id();
            }
            opts.skins = a.get<bool>("skins", true);
            opts.animation = a.get<bool>("animation", true);
            opts.fps = a.get<double>("fps", 0.0);
            opts.setPlayback = a.get<bool>("setPlayback", true);
            opts.materials = a.get<bool>("materials", true);
            ImportResult r;
            {
                progress::Scope scope("Importing " + textOf(pathOf(path).filename()));
                r = importCollada(g, pathOf(path), opts);
            }
            if (!r.ok)
                throw CommandError("importCollada: " + r.error);
            for (const std::string& w : r.warnings)
                log::warn("importCollada: {}", w);
            for (const std::string& s : r.skipped)
                log::info("importCollada: skipped {}", s);
            if (a.get<bool>("select", true) && !r.roots.empty())
                g.select(r.roots);
            if (!a.get<bool>("report", false))
                return names(g, r.nodes);
            return json{{"nodes", names(g, r.nodes)},     {"roots", names(g, r.roots)},   {"joints", names(g, r.joints)},
                        {"skinClusters", names(g, r.skins)}, {"materials", r.materials}, {"clips", r.clips},
                        {"fps", r.fps},                    {"start", scene::anim::frameOf(r.start)}, {"end", scene::anim::frameOf(r.end)},
                        {"curves", r.curves},              {"upAxis", r.upAxis},         {"unitMeters", r.unitMeters},
                        {"authoringTool", r.authoringTool}, {"skipped", r.skipped},      {"warnings", r.warnings}};
        },
    });

    ops::registerCommand({
        .name = "exportCollada",
        .doc = "Export objects to a COLLADA 1.4.1 file (.dae) laid out the way Maya's COLLADA exporter writes it, so pipelines "
               "built on Maya's files read it: Y up, each node's transform as translate, jointOrient, rotate and scale elements with "
               "every animated channel sampled per frame (or one matrix per node with bakeTransforms), meshes as polylists (or "
               "triangles) with normals and UV sets, skins as controllers with bind shape and inverse bind matrices, joints as "
               "JOINT nodes, several takes as animation clips, lights, cameras, and materials as lambert effects with diffuse, "
               "emission, transparency and bump maps. kind=animation writes the skeleton and its animation alone, like the "
               "animation files game engines take. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {{"kind", "k", T::String, "skeletal",
                   "static | skeletal | animation. skeletal writes meshes, skins and animation (Maya's Rigged and Animation exports), "
                   "static meshes and transforms only, animation the skeleton and its animation without meshes."},
                  {"bakeTransforms", "", T::Bool, false,
                   "Write each node's transform as one matrix animated as sampled matrices; off writes Maya's translate, jointOrient, "
                   "rotate and scale elements, each animated channel its own curve, as Maya's exporter does by default."},
                  {"triangles", "", T::Bool, false, "Write triangles instead of polylists."},
                  {"maxInfluences", "mi", T::Int, 8, "Skin influences per vertex, heaviest kept and renormalised.", {}, {}, 0.0},
                  {"animation", "anim", T::Bool, true, "Write animation."},
                  {"deformOnly", "do", T::Bool, true, "Skip non-deform joints (IK helpers and rig-only joints)."},
                  {"start", "st", T::Float, nullptr, "First frame; the playback start by default."},
                  {"end", "et", T::Float, nullptr, "Last frame; the playback end by default."},
                  {"take", "", T::String, "", "Clip name; the active take's, else Take 001."},
                  {"takes", "", T::Any, nullptr, "Scene takes to write, one clip each, one after another: 'all' or a list of names."},
                  {"normals", "", T::Bool, true, "Write normals."},
                  {"vertexColors", "", T::Bool, false, "Write vertex color sets."},
                  {"textures", "", T::Bool, true, "Write texture maps; off writes material colors only."},
                  {"copyTextures", "", T::Bool, false, "Copy textures next to the file; off references them where they are."},
                  {"lights", "", T::Bool, true, "Write point, spot and directional lights."},
                  {"worldSpace", "ws", T::Bool, false, "Bake world transforms into the meshes and flatten the hierarchy (static exports)."},
                  {"atOrigin", "", T::Bool, false, "Drop the exported roots' own transforms, keeping their children relative."},
                  {"children", "", T::Bool, true, "Include the descendants of the given objects."},
                  {"units", "u", T::String, "cm", "File units: cm (Maya's), m, mm, in or ft. Lengths are scaled to them.", {"cm", "m", "mm", "in", "ft"}},
                  {"selectionOnly", "sl", T::Bool, true, "Without objects: the selection, or every root when False."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("exportCollada: path required");
            const std::string path = a.positional[0].get<std::string>();
            const fs::path file = pathOf(path);
            Graph& g = Graph::current();

            ExportOptions opts;
            opts.bakeTransforms = a.get<bool>("bakeTransforms", false);
            opts.triangles = a.get<bool>("triangles", false);
            opts.vertexColors = a.get<bool>("vertexColors", false);
            opts.textures = a.get<bool>("textures", true);
            opts.copyTextures = a.get<bool>("copyTextures", false);
            opts.lights = a.get<bool>("lights", true);
            fbx::ExportOptions& go = opts.gather;
            go.triangulate = false;
            go.tangents = false;
            go.normals = a.get<bool>("normals", true);
            go.worldSpace = a.get<bool>("worldSpace", false);
            go.atOrigin = a.get<bool>("atOrigin", false);
            go.children = a.get<bool>("children", true);
            go.unitScale = fbx::game::cmPerUnit(a.get<std::string>("units", "cm"));
            if (go.unitScale <= 0.0)
                throw CommandError("exportCollada: units must be cm, m, mm, in or ft");
            go.maxInfluences = a.get<int>("maxInfluences", 8);
            go.animation = a.get<bool>("animation", true);
            go.bakeAnimation = true;
            go.deformOnly = a.get<bool>("deformOnly", true);
            go.takeName = a.get<std::string>("take", "");
            if (a.has("takes"))
            {
                const json& t = a.flags["takes"];
                if (t.is_string())
                    go.takes.push_back(t.get<std::string>());
                else if (t.is_array())
                    for (const json& n : t)
                        if (n.is_string())
                            go.takes.push_back(n.get<std::string>());
            }
            if (a.has("start"))
                go.start = scene::anim::secondsOf(a.get<double>("start"));
            if (a.has("end"))
                go.end = scene::anim::secondsOf(a.get<double>("end"));
            const std::string kind = a.get<std::string>("kind", "skeletal");
            if (!fbx::parseKind(kind, &go.kind) || go.kind == fbx::ExportKind::Sequence)
                throw CommandError("exportCollada: unknown kind '" + kind + "' (static, skeletal or animation)");

            std::vector<NodeId> ids;
            for (size_t k = 1; k < a.positional.size(); ++k)
            {
                const json& v = a.positional[k];
                std::vector<std::string> list;
                if (v.is_string())
                    list.push_back(v.get<std::string>());
                else if (v.is_array())
                    for (const json& x : v)
                        if (x.is_string())
                            list.push_back(x.get<std::string>());
                for (const std::string& name : list)
                {
                    scene::Node* n = g.find(name);
                    if (!n)
                        throw CommandError("exportCollada: no node named '" + name + "'");
                    ids.push_back(n->id());
                }
            }
            if (a.positional.size() <= 1)
                ids = a.get<bool>("selectionOnly", true) ? g.selection() : g.roots();

            progress::Scope scope("Exporting " + textOf(file.filename()) + "...", 3.0);
            gltf::ExportSnapshot snapshot;
            {
                progress::Scope gather("Gathering the scene");
                gltf::ExportOptions gopts;
                gopts.gather = go;
                snapshot = gltf::gatherExport(g, ids, gopts);
            }
            for (const std::string& w : snapshot.scene.warnings)
                log::warn("exportCollada: {}", w);
            if (snapshot.scene.nodes.empty())
                throw CommandError("exportCollada: nothing to export");
            std::string err;
            std::vector<std::string> warnings;
            bool ok = false;
            {
                progress::Scope write("Writing " + textOf(file.filename()), 0.0, progress::None, 2.0);
                progress::runOnWorker([&] { ok = writeCollada(snapshot, file, opts, &err, &warnings); });
            }
            for (const std::string& w : warnings)
                log::warn("exportCollada: {}", w);
            if (!ok)
                throw CommandError("exportCollada: " + err);
            return path;
        },
    });
}
} // namespace lila::collada
