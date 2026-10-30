// Tailor commands. A garment is addressed by its tailorGarment node or the mesh it feeds; with no garment given, the
// selection's. Every edit goes through Graph::setValue, so undo comes from the graph.

#include "Blocks.h"
#include "GarmentCodeImport.h"
#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace tailor
{
namespace
{
using T = ArgType;

int plugIndex(const Node& n, const char* name)
{
    const int k = n.type().attrIndex(name);
    if (k < 0)
        throw CommandError(std::string("tailor: ") + n.name() + " has no attribute " + name);
    return k;
}

Node* garmentFeeding(Graph& g, const Node& n)
{
    if (n.type().isA(kGarmentType))
        return g.node(n.id());
    const int in = n.type().attrIndex("inMesh");
    if (in < 0)
        return nullptr;
    const std::optional<PlugRef> src = g.source({n.id(), in});
    Node* s = src ? g.node(src->node) : nullptr;
    return s && s->type().isA(kGarmentType) ? s : nullptr;
}

/** The garment named by the first positional argument, else the first selected one. */
Node& garmentArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.positional.empty() && a.positional[0].is_string())
    {
        const std::string path = a.positional[0].get<std::string>();
        Node* n = g.find(path);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node " + path);
        if (Node* gn = garmentFeeding(g, *n))
            return *gn;
        throw CommandError(std::string(cmd) + ": " + path + " is not a garment or a mesh fed by one");
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gn = garmentFeeding(g, *n))
                return *gn;
    throw CommandError(std::string(cmd) + ": name a garment or select one");
}

Garment readGarment(const Node& n)
{
    const std::string& text = n.as<std::string>(plugIndex(n, "garment"));
    return text.empty() ? Garment{} : garmentFromJson(json::parse(text));
}

void writeGarment(Graph& g, const Node& n, const Garment& gm) { g.setValue({n.id(), plugIndex(n, "garment")}, garmentToJson(gm).dump()); }

/** A panel by name or index. */
int panelIndex(const Garment& gm, const json& ref, const char* cmd)
{
    if (ref.is_number_integer())
    {
        const int k = ref.get<int>();
        if (k >= 0 && size_t(k) < gm.panels.size())
            return k;
    }
    else if (ref.is_string())
    {
        const std::string s = ref.get<std::string>();
        for (size_t k = 0; k < gm.panels.size(); ++k)
            if (gm.panels[k].name == s)
                return int(k);
        if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos)
            return panelIndex(gm, json(std::stoi(s)), cmd);
    }
    throw CommandError(std::string(cmd) + ": no panel " + ref.dump());
}

/** The panels a List flag names; all of them when it is missing or empty. */
std::vector<int> panelsArg(const Garment& gm, const Args& a, const char* flag, const char* cmd)
{
    std::vector<int> out;
    const json list = a.has(flag) ? a.flags[flag] : json::array();
    if (list.is_array() && list.empty())
    {
        for (size_t k = 0; k < gm.panels.size(); ++k)
            out.push_back(int(k));
        return out;
    }
    if (!list.is_array())
        return {panelIndex(gm, list, cmd)};
    for (const json& r : list)
        out.push_back(panelIndex(gm, r, cmd));
    return out;
}

/** "Front:0:4" (panel, first and last outline vertex), several joined by commas for a chain. */
std::vector<SeamSide> parseSides(const Garment& gm, const std::string& text, const char* cmd)
{
    std::vector<SeamSide> out;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = std::min(text.find(',', start), text.size());
        const std::string part = text.substr(start, end - start);
        const size_t c1 = part.find(':'), c2 = c1 == std::string::npos ? c1 : part.find(':', c1 + 1);
        if (c2 == std::string::npos)
            throw CommandError(std::string(cmd) + ": '" + part + "' is not panel:from:to");
        SeamSide s;
        s.panel = panelIndex(gm, json(part.substr(0, c1)), cmd);
        try
        {
            s.from = std::stoi(part.substr(c1 + 1, c2 - c1 - 1));
            s.to = std::stoi(part.substr(c2 + 1));
        }
        catch (const std::exception&)
        {
            throw CommandError(std::string(cmd) + ": '" + part + "' is not panel:from:to");
        }
        const int n = int(gm.panels[size_t(s.panel)].outline.size());
        if (s.from < 0 || s.to < 0 || s.from >= n || s.to >= n)
            throw CommandError(std::string(cmd) + ": " + part + ": the panel's outline has " + std::to_string(n) + " points");
        out.push_back(s);
        start = end + 1;
    }
    return out;
}

