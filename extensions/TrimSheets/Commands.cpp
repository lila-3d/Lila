// trimSheet* make, inspect, edit, generate, bake, import, export and send sheets; trim* map faces onto a sheet's trims
// (one trim, automatic hotspotting, shifting between strips, edge chamfers).

#include "TrimSheets.h"
#include "Generator.h"
#include "Strips.h"
#include "TrimBake.h"

#include <lila/Bake.h>
#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/TextureGraph.h>
#include <lila/Transform.h>
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
using AT = ArgType;
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

FlagDef sheetFlag() { return {"sheet", "sh", AT::String, nullptr, "The .lilatrim file; the current sheet (trimSheetCurrent) when not given."}; }
FlagDef uvSetFlag() { return {"uvSet", "", AT::String, "uv0", "UV set to write."}; }

/** The sheet= flag, else the first positional (when the command takes the sheet there), else the current sheet. */
std::filesystem::path loadedPath(const Args& a, const char* cmd, std::string_view flag = "sheet", bool positional = true)
{
    std::string p;
    if (a.has(std::string(flag)))
        p = a.get<std::string>(std::string(flag));
    else if (positional && flag == "sheet" && !a.positional.empty() && a.positional[0].is_string())
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
            {"splitAngle", "sa", AT::Float, 30.0, "Faces meeting sharper than this, degrees, become separate islands; 0 keeps the seams.", {}, {}, 0.0, 180.0},
            {"orient", "o", AT::String, "worldUp", "worldUp (up runs up the trim), longestU or asIs.", {"worldUp", "longestU", "asIs"}},
            {"tag", "tg", AT::String, "", "Only trims with this tag (edge, radial, flat or your own); unique and flat trims need one."},
            {"seed", "s", AT::Int, int64_t(0), "Variation seed: offsets along strips, picks among equal trims."},
            {"cycle", "c", AT::Int, int64_t(0), "Take the n-th best trim instead of the best: run again with cycle+1 to try the next."},
            {"randomOffset", "ro", AT::Bool, true, "Slide islands along tiling strips so repeats don't line up."},
            {"randomFlip", "rf", AT::Bool, false, "Turn islands 180 degrees at random where the trim allows it."},
            {"randomMirror", "rm", AT::Bool, false, "Mirror islands at random."},
            {"allowRotate", "ar", AT::Bool, true, "Let islands turn 90 degrees to fit."},
            {"maxSizeError", "mse", AT::Float, 1.0, "Islands off a trim's world size by more than this many doublings count as poor matches.", {}, {}, 0.0, 8.0},
            {"aspectWeight", "aw", AT::Float, 1.0, "How much shape matters.", {}, {}, 0.0, 10.0},
            {"sizeWeight", "sw", AT::Float, 1.0, "How much world size matters.", {}, {}, 0.0, 10.0},
            {"normalWeight", "nw", AT::Float, 0.5, "How much facing matters, for trims with a preferred direction.", {}, {}, 0.0, 10.0}};
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

/** Layer at the bottom of a sheet's texture set carrying its baked normal and AO under the smart materials. */
constexpr const char* kBakeLayer = "Sheet Bake";
/** Used when neither the call nor the sheet names a smart material. */
constexpr const char* kDefaultSmartMaterial = "Worn Painted Metal";

/** The texture set painting a sheet's material: TS_<name without T_>. */
std::string textureSetName(const Sheet& s) { return "TS_" + materialName(s).substr(2); }

bool hasTextureSet(const std::string& set)
{
    const json sets = ops::execute("texSets", json::object());
    return std::ranges::find(sets, json(set)) != sets.end();
}

/** A bake project named after the sheet and pointed at its texture folder: texture sets read the sheet's bake through it. */
std::string ensureBakeProject(const Sheet& s, const std::filesystem::path& f, const char* cmd)
{
    const std::string folder = textureFolder(s, f).generic_string();
    if (Node* n = Graph::current().find(s.name))
    {
        if (n->type().name != "bakeProject")
            throw CommandError(std::string(cmd) + ": " + s.name + " is taken by a node that isn't a bake project");
        ops::execute("setBakeOutput", {{"project", s.name}, {"folder", folder}});
    }
    else
        ops::execute("createBakeProject", {{"name", s.name}, {"folder", folder}});
    return s.name;
}

