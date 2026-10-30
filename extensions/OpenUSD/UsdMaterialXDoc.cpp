// Standalone MaterialX documents: importMaterialX reads a .mtlx through usdMtlx (so it maps exactly as a .mtlx
// referenced from a stage does) and exportMaterialX writes Lila materials with the MaterialX library.

#include "UsdMaterials.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdShade/material.h>

#include <algorithm>
#include <format>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace mx = MaterialX;

namespace lila::usd
{
namespace
{
using scene::Graph;
using scene::NodeId;

/** Attributes of the material nodes Lila writes: its slot name and the settings MaterialX has no input for. */
constexpr const char* kSlotAttr = "lila_slot";
constexpr const char* kSettingsAttr = "lila_attrs";

/** MaterialX's standard libraries, as usdMtlx ships them in the package; null when they can't be found. */
mx::ConstDocumentPtr standardLibraries()
{
    static mx::DocumentPtr libs = [] {
        mx::DocumentPtr doc;
        const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginWithName("usdMtlx");
        if (!plugin)
            return doc;
        try
        {
            mx::DocumentPtr d = mx::createDocument();
            const mx::StringSet loaded = mx::loadLibraries({"libraries"}, mx::FileSearchPath(plugin->GetResourcePath()), d);
            if (!loaded.empty())
                doc = d;
        }
        catch (const std::exception& e)
        {
            log::warn("MaterialX: can't load the standard libraries: {}", e.what());
        }
        return doc;
    }();
    return libs;
}

/** Problems MaterialX finds in a document checked against the standard libraries, one per line. */
std::vector<std::string> validate(const mx::DocumentPtr& doc)
{
    std::vector<std::string> out;
    const mx::ConstDocumentPtr libs = standardLibraries();
    if (!libs)
        return out;
    mx::DocumentPtr check = doc->copy();
    check->importLibrary(libs);
    std::string message;
    if (check->validate(&message))
        return out;
    size_t start = 0;
    while (start < message.size())
    {
        size_t end = message.find('\n', start);
        if (end == std::string::npos)
            end = message.size();
        if (end > start)
            out.push_back("MaterialX: " + message.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string valueString(const mtlx::Input& in)
{
    if (in.type == "string" || in.type == "filename")
        return in.text;
    if (in.type == "boolean")
        return !in.numbers.empty() && in.numbers[0] != 0.0f ? "true" : "false";
    if (in.type == "integer")
        return std::to_string(in.numbers.empty() ? 0 : int(std::lround(in.numbers[0])));
    std::string out;
    for (size_t k = 0; k < in.numbers.size(); ++k)
        out += std::format("{}{}", k ? ", " : "", in.numbers[k]);
    return out;
}

/**
 * One material as MaterialX conventions lay it out: the surface shader and material at document scope, everything
 * feeding the shader in a nodegraph NG_<material> whose outputs connect to it.
 */
void addMaterial(const mx::DocumentPtr& doc, const mtlx::Network& net, const std::string& slot, const std::string& settings)
{
    const std::string name = doc->createValidChildName(slot);
    const auto surface = std::find_if(net.nodes.begin(), net.nodes.end(), [&](const mtlx::Node& n) { return n.name == net.surface; });
    mx::NodePtr shader = doc->addNode(surface->category, doc->createValidChildName("SR_" + name), surface->type);
    shader->setNodeDefString(surface->nodedef);
    mx::NodeGraphPtr graph;
    std::map<std::string, mx::NodePtr> made;
    for (const mtlx::Node& n : net.nodes)
    {
        if (n.name == net.surface)
            continue;
        if (!graph)
            graph = doc->addNodeGraph(doc->createValidChildName("NG_" + name));
        mx::NodePtr node = graph->addNode(n.category, n.name, n.type);
        node->setNodeDefString(n.nodedef);
        made[n.name] = node;
    }
    auto fill = [&](const mx::InputPtr& input, const mtlx::Input& in) {
        if (!in.colorSpace.empty())
            input->setColorSpace(in.colorSpace);
        if (in.node.empty())
            input->setValueString(valueString(in));
    };
    for (const mtlx::Node& n : net.nodes)
    {
        if (n.name == net.surface)
            continue;
        for (const mtlx::Input& in : n.inputs)
        {
            mx::InputPtr input = made[n.name]->addInput(in.name, in.type);
            if (!in.node.empty())
            {
                input->setNodeName(in.node);
                if (!in.output.empty())
                    input->setOutputString(in.output);
            }
            fill(input, in);
        }
    }
    for (const mtlx::Input& in : surface->inputs)
    {
        mx::InputPtr input = shader->addInput(in.name, in.type);
        if (!in.node.empty())
        {
            const std::string outName = graph->createValidChildName("out_" + in.name);
            mx::OutputPtr out = graph->addOutput(outName, in.type);
            out->setNodeName(in.node);
            if (!in.output.empty())
                out->setOutputString(in.output);
            input->setNodeGraphString(graph->getName());
            input->setOutputString(outName);
        }
        fill(input, in);
    }
    mx::NodePtr material = doc->addMaterialNode(name, shader);
    material->setAttribute(kSlotAttr, slot);
    if (!settings.empty())
        material->setAttribute(kSettingsAttr, settings);
}

/** The material slots the named nodes stand for: materials themselves, or the materials meshes at and below objects use. */
std::vector<std::string> slotsOf(Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& slot) {
        if (scene::materials::find(g, slot) && seen.insert(slot).second)
            out.push_back(slot);
    };
    std::vector<NodeId> stack(ids.rbegin(), ids.rend());
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (n->type().isA("material"))
        {
            add(n->name());
            continue;
        }
        if (n->type().isA("mesh"))
            if (const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1})))
                for (const auto& [slot, faces] : scene::materials::slotUsage(*m))
                    add(slot);
        const std::vector<NodeId> kids = g.children(id);
        stack.insert(stack.end(), kids.rbegin(), kids.rend());
    }
    return out;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
} // namespace

