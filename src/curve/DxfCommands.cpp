#include "curve/CurveNodes.h"
#include "curve/Dxf.h"
#include "curve/PlanarNodes.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "io/FileTranslators.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using T = ArgType;

namespace
{
/** File space (Z up, scale cm per unit) to the scene's. */
glm::dmat4 fileToScene(double scale, const std::string& up, const char* cmd)
{
    if (up != "y" && up != "z")
        throw CommandError(std::string(cmd) + ": upAxis must be y or z");
    const bool fileZ = up == "z", sceneZ = math::upAxis() == math::UpAxis::Z;
    const double s = scale;
    if (fileZ && !sceneZ) // (x, y, z) -> (x, z, -y)
        return glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, -s, 0), glm::dvec4(0, s, 0, 0), glm::dvec4(0, 0, 0, 1));
    if (!fileZ && sceneZ) // (x, y, z) -> (x, -z, y)
        return glm::dmat4(glm::dvec4(s, 0, 0, 0), glm::dvec4(0, 0, s, 0), glm::dvec4(0, -s, 0, 0), glm::dvec4(0, 0, 0, 1));
    glm::dmat4 m(s);
    m[3][3] = 1.0;
    return m;
}

std::string readFile(const std::string& path, const char* cmd)
{
    std::ifstream in(paths::fromUtf8(path), std::ios::binary);
    if (!in)
        throw CommandError(std::string(cmd) + ": cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string nodeName(const std::string& want, const char* fallback) { return Graph::validName(want) ? want : std::string(fallback); }

Node* makeCurve(Graph& g, const std::string& name, NodeId parent, Splines s)
{
    Node* c = g.createNode(*types::find("curve"), name, parent, true);
    g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(s)));
    return c;
}

Node* makeMesh(Graph& g, const std::string& name, NodeId parent, geo::Mesh m)
{
    Node* n = g.createNode(*types::find("mesh"), name, parent, true);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n;
}

void appendMesh(geo::Mesh& into, const geo::Mesh& from)
{
    std::vector<glm::dvec3> pts(into.positions().begin(), into.positions().end());
    std::vector<geo::Index> sizes, ids;
    auto add = [&](const geo::Mesh& m, geo::Index offset) {
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
            {
                std::vector<geo::Index> fp = m.facePoints(f);
                sizes.push_back(geo::Index(fp.size()));
                for (geo::Index p : fp)
                    ids.push_back(p + offset);
            }
    };
    add(into, 0);
    geo::Index offset = geo::Index(pts.size());
    pts.insert(pts.end(), from.positions().begin(), from.positions().end());
    add(from, offset);
    into = geo::Mesh::fromPolygons(pts, sizes, ids);
}