json bakeLayerGraph()
{
    return {{"name", "sheetBake"},
            {"typeVersion", 1},
            {"inputs", json::array()},
            {"outputs", {{{"name", "normal"}, {"type", "color"}}, {{"name", "ao"}, {"type", "gray"}}}},
            {"nodes",
             {{{"name", "GroupInput"}, {"type", "GroupInput"}, {"position", {-400, 0}}},
              {{"name", "Normal"}, {"type", "trimSheetNormal"}, {"position", {-200, 0}}},
              {{"name", "AO"}, {"type", "meshAo"}, {"position", {-200, 150}}},
              {{"name", "GroupOutput"}, {"type", "GroupOutput"}, {"position", {200, 0}}}}},
            {"links", {{"Normal", "out", "GroupOutput", "normal"}, {"AO", "out", "GroupOutput", "ao"}}}};
}

/**
 * The sheet's texture set, bound to its material M_<name> and reading its bake, made with a Sheet Bake layer when missing.
 * Bakes the sheet first when it has no textures.
 */
std::string ensureTextureSet(const Sheet& s, const std::filesystem::path& f, const char* cmd)
{
    if (!std::filesystem::exists(bakeRecord(s, f)))
        runBake(s, f, {}, cmd);
    else
        ensureMaterial(s, f);
    const std::string project = ensureBakeProject(s, f, cmd);
    const std::string set = textureSetName(s);
    if (hasTextureSet(set))
        return set;
    ops::execute("texCreateSet",
                 {{"args", {set}}, {"settings", {{"width", s.width}, {"height", s.height}, {"bakeProject", project}, {"material", materialName(s)}}}});
    ops::execute("texAddLayer", {{"args", {set, "procedural"}}, {"name", kBakeLayer}, {"index", 0}, {"graph", bakeLayerGraph()}});
    return set;
}

/** A new mesh node holding m (replacing one of that name), its faces on the sheet's material. Returns its path. */
std::string addTrimMesh(const Sheet& s, const std::filesystem::path& f, const std::string& name, geo::Mesh m)
{
    std::vector<uint32_t> faces;
    for (geo::Index fi = 0; fi < m.faceCapacity(); ++fi)
        if (m.faceValid(fi))
            faces.push_back(fi);
    scene::materials::assign(m, materialName(s), faces);
    Graph& g = Graph::current();
    if (Node* old = g.find(name))
        ops::execute("delete", {{"args", {old->path()}}});
    const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
    Node* n = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    tagNode(n->path(), f);
    return n->path();
}

/** The trim= flag as one index, else the best strip for the width among trims tagged `tag` (or all strips). */
int stripTrimArg(const Sheet& s, const Args& a, double width, std::string_view tag, const char* cmd)
{
    if (a.has("trim"))
    {
        const json t = a.flags["trim"];
        if (t.is_string())
            if (const int tagged = bestStripTrim(s, width, t.get<std::string>()); tagged >= 0)
                return tagged;
        return trimIndex(s, t, cmd);
    }
    int i = tag.empty() ? -1 : bestStripTrim(s, width, tag);
    if (i < 0)
        i = bestStripTrim(s, width, "");
    if (i < 0)
        throw CommandError(std::string(cmd) + ": the sheet has no tiling strips");
    return i;
}

