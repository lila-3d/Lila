// trimSheet* make, inspect, edit, generate, bake, import, export and send sheets; trim* map faces onto a sheet's trims
// (one trim, automatic hotspotting, shifting between strips, edge chamfers).

#include "TrimSheets.h"
#include "Generator.h"
#include "TrimBake.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/UnrealLink.h>
#include <lila/Uv.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

using namespace lila;

namespace trims
{
namespace
{
using scene::Graph;
using scene::Node;
using T = ArgType;
using Ids = scene::components::Ids;

constexpr const char* kPrefs = "trimSheets";

std::string presetChoices()
{
    std::string s;
    for (const PresetInfo& p : presets())
        s += (s.empty() ? "" : ", ") + p.id;
    return s;
}

std::vector<std::string> presetIds()
{
    std::vector<std::string> out;
    for (const PresetInfo& p : presets())
        out.push_back(p.id);
    return out;
}

FlagDef sheetFlag() { return {"sheet", "sh", T::String, nullptr, "The .lilatrim file; the current sheet (trimSheetCurrent) when not given."}; }
FlagDef uvSetFlag() { return {"uvSet", "", T::String, "uv0", "UV set to write."}; }

std::filesystem::path loadedPath(const Args& a, const char* cmd, std::string_view flag = "sheet")
{
    std::string p;
    if (a.has(std::string(flag)))
        p = a.get<std::string>(std::string(flag));
    else if (flag == "sheet" && !a.positional.empty() && a.positional[0].is_string())
        p = a.positional[0].get<std::string>();
    else
        p = currentSheet();
    if (p.empty())
        throw CommandError(std::string(cmd) + ": no sheet; give sheet= or make one with trimSheetCreate");
    std::filesystem::path f(p);
    if (f.is_relative() && Workspace::get().isOpen())
        f = Workspace::get().root() / f;
    if (!std::filesystem::exists(f))
        throw CommandError(std::string(cmd) + ": no trim sheet at " + f.generic_string());
    return f;
}

Sheet loadOr(const std::filesystem::path& f, const char* cmd)
{
    try
    {
        return load(f);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** A trim by index or name. */
int trimIndex(const Sheet& s, const json& v, const char* cmd)
{
    if (v.is_number_integer())
    {
        const int i = v.get<int>();
        if (i < 0 || size_t(i) >= s.trims.size())
            throw CommandError(std::format("{}: the sheet has {} trims, no trim {}", cmd, s.trims.size(), i));
        return i;
    }
    if (v.is_string())
        for (size_t i = 0; i < s.trims.size(); ++i)
            if (s.trims[i].name == v.get<std::string>())
                return int(i);
    throw CommandError(std::string(cmd) + ": no trim " + v.dump());
}

json trimSummary(const Sheet& s, size_t i)
{
    const Trim& t = s.trims[i];
    json j = t.toJson();
    j["index"] = int64_t(i);
    const glm::dvec2 w = s.worldSize(t);
    j["worldSizeCm"] = {w.x, w.y};
    const glm::dvec2 lo = s.uvMin(t), hi = s.uvMax(t);
    j["uv"] = {{"min", {lo.x, lo.y}}, {"max", {hi.x, hi.y}}};
    return j;
}

json sheetSummary(const Sheet& s, const std::filesystem::path& f)
{
    json trimsJ = json::array();
    for (size_t i = 0; i < s.trims.size(); ++i)
        trimsJ.push_back(trimSummary(s, i));
    const glm::dvec2 ext = s.worldExtent();
    json textures = json::object();
    for (const std::string& m : s.maps)
    {
        const std::filesystem::path p = mapFile(s, f, m);
        if (std::filesystem::exists(p))
            textures[m] = p.generic_string();
    }
    return {{"file", f.generic_string()},
            {"name", s.name},
            {"size", {s.width, s.height}},
            {"pxPerM", s.pxPerM},
            {"worldSizeCm", {ext.x, ext.y}},
            {"preset", s.preset},
            {"material", materialName(s)},
            {"textures", textures},
            {"baked", !textures.empty()},
            {"warnings", validate(s)},
            {"trims", trimsJ}};
}

/** The rotation that fits an island onto one trim best. */
bool bestRotation(const uv::IslandMeasure& m, const uv::HotspotTrim& t)
{
    if (!t.allowRotate || m.width <= 0.0 || m.height <= 0.0)
        return false;
    switch (t.kind)
    {
    case uv::TrimKind::StripU: return m.height > m.width;
    case uv::TrimKind::StripV: return m.width > m.height;
    default:
    {
        const double ta = t.worldSize.x / std::max(t.worldSize.y, 1e-9);
        return std::abs(std::log2((m.height / m.width) / ta)) < std::abs(std::log2((m.width / m.height) / ta));
    }
    }
}

/** Writes the trim index (+1, 0 for none) on the faces and points them at the sheet's material slot. */
void tagFaces(geo::Mesh& m, std::span<const geo::Index> faces, int trim, const std::string& slot)
{
    std::span<int32_t> attr = m.faceAttrs().getOrAdd<int32_t>(kFaceTrim);
    std::vector<uint32_t> ids;
    for (geo::Index f : faces)
        if (m.faceValid(f))
        {
            attr[f] = trim + 1;
            ids.push_back(f);
        }
    if (!ids.empty())
        scene::materials::assign(m, slot, ids);
}

/** Splits the faces by angle, unwraps them and returns their islands, ready for measureChart. */
std::vector<uv::Chart> freshIslands(geo::Mesh& m, std::span<const geo::Index> faces, const std::string& set, double angle)
{
    if (angle > 0.0)
        uv::markSeamsByAngle(m, faces, angle);
    uv::UnwrapFacesOptions uo;
    uo.keepPlacement = false;
    uo.pack = false;
    uv::unwrapFaces(m, faces, set, uo);
    uv::ChartOptions co;
    co.uvSet = set;
    return uv::buildCharts(m, faces, co);
}

std::vector<geo::Index> facesOrAll(const geo::Mesh& m, const Ids& ids)
{
    return uv::liveFaces(m, std::span<const geo::Index>(ids.data(), ids.size()));
}

/** Marks the mesh node with the sheet it is mapped with, so sends find it. */
void tagNode(const std::string& node, const std::filesystem::path& sheet)
{
    try
    {
        ops::execute("getAttr", {{"args", {node + "." + kNodeSheet}}});
    }
    catch (const std::exception&)
    {
        ops::execute("addAttr", {{"args", {node}}, {"longName", kNodeSheet}, {"dataType", "string"}, {"doc", "Trim sheet the faces are mapped with."}});
    }
    ops::execute("setAttr", {{"args", {node + "." + kNodeSheet, sheet.generic_string()}}});
}

/** The Lila material the sheet's faces use, made or updated with the baked maps. */
void ensureMaterial(const Sheet& s, const std::filesystem::path& f)
{
    Graph& g = Graph::current();
    const std::string name = materialName(s);
    json call = {{"args", {name}}};
    const std::pair<const char*, const char*> maps[] = {{"normal", "normalMap"}, {"ao", "ambientOcclusionMap"}};
    json sets = json::object();
    for (const auto& [map, attr] : maps)
        if (const std::filesystem::path p = mapFile(s, f, map); std::filesystem::exists(p))
            sets[attr] = p.generic_string();
    if (!scene::materials::find(g, name))
    {
        call["baseColor"] = {0.45, 0.45, 0.45};
        call["roughness"] = 0.5;
        ops::execute("createMaterial", call);
    }
    for (const auto& [attr, value] : sets.items())
        ops::execute("setAttr", {{"args", {name + "." + attr, value}}});
}

/** Sheet trims ordered top to bottom then left to right, among the same class (strips or rectangles) as trim i. */
std::vector<int> neighbourOrder(const Sheet& s, int i)
{
    auto isStrip = [&](int k) {
        const uv::TrimKind kind = s.trims[size_t(k)].kind;
        return kind == uv::TrimKind::StripU || kind == uv::TrimKind::StripV;
    };
    std::vector<int> out;
    for (int k = 0; k < int(s.trims.size()); ++k)
        if (isStrip(k) == isStrip(i) && !s.trims[size_t(k)].reserved)
            out.push_back(k);
    std::ranges::stable_sort(out, [&](int a, int b) {
        const Trim &ta = s.trims[size_t(a)], &tb = s.trims[size_t(b)];
        return ta.min.y != tb.min.y ? ta.min.y < tb.min.y : ta.min.x < tb.min.x;
    });
    return out;
}

uv::HotspotOptions hotspotOptions(const Args& a)
{
    uv::HotspotOptions o;
    o.splitAngle = a.get<double>("splitAngle", o.splitAngle);
    const std::string orient = a.get<std::string>("orient", "worldUp");
    o.orient = orient == "asIs" ? uv::HotspotOrient::AsIs : orient == "longestU" ? uv::HotspotOrient::LongestU : uv::HotspotOrient::WorldUp;
    o.tag = a.get<std::string>("tag", "");
    o.seed = uint32_t(a.get<int64_t>("seed", 0));
    o.cycle = int(a.get<int64_t>("cycle", 0));
    o.randomOffset = a.get<bool>("randomOffset", true);
    o.randomFlip = a.get<bool>("randomFlip", false);
    o.randomMirror = a.get<bool>("randomMirror", false);
    o.maxSizeError = a.get<double>("maxSizeError", o.maxSizeError);
    o.aspectWeight = a.get<double>("aspectWeight", o.aspectWeight);
    o.sizeWeight = a.get<double>("sizeWeight", o.sizeWeight);
    o.normalWeight = a.get<double>("normalWeight", o.normalWeight);
    o.allowRotate = a.get<bool>("allowRotate", true);
    return o;
}

std::vector<FlagDef> hotspotFlags()
{
    return {sheetFlag(),
            uvSetFlag(),
            {"splitAngle", "sa", T::Float, 30.0, "Faces meeting sharper than this, degrees, become separate islands; 0 keeps the seams.", {}, {}, 0.0, 180.0},
            {"orient", "o", T::String, "worldUp", "worldUp (up runs up the trim), longestU or asIs.", {"worldUp", "longestU", "asIs"}},
            {"tag", "tg", T::String, "", "Only trims with this tag (edge, radial, flat or your own); unique and flat trims need one."},
            {"seed", "s", T::Int, int64_t(0), "Variation seed: offsets along strips, picks among equal trims."},
            {"cycle", "c", T::Int, int64_t(0), "Take the n-th best trim instead of the best: run again with cycle+1 to try the next."},
            {"randomOffset", "ro", T::Bool, true, "Slide islands along tiling strips so repeats don't line up."},
            {"randomFlip", "rf", T::Bool, false, "Turn islands 180 degrees at random where the trim allows it."},
            {"randomMirror", "rm", T::Bool, false, "Mirror islands at random."},
            {"allowRotate", "ar", T::Bool, true, "Let islands turn 90 degrees to fit."},
            {"maxSizeError", "mse", T::Float, 1.0, "Islands off a trim's world size by more than this many doublings count as poor matches.", {}, {}, 0.0, 8.0},
            {"aspectWeight", "aw", T::Float, 1.0, "How much shape matters.", {}, {}, 0.0, 10.0},
            {"sizeWeight", "sw", T::Float, 1.0, "How much world size matters.", {}, {}, 0.0, 10.0},
            {"normalWeight", "nw", T::Float, 0.5, "How much facing matters, for trims with a preferred direction.", {}, {}, 0.0, 10.0}};
}

/** The bake result recorded beside the textures, for sends. */
std::filesystem::path bakeRecord(const Sheet& s, const std::filesystem::path& f) { return textureFolder(s, f) / (s.name + "_bake.json"); }

json runBake(const Sheet& s, const std::filesystem::path& f, const SheetBakeOptions& o, const char* cmd)
{
    json r;
    try
    {
        r = bakeSheet(s, f, o);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": baking failed: " + e.what());
    }
    if (o.only.empty())
    {
        std::ofstream out(bakeRecord(s, f));
        out << r.dump(2);
    }
    ensureMaterial(s, f);
    return r;
}
} // namespace

std::string materialName(const Sheet& s)
{
    std::string base = s.name;
    if (base.starts_with("T_"))
        base = base.substr(2);
    return "M_" + base;
}

std::string currentSheet() { return PrefStore::get().value<std::string>(kPrefs, "/current", std::string()); }

void setCurrentSheet(const std::string& path) { PrefStore::get().set(kPrefs, "/current", path); }

std::filesystem::path defaultSheetFolder()
{
    if (Workspace::get().isOpen())
        return Workspace::get().root() / "TrimSheets";
    return link::cacheDir().parent_path() / "TrimSheets";
}

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "trimSheetCreate",
        .doc = "Make a trim sheet from a preset and bake it: the layout and every trim's profile go in a .lilatrim file, the generated "
               "high is baked onto a flat plane into normal, AO, curvature, height and cavity maps plus an exact trim ID map, and a "
               "Lila material M_<name> shows them. An existing file is kept (its settings may have been changed) and only baked when "
               "its textures are missing, so calling this again is cheap. Presets: " + presetChoices() + ". The new sheet becomes "
               "current. Returns the sheet summary (trims with world sizes, textures, warnings) and the bake result.",
        .flags = {{"preset", "p", T::String, "industrialMetal", "Layout and profiles to start from.", presetIds()},
                  {"width", "w", T::Int, int64_t(0), "Texture width, px (0: the preset's).", {}, {}, 0.0, 8192.0},
                  {"file", "f", T::String, nullptr, "Where to save; default <workspace>/TrimSheets/<name>.lilatrim."},
                  {"name", "n", T::String, nullptr, "Sheet name (texture prefix); default the preset's, T_<Preset>."},
                  {"pxPerM", "", T::Float, nullptr, "Texels per metre: sets every trim's world size.", {}, {}, 1.0},
                  {"bake", "b", T::Bool, true, "Bake when the textures are missing."},
                  {"overwrite", "", T::Bool, false, "Replace an existing file with the preset."},
                  {"backend", "", T::String, "auto", "Bake backend: auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Sheet s;
            try
            {
                s = preset(a.get<std::string>("preset", "industrialMetal"), int(a.get<int64_t>("width", 0)));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("trimSheetCreate: ") + e.what());
            }
            if (a.has("name"))
                s.name = a.get<std::string>("name");
            if (a.has("pxPerM"))
                s.pxPerM = std::max(1.0, a.get<double>("pxPerM"));
            std::filesystem::path f = a.has("file") ? std::filesystem::path(a.get<std::string>("file")) : defaultSheetFolder() / (s.name + kExtension);
            if (f.extension().empty())
                f += kExtension;
            const bool existed = std::filesystem::exists(f);
            if (existed && !a.get<bool>("overwrite", false))
                s = loadOr(f, "trimSheetCreate");
            else
                save(s, f);
            setCurrentSheet(f.generic_string());

            json out = sheetSummary(s, f);
            out["created"] = !existed || a.get<bool>("overwrite", false);
            if (a.get<bool>("bake", true) && (!out["baked"].get<bool>() || out["created"].get<bool>()))
            {
                SheetBakeOptions o;
                o.backend = a.get<std::string>("backend", "auto");
                out["bake"] = runBake(s, f, o, "trimSheetCreate");
                out["textures"] = sheetSummary(s, f)["textures"];
                out["baked"] = true;
            }
            return out;
        },
        .examples = {"trimSheetCreate(preset='sciFiHull')", "trimSheetCreate(preset='woodTrims', width=1024, pxPerM=1024)['trims'][0]"},
    });

    ctx.registerCommand({
        .name = "trimSheetCurrent",
        .doc = "The current trim sheet, which trim tools use when no sheet= is given; give a .lilatrim to make it current.",
        .positionalDoc = "sheet to make current (optional)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.positional.empty() && a.positional[0].is_string())
            {
                const std::filesystem::path f = loadedPath(a, "trimSheetCurrent");
                setCurrentSheet(f.generic_string());
            }
            return currentSheet();
        },
    });

    ctx.registerCommand({
        .name = "trimSheetInfo",
        .doc = "What a trim sheet holds: size, texels per metre, world size, each trim (index, name, kind, pixel rect, uv rect, world "
               "size in cm, tags, profile), which textures are baked, and layout warnings (overlaps, borders off the mip grid, "
               "mixed bevel widths).",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetInfo");
            return sheetSummary(loadOr(f, "trimSheetInfo"), f);
        },
        .examples = {"[t['name'] for t in trimSheetInfo()['trims']]"},
    });

    ctx.registerCommand({
        .name = "trimSheetEdit",
        .doc = "Change a trim sheet and save it. values= merges into the trim given by trim= (index or name), or into the sheet "
               "itself without trim=: e.g. {'profile': {'style': 'groove', 'bevelPx': 6}}, {'tags': ['edge']}, {'pxPerM': 1024}. "
               "add= appends a trim ({'name', 'min': [x, y], 'max': [x, y], 'kind', 'profile'}) and remove= deletes one. Profile "
               "styles: flat, raised, recessed, doubleBevel, step, halfPipe, groove, ribs, rivets, bolts, vents, tread, planks. "
               "Kinds: stripU, stripV, rect, radial, unique, flat. Rebake afterwards (trimSheetBake, trims= for just the changed "
               "ones). Returns the sheet summary.",
        .flags = {sheetFlag(),
                  {"trim", "t", T::Any, nullptr, "Trim index or name to change."},
                  {"values", "v", T::Any, nullptr, "Fields to merge."},
                  {"add", "", T::Any, nullptr, "A trim to append."},
                  {"remove", "", T::Any, nullptr, "Trim index or name to delete."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetEdit");
            Sheet s = loadOr(f, "trimSheetEdit");
            if (a.has("values"))
            {
                const json v = a.flags["values"];
                if (!v.is_object())
                    throw CommandError("trimSheetEdit: values is a dict of fields");
                if (a.has("trim"))
                {
                    const int i = trimIndex(s, a.flags["trim"], "trimSheetEdit");
                    json t = s.trims[size_t(i)].toJson();
                    t.merge_patch(v);
                    s.trims[size_t(i)] = Trim::fromJson(t);
                }
                else
                {
                    json j = s.toJson();
                    j.merge_patch(v);
                    s = Sheet::fromJson(j);
                }
            }
            if (a.has("add"))
                s.trims.push_back(Trim::fromJson(a.flags["add"]));
            if (a.has("remove"))
                s.trims.erase(s.trims.begin() + trimIndex(s, a.flags["remove"], "trimSheetEdit"));
            save(s, f);
            return sheetSummary(s, f);
        },
        .examples = {"trimSheetEdit(trim='Plate128', values={'profile': {'style': 'rivets', 'spacingPx': 48}})",
                     "trimSheetEdit(add={'name': 'Cap', 'min': [0, 1792], 'max': [256, 2048], 'kind': 'radial', 'profile': {'style': 'halfPipe'}})"},
    });

    ctx.registerCommand({
        .name = "trimSheetGenerate",
        .doc = "Put a sheet's generated high and low in the scene as <name>_High and <name>_Low (replacing earlier ones), to look at "
               "or to sculpt on before baking from them yourself. The plane lies on XZ facing +Y, one texel per pxPerM. Returns both "
               "names and the high's face count.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(), {"tile", "", T::Bool, false, "Repeat tiling strips one sheet width to each side, as the bake does."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetGenerate");
            const Sheet s = loadOr(f, "trimSheetGenerate");
            GenerateOptions go;
            go.tile = a.get<bool>("tile", false);
            Generated gen = generate(s, go);
            Graph& g = Graph::current();
            json out = json::object();
            for (auto [suffix, mesh] : {std::pair<const char*, geo::Mesh*>{"_High", &gen.high}, {"_Low", &gen.low}})
            {
                const std::string name = s.name + suffix;
                if (Node* old = g.find(name))
                    ops::execute("delete", {{"args", {old->path()}}});
                json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
                const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                Node* n = g.find(path);
                g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(*mesh))));
                out[std::string(suffix).substr(1) == "High" ? "high" : "low"] = path;
            }
            out["highFaces"] = int64_t(gen.high.numFaces());
            out["reliefCm"] = {gen.above, gen.below};
            return out;
        },
    });

    ctx.registerCommand({
        .name = "trimSheetBake",
        .doc = "Bake a trim sheet: its generated high onto the flat low, tiling strips wrapped so the textures tile, maps per the "
               "sheet's maps list, into its texture folder as T_<name>_<map>.png, plus an exact trim ID map. trims= rebakes just "
               "those trims into the existing textures (after trimSheetEdit). Updates the Lila material M_<name>. Returns the "
               "Baker's result, the files, seamError (0 is seamless across the U wrap) and seconds.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"trims", "t", T::List, nullptr, "Trim indices or names to rebake; all when not given."},
                  {"backend", "", T::String, "auto", "auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetBake");
            const Sheet s = loadOr(f, "trimSheetBake");
            SheetBakeOptions o;
            o.backend = a.get<std::string>("backend", "auto");
            if (a.has("trims"))
                for (const json& t : a.flags["trims"])
                    o.only.push_back(size_t(trimIndex(s, t, "trimSheetBake")));
            return runBake(s, f, o, "trimSheetBake");
        },
        .examples = {"trimSheetBake()", "trimSheetBake(trims=['Plate128'])"},
    });

    ctx.registerCommand({
        .name = "trimSheetImport",
        .doc = "Make a trim sheet from another tool's layout: a Valve .rect (needs width and height), an SVG of rects, an ID map "
               "image (one trim per flat colour), or a mesh's uv islands (source is a mesh name). Full-width rows become tiling "
               "strips, the rest hotspot rectangles, all with a raised bevel profile to change with trimSheetEdit. Saves to file= "
               "(default beside the source) and makes it current. Returns the sheet summary.",
        .positionalDoc = "source file or mesh",
        .flags = {{"file", "f", T::String, nullptr, "Where to save the .lilatrim."},
                  {"width", "w", T::Int, int64_t(2048), "Sheet width, px, for .rect and mesh sources.", {}, {}, 16.0, 16384.0},
                  {"height", "h", T::Int, int64_t(2048), "Sheet height, px, for .rect and mesh sources.", {}, {}, 16.0, 16384.0},
                  {"name", "n", T::String, nullptr, "Sheet name; default T_<source name>."},
                  {"pxPerM", "", T::Float, 512.0, "Texels per metre.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("trimSheetImport: give a .rect, .svg or image file, or a mesh");
            const std::string src = a.positional[0].get<std::string>();
            const int w = int(a.get<int64_t>("width", 2048)), h = int(a.get<int64_t>("height", 2048));
            Sheet s;
            std::filesystem::path out;
            Graph& g = Graph::current();
            if (Node* n = g.find(src); n && n->type().isA("mesh"))
            {
                g.evaluate();
                const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
                if (!m)
                    throw CommandError("trimSheetImport: " + src + " has no mesh");
                s = fromUvs(*m, "uv0", w, h);
                out = defaultSheetFolder() / ("T_" + n->name() + kExtension);
                s.name = "T_" + n->name();
            }
            else
            {
                const std::filesystem::path p(src);
                std::string ext = p.extension().string();
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                std::ifstream in(p, std::ios::binary);
                if (!in)
                    throw CommandError("trimSheetImport: can't read " + src);
                std::stringstream text;
                text << in.rdbuf();
                if (ext == ".rect")
                    s = fromRect(text.str(), w, h);
                else if (ext == ".svg")
                    s = fromSvg(text.str());
                else
                {
                    std::string err;
                    const image::Image img = image::read(p, &err);
                    if (img.width == 0)
                        throw CommandError("trimSheetImport: " + src + ": " + err);
                    s = fromIdMap(img);
                }
                s.name = "T_" + p.stem().string();
                out = p.parent_path() / (p.stem().string() + kExtension);
            }
            if (s.trims.empty())
                throw CommandError("trimSheetImport: found no trims in " + src);
            if (a.has("name"))
                s.name = a.get<std::string>("name");
            s.pxPerM = std::max(1.0, a.get<double>("pxPerM", 512.0));
            if (a.has("file"))
                out = a.get<std::string>("file");
            save(s, out);
            setCurrentSheet(out.generic_string());
            return sheetSummary(s, out);
        },
        .examples = {"trimSheetImport('C:/maps/metal_trims.rect', width=1024, height=1024)", "trimSheetImport('C:/maps/hotspots_id.png')"},
    });

    ctx.registerCommand({
        .name = "trimSheetExport",
        .doc = "Write a trim sheet's layout for another tool: rect (Valve .rect for Source 2 and Hammer), svg, idmap (a png with "
               "one flat colour per trim) or json. Returns the file written.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"format", "fmt", T::String, "rect", "rect, svg, idmap or json.", {"rect", "svg", "idmap", "json"}},
                  {"file", "f", T::String, nullptr, "Output file; default beside the sheet."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetExport");
            const Sheet s = loadOr(f, "trimSheetExport");
            const std::string fmt = a.get<std::string>("format", "rect");
            const char* ext = fmt == "svg" ? ".svg" : fmt == "idmap" ? ".png" : fmt == "json" ? ".json" : ".rect";
            const std::filesystem::path out = a.has("file") ? std::filesystem::path(a.get<std::string>("file")) : f.parent_path() / (f.stem().string() + ext);
            if (fmt == "idmap")
                image::write(out, idMap(s), 8);
            else
            {
                std::ofstream o(out, std::ios::binary);
                if (!o)
                    throw CommandError("trimSheetExport: can't write " + out.generic_string());
                o << (fmt == "svg" ? toSvg(s) : fmt == "json" ? s.toJson().dump(2) : toRect(s));
            }
            return out.generic_string();
        },
    });

    ctx.registerCommand({
        .name = "trimApply",
        .doc = "Map the selected faces (whole selected meshes in object mode) onto one trim: each island is split by angle, "
               "rectified, turned to suit the trim and fitted to it (stretched to a strip's height and slid along it, or fitted "
               "inside a rectangle). Faces get the sheet's material and remember their trim (face int 'trim', index + 1). "
               "Returns per mesh the islands placed.",
        .flags = [] {
            std::vector<FlagDef> f = {sheetFlag(),
                                      uvSetFlag(),
                                      {"trim", "t", T::Any, nullptr, "Trim index or name."},
                                      {"rotate", "r", T::String, "auto", "auto (whichever fits), on or off.", {"auto", "on", "off"}},
                                      {"splitAngle", "sa", T::Float, 30.0, "Split islands where faces meet sharper than this, degrees.", {}, {}, 0.0, 180.0},
                                      {"orient", "o", T::String, "worldUp", "worldUp, longestU or asIs.", {"worldUp", "longestU", "asIs"}},
                                      {"seed", "s", T::Int, int64_t(0), "Offset along strips."},
                                      {"randomOffset", "ro", T::Bool, true, "Slide islands along tiling strips."}};
            return f;
        }(),
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimApply");
            const Sheet s = loadOr(f, "trimApply");
            if (!a.has("trim"))
                throw CommandError("trimApply: give trim= (index or name)");
            const int ti = trimIndex(s, a.flags["trim"], "trimApply");
            const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
            const std::string set = a.get<std::string>("uvSet", "uv0");
            const std::string rotate = a.get<std::string>("rotate", "auto");
            uv::HotspotOptions o = hotspotOptions(a);
            std::vector<std::string> nodes;
            json out = uv::editUvMeshes("Apply Trim", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node& node) -> json {
                const std::vector<geo::Index> faces = facesOrAll(m, ids);
                std::vector<uv::Chart> charts = freshIslands(m, faces, set, o.splitAngle);
                for (uv::Chart& ch : charts)
                {
                    const uv::IslandMeasure ms = uv::measureChart(ch, o);
                    const bool rot = rotate == "on" || (rotate == "auto" && bestRotation(ms, trims[size_t(ti)]));
                    const uint64_t key = ch.faces.empty() ? 0 : ch.faces.front();
                    uv::placeChart(ch, trims[size_t(ti)], rot, o, uv::hotspotRandom(o.seed, key, 1), uv::hotspotRandom(o.seed, key, 2),
                                   uv::hotspotRandom(o.seed, key, 3));
                }
                uv::writeCharts(m, charts, set);
                tagFaces(m, faces, ti, materialName(s));
                nodes.push_back(node.path());
                return {{"islands", int64_t(charts.size())}, {"trim", s.trims[size_t(ti)].name}};
            });
            for (const std::string& n : nodes)
                tagNode(n, f);
            return out;
        },
        .examples = {"trimApply(trim='Plate128')", "trimApply(trim=3, rotate='on')"},
    });

    ctx.registerCommand({
        .name = "trimAuto",
        .doc = "Hotspot the selected faces (whole selected meshes in object mode) onto a trim sheet in one go: islands split by "
               "angle and rectified, measured in world size from 3D, matched to the trim whose shape and size fit best (strips "
               "for long islands, rectangles for panels, round trims for discs), then placed with seeded variation. Same seed, "
               "same result; cycle=n takes the n-th best match. showError paints a colour set trimError (green good, yellow loose, "
               "red poor or unmatched). Faces get the sheet's material and remember their trim. Returns per mesh: islands, "
               "matched, poor, failed, and each island's trim and size error.",
        .flags = [] {
            std::vector<FlagDef> f = hotspotFlags();
            f.push_back({"showError", "se", T::Bool, false, "Paint match quality into the colour set trimError."});
            return f;
        }(),
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimAuto");
            const Sheet s = loadOr(f, "trimAuto");
            const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
            const std::string set = a.get<std::string>("uvSet", "uv0");
            const uv::HotspotOptions o = hotspotOptions(a);
            const bool showError = a.get<bool>("showError", false);
            std::vector<std::string> nodes;
            json out = uv::editUvMeshes("Auto Trim", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node& node) -> json {
                const std::vector<geo::Index> faces = facesOrAll(m, ids);
                const uv::HotspotResult r = uv::hotspot(m, faces, trims, set, o);
                json islands = json::array();
                if (showError)
                    geo::colors::add(m, "trimError", glm::vec4(1.0f));
                for (const uv::HotspotIsland& is : r.islands)
                {
                    tagFaces(m, is.faces, is.trim, materialName(s));
                    islands.push_back({{"faces", int64_t(is.faces.size())},
                                       {"trim", is.trim >= 0 ? json(s.trims[size_t(is.trim)].name) : json(nullptr)},
                                       {"sizeError", is.sizeError},
                                       {"rotated", is.rotated},
                                       {"poor", is.poor},
                                       {"sizeCm", {is.measure.width, is.measure.height}}});
                    if (showError)
                    {
                        const glm::vec4 c = is.trim < 0 || is.poor ? glm::vec4(0.9f, 0.15f, 0.1f, 1.0f)
                                            : is.sizeError > o.maxSizeError * 0.5 ? glm::vec4(0.95f, 0.8f, 0.1f, 1.0f)
                                                                                  : glm::vec4(0.2f, 0.8f, 0.3f, 1.0f);
                        std::span<glm::vec4> col = geo::colors::values(m, "trimError");
                        for (geo::Index fc : is.faces)
                            m.forEachFaceCorner(fc, [&](geo::Index k) { col[k] = c; });
                    }
                }
                nodes.push_back(node.path());
                return {{"islands", int64_t(r.islands.size())},
                        {"matched", int64_t(r.matched)},
                        {"poor", int64_t(r.poor)},
                        {"failed", int64_t(r.failed)},
                        {"seamsAdded", int64_t(r.seamsAdded)},
                        {"details", islands}};
            });
            for (const std::string& n : nodes)
                tagNode(n, f);
            return out;
        },
        .examples = {"trimAuto()", "trimAuto(seed=3, randomFlip=True, showError=True)", "trimAuto(tag='edge', orient='longestU')"},
    });

    ctx.registerCommand({
        .name = "trimShift",
        .doc = "Move the selected faces' islands to the next trim up or down the sheet (Insomniac's shift UVs): strips step to the "
               "neighbouring strip, rectangles to the neighbouring rectangle, keeping how each island was turned. Faces must "
               "have been mapped by trimApply or trimAuto. Returns per mesh how many islands moved and where.",
        .flags = {sheetFlag(), uvSetFlag(), {"steps", "n", T::Int, int64_t(1), "Trims to move; negative goes up the sheet.", {}, {}, -64.0, 64.0}},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimShift");
            const Sheet s = loadOr(f, "trimShift");
            const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
            const std::string set = a.get<std::string>("uvSet", "uv0");
            const int steps = int(a.get<int64_t>("steps", 1));
            uv::HotspotOptions o;
            o.orient = uv::HotspotOrient::AsIs;
            o.randomOffset = false;
            return uv::editUvMeshes("Shift Trim", scene::ComponentMode::Face, [&](geo::Mesh& m, const Ids& ids, Node&) -> json {
                const std::vector<geo::Index> faces = facesOrAll(m, ids);
                std::span<const int32_t> attr = std::as_const(m).faceAttrs().get<int32_t>(kFaceTrim);
                if (attr.empty())
                    throw CommandError("trimShift: these faces are not mapped to a trim; use trimApply or trimAuto first");
                std::map<int, std::vector<geo::Index>> byTrim;
                for (geo::Index fc : faces)
                    if (attr[fc] > 0 && size_t(attr[fc]) <= s.trims.size())
                        byTrim[attr[fc] - 1].push_back(fc);
                json moved = json::array();
                for (const auto& [from, group] : byTrim)
                {
                    const std::vector<int> order = neighbourOrder(s, from);
                    const auto at = std::ranges::find(order, from);
                    const int pos = std::clamp(int(at - order.begin()) + steps, 0, int(order.size()) - 1);
                    const int to = order[size_t(pos)];
                    uv::ChartOptions co;
                    co.source = uv::IslandSource::Uvs;
                    co.uvSet = set;
                    std::vector<uv::Chart> charts = uv::buildCharts(m, group, co);
                    for (uv::Chart& ch : charts)
                    {
                        const glm::dvec2 before = ch.uvMax() - ch.uvMin();
                        uv::measureChart(ch, o);
                        const glm::dvec2 after = ch.uvMax() - ch.uvMin();
                        // Measuring rectifies along the longer side; keep the turn the island had on its old trim.
                        const bool turned = (before.x >= before.y) != (after.x >= after.y);
                        uv::placeChart(ch, trims[size_t(to)], turned, o, 0.0, 1.0, 1.0);
                    }
                    uv::writeCharts(m, charts, set);
                    tagFaces(m, group, to, materialName(s));
                    moved.push_back({{"from", s.trims[size_t(from)].name}, {"to", s.trims[size_t(to)].name}, {"islands", int64_t(charts.size())}});
                }
                return {{"moved", moved}};
            });
        },
        .examples = {"trimShift(steps=1)", "trimShift(steps=-2)"},
    });

    ctx.registerCommand({
        .name = "trimEdges",
        .doc = "Trim the selected edges with a bevel strip: each edge gets a one-segment chamfer of width= cm, the chamfer faces are "
               "hotspotted onto the sheet's edge-tagged trims (long side along U, so the bevel's crest runs along the chamfer), and "
               "the meshes get face-weighted normals so their big faces stay flat. Returns the bevel, the trim result and the "
               "chamfer faces.",
        .flags = {sheetFlag(),
                  uvSetFlag(),
                  {"width", "w", T::Float, 1.0, "Chamfer width, cm.", {}, {}, 0.01},
                  {"tag", "tg", T::String, "edge", "Trims to use for the chamfers."},
                  {"weightedNormals", "wn", T::Bool, true, "Set face-weighted normals on the meshes afterwards."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimEdges");
            if (scene::components::mode() != scene::ComponentMode::Edge)
                throw CommandError("trimEdges: select edges");
            json out = json::object();
            out["bevel"] = ops::execute("polyBevel", {{"offset", a.get<double>("width", 1.0)}, {"segments", 1}, {"smoothingAngle", 180.0}});
            out["trim"] = ops::execute("trimAuto", {{"sheet", f.generic_string()},
                                                    {"uvSet", a.get<std::string>("uvSet", "uv0")},
                                                    {"tag", a.get<std::string>("tag", "edge")},
                                                    {"orient", "longestU"},
                                                    {"splitAngle", 0.0}});
            if (a.get<bool>("weightedNormals", true))
                out["normals"] = uv::editUvMeshes("Weighted Normals", scene::ComponentMode::Face, [](geo::Mesh& m, const Ids&, Node&) -> json {
                    geo::applyWeightedNormals(m);
                    return true;
                });
            return out;
        },
        .examples = {"trimEdges(width=1.5)"},
    });

    ctx.registerCommand({
        .name = "trimSheetSend",
        .doc = "Send a trim sheet to Unreal: its baked textures go to <folder>/Textures, the trim master material M_LilaTrim is "
               "generated (or rebuilt with regenerateMaster) and an instance MI_<name> is made with the sheet's maps, and every "
               "mapped mesh in the scene that uses this sheet gets the instance on its sheet slot. Returns what Unreal made; "
               "wait=False queues it.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"folder", "f", T::String, "/Game/Lila/Trims", "Content folder; the sheet's assets go in <folder>/<name>."},
                  {"regenerateMaster", "", T::Bool, false, "Rebuild M_LilaTrim's graph from this version of Lila, discarding edits to it."},
                  {"wait", "w", T::Bool, true, "Block until Unreal has imported everything."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetSend");
            const Sheet s = loadOr(f, "trimSheetSend");
            std::ifstream rec(bakeRecord(s, f));
            if (!rec)
                throw CommandError("trimSheetSend: " + s.name + " is not baked yet; run trimSheetBake");
            json baked;
            rec >> baked;
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("trimSheetSend: not connected to Unreal: ") + e.what());
            }
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::ranges::find(features, json("trimMaterials")) == features.end())
                throw CommandError("trimSheetSend: the LilaLink plugin in this editor is too old for trim sheets; rebuild it from Lila's ue/LilaLink");

            const std::string folder = a.get<std::string>("folder", "/Game/Lila/Trims") + "/" + s.name;
            const std::map<std::string, std::string> params = {
                {"normal", "SheetNormalMap"}, {"ao", "SheetAOMap"}, {"curvature", "SheetCurvatureMap"}, {"cavity", "SheetCavityMap"}, {"height", "SheetHeightMap"}};
            json textures = json::object();
            for (const json& o : baked.value("outputs", json::array()))
            {
                const auto param = params.find(o.value("map", std::string()));
                if (param == params.end())
                    continue;
                const std::string asset = link::textureAssetName(o.value("file", std::string()), o.value("udim", 1001));
                textures[param->second] = folder + "/Textures/" + asset + "." + asset;
            }
            json meshes = json::array();
            Graph& g = Graph::current();
            for (const scene::NodeId id : g.nodes())
            {
                Node* n = g.node(id);
                if (!n || !n->type().isA("mesh"))
                    continue;
                try
                {
                    const json v = ops::execute("getAttr", {{"args", {n->path() + "." + kNodeSheet}}});
                    if (v.is_string() && std::filesystem::path(v.get<std::string>()).lexically_normal() == f.lexically_normal())
                        if (const json m = link::mappingOf(n->path()); m.is_object() && m.contains("contentPath"))
                            meshes.push_back(m["contentPath"]);
                }
                catch (const std::exception&)
                {
                }
            }
            const glm::dvec2 ext = s.worldExtent();
            json request = {{"folder", folder + "/Materials"},
                            {"regenerate", a.get<bool>("regenerateMaster", false)},
                            {"slot", materialName(s)},
                            {"meshes", meshes},
                            {"instances", json::array({{{"name", "MI_" + materialName(s).substr(2)},
                                                        {"textures", textures},
                                                        {"scalars", {{"SheetWidthCm", ext.x}, {"SheetHeightCm", ext.y}}}}})}};
            const json importParams = link::textureImportParams(baked, {{"folder", folder + "/Textures"}, {"materialMode", "none"}});

            auto send = [importParams, request]() -> json {
                std::shared_ptr<link::RpcClient> client = link::Link::get().client();
                if (!client)
                    throw std::runtime_error("not connected to Unreal");
                json errors = json::array();
                link::RpcResult t = client->call("importTextures", importParams, link::importCallOptions());
                if (!t.ok)
                    errors.push_back("textures: " + t.error);
                link::RpcResult m = client->call("trimMaterials", request, link::importCallOptions());
                if (!m.ok)
                    errors.push_back("materials: " + m.error);
                return {{"textures", t.ok ? t.result : json()}, {"materials", m.ok ? m.result : json()}, {"errors", errors}};
            };
            if (a.get<bool>("wait", true))
            {
                try
                {
                    return send();
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("trimSheetSend: ") + e.what());
                }
            }
            const std::string name = s.name;
            link::Link::get().post([send, name] {
                json done;
                try
                {
                    done = send();
                }
                catch (const std::exception& e)
                {
                    done = {{"errors", json::array({e.what()})}};
                }
                mainq::post([done, name] {
                    for (const json& e : done.value("errors", json::array()))
                        log::error("trimSheetSend {}: {}", name, e.get<std::string>());
                });
            });
            return {{"queued", true}};
        },
        .examples = {"trimSheetSend()"},
    });
}
} // namespace trims
