#include "seq/MeshSequence.h"
#include "seq/RigidPieces.h"
#include "seq/Vat.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <stdexcept>

namespace lila::seq
{
namespace
{
using T = ArgType;

fs::path resolve(const std::string& s)
{
    fs::path p(s);
    if (p.is_relative())
        p = (Workspace::get().isOpen() ? Workspace::get().root() : fs::current_path()) / p;
    return p.lexically_normal();
}

/** Flags shared by exportMeshSequence and exportAlembic. */
std::vector<FlagDef> sampleFlags()
{
    return {
        {"start", "st", T::Float, nullptr, "First frame. Without start and end the playback range is used."},
        {"end", "e", T::Float, nullptr, "Last frame, inclusive."},
        {"step", "s", T::Float, 1.0, "Frame step.", {}, {}, 0.001},
        {"playbackRange", "pr", T::Bool, nullptr, "Sample the scene's playback range (the default without start and end)."},
        {"fps", "", T::Float, 0.0, "Frames per second; 0 uses the scene's.", {}, {}, 0.0},
        {"scale", "sc", T::Float, 1.0, "Global scale applied to every length (0.1 shrinks to a tenth).", {}, {}, 0.001},
        {"profile", "ax", T::String, "ue5", "Axis profile: ue5, ue6 or generic (the scene's axes).", {"ue5", "ue6", "generic"}},
        {"applyModifiers", "am", T::Bool, true, "Export the evaluated mesh; off exports the base mesh without its modifiers."},
        {"worldSpace", "ws", T::Bool, true, "Bake each object's world transform; off writes object space."},
        {"recenter", "rc", T::String, "none",
         "none, first (move the first frame's centre to the origin, the same offset for every frame) or each (centre every frame).",
         {"none", "first", "each"}},
        {"normals", "n", T::Bool, true, "Write normals."},
        {"uvs", "uv", T::Bool, true, "Write UV sets."},
        {"colors", "vc", T::Bool, true, "Write the first color set as vertex colors."},
        {"triangulate", "tri", T::Bool, true, "Write triangles, as the viewport shows them."},
        {"dryRun", "dr", T::Bool, false, "Return the files without writing anything."},
    };
}

SequenceOptions optionsFrom(const Args& a, const char* cmd)
{
    json j = json::object();
    for (const char* k : {"start", "end", "step", "fps", "scale", "profile", "applyModifiers", "worldSpace", "recenter", "normals", "uvs",
                          "colors", "triangulate", "format", "pattern", "padding"})
        if (a.has(k))
            j[k] = a.flags[k];
    try
    {
        SequenceOptions o = fromJson(j);
        o.playbackRange = a.has("playbackRange") ? a.get<bool>("playbackRange") : !a.has("start") && !a.has("end");
        if (!o.playbackRange && a.has("start") != a.has("end"))
        {
            // One end given: the other comes from the playback range.
            std::vector<double> pb = [&] {
                SequenceOptions p = o;
                p.playbackRange = true;
                return frames(p);
            }();
            if (!a.has("start"))
                o.start = pb.front();
            if (!a.has("end"))
                o.end = pb.back();
        }
        return o;
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

std::vector<scene::NodeId> targets(const Args& a, const char* cmd)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> roots;
    for (const std::string& name : a.objects())
    {
        scene::Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        roots.push_back(n->id());
    }
    std::vector<scene::NodeId> meshes = meshNodes(g, roots);
    if (meshes.empty())
        throw CommandError(std::string(cmd) + ": give or select at least one mesh");
    return meshes;
}

json run(const std::vector<scene::NodeId>& meshes, const SequenceOptions& o, bool dryRun, const char* cmd)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<fs::path> files;
    try
    {
        files = dryRun ? plannedFiles(g, meshes, o) : exportSequence(g, meshes, o);
    }
    catch (const progress::Cancelled&)
    {
        throw CommandError(std::string(cmd) + ": cancelled");
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    json out = json::array();
    for (const fs::path& f : files)
        out.push_back(f.generic_string());
    if (!dryRun)
        log::info("{} wrote {} file(s) to {}", cmd, files.size(), files.empty() ? std::string() : files.front().parent_path().generic_string());
    return out;
}
} // namespace

void registerCommands()
{
    registerAlembicImport();
    std::vector<FlagDef> seqFlags = sampleFlags();
    seqFlags.push_back({"format", "f", T::String, "fbx", "fbx or obj (a file per frame) or abc (one Alembic file).", {"fbx", "obj", "abc"}});
    seqFlags.push_back({"folder", "fo", T::String, "sequences", "Output folder; relative paths are under the workspace."});
    seqFlags.push_back({"pattern", "pt", T::String, "{name}_{index}",
                        "File name without extension: {name} is the first mesh, {index} counts from 0, {frame} is the frame number."});
    seqFlags.push_back({"padding", "pad", T::Int, 3, "Zero padding of {index} and {frame}.", {}, {}, 0.0, 10.0});
    seqFlags.push_back({"path", "p", T::String, nullptr, "Folder, pattern and extension in one, like out/rock_{frame}.obj (padding 4 unless given)."});
    ops::registerCommand({
        .name = "exportMeshSequence",
        .doc = "Export animated meshes (geometry nodes, deformers, skins, anything that changes over time) frame by frame: "
               "a file per frame (fbx, obj), as Unreal's Niagara mesh flipbooks want, or one Alembic file (abc). The given "
               "meshes (or the selection, or the meshes under them) go into each file together. Returns the files.",
        .positionalDoc = "meshes",
        .flags = seqFlags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Args b = a;
            if (a.has("path"))
            {
                fs::path p(a.get<std::string>("path"));
                b.flags["folder"] = p.parent_path().generic_string();
                b.flags["pattern"] = p.stem().string();
                if (!a.has("format") || a.flags["format"] == "fbx")
                    b.flags["format"] = p.has_extension() ? p.extension().string().substr(1) : std::string("fbx");
                if (!a.has("padding") || a.flags["padding"] == 3)
                    b.flags["padding"] = 4;
            }
            SequenceOptions o = optionsFrom(b, "exportMeshSequence");
            o.folder = resolve(b.get<std::string>("folder", "sequences")).generic_string();
            return run(targets(a, "exportMeshSequence"), o, a.get<bool>("dryRun"), "exportMeshSequence");
        },
    });