/** Runs of consecutive indices, as Trim Mask's first and last. */
std::vector<std::pair<size_t, size_t>> trimRuns(std::span<const size_t> sorted)
{
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i : sorted)
        if (!out.empty() && out.back().second + 1 == i)
            out.back().second = i;
        else
            out.push_back({i, i});
    return out;
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
        .flags = {{"preset", "p", AT::String, "industrialMetal", "Layout and profiles to start from.", presetIds()},
                  {"width", "w", AT::Int, int64_t(0), "Texture width, px (0: the preset's).", {}, {}, 0.0, 8192.0},
                  {"file", "f", AT::String, nullptr, "Where to save; default <workspace>/TrimSheets/<name>.lilatrim."},
                  {"name", "n", AT::String, nullptr, "Sheet name (texture prefix); default the preset's, T_<Preset>."},
                  {"pxPerM", "", AT::Float, nullptr, "Texels per metre: sets every trim's world size.", {}, {}, 1.0},
                  {"bake", "b", AT::Bool, true, "Bake when the textures are missing."},
                  {"overwrite", "", AT::Bool, false, "Replace an existing file with the preset."},
                  {"backend", "", AT::String, "auto", "Bake backend: auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
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
                  {"trim", "t", AT::Any, nullptr, "Trim index or name to change."},
                  {"values", "v", AT::Any, nullptr, "Fields to merge."},
                  {"add", "", AT::Any, nullptr, "A trim to append."},
                  {"remove", "", AT::Any, nullptr, "Trim index or name to delete."},
                  {"moveBorder", "", AT::Any, nullptr,
                   "Drag a border: {axis: 'x' or 'y', at: px of the line, along: px on it, to: px}. Moves it for the whole strip or row of "
                   "trims on the line, never thinner than a pixel."},
                  {"split", "", AT::Any, nullptr, "Split a trim in two: {trim, axis: 'x' (a vertical cut) or 'y', at: px}."}},
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
            json changed = json::array();
            if (a.has("moveBorder"))
            {
                const json m = a.flags["moveBorder"];
                if (!m.is_object() || !m.contains("at") || !m.contains("to"))
                    throw CommandError("trimSheetEdit: moveBorder needs at and to");
                const std::vector<size_t> moved = moveBorder(s, m.value("axis", std::string("y")) == "x" ? 0 : 1, m["at"].get<int>(),
                                                             m.value("along", 0), m["to"].get<int>());
                if (moved.empty())
                    throw CommandError("trimSheetEdit: no border at " + m["at"].dump());
                changed = moved;
            }
            if (a.has("split"))
            {
                const json sp = a.flags["split"];
                if (!sp.is_object() || !sp.contains("trim") || !sp.contains("at"))
                    throw CommandError("trimSheetEdit: split needs trim and at");
                const int i = trimIndex(s, sp["trim"], "trimSheetEdit");
                if (!splitTrim(s, size_t(i), sp.value("axis", std::string("y")) == "x" ? 0 : 1, sp["at"].get<int>()))
                    throw CommandError("trimSheetEdit: " + sp["at"].dump() + " isn't inside trim " + std::to_string(i));
                changed = {i, i + 1};
            }
            save(s, f);
            json out = sheetSummary(s, f);
            if (!changed.empty())
                out["changed"] = changed;
            return out;
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
        .flags = {sheetFlag(), {"tile", "", AT::Bool, false, "Repeat tiling strips one sheet width to each side, as the bake does."}},
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
                  {"trims", "t", AT::List, nullptr, "Trim indices or names to rebake; all when not given."},
                  {"backend", "", AT::String, "auto", "auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
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
        .flags = {{"file", "f", AT::String, nullptr, "Where to save the .lilatrim."},
                  {"width", "w", AT::Int, int64_t(2048), "Sheet width, px, for .rect and mesh sources.", {}, {}, 16.0, 16384.0},
                  {"height", "h", AT::Int, int64_t(2048), "Sheet height, px, for .rect and mesh sources.", {}, {}, 16.0, 16384.0},
                  {"name", "n", AT::String, nullptr, "Sheet name; default T_<source name>."},
                  {"pxPerM", "", AT::Float, 512.0, "Texels per metre.", {}, {}, 1.0}},
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
                  {"format", "fmt", AT::String, "rect", "rect, svg, idmap or json.", {"rect", "svg", "idmap", "json"}},
                  {"file", "f", AT::String, nullptr, "Output file; default beside the sheet."}},
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
                                      {"trim", "t", AT::Any, nullptr, "Trim index or name."},
                                      {"rotate", "r", AT::String, "auto", "auto (whichever fits), on or off.", {"auto", "on", "off"}},
                                      {"splitAngle", "sa", AT::Float, 30.0, "Split islands where faces meet sharper than this, degrees.", {}, {}, 0.0, 180.0},
                                      {"orient", "o", AT::String, "worldUp", "worldUp, longestU or asIs.", {"worldUp", "longestU", "asIs"}},
                                      {"seed", "s", AT::Int, int64_t(0), "Offset along strips."},
                                      {"randomOffset", "ro", AT::Bool, true, "Slide islands along tiling strips."}};
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
            f.push_back({"showError", "se", AT::Bool, false, "Paint match quality into the colour set trimError."});
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
        .flags = {sheetFlag(), uvSetFlag(), {"steps", "n", AT::Int, int64_t(1), "Trims to move; negative goes up the sheet.", {}, {}, -64.0, 64.0}},
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
                  {"width", "w", AT::Float, 1.0, "Chamfer width, cm.", {}, {}, 0.01},
                  {"tag", "tg", AT::String, "edge", "Trims to use for the chamfers."},
                  {"weightedNormals", "wn", AT::Bool, true, "Set face-weighted normals on the meshes afterwards."}},
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
        .name = "trimAlongCurve",
        .doc = "Lay a trim along curves: a strip of quads following each curve, width= cm across, on a tiling trim that repeats along U "
               "at the sheet's density (so it never stretches; loops repeat it a whole number of times), with head and tail caps from "
               "the sheet's cap-tagged trim. Flat strips lie on the ground (kerbs, floor edging, road lines), wall strips stand up "
               "(skirting, cornices, trim bands). Makes <curve>_Trim meshes with the sheet's material. Returns [{curve, mesh, trim, "
               "cap, length}].",
        .positionalDoc = "curves (default the selected ones)",
        .flags = {sheetFlag(),
                  {"trim", "t", AT::Any, nullptr, "Trim index, name or tag; the strip whose world height is closest to width= when not given."},
                  {"width", "w", AT::Float, nullptr, "Strip width, cm; the trim's world height when not given.", {}, {}, 0.01},
                  {"orient", "o", AT::String, "flat", "flat (lies on the ground) or wall (stands up).", {"flat", "wall"}},
                  {"offset", "of", AT::Float, 0.0, "Lift along the strip's normal, cm."},
                  {"caps", "cp", AT::Bool, true, "Cap the ends with the sheet's cap-tagged trim when it has one."},
                  {"seed", "s", AT::Int, int64_t(0), "Slides the pattern along tiling trims."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimAlongCurve", "sheet", false);
            const Sheet s = loadOr(f, "trimAlongCurve");
            Graph& g = Graph::current();
            std::vector<std::string> curves;
            for (const json& p : a.positional)
                if (p.is_string())
                    curves.push_back(p.get<std::string>());
            if (curves.empty())
                for (scene::NodeId id : g.selection())
                    if (Node* n = g.node(id))
                        curves.push_back(n->path());
            const StripOrient orient = a.get<std::string>("orient", "flat") == "wall" ? StripOrient::Wall : StripOrient::Flat;
            json out = json::array();
            for (const std::string& c : curves)
            {
                json data;
                try
                {
                    data = ops::execute("curveData", {{"args", {c}}, {"worldSpace", true}});
                }
                catch (const std::exception&)
                {
                    if (!a.positional.empty())
                        throw CommandError("trimAlongCurve: " + c + " is not a curve");
                    continue;
                }
                double width = a.get<double>("width", 0.0);
                const int trim = stripTrimArg(s, a, width > 0.0 ? width : 20.0, "", "trimAlongCurve");
                if (width <= 0.0)
                    width = s.worldSize(s.trims[size_t(trim)]).y;
                geo::Mesh all;
                double length = 0.0;
                for (const json& sp : data)
                {
                    curve::Polyline pl;
                    try
                    {
                        pl = curve::tessellate(curve::splineFromJson(sp));
                    }
                    catch (const std::exception& e)
                    {
                        throw CommandError("trimAlongCurve: " + c + ": " + e.what());
                    }
                    StripOptions o;
                    o.trim = size_t(trim);
                    o.closed = pl.closed;
                    o.cap = a.get<bool>("caps", true) ? capTrim(s) : -1;
                    o.uOffset = double(uint32_t(a.get<int64_t>("seed", 0)) * 2654435761u % 1000u) / 1000.0;
                    const geo::Mesh m = stripMesh(s, stationsAlong(pl.points, pl.closed, width, orient, a.get<double>("offset", 0.0)), o);
                    all.append(m);
                    length += pl.length;
                }
                if (all.numFaces() == 0)
                    continue;
                const std::string leaf = c.substr(c.find_last_of('|') + 1);
                out.push_back({{"curve", c},
                               {"mesh", addTrimMesh(s, f, leaf + "_Trim", std::move(all))},
                               {"trim", trim},
                               {"cap", a.get<bool>("caps", true) ? capTrim(s) : -1},
                               {"length", length}});
            }
            if (out.empty())
                throw CommandError("trimAlongCurve: select or name curves");
            return out;
        },
        .examples = {"trimAlongCurve('curve1', width=15)", "trimAlongCurve(orient='wall', trim='Plate128')"},
    });

    ctx.registerCommand({
        .name = "trimDecal",
        .doc = "Trim decals over the selected edges: a strip wrapping each run of edges, half its width on each face beside them, "
               "floating offset= cm off the surface and mapped onto an edge trim with its crest on the edge, as <mesh>_Decal with "
               "the sheet's material. cut=True carves it into the mesh instead (Trim Cut): a groove depth= cm deep whose floor and "
               "walls carry the trim. Returns [{mesh, decal or cut, chains, trim}].",
        .flags = {sheetFlag(),
                  {"trim", "t", AT::Any, nullptr, "Trim index, name or tag; the edge-tagged strip closest to width= when not given."},
                  {"width", "w", AT::Float, 2.0, "Strip width across the edge, cm.", {}, {}, 0.01},
                  {"offset", "of", AT::Float, 0.05, "Float off the surface, cm, so the decal doesn't fight it.", {}, {}, 0.0},
                  {"cut", "c", AT::Bool, false, "Carve the strip into the mesh as a groove instead of floating it."},
                  {"depth", "d", AT::Float, 0.5, "Groove depth for cut, cm.", {}, {}, 0.001}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimDecal");
            const Sheet s = loadOr(f, "trimDecal");
            const double width = a.get<double>("width", 2.0);
            const int trim = stripTrimArg(s, a, width, "edge", "trimDecal");
            const bool cut = a.get<bool>("cut", false);
            Graph& g = Graph::current();
            json out = json::array();
            for (const auto& [id, edges] : scene::components::selected(scene::ComponentMode::Edge))
            {
                Node* n = g.node(id);
                if (!n || edges.empty())
                    continue;
                const scene::MeshPtr mesh = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1}));
                if (!mesh)
                    continue;
                const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
                const glm::dmat3 normals = glm::transpose(glm::inverse(glm::dmat3(world)));
                const std::vector<geo::Index> ids(edges.begin(), edges.end());
                const std::vector<EdgeChain> chains = edgeChains(*mesh, ids);
                geo::Mesh all;
                for (const EdgeChain& chain : chains)
                {
                    std::vector<Station> st = stationsOverEdges(*mesh, chain, width, cut ? 0.0 : a.get<double>("offset", 0.05));
                    for (Station& x : st)
                    {
                        for (glm::dvec3& p : x.across)
                            p = glm::dvec3(world * glm::dvec4(p, 1.0));
                        x.normal = glm::normalize(normals * x.normal);
                    }
                    StripOptions o;
                    o.trim = size_t(trim);
                    o.closed = chain.closed;
                    o.cutDepth = cut ? a.get<double>("depth", 0.5) : 0.0;
                    all.append(stripMesh(s, st, o));
                }
                if (all.numFaces() == 0)
                    continue;
                const std::string leaf = n->name();
                const std::string made = addTrimMesh(s, f, leaf + (cut ? "_TrimCut" : "_Decal"), std::move(all));
                json r = {{"mesh", n->path()}, {"chains", chains.size()}, {"trim", trim}};
                if (cut)
                {
                    ops::execute("polyBoolean", {{"args", {n->path(), made}}, {"operation", "difference"}});
                    r["cut"] = true;
                }
                else
                    r["decal"] = made;
                out.push_back(r);
            }
            if (out.empty())
                throw CommandError("trimDecal: select edges");
            return out;
        },
        .examples = {"trimDecal(width=3)", "trimDecal(cut=True, depth=0.4)"},
    });

    ctx.registerCommand({
        .name = "trimSheetTexture",
        .doc = "Texture a trim sheet with a smart material. The sheet's bake becomes the mesh maps of its texture set TS_<name>, so "
               "edge wear, cavity dirt and chips land on the bevels, and the set paints M_<name> live on every mesh using the sheet. "
               "The first call makes the set with a Sheet Bake layer (the baked normal and AO) and bakes the sheet if it has no "
               "textures; each call adds the smart material on top, kept to trims= (indices, names or tags) by Trim Mask masks when "
               "given. Returns {set, project, layer, material, trims, maps}.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"material", "m", AT::String, nullptr, "Smart material name or .lilasmart; the sheet's own when not given."},
                  {"trims", "t", AT::Any, nullptr, "Only on these trims: an index, name or tag, or a list of them."},
                  {"name", "n", AT::String, nullptr, "Layer name; the smart material's when not given."},
                  {"replace", "r", AT::Bool, false, "First remove every layer but Sheet Bake, so the sheet starts over."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::filesystem::path f = loadedPath(a, "trimSheetTexture");
            Sheet s = loadOr(f, "trimSheetTexture");
            std::vector<size_t> only;
            if (a.has("trims"))
                try
                {
                    only = trimsMatching(s, a.flags["trims"]);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("trimSheetTexture: ") + e.what());
                }
            const std::string set = ensureTextureSet(s, f, "trimSheetTexture");
            if (a.get<bool>("replace", false))
                for (const json& l : ops::execute("texLayerStack", {{"args", {set}}}).value("layers", json::array()))
                    if (l.value("name", std::string()) != kBakeLayer)
                        ops::execute("texRemoveLayer", {{"args", {set, l.value("id", std::string())}}});

            const std::string material = a.get<std::string>("material", s.material.empty() ? std::string(kDefaultSmartMaterial) : s.material);
            json apply = {{"args", {set, material}}, {"addMaps", false}};
            if (a.has("name"))
                apply["name"] = a.get<std::string>("name");
            const json applied = ops::execute("texApplySmartMaterial", apply);
            const std::string layer = applied.value("layer", std::string());
            // Smart materials can read maps the sheet doesn't bake yet (dust reads the object normal): bake them too.
            bake::registerBuiltinMaps();
            bool more = false;
            for (const json& m : applied.value("maps", json::array()))
                if (const std::string id = tex::bakeMapName(m.get<std::string>()); bake::findMap(id) && std::ranges::find(s.maps, id) == s.maps.end())
                {
                    s.maps.push_back(id);
                    more = true;
                }
            if (more)
            {
                save(s, f);
                runBake(s, f, {}, "trimSheetTexture");
            }
            const auto runs = trimRuns(only);
            for (size_t k = 0; k < runs.size(); ++k)
                ops::execute("texAddMask", {{"args", {set, layer, "generator"}},
                                            {"type", "trimMask"},
                                            {"name", std::format("Trims {}-{}", runs[k].first, runs[k].second)},
                                            {"values", {{"first", runs[k].first}, {"last", runs[k].second}}},
                                            {"mode", k == 0 ? "normal" : "lighten"}});
            if (s.material != material && only.empty())
            {
                s.material = material;
                save(s, f);
            }
            return {{"set", set},
                    {"project", s.name},
                    {"layer", layer},
                    {"material", material},
                    {"trims", only},
                    {"maps", applied.value("maps", json::array())}};
        },
        .examples = {"trimSheetTexture()", "trimSheetTexture(material='Rusty Iron', trims=['edge'])"},
    });

    ctx.registerCommand({
        .name = "trimSheetPaint",
        .doc = "Get a trim sheet ready to paint by hand: its texture set (as trimSheetTexture makes it) and a flat <name>_Sheet plane "
               "with the sheet's material, selected, for the texture paint tool or the UV editor. Strokes land on the sheet's shared "
               "texels, so every mesh using the sheet shows them. Returns {set, plane}.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag()},
        .run = [](const Args& a) -> json {
            const std::filesystem::path f = loadedPath(a, "trimSheetPaint");
            const Sheet s = loadOr(f, "trimSheetPaint");
            const std::string set = ensureTextureSet(s, f, "trimSheetPaint");
            Graph& g = Graph::current();
            const std::string name = s.name + "_Sheet";
            Node* n = g.find(name);
            if (!n)
            {
                geo::Mesh plane = lowPlane(s);
                std::vector<geo::Index> faces;
                for (geo::Index fi = 0; fi < plane.faceCapacity(); ++fi)
                    if (plane.faceValid(fi))
                        faces.push_back(fi);
                tagFaces(plane, faces, -1, materialName(s));
                const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
                n = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
                g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(plane))));
                tagNode(n->path(), f);
            }
            ops::execute("select", {{"args", {n->path()}}});
            return {{"set", set}, {"plane", n->path()}};
        },
    });

    ctx.registerCommand({
        .name = "trimSheetSend",
        .doc = "Send a trim sheet to Unreal: its baked textures go to <folder>/Textures, the trim master material M_LilaTrim is "
               "generated (or rebuilt with regenerateMaster) and an instance MI_<name> is made with the sheet's maps, and every "
               "mapped mesh in the scene that uses this sheet gets the instance on its sheet slot. Returns what Unreal made; "
               "wait=False queues it.",
        .positionalDoc = "sheet (default the current one)",
        .flags = {sheetFlag(),
                  {"folder", "f", AT::String, "/Game/Lila/Trims", "Content folder; the sheet's assets go in <folder>/<name>."},
                  {"regenerateMaster", "", AT::Bool, false, "Rebuild M_LilaTrim's graph from this version of Lila, discarding edits to it."},
                  {"wait", "w", AT::Bool, true, "Block until Unreal has imported everything."}},
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
            json sent = json::array();
            auto add = [&](const json& o, const std::string& param) {
                const std::string asset = link::textureAssetName(o.value("file", std::string()), o.value("udim", 1001));
                textures[param] = folder + "/Textures/" + asset + "." + asset;
                sent.push_back(o);
            };
            for (const json& o : baked.value("outputs", json::array()))
                if (const auto param = params.find(o.value("map", std::string())); param != params.end())
                    add(o, param->second);
            // A textured sheet sends its painted maps too; its normal carries the layers' height over the bake.
            bool painted = false;
            if (const std::string set = textureSetName(s); hasTextureSet(set))
            {
                const json exported = ops::execute("texExport", {{"args", {set}},
                                                                 {"preset", "unreal"},
                                                                 {"folder", (textureFolder(s, f) / "Painted").generic_string()},
                                                                 {"incremental", true}});
                const std::map<std::string, std::string> paintParams = {
                    {"baseColor", "SheetBaseColorMap"}, {"orm", "SheetORMMap"}, {"normal", "SheetNormalMap"}};
                int found = 0;
                for (const json& o : exported.value("outputs", json::array()))
                    if (const auto param = paintParams.find(o.value("map", std::string())); param != paintParams.end())
                    {
                        if (param->first != "normal")
                            ++found;
                        sent.erase(std::remove_if(sent.begin(), sent.end(), [&](const json& b) { return b.value("map", std::string()) == param->first; }),
                                   sent.end());
                        add(o, param->second);
                    }
                painted = found == 2;
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
                                                        {"scalars", {{"SheetWidthCm", ext.x}, {"SheetHeightCm", ext.y}}},
                                                        {"switches", {{"UsePaintedMaps", painted}}}}})}};
            const json importParams = link::textureImportParams({{"outputs", sent}}, {{"folder", folder + "/Textures"}, {"materialMode", "none"}});

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
