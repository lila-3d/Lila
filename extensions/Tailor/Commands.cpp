// Tailor commands. A garment is addressed by its tailorGarment node or the mesh it feeds; with no garment given, the
// selection's. Every edit goes through Graph::setValue, so undo comes from the graph.

#include "Blocks.h"
#include "GarmentCodeImport.h"
#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Diagnostics.h>
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

/** [[x, y], ...] in cm to pattern points in metres. */
std::vector<Vec2> pointsArg(const json& list, const char* cmd)
{
    std::vector<Vec2> out;
    if (list.is_array())
        for (const json& p : list)
            if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number())
                out.push_back({float(p[0].get<double>() / kCmPerM), float(p[1].get<double>() / kCmPerM)});
            else
                throw CommandError(std::string(cmd) + ": points are [x, y] pairs in cm");
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give points as [[x, y], ...] in cm");
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

/** The collider's body measurements and where its feet stand, in metres. */
Body measureCollider(Graph& g, Node& collider, Vec3& offset)
{
    g.evaluate();
    const PlugValue v = g.value({collider.id(), MeshNode::kOutMesh});
    const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
    if (!mesh || !*mesh || (*mesh)->empty())
        return {};
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    colliderTriangles(**mesh, std::get<glm::dmat4>(g.value({collider.id(), Transform::kWorldMatrix})), pts, tris);
    return measureBody(pts, tris, &offset);
}

/** Where panels meet on a garment mesh (edges between faces of different panels), as world polylines in cm. */
json seamGuides(const geo::Mesh& m)
{
    const std::span<const int32_t> panel = m.faceAttrs().get<int32_t>("panel");
    if (panel.empty())
        return json::array();
    std::vector<std::vector<geo::Index>> next(m.pointCapacity());
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        const geo::Index c = m.edgeCorner(e);
        const geo::Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (f0 >= panel.size() || f1 >= panel.size() || panel[f0] == panel[f1])
            continue;
        const geo::Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        next[a].push_back(b);
        next[b].push_back(a);
    }
    json lines = json::array();
    std::vector<uint8_t> used(m.pointCapacity(), 0);
    auto point = [&](geo::Index p) {
        const glm::dvec3 q = m.position(p);
        return json::array({q.x, q.y, q.z});
    };
    // Start at chain ends first so open seams come out whole, then sweep up closed loops.
    for (int pass = 0; pass < 2; ++pass)
        for (geo::Index s = 0; s < m.pointCapacity(); ++s)
        {
            if (used[s] || next[s].empty() || (pass == 0 && next[s].size() == 2))
                continue;
            json line = json::array({point(s)});
            used[s] = 1;
            for (geo::Index cur = s;;)
            {
                geo::Index step = geo::kInvalid;
                for (geo::Index nb : next[cur])
                    if (!used[nb])
                    {
                        step = nb;
                        break;
                    }
                if (step == geo::kInvalid)
                    break;
                used[step] = 1;
                line.push_back(point(step));
                cur = step;
            }
            if (line.size() >= 2)
                lines.push_back(std::move(line));
        }
    return lines;
}

/**
 * Point float attribute clothMaxDistance: how far each point sits from the collider's surface, cm. Near zero where the
 * garment rests on the body, large where it hangs free; Chaos Cloth's Max Distance mask reads it directly.
 */
void writeMaxDistance(Graph& g, const Node& collider, geo::Mesh& garment)
{
    const PlugValue v = g.value({collider.id(), MeshNode::kOutMesh});
    const MeshPtr* body = std::get_if<MeshPtr>(&v);
    if (!body || !*body || (*body)->empty())
        return;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    colliderTriangles(**body, std::get<glm::dmat4>(g.value({collider.id(), Transform::kWorldMatrix})), pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.02f, 0.4f);
    std::span<float> dist = garment.pointAttrs().getOrAdd<float>("clothMaxDistance");
    for (geo::Index p = 0; p < garment.pointCapacity(); ++p)
    {
        const glm::dvec3 q = garment.position(p) / kCmPerM;
        dist[p] = std::max(0.f, sdf.sample({float(q.x), float(q.y), float(q.z)})) * float(kCmPerM);
    }
}

