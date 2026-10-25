// The OpenUSD extension: importUsd and exportUsd, and the "usd" file translator File > Open, Import and Export use.
// Pixar's libraries live in OpenUSDCore beside this file, loaded on the first USD command so startup never pays for them.

#include "UsdBackend.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileTranslator.h>
#include <lila/Log.h>

#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace lila;

namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path gPackageRoot;

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct Backend
{
    usd::BackendRun run = nullptr;
    std::string error;
};

/** Loads OpenUSDCore and registers Pixar's plugins once; it stays loaded, since OpenUSD does not unload cleanly. */
const Backend& backend()
{
    static Backend b;
    static std::once_flag once;
    std::call_once(once, [] {
        const fs::path file = gPackageRoot / usd::kBackendFile;
        void* init = nullptr;
        void* run = nullptr;
#if defined(_WIN32)
        // Search the package folder first, so Pixar's DLLs and TBB resolve from beside it.
        HMODULE lib = LoadLibraryExW(file.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!lib)
        {
            b.error = "can't load " + utf8(file) + " (error " + std::to_string(GetLastError()) + ")";
            return;
        }
        init = reinterpret_cast<void*>(GetProcAddress(lib, "lilaUsdInit"));
        run = reinterpret_cast<void*>(GetProcAddress(lib, "lilaUsdRun"));
#else
        void* lib = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib)
        {
            b.error = std::string("can't load ") + utf8(file) + ": " + dlerror();
            return;
        }
        init = dlsym(lib, "lilaUsdInit");
        run = dlsym(lib, "lilaUsdRun");
#endif
        if (!init || !run)
        {
            b.error = utf8(file) + " is missing its entry points";
            return;
        }
        std::string error;
        if (reinterpret_cast<usd::BackendInit>(init)(utf8(gPackageRoot).c_str(), &error) != 0)
        {
            b.error = error;
            return;
        }
        b.run = reinterpret_cast<usd::BackendRun>(run);
    });
    return b;
}