std::vector<std::string> presetChoices()
{
    std::vector<std::string> out;
    for (const Fabric& f : fabricPresets())
        out.push_back(f.name);
    return out;
}

const Fabric& fabricArg(const std::string& name, const char* cmd)
{
    const Fabric* f = findFabric(name);
    if (!f)
        throw CommandError(std::string(cmd) + ": no fabric preset " + name);
    return *f;
}

Node* colliderArg(Graph& g, const Args& a)
{
    if (a.has("collider"))
    {
        const std::string path = a.get<std::string>("collider");
        if (path.empty())
            return nullptr;
        Node* n = g.find(path);
        if (!n || !n->type().isA("mesh"))
            throw CommandError("tailor: collider " + path + " is not a mesh");
        return n;
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && !garmentFeeding(g, *n))
            return n;
    return nullptr;
}

/** A tailorGarment node holding gm, fed by the collider flag (or selection) and feeding a new mesh. Returns [mesh, garment]. */
json createGarment(const Garment& gm, const std::string& block, const Args& a, const char* cmd)
{
    const std::string fabric = a.get<std::string>("fabric");
    fabricArg(fabric, cmd);

    Graph& g = Graph::current();
    Node* collider = colliderArg(g, a);
    Node* op = g.createNode(*types::find(kGarmentType), {}, kNoNode, true);
    g.setValue({op->id(), plugIndex(*op, "garment")}, garmentToJson(gm).dump());
    g.setValue({op->id(), plugIndex(*op, "block")}, block);
    g.setValue({op->id(), plugIndex(*op, "fabric")}, fabric);
    g.setValue({op->id(), plugIndex(*op, "particleDistance")}, a.get<double>("particleDistance"));
    g.setValue({op->id(), plugIndex(*op, "simulate")}, a.get<bool>("simulate"));
    g.setValue({op->id(), plugIndex(*op, "frames")}, a.get<int64_t>("frames"));
    if (collider)
    {
        g.connect({collider->id(), MeshNode::kOutMesh}, {op->id(), plugIndex(*op, "colliderMesh")});
        g.connect({collider->id(), Transform::kWorldMatrix}, {op->id(), plugIndex(*op, "colliderMatrix")});
    }
    Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name"), kNoNode, true);
    g.connect({op->id(), plugIndex(*op, "outMesh")}, {mesh->id(), MeshNode::kInMesh});
    g.select({mesh->id()});
    return json::array({mesh->path(), op->path()});
}