json importMaterialX(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("importMaterialX: path required");
    const std::string path = a.positional[0].get<std::string>();
    const fs::path file = utf8Path(path);
    std::error_code ec;
    if (!fs::exists(file, ec))
        throw CommandError("importMaterialX: no file " + path);
    Graph& g = Graph::current();
    MaterialReader reader(g, file);
    std::vector<std::string> warnings;
    // usdMtlx drops attributes it doesn't know, so Lila's own settings are read from the document directly.
    try
    {
        mx::DocumentPtr doc = mx::createDocument();
        mx::readFromXmlFile(doc, mx::FilePath(utf8(file)));
        for (const mx::NodePtr& m : doc->getMaterialNodes())
            if (m->hasAttribute(kSettingsAttr))
                reader.external[m->getName()] = {m->getAttribute(kSlotAttr), m->getAttribute(kSettingsAttr)};
        for (std::string& w : validate(doc))
            warnings.push_back(std::move(w));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::format("importMaterialX: can't read {}: {}", path, e.what()));
    }

    Diagnostics diag;
    const UsdStageRefPtr stage = UsdStage::Open(utf8(file));
    if (!stage)
    {
        std::string why;
        for (const std::string& e : diag.errors())
            why += (why.empty() ? "" : "; ") + e;
        throw CommandError(std::format("importMaterialX: OpenUSD can't read {} as MaterialX{}{}", path, why.empty() ? "" : ": ", why));
    }
    std::vector<std::string> materials, kept;
    {
        Graph::Batch batch(g);
        for (const UsdPrim& p : stage->Traverse())
            if (p.IsA<UsdShadeMaterial>())
            {
                const std::string slot = reader.slot(UsdShadeMaterial(p));
                if (slot.empty())
                    continue;
                materials.push_back(slot);
                if (std::find(reader.created.begin(), reader.created.end(), slot) == reader.created.end())
                    kept.push_back(slot);
            }
    }
    for (const std::string& slot : kept)
        warnings.push_back(std::format("material {} is already in the scene; left as it is", slot));
    if (materials.empty())
        warnings.push_back(std::format("{} has no materials", path));
    warnings.insert(warnings.end(), reader.warnings.begin(), reader.warnings.end());
    for (const std::string& w : diag.warnings())
        warnings.push_back("OpenUSD: " + w);
    for (const std::string& s : reader.skipped)
        log::info("importMaterialX: skipped {}", s);
    for (const std::string& w : warnings)
        log::warn("importMaterialX: {}", w);
    if (!a.get<bool>("report", false))
        return reader.created;
    return {{"materials", materials}, {"created", reader.created}, {"skipped", reader.skipped}, {"warnings", warnings}};
}