/** Checks a garment against its collider's measurements and posts the findings as diagnostics on its mesh. */
json checkGarment(Graph& g, Node& op)
{
    const Garment gm = readGarment(op);
    std::optional<Body> body;
    if (const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")}))
        if (Node* collider = g.node(src->node))
        {
            Vec3 feet;
            body = measureCollider(g, *collider, feet);
        }
    const std::vector<PatternIssue> issues = checkPattern(gm, body ? &*body : nullptr);
    const std::vector<PlugRef> fed = g.destinations({op.id(), plugIndex(op, "outMesh")});
    const Node* mesh = fed.empty() ? &op : g.node(fed.front().node);
    diagnostics::clear("tailor", mesh->id());
    json out = json::array();
    for (const PatternIssue& i : issues)
    {
        diagnostics::Issue d;
        d.source = "tailor";
        d.severity = i.level == PatternIssue::Error ? diagnostics::Severity::Error
                     : i.level == PatternIssue::Warning ? diagnostics::Severity::Warning
                                                         : diagnostics::Severity::Info;
        d.target = mesh->path();
        d.message = i.message + ". " + i.tip;
        diagnostics::report(d);
        json j = {{"level", i.level == PatternIssue::Error ? "error" : i.level == PatternIssue::Warning ? "warning" : "info"},
                  {"message", i.message},
                  {"tip", i.tip}};
        if (i.panel >= 0)
            j["panel"] = gm.panels[size_t(i.panel)].name;
        if (i.seam >= 0)
            j["seam"] = i.seam;
        out.push_back(j);
    }
    return out;
}

/** A tailorGarment node holding gm, fed by the collider flag (or selection) and feeding a new mesh. Returns [mesh, garment]. */
json createGarment(const Garment& gm, const std::string& block, const Args& a, const char* cmd, const std::string& meshName = {})
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
    Node* mesh = g.createNode(*types::find("mesh"), meshName.empty() ? a.get<std::string>("name") : meshName, kNoNode, true);
    g.connect({op->id(), plugIndex(*op, "outMesh")}, {mesh->id(), MeshNode::kInMesh});
    g.select({mesh->id()});
    if (a.get<bool>("wait"))
    {
        DrapeNow now;
        g.touch(op->id());
        g.evaluate();
    }
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
    diagnostics::validators().add("tailor", {.doc = "Pattern checks on every garment (tailorCheck).", .run = [] {
        Graph& g = Graph::current();
        std::vector<NodeId> garments;
        for (NodeId id : g.nodes())
            if (const Node* n = g.node(id); n && n->type().isA(kGarmentType))
                garments.push_back(id);
        for (NodeId id : garments)
            if (Node* n = g.node(id))
                checkGarment(g, *n);
    }}, ctx.owner());

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
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames run to settle the drape.", {}, {}, 0.0, 10000.0},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."},
                  {"fit", "ft", T::Bool, true, "Size the block to the collider's measurements and stand it where the collider stands."},
                  {"params", "pr", T::Any, json::object(),
                   "Block parameters by name, metres and 0/1 for switches, e.g. {'sleeveStyle': 9, 'sleeveLength': 0.6}. "
                   "Sleeve styles: 0 set-in, 1 cuffed, 2 bishop, 3 bell, 4 puff, 5 cap, 6 flutter, 7 leg-of-mutton, 8 poet, "
                   "9 drop-shoulder, 10 batwing, 11 raglan, 12 marie, 13 flounce, 14 slit, 15 tiered, 16 petal, 17 cape."}},
        .run = [](const Args& a) -> json {
            Garment gm;
            std::string error;
            Body body;
            Vec3 offset{0.f, 0.f, 0.f};
            Graph& g = Graph::current();
            if (Node* collider = colliderArg(g, a); collider && a.get<bool>("fit"))
                body = measureCollider(g, *collider, offset);
            BlockParamValues values;
            if (const json pj = a.flags.value("params", json::object()); pj.is_object())
                for (const auto& [k, v] : pj.items())
                    if (v.is_number() || v.is_boolean())
                        values[k] = v.is_boolean() ? (v.get<bool>() ? 1.f : 0.f) : v.get<float>();
            if (!buildBlock(a.get<std::string>("block"), gm, &error, body, values))
                throw CommandError("tailorCreateBlock: " + error);
            for (Panel& p : gm.panels)
                for (int k = 0; k < 3; ++k)
                    p.origin[k] += offset[k];
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
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames run to settle the drape.", {}, {}, 0.0, 10000.0},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."}},
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
                  {"simulate", "sim", T::Bool, true, "Off shows the panels undraped."},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSimulate");
            if (a.has("frames"))
                g.setValue({op.id(), plugIndex(op, "frames")}, a.get<int64_t>("frames"));
            g.setValue({op.id(), plugIndex(op, "simulate")}, a.get<bool>("simulate", true));
            std::optional<DrapeNow> now;
            if (a.get<bool>("wait"))
            {
                now.emplace();
                g.touch(op.id());
            }
            return meshStats(g, op);
        },
        .examples = {"tailorSimulate('Garment', frames=200, wait=True)"},
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
        .name = "tailorMannequin",
        .doc = "Make a stand-in body with the measurements the blocks are cut for, to drape garments onto.\n\n"
               "Feet on the ground, facing +Z, in an A-pose. Blocks measure whatever they drape onto, so any size fits. Returns the new mesh.",
        .flags = {{"name", "n", T::String, "Mannequin", "Name of the new mesh."},
                  {"height", "ht", T::Float, 180.0, "Standing height, cm.", {}, {}, 100.0, 230.0},
                  {"chest", "ch", T::Float, 96.0, "Chest girth, cm.", {}, {}, 60.0, 160.0},
                  {"waist", "wa", T::Float, 82.0, "Waist girth, cm.", {}, {}, 50.0, 160.0},
                  {"hip", "hp", T::Float, 98.0, "Hip girth, cm.", {}, {}, 60.0, 170.0}},
        .run = [](const Args& a) -> json {
            Body body;
            const float s = float(a.get<double>("height")) / (body.height * float(kCmPerM));
            body.height *= s;
            body.shoulderWidth *= s;
            body.armLength *= s;
            body.inseam *= s;
            body.neck *= s;
            body.head *= s;
            body.chest = float(a.get<double>("chest") / kCmPerM);
            body.waist = float(a.get<double>("waist") / kCmPerM);
            body.hip = float(a.get<double>("hip") / kCmPerM);
            std::vector<Vec3> pts;
            std::vector<std::array<int, 3>> tris;
            mannequin(body, pts, tris);
            std::vector<glm::dvec3> positions(pts.size());
            for (size_t k = 0; k < pts.size(); ++k)
                positions[k] = glm::dvec3(pts[k][0], pts[k][1], pts[k][2]) * kCmPerM;
            std::vector<geo::Index> sizes(tris.size(), 3), idx;
            idx.reserve(tris.size() * 3);
            for (const auto& t : tris)
                for (int v : t)
                    idx.push_back(geo::Index(v));
            Graph& g = Graph::current();
            const std::string name = a.get<std::string>("name");
            Node* n = g.createNode(*types::find("mesh"), Graph::validName(name) ? name : "Mannequin", kNoNode, true);
            g.setValue({n->id(), MeshNode::kInMesh, -1},
                       MeshPtr(std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(positions, sizes, idx))));
            g.select({n->id()});
            return n->path();
        },
        .examples = {"tailorMannequin()", "tailorCreateBlock(block='cloakTunicBelt', collider=tailorMannequin())"},
    });

    ctx.registerCommand({
        .name = "tailorBake",
        .doc = "Turn a draped garment into a plain mesh for the game: the drape as it stands, seams welded and thickness "
               "applied as set on the garment, uv0 packed, uv1 the fabric pattern, and point attribute clothMaxDistance (cm from the body) as a runtime cloth mask.\n\n"
               "When the collider is skinned, the new mesh is bound to the same joints and takes its weights by closest "
               "point, ready to export with the character. Returns the new mesh.",
        .positionalDoc = "garment",
        .flags = {{"name", "n", T::String, json(), "Name of the new mesh; the garment mesh's name plus _baked when left out."},
                  {"skin", "sk", T::Bool, true, "Bind to the collider's joints and copy its weights when it is skinned."},
                  {"maxInfluences", "mi", T::Int, int64_t(4), "Influences per point.", {}, {}, 1.0, 16.0},
                  {"retopo", "rt", T::Int, int64_t(0), "Quad remesh to about this many faces, seams kept as edge loops; 0 keeps the cloth triangles.",
                   {}, {}, 0.0, 200000.0},
                  {"lods", "lod", T::Int, int64_t(0), "Extra levels of detail to generate in a LOD group.", {}, {}, 0.0, 7.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorBake");
            {
                DrapeNow now;
                g.evaluate();
            }
            const PlugValue v = g.value({op.id(), plugIndex(op, "outMesh")});
            const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
            if (!mesh || !*mesh || (*mesh)->empty())
                throw CommandError("tailorBake: " + op.name() + " has no mesh: " + op.as<std::string>(plugIndex(op, "warning")));

            std::string name = a.has("name") ? a.get<std::string>("name") : std::string();
            if (name.empty())
            {
                const std::vector<PlugRef> out = g.destinations({op.id(), plugIndex(op, "outMesh")});
                const Node* fed = out.empty() ? nullptr : g.node(out.front().node);
                name = (fed ? fed->name() : op.name()) + "_baked";
            }
            const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")});
            const Node* collider = src ? g.node(src->node) : nullptr;
            const json guides = seamGuides(**mesh);
            if (!Graph::validName(name))
                name = "Garment_baked";
            const bool remesh = a.get<int64_t>("retopo") > 0;
            Node* baked = g.createNode(*types::find("mesh"), remesh ? name + "_drape" : name, kNoNode, true);
            g.setValue({baked->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(**mesh)));
            if (const int64_t faces = a.get<int64_t>("retopo"); faces > 0)
            {
                const std::string drape = baked->path();
                const json r = ops::execute("quadRemesh", {{"args", {drape}},
                                                           {"targetFaces", faces},
                                                           {"guidePoints", guides},
                                                           {"timeout", 120.0},
                                                           {"keepSource", false},
                                                           {"name", name}});
                baked = g.find(r.value("name", drape));
                if (!baked)
                    throw CommandError("tailorBake: the remesh left no mesh");
            }
            if (collider)
            {
                g.evaluate();
                const PlugValue bv = g.value({baked->id(), MeshNode::kOutMesh});
                if (const MeshPtr* bm = std::get_if<MeshPtr>(&bv); bm && *bm)
                {
                    auto out = std::make_shared<geo::Mesh>(**bm);
                    writeMaxDistance(g, *collider, *out);
                    g.setValue({baked->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(out)));
                }
            }

            json result = {{"mesh", baked->path()}, {"skinned", false}};
            if (collider && a.get<bool>("skin"))
            {
                json info;
                try
                {
                    info = ops::execute("skinInfo", {{"args", {collider->path()}}});
                }
                catch (const std::exception&)
                {
                    info = json();
                }
                if (info.is_object() && info.contains("influences") && !info["influences"].empty())
                {
                    json bindArgs = info["influences"];
                    bindArgs.push_back(baked->path());
                    ops::execute("skinCluster", {{"args", bindArgs}, {"bindTo", "selected"},
                                                 {"maxInfluences", a.get<int64_t>("maxInfluences")}});
                    ops::execute("copySkinWeights", {{"args", {collider->path(), baked->path()}}});
                    result["skinned"] = true;
                }
            }
            if (const int64_t lods = a.get<int64_t>("lods"); lods > 0)
            {
                result["lodGroup"] = ops::execute("lodGroup", {{"args", {baked->path()}}, {"generate", lods}});
                result["mesh"] = baked->path();
            }
            g.select({baked->id()});
            return result;
        },
        .examples = {"tailorBake('Outfit')", "tailorBake('Cloak', name='SM_Cloak', skin=False)", "tailorBake('Tunic', retopo=3000, lods=2)"},
    });

    ctx.registerCommand({
        .name = "tailorEditPanel",
        .doc = "Change a panel: its outline points (same count), grain, particle distance, layer or name.\n\n"
               "Points are pattern space, cm, counter-clockwise. Seams keep their vertex indices. Returns the panel's name.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."},
                  {"outline", "o", T::List, json(), "Outline points [[x, y], ...], cm; the count must match."},
                  {"grainAngle", "ga", T::Float, json(), "Warp direction in pattern space, degrees."},
                  {"particleDistance", "pd", T::Float, json(), "This panel's cloth edge length, cm.", {}, {}, 0.1, 50.0},
                  {"layer", "l", T::Int, json(), "Layer: higher layers sit outside lower ones."},
                  {"rename", "rn", T::String, json(), "New name."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorEditPanel");
            Garment gm = readGarment(op);
            Panel& p = gm.panels[size_t(panelIndex(gm, a.flags.value("panel", json()), "tailorEditPanel"))];
            if (a.has("outline"))
            {
                const std::vector<Vec2> pts = pointsArg(a.flags["outline"], "tailorEditPanel");
                if (pts.size() != p.outline.size())
                    throw CommandError("tailorEditPanel: the outline has " + std::to_string(p.outline.size()) + " points, not " +
                                       std::to_string(pts.size()) + "; use tailorInsertPoint or tailorDeletePoint to change the count");
                p.outline = pts;
            }
            if (a.has("grainAngle"))
                p.grainAngle = float(a.get<double>("grainAngle") * std::numbers::pi / 180.0);
            if (a.has("particleDistance"))
                p.particleDistance = float(a.get<double>("particleDistance") / kCmPerM);
            if (a.has("layer"))
                p.layer = int(a.get<int64_t>("layer"));
            if (a.has("rename"))
            {
                const std::string name = a.get<std::string>("rename");
                for (const Panel& other : gm.panels)
                    if (&other != &p && other.name == name)
                        throw CommandError("tailorEditPanel: there already is a panel " + name);
                p.name = name;
            }
            const std::string name = p.name;
            writeGarment(g, op, gm);
            return name;
        },
        .examples = {"tailorEditPanel('Garment', panel='Front', grainAngle=90)"},
    });

    ctx.registerCommand({
        .name = "tailorInsertPoint",
        .doc = "Insert an outline point into a panel before point index, at a position in pattern space (cm). Seams keep "
               "sewing the same stretch of outline. Returns the new point's index.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."},
                  {"index", "i", T::Int, json(), "The new point's index; the old point there moves up one."},
                  {"position", "pos", T::List, json(), "[x, y], cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorInsertPoint");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorInsertPoint");
            Panel& p = gm.panels[size_t(pi)];
            const int n = int(p.outline.size());
            const int k = int(a.get<int64_t>("index"));
            if (k < 0 || k > n)
                throw CommandError("tailorInsertPoint: index out of 0.." + std::to_string(n));
            const std::vector<Vec2> pos = pointsArg(json::array({a.flags.value("position", json())}), "tailorInsertPoint");
            p.outline.insert(p.outline.begin() + k, pos.front());
            for (Seam& s : gm.seams)
                for (auto* sides : {&s.a, &s.b})
                    for (SeamSide& side : *sides)
                        if (side.panel == pi)
                        {
                            side.from += side.from >= k ? 1 : 0;
                            side.to += side.to >= k ? 1 : 0;
                        }
            writeGarment(g, op, gm);
            return int64_t(k);
        },
        .examples = {"tailorInsertPoint('Garment', panel='Front', index=2, position=[10, 40])"},
    });

    ctx.registerCommand({
        .name = "tailorDeletePoint",
        .doc = "Delete an outline point from a panel (it keeps at least 3). Seams ending there end at the next point; a seam "
               "left without length is removed. Returns the number of seams removed.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."}, {"index", "i", T::Int, json(), "The point to delete."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorDeletePoint");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorDeletePoint");
            Panel& p = gm.panels[size_t(pi)];
            const int n = int(p.outline.size());
            const int k = int(a.get<int64_t>("index"));
            if (k < 0 || k >= n)
                throw CommandError("tailorDeletePoint: index out of 0.." + std::to_string(n - 1));
            if (n <= 3)
                throw CommandError("tailorDeletePoint: a panel needs at least 3 points");
            p.outline.erase(p.outline.begin() + k);
            auto shift = [&](int v) { return v > k ? v - 1 : (v == k ? k % (n - 1) : v); };
            const size_t before = gm.seams.size();
            for (Seam& s : gm.seams)
                for (auto* sides : {&s.a, &s.b})
                    for (SeamSide& side : *sides)
                        if (side.panel == pi)
                        {
                            side.from = shift(side.from);
                            side.to = shift(side.to);
                        }
            std::erase_if(gm.seams, [](const Seam& s) {
                for (const auto* sides : {&s.a, &s.b})
                    for (const SeamSide& side : *sides)
                        if (side.from == side.to)
                            return true;
                return false;
            });
            writeGarment(g, op, gm);
            return int64_t(before - gm.seams.size());
        },
        .examples = {"tailorDeletePoint('Garment', panel='Front', index=2)"},
    });

    ctx.registerCommand({
        .name = "tailorAddPanel",
        .doc = "Add a rectangular panel, placed flat in front of the garment's other panels, ready to reshape and sew.\n\n"
               "Returns the new panel's name.",
        .positionalDoc = "garment",
        .flags = {{"name", "n", T::String, "Panel", "Name; made unique."},
                  {"width", "w", T::Float, 40.0, "cm.", {}, {}, 1.0, 1000.0},
                  {"height", "ht", T::Float, 60.0, "cm.", {}, {}, 1.0, 1000.0},
                  {"layer", "l", T::Int, int64_t(0), "Layer: higher layers sit outside lower ones."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddPanel");
            Garment gm = readGarment(op);
            Panel p;
            std::string base = a.get<std::string>("name");
            p.name = base;
            for (int k = 2; std::any_of(gm.panels.begin(), gm.panels.end(), [&](const Panel& o) { return o.name == p.name; }); ++k)
                p.name = base + std::to_string(k);
            const float w = float(a.get<double>("width") / kCmPerM), h = float(a.get<double>("height") / kCmPerM);
            p.outline = {{0.f, 0.f}, {w, 0.f}, {w, h}, {0.f, h}};
            p.layer = int(a.get<int64_t>("layer"));
            // In front of everything placed so far, centred on it, facing +Z.
            float zMax = 0.f, xMid = 0.f, yMid = 1.f;
            if (!gm.panels.empty())
            {
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                zMax = -1e9f;
                for (const Panel& o : gm.panels)
                {
                    x0 = std::min(x0, o.origin[0]);
                    x1 = std::max(x1, o.origin[0]);
                    y0 = std::min(y0, o.origin[1]);
                    y1 = std::max(y1, o.origin[1]);
                    zMax = std::max(zMax, o.origin[2]);
                }
                xMid = 0.5f * (x0 + x1);
                yMid = 0.5f * (y0 + y1);
            }
            p.origin = {xMid - 0.5f * w, yMid - 0.5f * h, zMax + 0.1f};
            p.right = {1.f, 0.f, 0.f};
            p.up = {0.f, 1.f, 0.f};
            const std::string name = p.name;
            gm.panels.push_back(std::move(p));
            writeGarment(g, op, gm);
            return name;
        },
        .examples = {"tailorAddPanel('Garment', name='Pocket', width=15, height=18)"},
    });

    ctx.registerCommand({
        .name = "tailorDeletePanel",
        .doc = "Delete panels and every seam that sews them. Returns the number of seams removed.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::List, json(), "Panels by name or index."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorDeletePanel");
            Garment gm = readGarment(op);
            if (!a.has("panel") || a.flags["panel"].empty())
                throw CommandError("tailorDeletePanel: name the panels");
            std::vector<int> gone = panelsArg(gm, a, "panel", "tailorDeletePanel");
            std::sort(gone.begin(), gone.end());
            auto removed = [&](int p) { return std::binary_search(gone.begin(), gone.end(), p); };
            const size_t before = gm.seams.size();
            std::erase_if(gm.seams, [&](const Seam& s) {
                for (const auto* sides : {&s.a, &s.b})
                    for (const SeamSide& side : *sides)
                        if (removed(side.panel))
                            return true;
                return false;
            });
            for (Seam& s : gm.seams)
                for (auto* sides : {&s.a, &s.b})
                    for (SeamSide& side : *sides)
                        side.panel -= int(std::lower_bound(gone.begin(), gone.end(), side.panel) - gone.begin());
            for (auto it = gone.rbegin(); it != gone.rend(); ++it)
                gm.panels.erase(gm.panels.begin() + *it);
            writeGarment(g, op, gm);
            return int64_t(before - gm.seams.size());
        },
        .examples = {"tailorDeletePanel('Garment', panel=['Pocket'])"},
    });

    ctx.registerCommand({
        .name = "tailorUnsew",
        .doc = "Remove seams by index. Returns how many were removed.",
        .positionalDoc = "garment",
        .flags = {{"seam", "s", T::List, json(), "Seam indices, as tailorSew returned them."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorUnsew");
            Garment gm = readGarment(op);
            std::vector<int64_t> idx;
            json list = a.flags.value("seam", json::array());
            if (!list.is_array())
                list = json::array({list});
            for (const json& v : list)
                if (v.is_number_integer() && v.get<int64_t>() >= 0 && size_t(v.get<int64_t>()) < gm.seams.size())
                    idx.push_back(v.get<int64_t>());
            std::sort(idx.begin(), idx.end());
            idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
            for (auto it = idx.rbegin(); it != idx.rend(); ++it)
                gm.seams.erase(gm.seams.begin() + *it);
            writeGarment(g, op, gm);
            return int64_t(idx.size());
        },
        .examples = {"tailorUnsew('Garment', seam=[3])"},
    });

    ctx.registerCommand({
        .name = "tailorLive",
        .doc = "Keep a garment simulating in real time (on) or stop it where it got to (off). Live garments answer pattern "
               "edits as they happen and can be pulled with the Grab Cloth tool. Returns whether it is live.",
        .positionalDoc = "garment",
        .flags = {{"on", "o", T::Bool, true, "Live or not."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorLive");
            g.setValue({op.id(), plugIndex(op, "live")}, a.get<bool>("on"));
            return a.get<bool>("on");
        },
        .examples = {"tailorLive('Garment')", "tailorLive('Garment', on=False)"},
    });

    ctx.registerCommand({
        .name = "tailorDrawPanel",
        .doc = "Make a panel from a closed outline drawn on the body: the points (world, cm) are unrolled around the body's "
               "vertical axis into a flat pattern piece that wraps back onto the body at a clearance.\n\n"
               "Adds to the named or selected garment; with none, starts a garment on the selected mesh. Returns [garment, "
               "panel names].",
        .positionalDoc = "garment",
        .flags = {{"points", "pt", T::List, json(), "The outline, [[x, y, z], ...] in world cm, at least 3."},
                  {"name", "n", T::String, "Panel", "Panel name; made unique."},
                  {"mirror", "m", T::Bool, false, "Also add the mirror image across the body's centre."},
                  {"clearance", "cl", T::Float, 1.5, "How far the panel starts outside the body, cm.", {}, {}, 0.0, 50.0},
                  {"layer", "l", T::Int, int64_t(0), "Layer: higher layers sit outside lower ones."},
                  {"collider", "c", T::String, json(), "The body, when a new garment is started; the selected mesh otherwise."},
                  {"fabric", "fab", T::String, "Cotton", "Fabric preset of a new garment.", presetChoices()},
                  {"particleDistance", "pd", T::Float, 2.0, "Cloth edge length of a new garment, cm.", {}, {}, 0.1, 50.0},
                  {"simulate", "sim", T::Bool, true, "Drape a new garment."},
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames for a new garment.", {}, {}, 0.0, 10000.0},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<glm::dvec3> pts;
            const json list = a.flags.value("points", json());
            if (list.is_array())
                for (const json& p : list)
                    if (p.is_array() && p.size() >= 3)
                        pts.push_back(glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()) / kCmPerM);
            if (pts.size() < 3)
                throw CommandError("tailorDrawPanel: give at least 3 points as [[x, y, z], ...] in cm");

            Node* op = nullptr;
            try
            {
                op = &garmentArg(g, a, "tailorDrawPanel");
            }
            catch (const CommandError&)
            {
                op = nullptr;
            }
            Node* collider = nullptr;
            if (op)
            {
                const std::optional<PlugRef> src = g.source({op->id(), plugIndex(*op, "colliderMesh")});
                collider = src ? g.node(src->node) : nullptr;
            }
            else
                collider = colliderArg(g, a);

            // The body's vertical axis: its feet centre, else the outline's own centre.
            glm::dvec2 axis(0.0);
            if (collider)
            {
                Vec3 feet{0.f, 0.f, 0.f};
                measureCollider(g, *collider, feet);
                axis = {feet[0], feet[2]};
            }
            else
            {
                for (const glm::dvec3& p : pts)
                    axis += glm::dvec2(p.x, p.z) / double(pts.size());
            }

            std::vector<double> theta(pts.size());
            double radius = 0.0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                theta[i] = std::atan2(pts[i].x - axis.x, pts[i].z - axis.y);
                // Keep the angle continuous along the outline so a panel crossing the back doesn't jump a full turn.
                if (i > 0)
                    while (theta[i] - theta[i - 1] > std::numbers::pi)
                        theta[i] -= 2.0 * std::numbers::pi;
                if (i > 0)
                    while (theta[i] - theta[i - 1] < -std::numbers::pi)
                        theta[i] += 2.0 * std::numbers::pi;
                radius += glm::length(glm::dvec2(pts[i].x, pts[i].z) - axis) / double(pts.size());
            }
            radius += a.get<double>("clearance") / kCmPerM;
            double centre = 0.0;
            for (double t : theta)
                centre += t / double(theta.size());

            Garment gm = op ? readGarment(*op) : Garment{};
            auto makePanel = [&](const std::string& base, double side) {
                Panel p;
                p.name = base;
                for (int k = 2; std::any_of(gm.panels.begin(), gm.panels.end(), [&](const Panel& o) { return o.name == p.name; }); ++k)
                    p.name = base + " " + std::to_string(k);
                for (size_t i = 0; i < pts.size(); ++i)
                    p.outline.push_back({float(side * (theta[i] - centre) * radius), float(pts[i].y)});
                double area = 0.0;
                for (size_t i = 0; i < p.outline.size(); ++i)
                {
                    const Vec2 &u = p.outline[i], &v = p.outline[(i + 1) % p.outline.size()];
                    area += double(u[0]) * v[1] - double(v[0]) * u[1];
                }
                if (area < 0.0)
                    std::reverse(p.outline.begin(), p.outline.end());
                // Pattern x runs around the body at this radius, y is height; see Panel's placement convention.
                const double c = side * centre;
                p.origin = {float(axis.x + radius * std::sin(c)), 0.f, float(axis.y + radius * std::cos(c))};
                p.right = {float(std::cos(c)), 0.f, float(-std::sin(c))};
                p.up = {0.f, 1.f, 0.f};
                p.wrapRadius = float(radius);
                p.layer = int(a.get<int64_t>("layer"));
                gm.panels.push_back(std::move(p));
                return gm.panels.back().name;
            };
            json names = json::array();
            const std::string base = a.get<std::string>("name");
            names.push_back(makePanel(a.get<bool>("mirror") ? base + " L" : base, 1.0));
            if (a.get<bool>("mirror"))
            {
                // Mirror across x = axis: the angle flips sign.
                for (double& t : theta)
                    t = -t;
                centre = -centre;
                names.push_back(makePanel(base + " R", 1.0));
            }
            if (op)
            {
                writeGarment(g, *op, gm);
                return json::array({op->path(), names});
            }
            const json made = createGarment(gm, std::string(), a, "tailorDrawPanel", "Garment");
            return json::array({made[1], names});
        },
        .examples = {"tailorDrawPanel(points=[[-15, 140, 12], [15, 140, 12], [15, 100, 14], [-15, 100, 14]], name='Bib', collider='Mannequin')"},
    });

    ctx.registerCommand({
        .name = "tailorBindRenderMesh",
        .doc = "Latch a render mesh to a garment's cloth, like APEX's latch to nearest: every render point follows the "
               "nearest cloth triangle with its offset kept, so a thick, sculpted or retopologised mesh moves with the drape, "
               "live drapes included. Both meshes are taken as they are now; keep their transforms the same (freeze them). "
               "Returns the tailorBinding node.",
        .positionalDoc = "garment",
        .flags = {{"mesh", "m", T::String, json(), "The render mesh to latch."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorBindRenderMesh");
            Node* render = a.has("mesh") ? g.find(a.get<std::string>("mesh")) : nullptr;
            if (!render || !render->type().isA("mesh"))
                throw CommandError("tailorBindRenderMesh: give mesh, the render mesh to latch");
            {
                DrapeNow now;
                g.evaluate();
            }
            const PlugValue sim = g.value({op.id(), plugIndex(op, "outMesh")});
            const PlugValue rest = g.value({render->id(), MeshNode::kOutMesh});
            if (!std::get_if<MeshPtr>(&sim) || !std::get<MeshPtr>(sim) || !std::get_if<MeshPtr>(&rest) || !std::get<MeshPtr>(rest))
                throw CommandError("tailorBindRenderMesh: the garment or the render mesh has no geometry");
            Node* bind = g.createNode(*types::find(kBindingType), {}, kNoNode, true);
            g.setValue({bind->id(), plugIndex(*bind, "restMesh")}, std::get<MeshPtr>(rest));
            g.setValue({bind->id(), plugIndex(*bind, "bindSim")}, std::get<MeshPtr>(sim));
            g.connect({op.id(), plugIndex(op, "outMesh")}, {bind->id(), plugIndex(*bind, "simMesh")});
            g.connect({bind->id(), plugIndex(*bind, "outMesh")}, {render->id(), MeshNode::kInMesh});
            return bind->path();
        },
        .examples = {"tailorBindRenderMesh('Cloak', mesh='SM_Cloak_Thick')"},
    });

    ctx.registerCommand({
        .name = "tailorCheck",
        .doc = "Check a garment's pattern the way a pattern maker would: sleeve cap ease, dropped-shoulder sleeves on an "
               "undropped shoulder, seams that don't match, ease round the chest against the collider, panels too small for the "
               "cloth, crossed outlines, grain. Each finding comes with a tip, shows in the Pattern Editor and as a diagnostic "
               "on the garment mesh. Returns the findings.",
        .positionalDoc = "garment",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return checkGarment(g, garmentArg(g, a, "tailorCheck"));
        },
        .examples = {"tailorCheck('Tunic')"},
    });

    ctx.registerCommand({
        .name = "tailorExportAnimation",
        .doc = "Export an animated garment's simulation for a game or a cutscene: simulates the frames first, then writes an "
               "Alembic file (an Unreal Geometry Cache) or vertex animation textures. Returns what the exporter returns.",
        .positionalDoc = "garment",
        .flags = {{"format", "fmt", T::String, "alembic", "alembic or vat.", {"alembic", "vat"}},
                  {"start", "st", T::Int, json(), "First frame; the garment's startFrame when left out."},
                  {"end", "e", T::Int, json(), "Last frame, inclusive."},
                  {"path", "p", T::String, json(), "Alembic: the .abc file. VAT: the output folder."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorExportAnimation");
            if (!op.as<bool>(plugIndex(op, "animate")))
                throw CommandError("tailorExportAnimation: " + op.name() + " isn't animated; turn on its animate attribute");
            const std::vector<PlugRef> fed = g.destinations({op.id(), plugIndex(op, "outMesh")});
            const Node* mesh = fed.empty() ? nullptr : g.node(fed.front().node);
            if (!mesh)
                throw CommandError("tailorExportAnimation: the garment feeds no mesh");
            json call = {{"args", {mesh->path()}}};
            const int64_t start = a.has("start") ? a.get<int64_t>("start") : op.as<int64_t>(plugIndex(op, "startFrame"));
            call["start"] = double(start);
            if (!a.has("end"))
                throw CommandError("tailorExportAnimation: give end, the last frame");
            call["end"] = double(a.get<int64_t>("end"));
            // Every exported frame must be in the cache: the garment shows cached frames as the exporter steps time.
            ops::execute("simulate", {{"args", {op.path()}}, {"frame", a.get<int64_t>("end")}});
            const bool vat = a.get<std::string>("format") == "vat";
            if (a.has("path"))
                call[vat ? "folder" : "path"] = a.get<std::string>("path");
            return ops::execute(vat ? "exportVertexAnimation" : "exportAlembic", call);
        },
        .examples = {"tailorExportAnimation('Cloak', end=120, path='anim/cloak.abc')", "tailorExportAnimation('Cloak', format='vat')"},
    });

    ctx.registerCommand({
        .name = "tailorRefit",
        .doc = "Move a garment onto another body: grades the pattern by the bodies' measurements (widths by chest, lengths by "
               "height), stands it where the new body stands and drapes it there. Returns the scale factors used.",
        .positionalDoc = "garment",
        .flags = {{"collider", "c", T::String, json(), "The new body mesh."},
                  {"grade", "gr", T::Bool, true, "Scale the pattern to the new body; off only moves it."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorRefit");
            Node* target = a.has("collider") ? g.find(a.get<std::string>("collider")) : nullptr;
            if (!target || !target->type().isA("mesh"))
                throw CommandError("tailorRefit: give collider, the new body mesh");
            Body from;
            Vec3 fromFeet{0.f, 0.f, 0.f};
            if (const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")}))
                if (Node* old = g.node(src->node))
                    from = measureCollider(g, *old, fromFeet);
            Vec3 toFeet{0.f, 0.f, 0.f};
            const Body to = measureCollider(g, *target, toFeet);
            const bool grade = a.get<bool>("grade");
            const float sx = grade ? to.chest / std::max(from.chest, 0.01f) : 1.f;
            const float sy = grade ? to.height / std::max(from.height, 0.01f) : 1.f;
            Garment gm = readGarment(op);
            auto scale = [&](std::vector<Vec2>& pts) {
                for (Vec2& v : pts)
                    v = {v[0] * sx, v[1] * sy};
            };
            for (Panel& p : gm.panels)
            {
                scale(p.outline);
                for (auto& h : p.holes)
                    scale(h);
                for (auto& l : p.internalLines)
                    scale(l);
                p.origin = {toFeet[0] + (p.origin[0] - fromFeet[0]) * sx, toFeet[1] + (p.origin[1] - fromFeet[1]) * sy,
                            toFeet[2] + (p.origin[2] - fromFeet[2]) * sx};
                p.wrapRadius *= sx;
            }
            writeGarment(g, op, gm);
            g.connect({target->id(), MeshNode::kOutMesh}, {op.id(), plugIndex(op, "colliderMesh")}, true);
            g.connect({target->id(), Transform::kWorldMatrix}, {op.id(), plugIndex(op, "colliderMatrix")}, true);
            return {{"width", sx}, {"length", sy}};
        },
        .examples = {"tailorRefit('Tunic', collider='Hero')"},
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