json meshStats(Graph& g, const Node& op)
{
    g.evaluate();
    const PlugValue v = g.value({op.id(), plugIndex(op, "outMesh")});
    const MeshPtr& m = std::get<MeshPtr>(v);
    return {{"vertices", m ? int64_t(m->numPoints()) : 0},
            {"triangles", m ? int64_t(m->numFaces()) : 0},
            {"warning", op.as<std::string>(plugIndex(op, "warning"))}};
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "tailorCreateBlock",
        .doc = "Start a garment from a parametric block (tee, tunic, cloak, ...) and drape it onto a collider mesh.\n\n"
               "Makes a tailorGarment node feeding a new mesh. With no collider, the first selected mesh is used. Returns [mesh, garment].",
        .flags = {{"block", "b", T::String, "tee", "The block to start from.", blockNames()},
                  {"collider", "c", T::String, json(), "The body mesh to drape onto; empty for none."},
                  {"name", "n", T::String, "Garment", "Name of the new mesh."},
                  {"fabric", "fab", T::String, "Cotton", "Fabric preset.", presetChoices()},
                  {"particleDistance", "pd", T::Float, 2.0, "Target cloth edge length, cm.", {}, {}, 0.1, 50.0},
                  {"simulate", "sim", T::Bool, true, "Drape it; off leaves the panels where the block placed them."},
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames run to settle the drape.", {}, {}, 0.0, 10000.0}},
        .run = [](const Args& a) -> json {
            Garment gm;
            std::string error;
            // TODO: fit the block to the collider (size from its bounds, placement from arrangement points).
            if (!buildBlock(a.get<std::string>("block"), gm, &error))
                throw CommandError("tailorCreateBlock: " + error);
            return createGarment(gm, a.get<std::string>("block"), a, "tailorCreateBlock");
        },
        .examples = {"tailorCreateBlock(block='tunic', collider='Body')"},
    });

    ctx.registerCommand({
        .name = "tailorImportGarmentCode",
        .doc = "Import a GarmentCode / ChatGarment sewing pattern (pygarment JSON) as a garment and drape it onto a collider mesh.\n\n"
               "Makes a tailorGarment node feeding a new mesh. With no collider, the first selected mesh is used. Returns [mesh, garment].",
        .flags = {{"file", "f", T::String, json(), "The pattern specification JSON."},
                  {"collider", "c", T::String, json(), "The body mesh to drape onto; empty for none."},
                  {"name", "n", T::String, "Garment", "Name of the new mesh."},
                  {"fabric", "fab", T::String, "Cotton", "Fabric preset.", presetChoices()},
                  {"particleDistance", "pd", T::Float, 2.0, "Target cloth edge length, cm.", {}, {}, 0.1, 50.0},
                  {"simulate", "sim", T::Bool, true, "Drape it; off leaves the panels where the file placed them."},
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames run to settle the drape.", {}, {}, 0.0, 10000.0}},
        .run = [](const Args& a) -> json {
            if (!a.has("file"))
                throw CommandError("tailorImportGarmentCode: give a file");
            Garment gm;
            std::string error;
            if (!importGarmentCodeFile(a.get<std::string>("file"), gm, &error))
                throw CommandError("tailorImportGarmentCode: " + error);
            return createGarment(gm, std::string(), a, "tailorImportGarmentCode");
        },
        .examples = {"tailorImportGarmentCode(file='C:/patterns/tee_spec.json', collider='Body')"},
    });

    ctx.registerCommand({
        .name = "tailorSimulate",
        .doc = "Drape a garment: run the solver for a number of frames from its placed panels and keep the result.\n\n"
               "Returns the draped mesh's vertex and triangle counts and any warning.",
        .positionalDoc = "garment",
        .flags = {{"frames", "fr", T::Int, json(), "Solver frames to run; the garment's own setting when left out.", {}, {}, 0.0, 10000.0},
                  {"simulate", "sim", T::Bool, true, "Off shows the panels undraped."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSimulate");
            if (a.has("frames"))
                g.setValue({op.id(), plugIndex(op, "frames")}, a.get<int64_t>("frames"));
            g.setValue({op.id(), plugIndex(op, "simulate")}, a.get<bool>("simulate", true));
            return meshStats(g, op);
        },
        .examples = {"tailorSimulate('Garment', frames=200)"},
    });

    ctx.registerCommand({
        .name = "tailorSetFabric",
        .doc = "Set the fabric of a garment, or of some of its panels.\n\n"
               "Without panels it sets the garment's fabric preset; with them, those panels get the preset as their own fabric.",
        .positionalDoc = "garment",
        .flags = {{"fabric", "fab", T::String, "Cotton", "Fabric preset.", presetChoices()},
                  {"panel", "p", T::List, json(), "Panels by name or index; all panels' shared fabric when left out."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSetFabric");
            const std::string name = a.get<std::string>("fabric");
            const Fabric& preset = fabricArg(name, "tailorSetFabric");
            if (!a.has("panel"))
            {
                g.setValue({op.id(), plugIndex(op, "fabric")}, name);
                return name;
            }
            Garment gm = readGarment(op);
            int index = -1;
            for (size_t k = 0; k < gm.fabrics.size(); ++k)
                if (gm.fabrics[k].name == name)
                    index = int(k);
            if (index < 0)
            {
                index = int(gm.fabrics.size());
                gm.fabrics.push_back(preset);
            }
            json changed = json::array();
            for (int p : panelsArg(gm, a, "panel", "tailorSetFabric"))
            {
                gm.panels[size_t(p)].fabric = index;
                changed.push_back(gm.panels[size_t(p)].name);
            }
            writeGarment(g, op, gm);
            return changed;
        },
        .examples = {"tailorSetFabric('Garment', fabric='Leather', panel=['Belt'])"},
    });

    ctx.registerCommand({
        .name = "tailorSew",
        .doc = "Sew two runs of panel outline together, point to point by arc length.\n\n"
               "Each side is panel:from:to (outline vertex indices, walking forward), several joined by commas for a chain. "
               "Returns the new seam's index.",
        .positionalDoc = "garment",
        .flags = {{"sideA", "a", T::String, json(), "First side, e.g. \"Front:0:4\" or \"Front:0:4,Sleeve:2:5\"."},
                  {"sideB", "b", T::String, json(), "Second side, same form."},
                  {"reversed", "r", T::Bool, false, "Walk side B backwards."},
                  {"foldAngle", "fa", T::Float, 0.0, "Rest angle across the seam, degrees; 0 lies flat.", {}, {}, -180.0, 180.0},
                  {"shrink", "sh", T::Float, 1.0, "Rest length scale across the seam.", {}, {}, 0.0, 2.0}},
        .run = [](const Args& a) -> json {
            if (!a.has("sideA") || !a.has("sideB"))
                throw CommandError("tailorSew: give sideA and sideB");
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSew");
            Garment gm = readGarment(op);
            Seam s;
            s.a = parseSides(gm, a.get<std::string>("sideA"), "tailorSew");
            s.b = parseSides(gm, a.get<std::string>("sideB"), "tailorSew");
            s.reversed = a.get<bool>("reversed");
            s.foldAngle = float(a.get<double>("foldAngle") * std::numbers::pi / 180.0);
            s.shrink = float(a.get<double>("shrink"));
            gm.seams.push_back(std::move(s));
            writeGarment(g, op, gm);
            return int64_t(gm.seams.size() - 1);
        },
        .examples = {"tailorSew('Garment', sideA='Front:1:2', sideB='Back:3:4', reversed=True)"},
    });

    ctx.registerCommand({
        .name = "tailorFreeze",
        .doc = "Freeze panels so they collide without moving, or set them back to active, deactivated or strengthened.\n\n"
               "Returns the panels changed.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::List, json(), "Panels by name or index; all when left out."},
                  {"state", "s", T::String, "frozen", "What the panels become.", {"active", "frozen", "deactivated", "strengthened"}}},
        .run = [](const Args& a) -> json {
            PanelState state;
            if (!parsePanelState(a.get<std::string>("state"), &state))
                throw CommandError("tailorFreeze: unknown state " + a.get<std::string>("state"));
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorFreeze");
            Garment gm = readGarment(op);
            json changed = json::array();
            for (int p : panelsArg(gm, a, "panel", "tailorFreeze"))
            {
                gm.panels[size_t(p)].state = state;
                changed.push_back(gm.panels[size_t(p)].name);
            }
            writeGarment(g, op, gm);
            return changed;
        },
        .examples = {"tailorFreeze('Tunic')", "tailorFreeze('Cloak', panel=['Hood'], state='active')"},
    });
}
} // namespace tailor
