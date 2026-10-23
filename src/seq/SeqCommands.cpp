#include "seq/MeshSequence.h"

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
        {"step", "s", T::Float, 1.0, "Frame step."},
        {"playbackRange", "pr", T::Bool, nullptr, "Sample the scene's playback range (the default without start and end)."},
        {"fps", "", T::Float, 0.0, "Frames per second; 0 uses the scene's."},
        {"scale", "sc", T::Float, 1.0, "Global scale applied to every length (0.1 shrinks to a tenth)."},
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
    std::vector<FlagDef> seqFlags = sampleFlags();
    seqFlags.push_back({"format", "f", T::String, "fbx", "fbx or obj (a file per frame) or abc (one Alembic file).", {"fbx", "obj", "abc"}});
    seqFlags.push_back({"folder", "fo", T::String, "sequences", "Output folder; relative paths are under the workspace."});
    seqFlags.push_back({"pattern", "pt", T::String, "{name}_{index}",
                        "File name without extension: {name} is the first mesh, {index} counts from 0, {frame} is the frame number."});
    seqFlags.push_back({"padding", "pad", T::Int, 3, "Zero padding of {index} and {frame}."});
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
            o.folder = p.parent_path().generic_string();
            o.pattern = p.stem().string();
            json files = run(targets(a, "exportAlembic"), o, a.get<bool>("dryRun"), "exportAlembic");
            return files.empty() ? json() : files[0];
        },
    });
}
} // namespace lila::seq