json exportMaterialX(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("exportMaterialX: path required");
    const std::string path = a.positional[0].get<std::string>();
    const fs::path file = utf8Path(path);
    if (lower(utf8(file.extension())) != ".mtlx")
        throw CommandError("exportMaterialX: the file must end in .mtlx");
    mtlx::Surface surface;
    const std::string surfaceArg = a.get<std::string>("surface", "openPBR");
    if (!mtlx::parseSurface(surfaceArg, &surface) || surface == mtlx::Surface::None)
        throw CommandError("exportMaterialX: surface is openPBR or standardSurface");
    TextureMode mode = TextureMode::Relative;
    const std::string textures = a.get<std::string>("textures", "relative");
    if (textures == "absolute")
        mode = TextureMode::Absolute;
    else if (textures == "copy")
        mode = TextureMode::Copy;
    else if (textures != "relative")
        throw CommandError("exportMaterialX: textures is relative, absolute or copy");

    Graph& g = Graph::current();
    std::vector<NodeId> ids;
    for (size_t k = 1; k < a.positional.size(); ++k)
    {
        std::vector<std::string> names;
        if (a.positional[k].is_string())
            names.push_back(a.positional[k].get<std::string>());
        else if (a.positional[k].is_array())
            for (const json& v : a.positional[k])
                if (v.is_string())
                    names.push_back(v.get<std::string>());
        for (const std::string& name : names)
        {
            scene::Node* n = g.find(name);
            if (!n)
                throw CommandError("exportMaterialX: no node named '" + name + "'");
            ids.push_back(n->id());
        }
    }
    const bool named = !ids.empty();
    if (!named && a.get<bool>("selectionOnly", true))
        ids = g.selection();
    std::vector<std::string> slots = slotsOf(g, ids);
    if (slots.empty() && !named)
        for (scene::Node* m : scene::materials::all(g))
            slots.push_back(m->name());
    if (slots.empty())
        throw CommandError("exportMaterialX: no materials to export");

    MaterialWriter writer(g, UsdStagePtr(), SdfPath(), file, mode);
    mx::DocumentPtr doc = mx::createDocument();
    for (const std::string& slot : slots)
    {
        const scene::Node* n = scene::materials::find(g, slot);
        addMaterial(doc, writer.network(slot, surface, false), slot, n ? materialSettings(*n) : std::string());
    }
    std::vector<std::string> warnings = writer.warnings;
    for (std::string& w : validate(doc))
        warnings.push_back(std::move(w));
    try
    {
        if (!file.parent_path().empty())
            fs::create_directories(file.parent_path());
        mx::writeToXmlFile(doc, mx::FilePath(utf8(file)));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::format("exportMaterialX: can't write {}: {}", path, e.what()));
    }
    for (const std::string& w : warnings)
        log::warn("exportMaterialX: {}", w);
    if (a.get<bool>("report", false))
        return {{"path", path}, {"materials", slots}, {"warnings", warnings}};
    return path;
}
} // namespace lila::usd
