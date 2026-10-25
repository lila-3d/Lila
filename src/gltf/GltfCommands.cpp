#include "gltf/Gltf.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/GameExport.h"
#include "gltf/GltfInternal.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Skin.h"

namespace lila::gltf
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

std::vector<std::string> positionalNames(const Args& a, size_t from)
{
    std::vector<std::string> out;
    for (size_t k = from; k < a.positional.size(); ++k)
    {
        const json& v = a.positional[k];
        if (v.is_string())
            out.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(x.get<std::string>());
    }
    return out;
}

bool isGlb(const fs::path& p)
{
    std::string ext = utf8(p.extension());
    for (char& c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".glb";
}

/** Sequence when every node is a camera, static when baking to world space, else everything (skeletal). */
fbx::ExportKind autoKind(Graph& g, const std::vector<NodeId>& ids, bool worldSpace)
{
    bool cameras = !ids.empty();
    for (NodeId id : ids)
        cameras = cameras && g.node(id) && g.node(id)->type().isA("camera");
    if (cameras)
        return fbx::ExportKind::Sequence;
    return worldSpace ? fbx::ExportKind::Static : fbx::ExportKind::Skeletal;
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "importGltf",
        .doc = "Import a glTF 2.0 file (.gltf with its .bin and images, embedded data URIs, or binary .glb): the default scene's nodes as "
               "transforms, joints, meshes and cameras, converted from glTF's Y-up metres to the scene's axes and cm.\n\n"
               "Each mesh becomes one Lila mesh, its primitives becoming material slots, with normals (hard edges where they split), "
               "tangents, every UV set (flipped to Lila's V-up) and every vertex color set; vertices split for seams weld back into "
               "points. Skins become skinClusters on joints with every JOINTS_n/WEIGHTS_n influence and the inverse bind matrices; a "
               "joint's rest rotation goes into its jointOrient. Morph targets become a morph with their names and default weights. "
               "Animation channels become curves: LINEAR and STEP keys exactly, CUBICSPLINE with its tangents (rotations sampled every "
               "frame); with several animations each becomes a take, the first active. Cameras keep their field of view and clipping.\n\n"
               "Materials become material nodes: base color and alpha mode, metallic and roughness (packed into an ORM map, written "
               "as a new image when the file's layout differs), occlusion, emissive with KHR_materials_emissive_strength, normal map "
               "(kept OpenGL style), double sided, KHR_materials_ior/specular as specular, KHR_texture_transform baked into the UVs, "
               "and KHR_materials_unlit approximated as emissive. Embedded and converted images are written to textureDir. "
               "KHR_mesh_quantization, sparse accessors, EXT_meshopt_compression and KHR_draco_mesh_compression are decoded (a Draco "
               "primitive that fails to decode falls back to its uncompressed data when the file has it). Lights are reported, not "
               "imported (Lila has no lights).\n\n"
               "Returns the new node names, or with report=True a dict of nodes, roots, joints, skinClusters, cameras, morphs, "
               "materials, images, takes, take, fps, start, end, curves, skipped, warnings and errors.",
        .positionalDoc = "path",
        .flags = {{"parent", "p", T::String, nullptr, "Parent the imported roots under this node."},
                  {"skins", "", T::Bool, true, "Rebuild skins as joints and skinClusters with their weights and bind matrices."},
                  {"animation", "anim", T::Bool, true, "Import the animations as curves."},
                  {"take", "", T::String, "", "Import only this animation, by name or index; every animation by default."},
                  {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the (first) animation."},
                  {"materials", "", T::Bool, true, "Create material nodes; off keeps only the slot names on the faces."},
                  {"textureDir", "", T::String, "", "Folder for embedded and converted images; <file folder>/<file name>_textures by default."},
                  {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                  {"select", "", T::Bool, true, "Select the imported roots."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importGltf: path required");
            const std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            ImportOptions opts;
            if (a.has("parent"))
            {
                scene::Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("importGltf: no node named '" + a.get<std::string>("parent") + "'");
                opts.parent = p->id();
            }
            opts.skins = a.get<bool>("skins", true);
            opts.animation = a.get<bool>("animation", true);
            opts.take = a.get<std::string>("take", "");
            opts.setPlayback = a.get<bool>("setPlayback", true);
            opts.materials = a.get<bool>("materials", true);
            if (std::string dir = a.get<std::string>("textureDir", ""); !dir.empty())
                opts.textureDir = utf8Path(dir);
            ImportResult r = importGltf(g, utf8Path(path), opts);
            if (!r.ok)
                throw CommandError("importGltf: " + r.error);
            for (const std::string& s : r.skipped)
                log::info("importGltf: skipped {}", s);
            for (const std::string& w : r.warnings)
                log::warn("importGltf: {}", w);
            for (const std::string& e : r.errors)
                log::error("importGltf: {}", e);
            if (a.get<bool>("select", true))
                g.select(r.roots);
            if (!a.get<bool>("report", false))
                return names(g, r.nodes);
            return {{"nodes", names(g, r.nodes)},     {"roots", names(g, r.roots)},   {"joints", names(g, r.joints)},
                    {"skinClusters", names(g, r.skins)}, {"cameras", names(g, r.cameras)}, {"morphs", names(g, r.morphs)},
                    {"materials", r.materials},       {"images", r.images},           {"takes", r.takes},
                    {"take", r.take},                 {"fps", r.fps},                 {"start", r.start},
                    {"end", r.end},                   {"curves", r.curves},           {"generator", r.generator},
                    {"extensionsUsed", r.extensionsUsed}, {"skipped", r.skipped},     {"warnings", r.warnings},
                    {"errors", r.errors}};
        },
    });

    ops::registerCommand({
        .name = "exportGltf",
        .doc = "Export the evaluated meshes, skeletons and cameras to glTF 2.0: binary .glb, or JSON .gltf with a .bin and image files "
               "next to it (or everything embedded as data URIs). Objects default to the selection (or the whole scene with "
               "selectionOnly=False). Axes become glTF's Y up with the model facing +Z, lengths metres (see units).\n\n"
               "Meshes go out triangulated as the viewport splits them, one primitive per material slot, with normals, MikkTSpace "
               "tangents, every UV set (flipped to glTF's V-down) and every color set. Skinned meshes go out in their bind pose with "
               "JOINTS_n/WEIGHTS_n sets (four influences each, up to maxInfluences) and inverse bind matrices; morph targets with "
               "position and normal deltas (sparse where few points move), names in extras.targetNames and their weights. Each "
               "take becomes one animation (takes=, or the current animation over start/end): sampled every frame and thinned "
               "where interpolation reproduces the samples, translate and scale curves written as their own keys with "
               "bakeAnimation=False. Cameras keep their field of view, aspect and clipping.\n\n"
               "Materials go out metallic-roughness: base color and opacity, ORM maps shared as glTF's metallic-roughness and "
               "occlusion textures (other layouts and separate maps repacked), DirectX normal maps green-flipped to OpenGL, "
               "emissive with KHR_materials_emissive_strength above 1, specular through KHR_materials_specular, alpha mode, "
               "cutoff and double sided. Textures that aren't PNG or JPEG are converted to PNG.\n\n"
               "draco=True compresses every primitive with KHR_draco_mesh_compression (then required to read the file), quantized "
               "per the dracoBits flags: positions, normals, UVs, colors and generic attributes (tangents, skin weights; joint "
               "indices stay exact). Morph targets stay uncompressed accessors, as the extension requires, with the points "
               "encoded in order so they still line up. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {{"binary", "", T::Bool, nullptr, "Write binary .glb; by default from the extension."},
                  {"embedImages", "", T::Bool, nullptr, "Put images inside the file (default for .glb); off writes them next to it."},
                  {"embedBuffers", "", T::Bool, false, ".gltf only: put the geometry inside the file as a data URI instead of a .bin."},
                  {"textures", "", T::Bool, true, "Write texture maps; off writes material factors only."},
                  {"copyTextures", "", T::Bool, true, "Unembedded images: copy textures next to the file; off references them where they are."},
                  {"kind", "k", T::String, "auto",
                   "auto | static | skeletal | animation | sequence. auto writes everything (meshes, skins, morph targets, cameras, "
                   "animation), static only meshes and transforms (no animation, allows worldSpace), animation the skeleton and its "
                   "animation, sequence cameras and transforms. auto picks sequence when only cameras are given."},
                  {"maxInfluences", "mi", T::Int, 8, "Skin influences per vertex, heaviest kept and renormalised (4 for one JOINTS_0 set)."},
                  {"animation", "anim", T::Bool, true, "Write animation."},
                  {"bakeAnimation", "bake", T::Bool, true, "Sample every frame; False writes plain translate and scale curves as their own keys."},
                  {"deformOnly", "do", T::Bool, true, "Skip non-deform joints (IK helpers and rig-only joints)."},
                  {"start", "st", T::Float, nullptr, "First frame; the playback start by default."},
                  {"end", "et", T::Float, nullptr, "Last frame; the playback end by default."},
                  {"take", "", T::String, "", "Animation name; the active take's, else Take 001."},
                  {"takes", "", T::Any, nullptr, "Scene takes to write, one glTF animation each: 'all' or a list of names."},
                  {"tangents", "", T::Bool, true, "Write MikkTSpace tangents from uv0."},
                  {"normals", "", T::Bool, true, "Write normals (and morph target normal deltas)."},
                  {"morphNormals", "", T::Bool, true, "Write morph target normal deltas."},
                  {"draco", "", T::Bool, false, "Compress mesh primitives with KHR_draco_mesh_compression."},
                  {"dracoPositionBits", "", T::Int, 14, "Draco quantization bits for positions."},
                  {"dracoNormalBits", "", T::Int, 10, "Draco quantization bits for normals."},
                  {"dracoUvBits", "", T::Int, 12, "Draco quantization bits for UVs."},
                  {"dracoColorBits", "", T::Int, 8, "Draco quantization bits for vertex colors."},
                  {"dracoGenericBits", "", T::Int, 12, "Draco quantization bits for tangents and skin weights."},
                  {"dracoLevel", "", T::Int, 7, "Draco compression level, 0 fastest to 10 smallest."},
                  {"worldSpace", "ws", T::Bool, false, "Bake world transforms into the meshes and flatten the hierarchy (static exports)."},
                  {"atOrigin", "", T::Bool, false, "Drop the exported roots' own transforms, keeping their children relative."},
                  {"children", "", T::Bool, true, "Include the descendants of the given objects."},
                  {"units", "u", T::String, "m", "File units: m (glTF's), cm, mm, in or ft. Lengths are scaled to them.", {"m", "cm", "mm", "in", "ft"}},
                  {"selectionOnly", "sl", T::Bool, true, "Without objects: the selection, or every root when False."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("exportGltf: path required");
            const std::string path = a.positional[0].get<std::string>();
            const fs::path file = utf8Path(path);
            Graph& g = Graph::current();

            ExportOptions opts;
            opts.binary = a.has("binary") ? a.get<bool>("binary") : isGlb(file);
            opts.embedImages = a.has("embedImages") ? a.get<bool>("embedImages") : opts.binary;
            opts.embedBuffers = a.get<bool>("embedBuffers", false);
            opts.textures = a.get<bool>("textures", true);
            opts.copyTextures = a.get<bool>("copyTextures", true);
            opts.morphNormals = a.get<bool>("morphNormals", true);
            opts.draco = a.get<bool>("draco", false);
            opts.dracoPositionBits = a.get<int>("dracoPositionBits", 14);
            opts.dracoNormalBits = a.get<int>("dracoNormalBits", 10);
            opts.dracoUvBits = a.get<int>("dracoUvBits", 12);
            opts.dracoColorBits = a.get<int>("dracoColorBits", 8);
            opts.dracoGenericBits = a.get<int>("dracoGenericBits", 12);
            opts.dracoLevel = a.get<int>("dracoLevel", 7);
            for (int bits : {opts.dracoPositionBits, opts.dracoNormalBits, opts.dracoUvBits, opts.dracoColorBits, opts.dracoGenericBits})
                if (bits < 1 || bits > 30)
                    throw CommandError("exportGltf: Draco quantization bits must be 1 to 30");
            fbx::ExportOptions& go = opts.gather;
            go.triangulate = true;
            go.tangents = a.get<bool>("tangents", true);
            go.normals = a.get<bool>("normals", true);
            go.worldSpace = a.get<bool>("worldSpace", false);
            go.atOrigin = a.get<bool>("atOrigin", false);
            go.children = a.get<bool>("children", true);
            go.unitScale = fbx::game::cmPerUnit(a.get<std::string>("units", "m"));
            if (go.unitScale <= 0.0)
                throw CommandError("exportGltf: units must be m, cm, mm, in or ft");
            go.maxInfluences = a.get<int>("maxInfluences", 8);
            go.animation = a.get<bool>("animation", true);
            go.bakeAnimation = a.get<bool>("bakeAnimation", true);
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

            std::vector<NodeId> ids;
            const std::vector<std::string> objs = positionalNames(a, 1);
            for (const std::string& name : objs)
            {
                scene::Node* n = g.find(name);
                if (!n)
                    throw CommandError("exportGltf: no node named '" + name + "'");
                ids.push_back(n->id());
            }
            if (objs.empty())
                ids = a.get<bool>("selectionOnly", true) ? g.selection() : g.roots();
            const std::string kind = a.get<std::string>("kind", "auto");
            if (kind == "auto")
                go.kind = autoKind(g, ids, go.worldSpace);
            else if (!fbx::parseKind(kind, &go.kind))
                throw CommandError("exportGltf: unknown kind '" + kind + "' (auto, static, skeletal, animation or sequence)");

            progress::Scope scope("Exporting " + utf8(file.filename()) + "...", 3.0);
            ExportSnapshot snapshot;
            {
                progress::Scope gather("Gathering the scene");
                snapshot = gatherExport(g, ids, opts);
            }
            for (const std::string& w : snapshot.scene.warnings)
                log::warn("exportGltf: {}", w);
            if (snapshot.scene.nodes.empty())
                throw CommandError("exportGltf: nothing to export");
            std::string err;
            std::vector<std::string> warnings;
            bool ok = false;
            {
                progress::Scope write("Writing " + utf8(file.filename()), 0.0, progress::None, 2.0);
                progress::runOnWorker([&] { ok = writeGltf(snapshot, file, opts, &err, &warnings); });
            }
            for (const std::string& w : warnings)
                log::warn("exportGltf: {}", w);
            if (!ok)
                throw CommandError("exportGltf: " + err);
            return path;
        },
    });
}
} // namespace lila::gltf