    std::vector<FlagDef> abcFlags = sampleFlags();
    abcFlags.push_back({"path", "p", T::String, nullptr, "The .abc file; relative paths are under the workspace."});
    abcFlags.push_back({"rigidPieces", "rp", T::Bool, false,
                        "Write each rigid piece (the piece point attribute, or each connected part) as its own object: its shape once "
                        "and a transform per frame. Far smaller for a broken-apart mesh."});
    ops::registerCommand({
        .name = "exportAlembic",
        .doc = "Write animated meshes to one Alembic (.abc, Ogawa) file: a sample per frame with points, faces (topology may change), "
               "normals, UVs and vertex colors, ready for an Unreal Geometry Cache or another DCC. Returns the file.",
        .positionalDoc = "meshes",
        .flags = abcFlags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            if (!a.has("path"))
                throw CommandError("exportAlembic: path= required");
            fs::path p = resolve(a.get<std::string>("path"));
            if (p.extension() != ".abc")
                p += ".abc";
            Args b = a;
            b.flags["format"] = "abc";
            SequenceOptions o = optionsFrom(b, "exportAlembic");
            o.rigidPieces = a.get<bool>("rigidPieces", false);
            o.folder = p.parent_path().generic_string();
            o.pattern = p.stem().string();
            json files = run(targets(a, "exportAlembic"), o, a.get<bool>("dryRun"), "exportAlembic");
            return files.empty() ? json() : files[0];
        },
    });

