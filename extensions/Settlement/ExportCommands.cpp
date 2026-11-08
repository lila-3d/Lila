// settlementExport: a settlement's layout as files: Lila JSON, GeoJSON, USD, a DXF plan, or a Unity or Godot folder (engine
// JSON, FBX meshes and the importer script).

#include "Export.h"
#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Dxf.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/Graph.h>
#include <lila/Profile.h>

#include <filesystem>
#include <fstream>

using namespace lila;

namespace settlement
{
namespace
{
namespace fs = std::filesystem;

fs::path gEngineScripts;

void writeText(const fs::path& path, std::string_view text, const char* cmd)
{
    std::error_code ec;
    if (const fs::path dir = path.parent_path(); !dir.empty())
        fs::create_directories(dir, ec);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw CommandError(std::string(cmd) + ": cannot write " + path.generic_string());
    f << text;
}

void writeMeshFbx(const geo::Mesh& mesh, const std::string& name, const fs::path& file, const char* cmd)
{
    fbx::ExportScene scene;
    fbx::ExportNode node;
    node.name = name;
    node.mesh = std::make_shared<const geo::Mesh>(mesh);
    scene.nodes.push_back(std::move(node));
    fbx::ExportOptions o;
    o.profile = fbx::AxisProfile::Generic;
    std::string err;
    if (!fbx::writeFbx(scene, file, o, &err))
        throw CommandError(std::string(cmd) + ": writing " + file.generic_string() + ": " + err);
}

std::string formatOf(const Args& a, const fs::path& path)
{
    std::string f = a.get<std::string>("format", "");
    if (!f.empty())
        return f;
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".geojson")
        return "geojson";
    if (ext == ".usda" || ext == ".usd" || ext == ".usdc" || ext == ".usdz")
        return "usd";
    if (ext == ".dxf")
        return "dxf";
    return "json";
}

std::string baseName(const Rebuilt& r)
{
    std::string label = r.mesh.empty() ? r.modifier : r.mesh;
    if (const size_t slash = label.find_last_of("|/"); slash != std::string::npos)
        label = label.substr(slash + 1);
    std::string out;
    for (char c : label)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out.empty() ? std::string("Settlement") : out;
}

/** Engine folder: the engine JSON, streets and massing as FBX, and the importer script. */
json exportEngine(const Rebuilt& r, const RoadNetwork& net, const fs::path& folder, EngineAxes axes, bool meshes, const char* cmd)
{
    LILA_ZONE();

    const std::string name = baseName(r);
    std::error_code ec;
    fs::create_directories(folder, ec);
    json data = toEngineJson(r.layout, r.ground, net, name, axes);
    json files = json::object();
    json warnings = json::array();
    json meshFiles = json::object();
    if (meshes)
    {
        const MeshParts parts = toMeshes(r.layout, r.ground);
        if (parts.streets.numFaces() > 0)
        {
            const fs::path f = folder / (name + "_Streets.fbx");
            writeMeshFbx(parts.streets, name + "_Streets", f, cmd);
            meshFiles["streets"] = f.filename().string();
        }
        if (!r.layout.envelopes.empty())
        {
            const fs::path f = folder / (name + "_Massing.fbx");
            writeMeshFbx(massing(r.layout), name + "_Massing", f, cmd);
            meshFiles["massing"] = f.filename().string();
        }
    }
    data["meshes"] = {{"streets", meshFiles.value("streets", std::string())}, {"massing", meshFiles.value("massing", std::string())}};
    const fs::path settings = folder / (name + ".json");
    writeText(settings, data.dump(1), cmd);
    files["data"] = settings.generic_string();
    for (const auto& [k, v] : meshFiles.items())
        files[k] = (folder / v.get<std::string>()).generic_string();

    const char* scriptName = axes == EngineAxes::Unity ? "LilaSettlementImporter.cs" : "lila_settlement_import.gd";
    const fs::path source = gEngineScripts.empty() ? fs::path() : gEngineScripts / scriptName;
    if (source.empty() || !fs::exists(source))
        warnings.push_back(std::string("the importer script wasn't copied: ") + scriptName + " is missing from the Settlement extension's resources");
    else if (axes == EngineAxes::Unity)
    {
        const fs::path script = folder / scriptName;
        fs::copy_file(source, script, fs::copy_options::overwrite_existing, ec);
        files["importer"] = script.generic_string();
    }
    else
    {
        // Each export gets its own copy naming its JSON, so the script runs from the Script Editor without arguments.
        std::ifstream in(source, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string token = "%SETTLEMENT_JSON%";
        if (const size_t at = text.find(token); at != std::string::npos)
            text.replace(at, token.size(), settings.filename().string());
        const fs::path script = folder / (name + "_settlement.gd");
        writeText(script, text, cmd);
        files["importer"] = script.generic_string();
    }
    return {{"target", axes == EngineAxes::Unity ? "unity" : "godot"},
            {"folder", folder.generic_string()},
            {"files", files},
            {"streets", data["streets"].size()},
            {"lots", data["lots"].size()},
            {"envelopes", data["envelopes"].size()},
            {"placements", data["placements"].size()},
            {"warnings", warnings}};
}

json exportFile(const Rebuilt& r, const Args& a, const char* cmd)
{
    LILA_ZONE();

    const std::string pathText = a.get<std::string>("path", "");
    if (pathText.empty())
        throw CommandError(std::string(cmd) + ": path= required");
    const fs::path path = fs::path(pathText);
    const std::string format = formatOf(a, path);
    const RoadNetwork net = roadNetwork(r.layout, r.ground);
    if (format == "unity" || format == "godot")
        return exportEngine(r, net, path, format == "unity" ? EngineAxes::Unity : EngineAxes::Godot, a.get<bool>("meshes", true), cmd);

    json out = {{"path", path.generic_string()}, {"format", format}};
    if (format == "json")
    {
        const json data = toJson(r.layout);
        writeText(path, data.dump(1), cmd);
        out["streets"] = data["streets"].size();
        out["lots"] = data["lots"].size();
        out["envelopes"] = data["envelopes"].size();
    }
    else if (format == "geojson")
    {
        const json data = toGeoJson(r.layout, net, baseName(r));
        writeText(path, data.dump(1), cmd);
        json layers = json::object();
        for (const json& f : data["features"])
        {
            const std::string layer = f["properties"].value("layer", std::string());
            layers[layer] = layers.value(layer, 0) + 1;
        }
        out["features"] = data["features"].size();
        out["layers"] = layers;
    }
    else if (format == "usd")
    {
        std::string ext = path.extension().string();
        if (ext == ".usdc" || ext == ".usdz")
            throw CommandError(std::string(cmd) + ": USD goes out as text; give a .usda or .usd path");
        const std::string text = toUsda(r.layout, r.ground, net, baseName(r));
        writeText(path, text, cmd);
        size_t prims = 0;
        for (size_t at = text.find("def "); at != std::string::npos; at = text.find("def ", at + 4))
            ++prims;
        out["prims"] = prims;
    }
    else if (format == "dxf")
    {
        const std::string unit = a.get<std::string>("unit", "m");
        const double scale = curve::dxfUnitScale(unit);
        if (!(scale > 0.0))
            throw CommandError(std::string(cmd) + ": unit is mm, cm, m, in or ft");
        curve::DxfWriteOptions o;
        o.unit = unit;
        o.version = a.get<std::string>("dxfVersion", "2000") == "r12" ? curve::DxfVersion::R12 : curve::DxfVersion::R2000;
        auto layers = dxfLayers(r.layout, r.ground, net, scale);
        std::vector<curve::DxfObject> objects;
        json counts = json::object();
        for (auto& [name, curves] : layers)
        {
            counts[name] = curves.size();
            objects.push_back({.layer = name, .curves = std::move(curves)});
        }
        if (objects.empty())
            throw CommandError(std::string(cmd) + ": the settlement has nothing to draw");
        writeText(path, curve::writeDxf(objects, o), cmd);
        out["layers"] = counts;
    }
    else
        throw CommandError(std::string(cmd) + ": format is json, geojson, usd, dxf, unity or godot, not '" + format + "'");
    return out;
}
} // namespace

void addPlacements(Layout& l, const Args& a, const char* cmd)
{
    if (!a.has("placements"))
        return;
    json list;
    try
    {
        const json given = a.get<json>("placements");
        list = given.is_string() ? json::parse(given.get<std::string>()) : given;
        for (Placement& p : placementsFrom(list))
            l.placements.push_back(std::move(p));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": placements: " + e.what());
    }
}

