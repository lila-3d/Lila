#include "mcp/McpServer.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "python/Python.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

namespace lila::mcp
{
namespace
{
/** Log output emitted while fn runs, as text. */
template <class F> std::string capture(F&& fn, bool& ok)
{
    uint64_t start = log::since(0, 1).empty() ? 0 : log::since(0, 1).back().seq;
    ok = fn();
    std::string out;
    for (const LogEntry& e : log::since(start, 5000))
    {
        if (e.level == LogLevel::Output || e.level == LogLevel::Echo)
            out += e.text;
        // Commands' info chatter would cost the agent tokens on every call; get_log still has it.
        else if (e.level >= LogLevel::Warning)
            out += std::string("[") + log::levelName(e.level) + "] " + e.text + "\n";
    }
    return out;
}

std::string fmtVec(const json& v)
{
    return std::format("[{:.4g}, {:.4g}, {:.4g}]", v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

void describe(scene::Graph& g, scene::NodeId id, int depth, std::string& out)
{
    scene::Node* n = g.node(id);
    glm::dvec3 t = n->as<glm::dvec3>(scene::Transform::kTranslate);
    glm::dvec3 r = n->as<glm::dvec3>(scene::Transform::kRotate);
    glm::dvec3 sc = n->as<glm::dvec3>(scene::Transform::kScale);
    out += std::string(depth * 2, ' ') + n->name() + " (" + n->type().name + ")";
    if (t != glm::dvec3(0.0))
        out += std::format(" t=[{:.4g}, {:.4g}, {:.4g}]", t.x, t.y, t.z);
    if (r != glm::dvec3(0.0))
        out += std::format(" r=[{:.4g}, {:.4g}, {:.4g}]", r.x, r.y, r.z);
    if (sc != glm::dvec3(1.0))
        out += std::format(" s=[{:.4g}, {:.4g}, {:.4g}]", sc.x, sc.y, sc.z);
    if (!n->as<bool>(scene::Transform::kVisibility))
        out += " hidden";
    if (g.isSelected(id))
        out += " *selected*";
    if (n->type().isA("mesh"))
    {
        json s = cmds::execute("meshInfo", {{"args", {n->name()}}});
        out += std::format("\n{}  mesh: {} faces ({} quads, {} tris, {} ngons), {} triangles, {} points", std::string(depth * 2, ' '),
                           s["faces"].get<int>(), s.value("quads", 0), s.value("triFaces", 0), s.value("ngons", 0), s["triangles"].get<int>(),
                           s["points"].get<int>());
        if (s.contains("worldBoundsMin"))
            out += " | world bounds " + fmtVec(s["worldBoundsMin"]) + " to " + fmtVec(s["worldBoundsMax"]);
        out += " | symmetry " + s.value("symmetry", std::string("off"));
        if (s.value("smoothPreview", 0) > 0)
            out += " | smooth preview " + std::to_string(s.value("smoothPreview", 0));
        if (!s.value("closed", true))
            out += std::format(" | OPEN: {} border edges, {} non-manifold", s.value("borderEdges", 0), s.value("nonManifoldEdges", 0));
        if (s.value("shells", 1) > 1)
            out += " | " + std::to_string(s.value("shells", 1)) + " shells";
        if (s.contains("modifiers") && !s["modifiers"].empty())
        {
            out += " | modifiers:";
            for (const json& m : s["modifiers"])
                out += " " + m.get<std::string>();
        }
        if (s.contains("materialSlots") && !s["materialSlots"].empty())
        {
            out += " | materials:";
            for (const json& m : s["materialSlots"])
                out += " " + m.get<std::string>();
        }
    }
    out += "\n";
    for (scene::NodeId c : g.children(id))
        describe(g, c, depth + 1, out);
}
} // namespace

void registerBuiltinTools()
{
    Server& s = Server::get();

    s.addTool({
        .name = "execute_python",
        .description = "Run Python in Lila's interpreter (__main__, persistent between calls). `lila` and `ops` are "
                       "pre-imported. Returns printed output, warnings, errors and the value of a trailing expression. Put a whole "
                       "modeling stage (many commands) in one call and print only what you need.",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"code", {{"type", "string"}, {"description", "Python source"}}}}},
                        {"required", {"code"}}},
        .run = [](const json& args) {
            std::string code = args.value("code", "");
            bool ok = false;
            std::string out = capture([&] { return python::run(code, "<mcp>").ok; }, ok);
            if (!ok)
                throw std::runtime_error(out.empty() ? "failed" : out);
            return out.empty() ? std::string("(ok, no output)") : out;
        },
    });

    s.addTool({
        .name = "run_command",
        .description = "Run one Lila command by name. `args` is {\"args\": [positional...], flag: value}.",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"name", {{"type", "string"}}}, {"args", {{"type", "object"}}}}},
                        {"required", {"name"}}},
        .run = [](const json& args) {
            return cmds::execute(args.value("name", ""), args.value("args", json::object()), CallSource::UI).dump(2);
        },
    });

    s.addTool({
        .name = "list_commands",
        .description = "Lila commands with their docs and flags. Optional `filter` substring.",
        .inputSchema = {{"type", "object"}, {"properties", {{"filter", {{"type", "string"}}}}}},
        .run = [](const json& args) {
            std::string filter = args.value("filter", "");
            std::string out;
            for (const CommandDef* d : cmds::all())
            {
                if (!filter.empty() && d->name.find(filter) == std::string::npos)
                    continue;
                out += d->name + (d->positionalDoc.empty() ? "" : "(" + d->positionalDoc + ")") + ": " + d->doc + "\n";
                for (const FlagDef& f : d->flags)
                    out += "    " + f.name + (f.shortName.empty() ? "" : "/" + f.shortName) + "  " + f.doc + "\n";
            }
            return out;
        },
    });

    s.addTool({
        .name = "get_log",
        .description = "Recent log lines (info, warnings, errors, script output). `since` is a sequence number from a "
                       "previous call; the reply ends with the latest one.",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"since", {{"type", "integer"}}}, {"max", {{"type", "integer"}}}}}},
        .run = [](const json& args) {
            auto entries = log::since(args.value("since", uint64_t(0)), args.value("max", size_t(200)));
            std::string out;
            uint64_t last = 0;
            for (const LogEntry& e : entries)
            {
                out += std::string("[") + log::levelName(e.level) + "] " + e.text + (e.text.ends_with('\n') ? "" : "\n");
                last = e.seq;
            }
            return out + "(last seq " + std::to_string(last) + ")";
        },
    });

    s.addTool({
        .name = "scene_summary",
        .description = "The scene at a glance: transform hierarchy with types, non-default translate/rotate/scale, visibility and "
                       "selection; per mesh its face/quad/tri/ngon and triangle counts, world bounding box, symmetry, smooth preview, "
                       "open borders, shells, modifiers and materials; then the component mode and selection.",
        .inputSchema = {{"type", "object"}, {"properties", json::object()}},
        .run = [](const json&) {
            scene::Graph& g = scene::Graph::current();
            g.evaluate();
            std::string out;
            for (scene::NodeId id : g.roots())
                describe(g, id, 0, out);
            if (out.empty())
                out = "(empty scene)\n";
            out += "component mode: " + cmds::execute("selectMode", json::object()).get<std::string>();
            json sel = cmds::execute("componentSelection", json::object());
            for (auto& [mesh, kinds] : sel.items())
                for (auto& [kind, ids] : kinds.items())
                    out += std::format("\n  {} selected {}: {}", mesh, kind, ids.size() > 40 ? std::to_string(ids.size()) + " ids" : ids.dump());
            return out;
        },
    });

    s.addTool({
        .name = "mesh_info",
        .description = "Topology and health of one mesh: points, edges, faces, triangles, tri/quad/ngon face counts, open border "
                       "edges and loops, non-manifold edges, shells, closed, bounds, symmetry, modifiers, material slots. Check "
                       "this after edits: a clean low-poly prop is closed, single-shell (unless intended) and mostly quads.",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"mesh", {{"type", "string"}, {"description", "Mesh name (default: the selected one)."}}}}}},
        .run = [](const json& args) {
            json call = json::object();
            if (args.contains("mesh") && args["mesh"].is_string())
                call["args"] = {args["mesh"]};
            return cmds::execute("meshInfo", call).dump();
        },
    });

    s.addTool({
        .name = "mesh_check",
        .description = "Pass/fail validation of a model in a few lines: connected shells (each with its world box, largest first), open "
                       "borders, non-manifold edges/vertices, zero-area faces, counts, symmetry drift, and how to fix each issue. Run it "
                       "after each block of edits and before reporting done: a character should be ONE shell, closed and manifold.",
        .inputSchema = {{"type", "object"},
                        {"properties",
                         {{"mesh", {{"type", "string"}, {"description", "Mesh name (default: the selected one)."}}},
                          {"expect_shells", {{"type", "integer"}, {"description", "Connected parts expected (default 1, 0 skips)."}}},
                          {"max_triangles", {{"type", "integer"}, {"description", "Triangle budget."}}}}}},
        .run = [](const json& args) {
            json call = json::object();
            if (args.contains("mesh") && args["mesh"].is_string())
                call["args"] = {args["mesh"]};
            if (args.contains("expect_shells"))
                call["expectShells"] = args["expect_shells"];
            if (args.contains("max_triangles"))
                call["maxTriangles"] = args["max_triangles"];
            json r = cmds::execute("meshCheck", call);
            std::string out = std::format("{}: {} | {} faces ({} quads, {} tris, {} ngons), {} triangles | {} shell{} | box {}\n",
                                          r["mesh"].get<std::string>(), r["ok"].get<bool>() ? "OK" : "PROBLEMS", r["faces"].get<int64_t>(),
                                          r["quads"].get<int64_t>(), r["tris"].get<int64_t>(), r["ngons"].get<int64_t>(),
                                          r["triangles"].get<int64_t>(), r["shells"].get<int64_t>(), r["shells"].get<int64_t>() == 1 ? "" : "s",
                                          r["box"].dump());
            if (r["shells"].get<int64_t>() > 1)
                for (size_t i = 0; i < r["shellList"].size(); ++i)
                    out += std::format("  shell {}: {} faces, box {}\n", i, r["shellList"][i]["faces"].get<int64_t>(), r["shellList"][i]["box"].dump());
            if (r.contains("symmetry"))
                out += std::format("  symmetry {}: max drift {:.3g} cm, {} unmatched\n", r["symmetry"]["axis"].get<std::string>(),
                                   r["symmetry"]["maxAsymmetry"].get<double>(), r["symmetry"]["unmatched"].get<int64_t>());
            for (const json& i : r["issues"])
                out += "  ISSUE: " + i.get<std::string>() + "\n";
            for (const json& i : r.value("notes", json::array()))
                out += "  note: " + i.get<std::string>() + "\n";
            return out;
        },
    });
}
} // namespace lila::mcp