json runBackend(const char* command, const Args& a)
{
    const Backend& b = backend();
    if (!b.run)
        throw CommandError(std::string(command) + ": OpenUSD failed to load: " + b.error);
    json result;
    std::string error;
    if (b.run(command, &a, &result, &error) != 0)
        throw CommandError(error);
    return result;
}
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    gPackageRoot = ctx.root();
    using T = ArgType;
    if (!ctx.registerCommand({
            .name = "importUsd",
            .doc = "Import a USD file (.usd, .usda, .usdc or .usdz) into the scene. The stage is opened and composed first, so "
                   "references, payloads, sublayers and variant selections all resolve, and what arrives is the composed result.\n\n"
                   "Prims become nodes with their names and hierarchy: Xform, Scope and untyped prims transforms (every xformOp "
                   "kind and order resolved; translate, rotate in any order, scale and pivot land on the channels as authored), Mesh prims meshes with their n-gons, "
                   "normals, every UV set (st, st1, ... in order, then other texCoord primvars), displayColor/displayOpacity and "
                   "other color primvars as color sets, other numeric primvars as mesh attributes, edge creases, holes left out and "
                   "materialBind GeomSubsets as material slots. Cube, Sphere, Cylinder, Cone, Capsule and Plane are tessellated to meshes. "
                   "Cameras keep their lens, film back, clipping and projection; BasisCurves (linear, bezier, bspline, catmullRom; "
                   "periodic and pinned) and NurbsCurves become curve nodes. Invisible prims arrive hidden; proxy and guide prims are "
                   "skipped unless purposes asks for them.\n\n"
                   "UsdPreviewSurface materials become Lila materials (Unreal's default lit) with their textures: packed "
                   "occlusion/roughness/metallic textures as an ORM or MRO map, normal maps' green convention read from the texture's "
                   "scale, emissive intensity from its scale, and opacity from the base color texture's alpha as masked or translucent. "
                   "Direct and collection bindings both resolve. Textures inside a .usdz are unpacked beside it.\n\n"
                   "UsdSkel skeletons become joints, SkelBindingAPI skins skinClusters with their weights and bind pose, BlendShapes "
                   "morph targets with their in-betweens, and SkelAnimation keys on the joints and target weights. Time samples become "
                   "keys (linear, or stepped on a held stage), and the stage's timeCodesPerSecond, framesPerSecond and time range set the "
                   "scene's fps and playback range. Instanceable prims and PointInstancers are realized: each instance its own "
                   "transform, sharing the prototype's mesh data.\n\n"
                   "Axes and units convert as FBX import does: a Z-up stage is read in Unreal's axes, a Y-up one as Maya's, and "
                   "metersPerUnit scales everything to cm. Lights, point clouds and anything else Lila has no node for are listed in "
                   "the report's skipped entries (lights and point clouds keep their place as transforms). Returns the new node names, "
                   "or with report=True a dict of nodes, roots, joints, skinClusters, cameras, curves, materials, take, takes, fps, "
                   "start, end, keyedCurves, instances, upAxis, metersPerUnit, skipped and warnings.",
            .positionalDoc = "path",
            .flags = {{"parent", "p", T::String, nullptr, "Parent the imported roots under this node."},
                      {"prim", "", T::String, "", "Import only this prim and what is under it, by its path in the stage (\"/World/Hero\")."},
                      {"skins", "", T::Bool, true, "Rebuild skeleton bindings as skinClusters; False imports skinned meshes in their bind shape."},
                      {"animation", "anim", T::Bool, true, "Turn time samples (transforms, joints, blend shape weights, camera lens) into keys."},
                      {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the stage."},
                      {"materials", "", T::Bool, true, "Create materials from the bound UsdPreviewSurface networks and assign them."},
                      {"loadPayloads", "", T::Bool, true, "Load payloads; False leaves them out and lists them as skipped."},
                      {"variants", "", T::Any, nullptr,
                       "Variant selections before importing: {'set': 'variant'} for every prim with that set, or "
                       "{'/prim/path': {'set': 'variant'}} for one prim.",
                       {},
                       "importUsd(\"chair.usd\", variants={\"modelingVariant\": \"Rocking\"})"},
                      {"take", "", T::String, "", "For files Lila exported with several takes: the take to import (a variant of the root's 'take' set)."},
                      {"purposes", "", T::Any, nullptr,
                       "Prim purposes to import: a list of default, render, proxy and guide, or 'all'. Default and render by default."},
                      {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                      {"select", "", T::Bool, true, "Select the imported roots."}},
            .run = [](const Args& a) { return runBackend("importUsd", a); },
            .examples = {"importUsd(\"C:/assets/kitchen.usd\")", "importUsd(\"hero.usdz\", report=True)",
                         "importUsd(\"set.usd\", loadPayloads=False, variants={\"lod\": \"proxy\"})"},
        }))
        return 2;

    if (!ctx.registerCommand({
            .name = "exportUsd",
            .doc = "Export to a USD file. The extension picks the format: .usda text, .usdc binary, .usd binary (or text with "
                   "format='usda') and .usdz, a package holding the stage and its textures. Objects default to the selection (or "
                   "every root with selectionOnly=False); their descendants come too.\n\n"
                   "Defaults suit Unreal's USD importer: Z up, metersPerUnit 0.01 (centimetres), UsdPreviewSurface materials and "
                   "UsdSkel. The stage has one root prim named after the file, its defaultPrim, with the exported nodes under it and "
                   "the materials in a Materials scope. Transforms are written as translate, rotate (in the node's rotate order) and "
                   "scale ops, or a matrix when sheared. Meshes keep their n-gons (triangulate=True writes the viewport's triangles), "
                   "split normals, every UV set (as st, st1, ...), the current color set as displayColor/displayOpacity and the "
                   "others as color primvars, edge creases, and a GeomSubset per material slot when there are several. Materials map "
                   "to UsdPreviewSurface: a UsdUVTexture per map read through one UsdPrimvarReader_float2, packed ORM/MRO maps as "
                   "one texture feeding occlusion, roughness and metallic, DirectX normal maps flipped to USD's OpenGL convention by "
                   "the texture's scale and bias, emissive intensity as the texture's scale, masked and translucent opacity from the "
                   "base color's alpha, and twoSided as doubleSided on the meshes using it.\n\n"
                   "Joints become a Skeleton (bind and rest transforms) under a SkelRoot shared with the meshes skinned to it; skins "
                   "write jointIndices and jointWeights (pruned to maxInfluences) and the mesh's geomBindTransform, in the bind pose. "
                   "Morph targets become BlendShapes with their in-betweens; a mesh with morphs and no skin gets a one-joint skeleton "
                   "of its own so Unreal imports them. Nodes parented to joints are written beside the skeleton, their joint's motion "
                   "baked into them. Cameras write their lens, film back and clipping. Curves write as BasisCurves (linear or cubic "
                   "bezier) or, for NURBS, NurbsCurves. LOD groups whose levels are plain meshes become Unreal's LOD variant set.\n\n"
                   "Animation is sampled every frame over the range and written as time samples, at the take's fps "
                   "(timeCodesPerSecond and framesPerSecond) with the stage's start and end time codes. One take goes straight "
                   "into the file. Several (takes='all' or a list of names) become a variant set named 'take' on the root prim, one "
                   "variant per take with its own samples and its range kept in the prim's customData; the first take is selected, "
                   "which is what Unreal and other tools play, and importUsd(take=...) picks another. splitLayers=True writes the "
                   "geometry, materials and animation into sublayers (<file>.geometry, .materials and .anim) that the file "
                   "composes. Returns the path, or with report=True the files written and the warnings.",
            .positionalDoc = "path objects...",
            .flags = {{"format", "", T::String, "auto", "auto picks by extension; usda or usdc chooses the encoding of a .usd file.", {"auto", "usda", "usdc"}},
                      {"upAxis", "up", T::String, "z", "File up axis: z (Unreal) or y. Assets land the same way through either, as with FBX.", {"z", "y"}},
                      {"metersPerUnit", "mpu", T::Float, 0.01, "Scene unit of the file in metres: 0.01 is centimetres (Unreal), 1 metres. Lengths are scaled to it."},
                      {"materials", "mat", T::Bool, true, "Write UsdPreviewSurface materials and bind them."},
                      {"textures", "tex", T::String, "relative",
                       "How texture files are referenced: relative to the file, absolute, or copy (copied into <name>_textures beside "
                       "the file). A .usdz packages them whatever this says.",
                       {"relative", "absolute", "copy"}},
                      {"animation", "anim", T::Bool, true, "Write the animation as time samples."},
                      {"takes", "", T::Any, nullptr, "Scene takes to write: 'all' or a list of names, one 'take' variant each. Default: the active take.",
                       {}, "exportUsd(\"hero.usd\", \"Hero\", takes=\"all\")"},
                      {"start", "st", T::Float, nullptr, "First frame; the playback start by default."},
                      {"end", "et", T::Float, nullptr, "Last frame; the playback end by default."},
                      {"normals", "", T::Bool, true, "Write the split normals; False leaves them for the importer to compute."},
                      {"triangulate", "", T::Bool, false, "Write triangles exactly as the viewport splits faces instead of the n-gons."},
                      {"maxInfluences", "mi", T::Int, 8, "Skin influences per point, heaviest kept and renormalised (4 for mobile)."},
                      {"deformOnly", "do", T::Bool, true, "Deform joints only (IK helpers and rig-only joints stay behind)."},
                      {"atOrigin", "", T::Bool, false, "Drop the exported roots' own transforms, keeping their children relative."},
                      {"children", "", T::Bool, true, "Include the descendants of the given objects."},
                      {"worldSpace", "ws", T::Bool, false, "Bake world transforms into the meshes and flatten the hierarchy (current pose, no skins or animation)."},
                      {"lods", "", T::Bool, true, "Write LOD groups as Unreal's LOD variant set; False writes every level as a plain child."},
                      {"splitLayers", "", T::Bool, false, "Geometry, materials and animation in their own sublayers, composed by the file."},
                      {"selectionOnly", "sl", T::Bool, true, "Without objects: the selection, or every root when False."},
                      {"report", "", T::Bool, false, "Return {files, warnings} instead of the path."}},
            .undoable = false,
            .run = [](const Args& a) { return runBackend("exportUsd", a); },
            .examples = {"exportUsd(\"C:/out/hero.usd\", \"Hero\")", "exportUsd(\"C:/out/hero.usdz\", \"Hero\", textures=\"copy\")",
                         "exportUsd(\"C:/out/shot.usda\", selectionOnly=False, takes=\"all\", splitLayers=True)"},
        }))
        return 3;

    if (!io::translators::add(ctx, {.id = "usd",
                                    .label = "USD",
                                    .doc = "Universal Scene Description through OpenUSD: meshes, materials, skeletons, cameras, curves and "
                                           "animation, composition resolved on import.",
                                    .extensions = {".usd", ".usda", ".usdc", ".usdz"},
                                    .import = {.command = "importUsd"},
                                    .exportSelection = {.command = "exportUsd"}}))
        return 4;
    return 0;
}