void registerImport()
{
    ops::registerCommand({
        .name = "importDxf",
        .doc = "Import a DXF file (ASCII or binary): lines, polylines with their arcs, arcs, circles, ellipses and splines as exact "
               "curves, and 3D faces, polyface meshes and MESH entities as meshes, with blocks expanded where they are inserted. One "
               "curve and one mesh per layer, under a group named after the file. Returns the curve and mesh names.",
        .positionalDoc = "path",
        .flags = {{"unit", "", T::String, "auto", "The file's length unit; auto reads it from the file, and takes mm when it has none.",
                   {"auto", "mm", "cm", "m", "in", "ft"}},
                  {"upAxis", "", T::String, "z", "The file's up axis; converted to the scene's.", {"y", "z"}},
                  {"merge", "", T::Bool, false, "One curve and one mesh for the whole file instead of one per layer."},
                  {"polylines", "", T::Bool, false, "Turn every curve into straight segments, within tolerance."},
                  {"tolerance", "tol", T::Float, 0.01, "cm the segments may stray from the curve, with polylines on."},
                  {"name", "n", T::String, nullptr, "Name of the group; the file name by default."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importDxf: path required");
            std::string path = a.positional[0].get<std::string>();
            DxfDocument doc = readDxf(readFile(path, "importDxf"));
            if (!doc.ok())
                throw CommandError("importDxf: " + path + ": " + doc.error);
            for (const std::string& w : doc.warnings)
                log::warn("importDxf: {}", w);
            if (doc.layers.empty())
                throw CommandError("importDxf: " + path + " has no curves or faces to import");

            std::string unit = a.get<std::string>("unit", "auto");
            double scale = unit == "auto" ? (doc.unitScale > 0.0 ? doc.unitScale : 0.1) : dxfUnitScale(unit);
            if (!(scale > 0.0))
                throw CommandError("importDxf: unit must be auto, mm, cm, m, in or ft");
            const glm::dmat4 m = fileToScene(scale, a.get<std::string>("upAxis", "z"), "importDxf");
            const bool polylines = a.get<bool>("polylines", false);
            const double tol = std::max(1e-6, a.get<double>("tolerance", 0.01));
            for (DxfLayer& l : doc.layers)
            {
                for (Spline& s : l.curves)
                {
                    transform(s, m);
                    if (polylines)
                        s = convert(s, Kind::Poly, tol);
                }
                for (geo::Index p = 0; p < l.mesh.pointCapacity(); ++p)
                    if (l.mesh.pointValid(p))
                        l.mesh.setPosition(p, glm::dvec3(m * glm::dvec4(l.mesh.position(p), 1.0)));
            }

            Graph& g = Graph::current();
            Graph::Batch batch(g);
            std::string stem = paths::toUtf8(paths::fromUtf8(path).stem());
            std::string base = a.get<std::string>("name", nodeName(stem, "Dxf"));
            json names = json::array();
            std::vector<NodeId> made;
            auto keep = [&](Node* n) {
                names.push_back(n->path());
                made.push_back(n->id());
            };
            if (a.get<bool>("merge", false))
            {
                Splines all;
                geo::Mesh mesh;
                for (DxfLayer& l : doc.layers)
                {
                    all.insert(all.end(), std::make_move_iterator(l.curves.begin()), std::make_move_iterator(l.curves.end()));
                    if (l.mesh.numFaces() > 0)
                        appendMesh(mesh, l.mesh);
                }
                if (!all.empty() && mesh.numFaces() > 0)
                {
                    Node* group = g.createNode(*types::find("transform"), base, kNoNode, true);
                    keep(makeCurve(g, base + "Curves", group->id(), std::move(all)));
                    keep(makeMesh(g, base + "Mesh", group->id(), std::move(mesh)));
                }
                else if (!all.empty())
                    keep(makeCurve(g, base, kNoNode, std::move(all)));
                else
                    keep(makeMesh(g, base, kNoNode, std::move(mesh)));
            }
            else
            {
                Node* group = g.createNode(*types::find("transform"), base, kNoNode, true);
                for (DxfLayer& l : doc.layers)
                {
                    std::string ln = nodeName(l.name, "Layer");
                    const bool both = !l.curves.empty() && l.mesh.numFaces() > 0;
                    NodeId parent = group->id();
                    Node* top = nullptr;
                    if (both)
                    {
                        top = g.createNode(*types::find("transform"), ln, parent, true);
                        parent = top->id();
                    }
                    if (!l.curves.empty())
                    {
                        Node* c = makeCurve(g, both ? ln + "Curves" : ln, parent, std::move(l.curves));
                        keep(c);
                        top = top ? top : c;
                    }
                    if (l.mesh.numFaces() > 0)
                    {
                        Node* n = makeMesh(g, both ? ln + "Mesh" : ln, parent, std::move(l.mesh));
                        keep(n);
                        top = top ? top : n;
                    }
                    if (l.hidden && top)
                        g.setValue({top->id(), Transform::kVisibility, -1}, false);
                }
            }
            g.select(made);
            return names;
        },
    });
}

void registerExport()
{
    ops::registerCommand({
        .name = "exportDxf",
        .doc = "Write curves and meshes to a DXF file, one layer per object. R2000 writes curves as splines and polylines and meshes "
               "as polyface meshes; R12 writes everything as polylines, for older laser and CNC software. Defaults to the selection "
               "and everything under it.",
        .positionalDoc = "path nodes...",
        .flags = {{"unit", "", T::String, "mm", "The file's length unit, recorded in the file.", {"mm", "cm", "m", "in", "ft"}},
                  {"upAxis", "", T::String, "z", "The file's up axis; converted from the scene's.", {"y", "z"}},
                  {"version", "", T::String, "2000", "DXF version: 2000 or r12.", {"2000", "r12"}},
                  {"splines", "", T::Bool, true, "R2000: curves as SPLINE entities. Off writes them as polylines within tolerance."},
                  {"tolerance", "tol", T::Float, 0.01, "cm, for curves written as polylines."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("exportDxf: path required");
            std::string path = a.positional[0].get<std::string>();
            DxfWriteOptions o;
            o.unit = a.get<std::string>("unit", "mm");
            double scale = dxfUnitScale(o.unit);
            if (!(scale > 0.0))
                throw CommandError("exportDxf: unit must be mm, cm, m, in or ft");
            std::string version = a.get<std::string>("version", "2000");
            if (version != "2000" && version != "r12")
                throw CommandError("exportDxf: version is 2000 or r12");
            o.version = version == "r12" ? DxfVersion::R12 : DxfVersion::R2000;
            o.splines = a.get<bool>("splines", true);
            o.tolerance = std::max(1e-6, a.get<double>("tolerance", 0.01)) / scale;
            const glm::dmat4 toFile = glm::inverse(fileToScene(scale, a.get<std::string>("upAxis", "z"), "exportDxf"));

            Graph& g = Graph::current();
            std::vector<NodeId> roots;
            for (size_t k = 1; k < a.positional.size(); ++k)
            {
                Node* n = g.find(a.positional[k].get<std::string>());
                if (!n)
                    throw CommandError("exportDxf: no node named '" + a.positional[k].get<std::string>() + "'");
                roots.push_back(n->id());
            }
            if (roots.empty())
                roots = g.selection();
            std::vector<NodeId> nodes;
            for (NodeId r : roots)
            {
                nodes.push_back(r);
                for (NodeId d : g.descendants(r))
                    nodes.push_back(d);
            }
            g.evaluate();
            std::vector<DxfObject> objects;
            std::vector<std::unique_ptr<geo::Mesh>> meshes;
            std::vector<NodeId> seen;
            for (NodeId id : nodes)
            {
                Node* n = g.node(id);
                if (!n || std::ranges::find(seen, id) != seen.end())
                    continue;
                seen.push_back(id);
                if (n->type().isA("curve"))
                {
                    DxfObject obj{.layer = n->name(), .curves = splinesOf(g.value({id, CurveShape::kWorldSpace, -1}))};
                    for (Spline& s : obj.curves)
                        transform(s, toFile);
                    if (!obj.curves.empty())
                        objects.push_back(std::move(obj));
                }
                else if (n->type().isA("mesh"))
                {
                    MeshPtr m = modifiers::exportMesh(g, id);
                    if (!m)
                        continue;
                    auto mesh = std::make_unique<geo::Mesh>(*m);
                    const glm::dmat4 w = toFile * n->as<glm::dmat4>(Transform::kWorldMatrix);
                    for (geo::Index p = 0; p < mesh->pointCapacity(); ++p)
                        if (mesh->pointValid(p))
                            mesh->setPosition(p, glm::dvec3(w * glm::dvec4(mesh->position(p), 1.0)));
                    objects.push_back({.layer = n->name(), .mesh = mesh.get()});
                    meshes.push_back(std::move(mesh));
                }
            }
            if (objects.empty())
                throw CommandError("exportDxf: select curves or meshes to export");
            std::ofstream out(paths::fromUtf8(path), std::ios::binary);
            std::string data = writeDxf(objects, o);
            if (!out || !out.write(data.data(), std::streamsize(data.size())))
                throw CommandError("exportDxf: cannot write " + path);
            return path;
        },
    });
}
} // namespace

void registerDxfCommands()
{
    registerImport();
    registerExport();
    if (!io::translators::find("dxf"))
        io::translators::add({.id = "dxf",
                              .label = "DXF",
                              .doc = "AutoCAD Drawing Exchange: curves in as exact splines and faces as meshes; curves and meshes out.",
                              .extensions = {".dxf"},
                              .import = {.command = "importDxf"},
                              .exportSelection = {.command = "exportDxf"}});
}
} // namespace lila::curve
