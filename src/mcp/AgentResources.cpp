#include "mcp/McpServer.h"

#include "ops/Command.h"
#include "core/Paths.h"

#include <fstream>
#include <map>
#include <sstream>

namespace lila::mcp
{
namespace
{
std::filesystem::path agentsDir() { return paths::resourcesDir() / "agents"; }

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("missing " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** Replaces {{name}} with args[name] (or the fallback when it is missing). */
std::string fill(std::string text, const json& args, const std::map<std::string, std::string>& fallback)
{
    for (auto& [key, def] : fallback)
    {
        std::string value = args.contains(key) && args[key].is_string() && !args[key].get<std::string>().empty() ? args[key].get<std::string>() : def;
        std::string tag = "{{" + key + "}}";
        for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
            text.replace(at, tag.size(), value);
    }
    return text;
}

/** Every command name grouped by its first word, appended to the API guide so it never goes stale. */
std::string commandIndex()
{
    std::string out = "\n\n## Every registered command\n\nCall any of these as `lila.ops.<name>(...)` (run `help(lila.ops.<name>)` or the `list_commands` tool with `filter` for flags).\n\n";
    std::string line;
    for (const CommandDef* d : ops::all())
    {
        if (line.size() + d->name.size() > 110)
        {
            out += line + "\n";
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + d->name;
    }
    return out + line + "\n";
}

std::string socketLine(const json& s)
{
    std::string out = s.value("id", "") + " " + s.value("type", "");
    if (s.value("field", false))
        out += " field";
    if (s.contains("implicit"))
        out += " (implicit " + s["implicit"].get<std::string>() + ")";
    else if (s.contains("default") && !s["default"].is_null())
        out += " = " + s["default"].dump();
    return out;
}

/** The geometry-node catalogue from the live node registry, appended to the geometry-nodes guide. */
std::string nodeCatalogue()
{
    const CommandDef* d = ops::find("gnNodeTypes");
    if (!d)
        return {};
    Args a;
    a.flags = {{"detail", true}};
    json types;
    try
    {
        types = d->run(a);
    }
    catch (const std::exception&)
    {
        return {};
    }
    std::map<std::string, std::vector<const json*>> byCategory;
    for (const json& t : types)
        byCategory[t.value("category", "Other")].push_back(&t);
    std::string out = "\n\n## Node catalogue\n\nSockets are listed as they are with default properties; nodes marked \"sockets follow props\" change "
                      "their sockets with their properties (gnGroupInfo shows the resolved ones).\n";
    for (auto& [cat, list] : byCategory)
    {
        out += "\n### " + cat + "\n\n";
        for (const json* t : list)
        {
            out += "- `" + t->value("type", "") + "`: " + t->value("doc", "");
            if (t->value("dynamicSockets", false))
                out += " (sockets follow props)";
            out += "\n";
            std::string props;
            for (const json& p : t->value("props", json::array()))
            {
                props += (props.empty() ? "" : "; ") + p.value("id", "");
                if (!p["options"].empty())
                {
                    std::string opts;
                    for (const json& o : p["options"])
                        opts += (opts.empty() ? "" : "|") + o.get<std::string>();
                    props += " " + opts;
                }
                props += " (default " + p["default"].dump() + ")";
            }
            if (!props.empty())
                out += "  - props: " + props + "\n";
            std::string ins, outs;
            for (const json& s : t->value("inputs", json::array()))
                ins += (ins.empty() ? "" : ", ") + socketLine(s);
            for (const json& s : t->value("outputs", json::array()))
                outs += (outs.empty() ? "" : ", ") + s.value("id", "") + " " + s.value("type", "");
            if (!ins.empty())
                out += "  - in: " + ins + "\n";
            if (!outs.empty())
                out += "  - out: " + outs + "\n";
        }
    }
    return out;
}

struct Doc
{
    const char* uri;
    const char* file;
    const char* name;
    const char* description;
};

constexpr Doc kDocs[] = {
    {"lila://guide/modeling", "guide-modeling.md", "Modeling guide",
     "How to model in Lila from code: one connected mesh grown from one primitive with symmetry and extrusion, recipes for "
     "limbs, head, snout, eyes, ears and tail, selecting components by position and normal, checking with mesh_check and "
     "captures, cheap token use. Read before modeling anything."},
    {"lila://guide/api", "guide-api.md", "API guide", "Where Lila's commands and Python modules live and how to call them, plus an index of every command."},
    {"lila://guide/geometry-nodes", "guide-geometry-nodes.md", "Geometry nodes guide",
     "Authoring geometry-node groups from code: the one-call gnBuildGroup spec, group interfaces, nested groups, colours and "
     "normals for export, sharing as .lilanodes, and the full node catalogue with sockets and options."},
    {"lila://examples/low-poly-dog", "examples/low_poly_dog.py", "Example: low-poly dog",
     "A complete script: a very low-poly dog as one closed symmetric mesh (legs, head, snout, nose, eyes, floppy ears, tail "
     "all extruded), checked with meshCheck and captures."},
};
} // namespace

std::string agentInstructions()
{
    try
    {
        return readFile(agentsDir() / "instructions.md");
    }
    catch (const std::exception&)
    {
        return "You are connected to Lila, a Maya-style 3D DCC. Do the 3D work here with these tools, not in Blender or Maya. Read "
               "the lila://guide/modeling resource (or the read_guide tool) before modeling.";
    }
}

void registerAgentResources()
{
    Server& s = Server::get();
    s.setInstructions(agentInstructions());

    for (const Doc& d : kDocs)
    {
        std::string uri = d.uri, file = d.file;
        bool api = uri == "lila://guide/api";
        bool nodes = uri == "lila://guide/geometry-nodes";
        s.addResource({
            .uri = uri,
            .name = d.name,
            .description = d.description,
            .mimeType = file.ends_with(".py") ? "text/x-python" : "text/markdown",
            .read = [file, api, nodes] { return readFile(agentsDir() / file) + (api ? commandIndex() : nodes ? nodeCatalogue() : std::string()); },
        });
    }

    s.addTool({
        .name = "read_guide",
        .description = "Lila's agent guides, for clients that cannot read MCP resources. topic: modeling (read before modeling), api, "
                       "geometryNodes (read before building node groups: spec format and node catalogue), or example (a complete "
                       "low-poly dog script).",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"topic", {{"type", "string"}, {"enum", {"modeling", "api", "geometryNodes", "example"}}}}}},
                        {"required", {"topic"}}},
        .run = [](const json& args) {
            std::string topic = args.value("topic", "modeling");
            if (topic == "api")
                return readFile(agentsDir() / "guide-api.md") + commandIndex();
            if (topic == "geometryNodes")
                return readFile(agentsDir() / "guide-geometry-nodes.md") + nodeCatalogue();
            if (topic == "example")
                return readFile(agentsDir() / "examples" / "low_poly_dog.py");
            return readFile(agentsDir() / "guide-modeling.md");
        },
    });

    s.addPrompt({
        .name = "model_asset",
        .description = "Model a game asset in Lila step by step: block out one symmetric mesh, refine with edge loops, check captures "
                       "from four angles, iterate.",
        .arguments = json::array({
            {{"name", "subject"}, {"description", "What to model, e.g. \"a small cute dog with big round eyes\""}, {"required", true}},
            {{"name", "style"}, {"description", "Style and density, e.g. \"very low poly, flat shaded\""}, {"required", false}},
            {{"name", "budget"}, {"description", "Triangle budget, e.g. \"500\""}, {"required", false}},
        }),
        .get = [](const json& args) {
            return fill(readFile(agentsDir() / "prompts" / "model_asset.md"), args,
                        {{"subject", "the requested asset"}, {"style", "low poly, clean quads"}, {"budget", "800"}});
        },
    });
}
} // namespace lila::mcp
