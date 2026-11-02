#include "scene/Diagnostics.h"

#include "ops/Command.h"
#include "python/Python.h"
#include "scene/Graph.h"

#include <algorithm>
#include <map>

namespace lila::scene::diagnostics
{
namespace
{
struct Store
{
    std::vector<Issue> issues;
    uint64_t nextId = 1;
    uint64_t version = 0;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    int graphListener = 0;
    /** Issues whose object resolved to nothing are tried again before the next read. */
    bool retry = false;
    /** The validator running now, which reports default to. */
    std::string running;
};

Store& store()
{
    static Store s;
    return s;
}

/** The names the gate never stops, so a blocking issue can always be cleared. */
bool ownCommand(std::string_view name)
{
    for (const char* c : {"reportIssue", "clearIssues", "listIssues", "fixIssue", "validate", "addValidator", "removeValidator"})
        if (name == c)
            return true;
    return false;
}

void changed()
{
    Store& s = store();
    ++s.version;
    auto copy = s.listeners;
    for (auto& [id, fn] : copy)
        fn();
}

NodeId resolve(const std::string& object)
{
    if (object.empty())
        return kNoNode;
    Graph& g = Graph::current();
    if (object.front() != '/')
        try
        {
            if (Node* n = g.find(object))
                return n->id();
        }
        catch (const CommandError&)
        {
        }
    for (const Resolver* r : resolvers().all())
        if (*r)
            if (NodeId id = (*r)(object); id != kNoNode && g.node(id))
                return id;
    return kNoNode;
}

/**
 * The part after the dot is an attribute when the node has one by that name; otherwise a component, with ":attr" in
 * it, "<component>:<attr>" when the node has that and "<attr>" when not.
 */
void settle(Issue& i)
{
    if (i.node == kNoNode || i.component.empty())
        return;
    const Node* n = Graph::current().node(i.node);
    if (!n)
        return;
    const NodeType& t = n->type();
    const std::string whole = i.attr.empty() ? i.component : i.component + ":" + i.attr;
    if (t.attrIndex(whole) >= 0)
    {
        i.attr = whole;
        i.component.clear();
        return;
    }
}

void watchGraph()
{
    Store& s = store();
    if (s.graphListener)
        return;
    s.graphListener = Graph::current().addListener(
        [](Change c, NodeId id) {
            Store& st = store();
            if (c == Change::Cleared)
            {
                if (!st.issues.empty())
                {
                    st.issues.clear();
                    changed();
                }
                return;
            }
            if (c == Change::Deleted)
            {
                bool any = false;
                for (Issue& i : st.issues)
                    if (i.node == id)
                    {
                        i.node = kNoNode;
                        any = true;
                    }
                st.retry = st.retry || any;
                if (any)
                    changed();
            }
            else if (c == Change::Created || c == Change::Renamed || c == Change::Reparented)
                st.retry = st.retry || std::ranges::any_of(st.issues, [](const Issue& i) { return i.node == kNoNode && !i.object.empty(); });
        },
        true);
}

/** Attaches waiting issues to the nodes their objects now resolve to. */
void retryUnresolved()
{
    Store& s = store();
    if (!s.retry)
        return;
    s.retry = false;
    bool any = false;
    for (Issue& i : s.issues)
        if (i.node == kNoNode && !i.object.empty())
            if (NodeId id = resolve(i.object); id != kNoNode)
            {
                i.node = id;
                settle(i);
                any = true;
            }
    if (any)
        changed();
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

/** The target with its object part following the node through renames and reparenting. */
std::string currentTarget(const Issue& i, const Graph& g)
{
    const Node* n = g.node(i.node);
    if (!n || !i.target.starts_with(i.object))
        return i.target;
    const bool path = i.object.find_first_of("|/") != std::string::npos;
    return (path ? g.fullPath(i.node) : n->name()) + i.target.substr(i.object.size());
}

json toJson(const Issue& i)
{
    Graph& g = Graph::current();
    json j = {{"id", i.id},           {"source", i.source},   {"severity", severityName(i.severity)},
              {"target", currentTarget(i, g)},   {"object", g.node(i.node) ? g.fullPath(i.node) : i.object},
              {"component", i.component}, {"attribute", i.attr}, {"message", i.message},
              {"resolved", i.node != kNoNode || i.object.empty()}};
    if (!i.blocks.empty())
        j["blocks"] = i.blocks;
    if (!i.fix.empty())
        j["fix"] = i.fix;
    return j;
}

std::vector<std::string> stringList(const json& v)
{
    std::vector<std::string> out;
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& e : v)
            if (e.is_string())
                out.push_back(e.get<std::string>());
    return out;
}

Severity severityFlag(const Args& a, Severity fallback)
{
    if (!a.has("severity"))
        return fallback;
    std::optional<Severity> s = parseSeverity(a.get<std::string>("severity"));
    if (!s)
        throw CommandError("severity is info, warning or error");
    return *s;
}

/** The nodes positional args name, resolving prim paths too. Throws for one that resolves to nothing. */
std::vector<NodeId> nodesOf(const Args& a)
{
    std::vector<NodeId> out;
    for (const json& v : a.positional)
    {
        if (!v.is_string())
            continue;
        const Target t = parseTarget(v.get<std::string>());
        NodeId id = resolve(t.object);
        if (id == kNoNode)
            throw CommandError("no object '" + t.object + "'");
        out.push_back(id);
    }
    return out;
}
} // namespace

const char* severityName(Severity s)
{
    switch (s)
    {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return "warning";
}

std::optional<Severity> parseSeverity(std::string_view name)
{
    if (iequals(name, "info"))
        return Severity::Info;
    if (iequals(name, "warning") || iequals(name, "warn"))
        return Severity::Warning;
    if (iequals(name, "error"))
        return Severity::Error;
    return std::nullopt;
}

Target parseTarget(std::string_view target)
{
    Target t;
    const size_t dot = target.find('.');
    t.object = std::string(target.substr(0, dot));
    if (dot == std::string_view::npos)
        return t;
    if (t.object.empty())
        throw CommandError("target '" + std::string(target) + "' has no object before the dot");
    std::string_view rest = target.substr(dot + 1);
    const size_t colon = rest.find(':');
    t.component = std::string(rest.substr(0, colon));
    if (colon != std::string_view::npos)
        t.attr = std::string(rest.substr(colon + 1));
    return t;
}

uint64_t report(Issue issue)
{
    watchGraph();
    Store& s = store();
    if (issue.object.empty() && issue.component.empty() && issue.attr.empty() && !issue.target.empty())
    {
        Target t = parseTarget(issue.target);
        issue.object = std::move(t.object);
        issue.component = std::move(t.component);
        issue.attr = std::move(t.attr);
    }
    if (issue.node == kNoNode)
        issue.node = resolve(issue.object);
    else if (issue.object.empty())
        issue.object = Graph::current().fullPath(issue.node);
    if (issue.target.empty())
        issue.target = issue.object + (issue.component.empty() && issue.attr.empty() ? ""
                                       : "." + (issue.component.empty() ? issue.attr : issue.component + (issue.attr.empty() ? "" : ":" + issue.attr)));
    if (!issue.component.empty())
    {
        // "<object>.<attr>" names an attribute when the node has one by that name.
        if (issue.attr.empty())
            if (const Node* n = Graph::current().node(issue.node); n && n->type().attrIndex(issue.component) >= 0)
                std::swap(issue.component, issue.attr);
        settle(issue);
    }
    if (issue.source.empty())
        issue.source = s.running.empty() ? "user" : s.running;
    std::erase_if(issue.blocks, [](const std::string& b) { return b.empty() || ownCommand(b); });
    issue.id = s.nextId++;
    const uint64_t id = issue.id;
    s.issues.push_back(std::move(issue));
    changed();
    return id;
}

size_t clear(std::string_view source, std::optional<NodeId> node, std::optional<Severity> severity)
{
    Store& s = store();
    const size_t n = std::erase_if(s.issues, [&](const Issue& i) {
        return (source.empty() || i.source == source) && (!node || i.node == *node) && (!severity || i.severity == *severity);
    });
    if (n)
        changed();
    return n;
}

bool remove(uint64_t id)
{
    Store& s = store();
    if (!std::erase_if(s.issues, [id](const Issue& i) { return i.id == id; }))
        return false;
    changed();
    return true;
}

const std::vector<Issue>& issues()
{
    retryUnresolved();
    return store().issues;
}

const Issue* find(uint64_t id)
{
    for (const Issue& i : issues())
        if (i.id == id)
            return &i;
    return nullptr;
}

std::vector<const Issue*> issuesOf(NodeId node)
{
    std::vector<const Issue*> out;
    if (node == kNoNode)
        return out;
    for (const Issue& i : issues())
        if (i.node == node)
            out.push_back(&i);
    return out;
}

uint64_t version() { return store().version; }

int addListener(std::function<void()> fn)
{
    Store& s = store();
    s.listeners[s.nextListener] = std::move(fn);
    return s.nextListener++;
}

void removeListener(int id) { store().listeners.erase(id); }

bool componentMatches(std::string_view component, std::string_view typeName, std::string_view category, std::string_view attr)
{
    if (component.empty())
        return false;
    if (iequals(component, typeName) || iequals(component, category))
        return true;
    return attr.size() > component.size() && attr[component.size()] == ':' && iequals(attr.substr(0, component.size()), component);
}

bool attrMatches(const Issue& issue, std::string_view attr)
{
    if (issue.attr.empty() || attr.empty())
        return false;
    if (attr == issue.attr)
        return true;
    // "weights" marks every element row; "weights[3]" only its own.
    const size_t bracket = attr.find('[');
    return bracket != std::string_view::npos && attr.substr(0, bracket) == issue.attr;
}

Registry<Resolver>& resolvers()
{
    static Registry<Resolver> r;
    return r;
}

Registry<Validator>& validators()
{
    static Registry<Validator> r;
    return r;
}

void validate(const std::vector<std::string>& names)
{
    std::vector<std::string> run = names;
    if (run.empty())
        for (const std::string& id : validators().ids())
            run.push_back(id);
    for (const std::string& name : run)
        if (!validators().find(name))
            throw CommandError("no validator '" + name + "'");
    Store& s = store();
    for (const std::string& name : run)
    {
        const Validator* v = validators().find(name);
        if (!v || !v->run)
            continue;
        clear(name);
        const std::string outer = std::exchange(s.running, name);
        try
        {
            v->run();
        }
        catch (const std::exception& e)
        {
            s.running = outer;
            Issue failed;
            failed.source = name;
            failed.severity = Severity::Error;
            failed.message = std::string("validator failed: ") + e.what();
            report(std::move(failed));
            continue;
        }
        s.running = outer;
    }
}

std::vector<const Issue*> blockers(std::string_view command)
{
    std::vector<const Issue*> out;
    for (const Issue& i : store().issues)
        if (std::ranges::find(i.blocks, command) != i.blocks.end())
            out.push_back(&i);
    return out;
}

void registerCommands()
{
    using T = ArgType;

    ops::setExecuteGate([](std::string_view name) {
        if (store().issues.empty() || ownCommand(name))
            return;
        std::vector<const Issue*> stop = blockers(name);
        if (stop.empty())
            return;
        std::string msg = std::string(name) + " is blocked by " + std::to_string(stop.size()) + (stop.size() == 1 ? " issue" : " issues") + ":";
        const size_t shown = std::min<size_t>(stop.size(), 5);
        for (size_t k = 0; k < shown; ++k)
            msg += "\n  " + std::string(severityName(stop[k]->severity)) + " " + (stop[k]->target.empty() ? "scene" : currentTarget(*stop[k], Graph::current())) + ": " +
                   stop[k]->message + " (" + stop[k]->source + ")";
        if (stop.size() > shown)
            msg += "\n  ...";
        throw CommandError(msg);
    });

    ops::registerCommand({
        .name = "reportIssue",
        .doc = "Attaches info, a warning or an error to an object, one of its components or an attribute.\n\n"
               "The target is \"<object>[.<component>[:<attribute>]]\": a Lila path or a USD prim path, then an attribute "
               "(\"crate.translateX\") or a component (a node type, attribute category, Attribute Editor section or "
               "attribute namespace) with an optional attribute in it. No target attaches it to the scene. The outliner "
               "badges the object, the Attribute Editor marks the component and attribute. Issues are not saved with "
               "the scene; validators report them again when they run.",
        .positionalDoc = "target",
        .flags = {
            {"message", "m", T::String, nullptr, "What is wrong and, ideally, how to fix it."},
            {"severity", "sv", T::String, "warning", "How bad it is.", {"info", "warning", "error"}},
            {"source", "src", T::String, nullptr, "Who reports it, so it can clear its own. Defaults to the validator running, else \"user\"."},
            {"blocks", "b", T::List, nullptr, "Commands it refuses to let run until it is cleared.", {}, "reportIssue(\"crate\", m=\"No collision\", severity=\"error\", blocks=[\"exportFbx\"])"},
            {"fix", "fx", T::String, nullptr, "Python that fixes it, run by fixIssue and the Fix button."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() > 1)
                throw CommandError("reportIssue takes one target");
            Issue i;
            if (!a.positional.empty())
                i.target = a.positional[0].get<std::string>();
            i.message = a.get<std::string>("message");
            if (i.message.empty())
                throw CommandError("reportIssue needs a message");
            i.severity = severityFlag(a, Severity::Warning);
            i.source = a.get<std::string>("source");
            i.blocks = stringList(a.flags.value("blocks", json()));
            i.fix = a.get<std::string>("fix");
            return report(std::move(i));
        },
        .examples = {"reportIssue(\"/World/crate.health:max\", m=\"Max health must be above 0\", severity=\"error\")",
                     "reportIssue(\"crate\", m=\"Over the triangle budget\", fix=\"lila.ops.polyReduce('crate', percentage=50)\")"},
    });

    ops::registerCommand({
        .name = "clearIssues",
        .doc = "Removes issues: those of the given objects, source or severity, or every one when nothing narrows it.",
        .positionalDoc = "objects",
        .flags = {
            {"source", "src", T::String, nullptr, "Only this reporter's."},
            {"severity", "sv", T::String, nullptr, "Only this severity.", {"info", "warning", "error"}},
            {"id", "i", T::Int, nullptr, "Only the issue with this id."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("id"))
                return remove(uint64_t(a.get<int64_t>("id"))) ? 1 : 0;
            const std::string source = a.get<std::string>("source");
            std::optional<Severity> sev;
            if (a.has("severity"))
                sev = severityFlag(a, Severity::Warning);
            std::vector<NodeId> nodes = nodesOf(a);
            if (nodes.empty())
                return clear(source, std::nullopt, sev);
            size_t n = 0;
            for (NodeId id : nodes)
                n += clear(source, id, sev);
            return n;
        },
    });

    ops::registerCommand({
        .name = "listIssues",
        .doc = "The current issues, as dicts {id, source, severity, target, object, component, attribute, message, resolved, blocks, fix}.",
        .positionalDoc = "objects",
        .flags = {
            {"source", "src", T::String, nullptr, "Only this reporter's."},
            {"severity", "sv", T::String, nullptr, "Only this severity and worse.", {"info", "warning", "error"}},
            {"blocking", "bl", T::String, nullptr, "Only issues that block this command."},
            {"hierarchy", "hi", T::Bool, false, "With objects: their descendants' issues too."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::string source = a.get<std::string>("source");
            std::optional<Severity> atLeast;
            if (a.has("severity"))
                atLeast = severityFlag(a, Severity::Info);
            const std::string blocking = a.get<std::string>("blocking");
            std::vector<NodeId> nodes = nodesOf(a);
            if (a.get<bool>("hierarchy"))
                for (size_t k = 0, n = nodes.size(); k < n; ++k)
                    for (NodeId d : g.descendants(nodes[k]))
                        nodes.push_back(d);
            json out = json::array();
            for (const Issue& i : issues())
            {
                if (!source.empty() && i.source != source)
                    continue;
                if (atLeast && i.severity < *atLeast)
                    continue;
                if (!blocking.empty() && std::ranges::find(i.blocks, blocking) == i.blocks.end())
                    continue;
                if (!nodes.empty() && std::ranges::find(nodes, i.node) == nodes.end())
                    continue;
                out.push_back(toJson(i));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "fixIssue",
        .doc = "Runs an issue's fix, then runs its validator again (or drops the issue when no validator reported it).",
        .positionalDoc = "ids",
        .undoable = false,
        .run = [](const Args& a) -> json {
            json fixed = json::array();
            for (const json& v : a.positional)
            {
                const uint64_t id = v.is_number() ? v.get<uint64_t>() : std::stoull(v.get<std::string>());
                const Issue* i = find(id);
                if (!i)
                    throw CommandError("no issue " + std::to_string(id));
                if (i->fix.empty())
                    throw CommandError("issue " + std::to_string(id) + " has no fix");
                if (!python::initialized())
                    throw CommandError("fixes run Python, which this session does not have");
                const std::string fix = i->fix, source = i->source;
                if (!python::run(fix, "<fix " + std::to_string(id) + ">").ok)
                    throw CommandError("the fix for issue " + std::to_string(id) + " failed; see the log");
                if (validators().find(source))
                    validate({source});
                else
                    remove(id);
                fixed.push_back(id);
            }
            return fixed;
        },
    });

    ops::registerCommand({
        .name = "validate",
        .doc = "Runs validators, each clearing the issues it reported last time first. Every one when none are named. "
               "Returns the issue counts by severity.",
        .positionalDoc = "validators",
        .flags = {{"list", "l", T::Bool, false, "Return the validators {name: doc} instead of running any."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("list"))
            {
                json out = json::object();
                for (const std::string& id : validators().ids())
                    out[id] = validators().find(id)->doc;
                return out;
            }
            std::vector<std::string> names;
            for (const json& v : a.positional)
                names.push_back(v.get<std::string>());
            validate(names);
            json counts = {{"info", 0}, {"warning", 0}, {"error", 0}, {"blocking", 0}};
            for (const Issue& i : issues())
            {
                counts[severityName(i.severity)] = counts[severityName(i.severity)].get<int>() + 1;
                if (i.blocking())
                    counts["blocking"] = counts["blocking"].get<int>() + 1;
            }
            return counts;
        },
    });

    ops::registerCommand({
        .name = "addValidator",
        .doc = "Registers a command as a validator: validate runs it, and what it reports belongs to the validator.",
        .positionalDoc = "name",
        .flags = {
            {"command", "c", T::String, nullptr, "The command to run; it reports with reportIssue."},
            {"doc", "d", T::String, "", "What it checks."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("addValidator takes one name");
            const std::string name = a.positional[0].get<std::string>();
            const std::string command = a.get<std::string>("command");
            const CommandDef* def = ops::find(command);
            if (!def)
                throw CommandError("no command '" + command + "'");
            validators().put(name, {a.get<std::string>("doc"), [command] { ops::execute(command, json::object()); }}, def->owner);
            return name;
        },
        .examples = {"lila.extension.register_command(\"checkNaming\", check_naming)\naddValidator(\"naming\", command=\"checkNaming\", doc=\"Prefixes and suffixes\")"},
    });

    ops::registerCommand({
        .name = "removeValidator",
        .doc = "Unregisters validators and clears what they reported.",
        .positionalDoc = "names",
        .undoable = false,
        .run = [](const Args& a) -> json {
            int n = 0;
            for (const json& v : a.positional)
            {
                const std::string name = v.get<std::string>();
                if (validators().remove(name))
                {
                    clear(name);
                    ++n;
                }
            }
            return n;
        },
    });
}
} // namespace lila::scene::diagnostics
