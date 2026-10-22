#include "scene/LastOp.h"

#include "cmds/Undo.h"
#include "scene/Graph.h"

#include <algorithm>
#include <map>
#include <optional>

namespace lila::scene::lastOp
{
namespace
{
struct State
{
    bool installed = false;
    std::optional<Op> op;
    ComponentMode pendingMode = ComponentMode::Object;
    bool rerunning = false;
    std::map<int, std::function<void(Change)>> listeners;
    int nextListener = 1;
};

State& st()
{
    static State s;
    return s;
}

void notify(Change c)
{
    auto copy = st().listeners;
    for (auto& [id, fn] : copy)
        fn(c);
}

void onExecute(const ExecuteEvent& e)
{
    State& s = st();
    if (s.rerunning)
        return;
    if (!e.done)
    {
        s.pendingMode = components::mode();
        return;
    }
    UndoStack& undo = UndoStack::get();
    const CommandDef* def = cmds::find(e.name);
    bool recorded = e.ok && e.source == CallSource::UI && def && def->adjustable && def->undoable && undo.undoLabel() == def->name &&
                    (!s.op || undo.serial() != s.op->serial);
    if (recorded)
    {
        s.op = Op{std::string(e.name), e.call.is_object() ? e.call : json::object(), s.pendingMode, components::mode(), undo.serial()};
        notify(Change::Recorded);
    }
    else if (e.source == CallSource::UI)
        notify(Change::Other);
}

std::vector<std::string> meshesOf(const Op& op)
{
    Graph& g = Graph::current();
    std::vector<std::string> out;
    auto addNode = [&](NodeId id) {
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n->name());
    };
    if (op.call.contains("args"))
    {
        const json& args = op.call["args"];
        for (const json& a : args.is_array() ? args : json::array({args}))
            if (a.is_string())
                out.push_back(a.get<std::string>());
    }
    else if (op.modeBefore != ComponentMode::Object && !components::selected(op.modeBefore).empty())
        for (auto& [id, ids] : components::selected(op.modeBefore))
            addNode(id);
    else
        for (NodeId id : g.selection())
            addNode(id);
    return out;
}
} // namespace

void install()
{
    if (st().installed)
        return;
    st().installed = true;
    cmds::addExecuteListener(onExecute);
}

const Op* current()
{
    return st().op ? &*st().op : nullptr;
}

bool live()
{
    const State& s = st();
    return s.op && !s.rerunning && UndoStack::get().serial() == s.op->serial &&
           (s.op->empty || UndoStack::get().undoLabel() == s.op->command);
}

void clear()
{
    if (!st().op)
        return;
    st().op.reset();
    notify(Change::Cleared);
}

void rerun(const json& flags, CallSource source)
{
    State& s = st();
    if (!live())
        throw CommandError("nothing to adjust: the last operation is no longer the latest undo step");
    Op op = *s.op;
    json call = op.call;
    for (auto& [k, v] : flags.items())
        call[k] = v;

    UndoStack& undo = UndoStack::get();
    s.rerunning = true;
    if (!op.empty)
        undo.undo();
    components::setMode(op.modeBefore);
    uint64_t before = undo.serial();
    auto restore = [&] {
        // Nothing new landed on the stack, so the original result can come back.
        if (!op.empty && undo.serial() == before && undo.redo())
            components::setMode(op.modeAfter);
        s.rerunning = false;
        s.op->serial = undo.serial();
    };
    try
    {
        cmds::execute(op.command, call, source);
    }
    catch (...)
    {
        restore();
        notify(Change::Rerun);
        throw;
    }
    s.rerunning = false;
    s.op->empty = undo.serial() == before;
    s.op->call = std::move(call);
    s.op->modeAfter = components::mode();
    s.op->serial = undo.serial();
    notify(Change::Rerun);
}

std::vector<const FlagDef*> editableFlags(const std::string& command)
{
    std::vector<const FlagDef*> out;
    if (const CommandDef* def = cmds::find(command))
        for (const FlagDef& f : def->flags)
            if (f.type == ArgType::Bool || f.type == ArgType::Int || f.type == ArgType::Float ||
                (f.type == ArgType::String && !f.choices.empty()))
                out.push_back(&f);
    return out;
}

json flagValue(const Op& op, const FlagDef& flag)
{
    if (op.call.contains(flag.name) && !op.call[flag.name].is_null())
        return op.call[flag.name];
    if (!flag.shortName.empty() && op.call.contains(flag.shortName))
        return op.call[flag.shortName];
    return flag.defaultValue;
}

std::string modifierEquivalent(const std::string& command)
{
    static const std::map<std::string, std::string> kinds = {
        {"polyBevel", "bevel"}, {"polyMirror", "mirror"}, {"polySubdivide", "subdivision"}, {"polyTriangulate", "triangulate"}};
    auto it = kinds.find(command);
    return it == kinds.end() ? std::string() : it->second;
}

void makeNonDestructive()
{
    if (!live())
        throw CommandError("makeNonDestructive: the last operation is no longer the latest undo step");
    Op op = *st().op;
    std::string kind = modifierEquivalent(op.command);
    if (kind.empty())
        throw CommandError("makeNonDestructive: " + op.command + " has no modifier equivalent");
    const CommandDef* def = cmds::find(op.command);
    auto value = [&](const char* flag) {
        for (const FlagDef& f : def->flags)
            if (f.name == flag)
                return flagValue(op, f);
        return json();
    };
    json params = json::object();
    if (kind == "bevel")
        params = {{"offset", value("offset")}, {"segments", value("segments")}, {"profile", value("profile")}};
    else if (kind == "mirror")
    {
        std::string axis = value("axis").is_string() ? value("axis").get<std::string>() : "x";
        const std::string direction = value("direction").is_string() ? value("direction").get<std::string>() : "positive";
        params = {{"axis", axis.empty() ? 0 : std::clamp(axis[0] - 'x', 0, 2)},
                  {"direction", direction == "negative" ? 1 : direction == "both" ? 2 : 0},
                  {"weld", value("weld")}};
    }
    else if (kind == "subdivision")
        params = {{"levels", value("levels")}};

    if (!op.empty)
        UndoStack::get().undo();
    components::setMode(op.modeBefore);
    std::vector<std::string> meshes = meshesOf(op);
    st().op.reset();
    notify(Change::Cleared);
    UndoChunk chunk("Make Non-Destructive");
    for (const std::string& mesh : meshes)
    {
        json call = {{"args", {mesh}}, {"kind", kind}};
        if (!params.empty())
            call["params"] = params;
        cmds::execute("addModifier", call, CallSource::UI);
    }
}

int addListener(std::function<void(Change)> fn)
{
    st().listeners[st().nextListener] = std::move(fn);
    return st().nextListener++;
}

void removeListener(int id)
{
    st().listeners.erase(id);
}
} // namespace lila::scene::lastOp