void registerExportCommands(ext::Context& ctx)
{
    gEngineScripts = ctx.resources() / "engines";

    ctx.registerCommand({
        .name = "settlementExport",
        .doc = "Writes a settlement as files. format='json' (lila.settlement): street nodes and edges with class, width and era, "
               "blocks, lots, envelopes, 2D points [x, z] in cm, heights world Y; what building generators read. 'geojson': a "
               "FeatureCollection with a layer property per feature (street, junction, block, lot, envelope, lane, placement) and "
               "their fields, metres, x east and -z north. 'usd': USD text with Scopes Streets and Lanes (BasisCurves), "
               "Junctions (Xforms), Blocks, Lots and Envelopes (Meshes) and Placements (Xforms), fields as lila: attributes, "
               "Y up, cm. 'dxf': a plan with layers Streets, Paths, Water, Blocks, Lots, Buildings and Lanes, Z up, in unit. "
               "'unity' and 'godot': path is a folder that gets the engine JSON (engine space, metres), the streets and massing "
               "as FBX and the importer script (Unity: an Editor script, Tools > Lila > Import Settlement; Godot: an "
               "EditorScript run from the Script Editor). The format follows the path's extension when not given. Returns the "
               "files and counts.",
        .positionalDoc = "mesh or its settlement modifier (default the selection)",
        .flags = {{"path", "p", ArgType::String, "", "File to write; a folder for unity and godot."},
                  {"format", "fm", ArgType::String, "", "File format (default from the extension: .geojson, .usda, .usd, .dxf, else json).",
                   {"", "json", "geojson", "usd", "dxf", "unity", "godot"}},
                  {"unit", "u", ArgType::String, "m", "dxf: the drawing unit.", {"mm", "cm", "m", "in", "ft"}},
                  {"dxfVersion", "dv", ArgType::String, "2000", "dxf: 2000, or r12 for older CAD and CNC software.", {"2000", "r12"}},
                  {"meshes", "me", ArgType::Bool, true, "unity and godot: write the streets and massing as FBX next to the JSON."},
                  {"placements", "pl", ArgType::Any, nullptr,
                   "Extra placements, a list or its JSON text: [{id, asset, position: [x, y, z], rotation: [x, y, z, w], scale, tags}], cm."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementExport";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one mesh");
            Rebuilt r = rebuildSettlement(scene::Graph::current(), names[0], cmd);
            addPlacements(r.layout, a, cmd);
            return exportFile(r, a, cmd);
        },
        .examples = {"settlementExport('Settlement', path='C:/work/town.json')",
                     "settlementExport('Settlement', path='C:/work/town.geojson')",
                     "settlementExport('Settlement', path='C:/work/town.usda')",
                     "settlementExport('Settlement', path='C:/work/town_plan.dxf', unit='m')",
                     "settlementExport('Settlement', path='C:/work/UnityTown', format='unity')"},
    });
}
} // namespace settlement