    std::vector<FlagDef> vatFlags;
    for (FlagDef& f : sampleFlags())
    {
        if (f.name == "triangulate")
            continue;
        if (f.name == "normals")
            f.doc = "Bake a normal texture (<name>_nrm.png).";
        vatFlags.push_back(std::move(f));
    }
    vatFlags.push_back({"folder", "fo", T::String, "vat", "Output folder; relative paths are under the workspace."});
    vatFlags.push_back({"name", "nm", T::String, "{name}", "File base name; {name} is the first mesh."});
    vatFlags.push_back({"format", "f", T::String, "exr",
                        "Position texture: exr (half float offsets, HDR=1), png16 or png8 (normalised by BoundsMin/BoundsMax, HDR=0).",
                        {"exr", "png16", "png8"}});
    vatFlags.push_back({"mode", "md", T::String, "auto",
                        "constant (every frame keeps the rest frame's vertices), soup (unwelded triangles, for changing topology), "
                        "rigid (loose pieces that only move and rotate: one position and one rotation texel per piece per frame, pieces "
                        "from the int point attribute 'piece' or connected components) or auto (soup when the topology changes, rigid "
                        "when every mesh has 'piece' and every piece fits a rigid motion within 0.01 units, constant otherwise).",
                        {"auto", "constant", "soup", "rigid"}});
    vatFlags.push_back({"positions", "po", T::String, "offset", "offset (from the rest mesh, for World Position Offset) or absolute.",
                        {"offset", "absolute"}});
    vatFlags.push_back({"normalBits", "nb", T::Int, 8, "Normal texture depth, 8 or 16.", {}, {}, 8.0, 16.0});
    vatFlags.push_back({"restFrame", "rf", T::Float, nullptr, "Frame whose shape is the rest mesh (default the first sampled frame)."});
    vatFlags.push_back({"maxWidth", "mw", T::Int, nullptr, "Widest texture before a frame wraps to more rows (default the vat maxWidth pref, 4096).", {}, {}, 1.0, 16384.0});
    vatFlags.push_back({"exrBits", "eb", T::Int, nullptr, "EXR depth: 16 (half float) or 32 (float). Default 32 for rigid, 16 otherwise.", {}, {}, 16.0, 32.0});
    ops::registerCommand({
        .name = "exportVertexAnimation",
        .doc = "Bake animated meshes (geometry nodes, deformers, skins, anything that changes over time) into Vertex Animation "
               "Textures: a rest mesh FBX with an extra UV channel addressing each vertex's texels, a position texture (offsets "
               "or positions per vertex per frame, Unreal space), a normal texture and a JSON sidecar describing the layout. "
               "Changing topology is baked as a padded triangle soup; rigid pieces as a position and a rotation texture with one "
               "column per piece. Returns {files, meta, materialParams} (the material inputs: HDR, BoundsMin, BoundsMax, FrameCount, "
               "RowsPerFrame, TextureHeight, FrameRate, PositionTexture, NormalTexture; rigid adds RotationTexture, PieceCount, "
               "PivotChannelXY, PivotChannelZ).",
        .positionalDoc = "meshes",
        .flags = vatFlags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Args b = a;
            for (const char* k : {"format", "normals"})
                b.flags.erase(k);
            SequenceOptions so = optionsFrom(b, "exportVertexAnimation");
            json j = json::object();
            for (const char* k : {"name", "format", "mode", "positions", "normals", "normalBits", "restFrame", "maxWidth", "exrBits"})
                if (a.has(k))
                    j[k] = a.flags[k];
            seq::VatOptions o;
            try
            {
                o = vatFromJson(j, so);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("exportVertexAnimation: ") + e.what());
            }
            o.folder = resolve(a.get<std::string>("folder", "vat")).generic_string();
            const std::vector<scene::NodeId> meshes = targets(a, "exportVertexAnimation");
            scene::Graph& g = scene::Graph::current();
            VatResult r;
            try
            {
                r = a.get<bool>("dryRun") ? plannedVat(g, meshes, o) : exportVat(g, meshes, o);
            }
            catch (const progress::Cancelled&)
            {
                throw CommandError("exportVertexAnimation: cancelled");
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("exportVertexAnimation: ") + e.what());
            }
            json out = {{"files", r.files()}, {"meta", r.metadata}};
            if (r.metadata.contains("materialParams"))
                out["materialParams"] = r.metadata["materialParams"];
            return out;
        },
    });
    std::vector<FlagDef> jointFlags;
    for (const FlagDef& f : sampleFlags())
        if (f.name == "start" || f.name == "end" || f.name == "step" || f.name == "playbackRange" || f.name == "fps")
            jointFlags.push_back(f);
    jointFlags.push_back({"name", "n", T::String, nullptr, "Name of the new mesh (its joints take it as a prefix); default <mesh>_Rigid."});
    jointFlags.push_back({"hideSource", "hs", T::Bool, true, "Hide the animated mesh baked from."});
    ops::registerCommand({
        .name = "bakePiecesToJoints",
        .doc = "Bake a mesh of moving rigid pieces (a destruction simulation) onto joints.\n\n"
               "Makes a new mesh at the first frame's shape, skinned one piece per joint under a root at the origin, and keys the "
               "joints on every frame, so the motion can be edited with the animation tools or exported as an FBX skeletal "
               "mesh with animation. Pieces are the int piece point attribute, or the connected parts. Returns {mesh, root, "
               "joints, residual} (residual: worst point error of the fit, cm).",
        .positionalDoc = "mesh (default the selection)",
        .flags = jointFlags,
        .run = [](const Args& a) -> json {
            std::vector<scene::NodeId> meshes = targets(a, "bakePiecesToJoints");
            if (meshes.size() != 1)
                throw CommandError("bakePiecesToJoints: give one mesh");
            const SequenceOptions o = optionsFrom(a, "bakePiecesToJoints");
            try
            {
                PieceJointsResult r = bakePiecesToJoints(scene::Graph::current(), meshes[0], o, a.get<std::string>("name", ""),
                                                         a.get<bool>("hideSource", true));
                return {{"mesh", r.mesh}, {"root", r.root}, {"joints", r.joints}, {"residual", r.residual}};
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("bakePiecesToJoints: ") + e.what());
            }
        },
        .examples = {"bakePiecesToJoints('Wall', start=1, end=72)"},
    });
}
} // namespace lila::seq
