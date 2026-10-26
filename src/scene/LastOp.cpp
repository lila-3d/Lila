#include "scene/LastOp.h"

#include "core/Profile.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <utility>

namespace lila::scene::lastOp
{
namespace
{
/** One setting of the op: the undo step holding its result and the call that made it. */
struct Entry
{
    uint64_t step = 0;
    json call;
    ComponentMode modeAfter = ComponentMode::Object;
    std::map<NodeId, components::Ids> made;
};

struct State
{
    bool installed = false;
    std::optional<Op> op;
    /** The op's original run first, then each adjustment still reachable by undo or redo. */
    std::vector<Entry> entries;
    size_t at = 0;
    bool dragging = false;
    /** Undo went below the op or another step went on top, so landing back on an entry restores it. */
    bool away = false;
    bool wasReachable = false;
    ComponentMode pendingMode = ComponentMode::Object;
    uint64_t pendingStep = 0;
    bool rerunning = false;
    json limited = json::object();
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

std::map<NodeId, components::Ids> madeNow()
{
    const ComponentMode m = components::mode();
    if (m == ComponentMode::Vertex || m == ComponentMode::Edge || m == ComponentMode::Face)
        return components::selected(m);
    return {};
}

void moveTo(size_t index)
{
    State& s = st();
    s.at = index;
    s.op->call = s.entries[index].call;
    s.op->modeAfter = s.entries[index].modeAfter;
    s.op->made = s.entries[index].made;
    components::setMode(s.op->modeAfter);
}

void onExecute(const ExecuteEvent& e)
{
    State& s = st();
    if (s.rerunning)
        return;
    UndoStack& undo = UndoStack::get();
    if (!e.done)
    {
        s.pendingMode = components::mode();
        s.pendingStep = undo.newestId();
        s.limited = json::object();
        return;
    }
    const CommandDef* def = ops::find(e.name);
    bool recorded = e.ok && e.source == CallSource::UI && def && def->adjustable && def->undoable && undo.undoLabel() == def->name &&
                    undo.newestId() != s.pendingStep;
    if (recorded)
    {
        s.op = Op{std::string(e.name), e.call.is_object() ? e.call : json::object(), s.pendingMode, components::mode(), madeNow()};
        s.op->call.update(s.limited);
        s.entries = {Entry{undo.newestId(), s.op->call, s.op->modeAfter, s.op->made}};
        s.at = 0;
        s.dragging = false;
        s.away = false;
        s.wasReachable = true;
        notify(Change::Recorded);
    }
}

void onUndoChanged()
{
    State& s = st();
    if (s.rerunning || !s.op)
        return;
    UndoStack& undo = UndoStack::get();
    const uint64_t newest = undo.newestId();
    for (size_t i = 0; i < s.entries.size(); ++i)
        if (s.entries[i].step == newest)
        {
            s.wasReachable = true;
            if (i != s.at || std::exchange(s.away, false))
            {
                s.dragging = false;
                moveTo(i);
                notify(Change::Restored);
            }
            return;
        }
    s.away = true;
    // Selection steps on top of an entry keep it adjustable; a jump in the history can land on another entry.
    const uint64_t edit = undo.stateId();
    for (size_t i = 0; i < s.entries.size(); ++i)
        if (s.entries[i].step == edit && i != s.at)
            moveTo(i);
    const bool now = reachable();
    if (const bool was = std::exchange(s.wasReachable, now); was != now)
        notify(now ? Change::Restored : Change::Other);
}

/** Undoes the selection steps above the op's current entry, so it is the newest step again. False if it is not reachable. */
bool undoSelectionOnTop()
{
    State& s = st();
    if (!reachable())
        return false;
    UndoStack& undo = UndoStack::get();
    s.rerunning = true;
    while (undo.newestId() != s.entries[s.at].step && undo.undo())
    {
    }
    s.rerunning = false;
    return live();
}

std::vector<std::string> meshesOf(const Op& op)
{
    Graph& g = Graph::current();
    std::vector<std::string> out;
    auto addNode = [&](NodeId id) {
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n->path());
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
    ops::addExecuteListener(onExecute);
    UndoStack::get().addListener(onUndoChanged);
}

const Op* current()
{
    return st().op ? &*st().op : nullptr;
}

bool live()
{
    const State& s = st();
    return s.op && !s.rerunning && s.at < s.entries.size() && UndoStack::get().newestId() == s.entries[s.at].step;
}

bool reachable()
{
    const State& s = st();
    // A selection op (selectByAngle) is itself a selection step, so only live() keeps it.
    return live() || (s.op && !s.rerunning && s.at < s.entries.size() && UndoStack::get().stateId() == s.entries[s.at].step);
}

void limitFlag(const std::string& name, const json& value)
{
    st().limited[name] = value;
}

void clear()
{
    if (!st().op)
        return;
    st().op.reset();
    st().entries.clear();
    st().wasReachable = false;
    notify(Change::Cleared);
}

json rerun(const json& flags, CallSource source, bool drag)
{
    LILA_ZONE();

    State& s = st();
    if (!reachable())
        throw CommandError("nothing to adjust: the last operation is no longer the latest undo step");
    json call = s.op->call;
    for (auto& [k, v] : flags.items())
        call[k] = v;
    if (call == s.op->call)
        return nullptr;
    UndoStack& undo = UndoStack::get();
    const size_t redoBefore = undo.redoCount();
    if (!undoSelectionOnTop())
        throw CommandError("nothing to adjust: the last operation is no longer the latest undo step");
    const size_t selectionUndone = undo.redoCount() - redoBefore;
    // A drag keeps replacing the step it made, never the op's own run.
    const bool replace = drag && s.dragging && s.at > 0 && s.at + 1 == s.entries.size();

    // Named after the flags that differ from the settings the step replaces.
    const json& from = replace ? s.entries[s.at - 1].call : s.op->call;
    std::string changed;
    for (auto& [k, v] : flags.items())
        if (k != "args" && (!from.contains(k) || from[k] != v))
            changed += (changed.empty() ? "" : ", ") + k;
    const std::string label = "Adjust " + displayName(s.op->command) + (changed.empty() ? "" : ": " + changed);
    auto redoSelection = [&] {
        for (size_t i = 0; i < selectionUndone; ++i)
            undo.redo();
    };
    s.rerunning = true;
    s.limited = json::object();
    if (!(replace ? undo.reopenNewest(label) : undo.openAmend(label)))
    {
        redoSelection();
        s.rerunning = false;
        throw CommandError("nothing to adjust: the undo stack is busy");
    }
    components::setMode(s.op->modeBefore);
    json result;
    try
    {
        result = ops::execute(s.op->command, call, source);
    }
    catch (...)
    {
        undo.cancelChunk();
        redoSelection();
        s.rerunning = false;
        // A replaced drag step is gone with the cancelled chunk.
        if (replace)
        {
            s.entries.pop_back();
            s.dragging = false;
        }
        moveTo(std::min(s.at, s.entries.size() - 1));
        notify(Change::Rerun);
        throw;
    }
    undo.closeChunk();
    s.rerunning = false;
    call.update(s.limited);
    Entry entry{undo.newestId(), call, components::mode(), madeNow()};
    if (replace)
        s.entries.back() = std::move(entry);
    else
    {
        s.entries.resize(s.at + 1);
        s.entries.push_back(std::move(entry));
    }
    s.dragging = drag;
    moveTo(s.entries.size() - 1);
    notify(Change::Rerun);
    return result;
}

void endDrag(bool keep)
{
    State& s = st();
    if (!std::exchange(s.dragging, false) || keep || !live() || s.at == 0)
        return;
    UndoStack& undo = UndoStack::get();
    s.rerunning = true;
    undo.undo();
    undo.flushFuture();
    s.rerunning = false;
    s.entries.pop_back();
    moveTo(s.entries.size() - 1);
    notify(Change::Rerun);
}

std::vector<const FlagDef*> editableFlags(const std::string& command)
{
    std::vector<const FlagDef*> out;
    const ops::options::Override* o = ops::options::findOverride(command);
    if (const CommandDef* def = ops::find(command))
        for (const FlagDef& f : def->flags)
            if (f.type == ArgType::Bool || f.type == ArgType::Int || f.type == ArgType::Float ||
                (f.type == ArgType::String && !f.choices.empty()) ||
                (f.type == ArgType::Vec3 && o && std::ranges::find(o->adjust, f.name) != o->adjust.end()))
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

std::string displayName(const std::string& command)
{
    std::string s = command.rfind("poly", 0) == 0 && command.size() > 4 ? command.substr(4) : command;
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (i > 0 && std::isupper(c))
            out += ' ';
        out += i == 0 ? char(std::toupper(c)) : char(c);
    }
    return out;
}

std::string modifierEquivalent(const std::string& command)
{
    static const std::map<std::string, std::string> kinds = {
        {"polyBevel", "bevel"},           {"polyExtrude", "extrude"},         {"polyMirror", "mirror"},
        {"polySubdivide", "subdivision"}, {"polyTriangulate", "triangulate"}};
    auto it = kinds.find(command);
    return it == kinds.end() ? std::string() : it->second;
}

void makeNonDestructive()
{
    if (!reachable())
        throw CommandError("makeNonDestructive: the last operation is no longer the latest undo step");
    Op op = *st().op;
    std::string kind = modifierEquivalent(op.command);
    if (kind.empty())
        throw CommandError("makeNonDestructive: " + op.command + " has no modifier equivalent");
    undoSelectionOnTop();
    const CommandDef* def = ops::find(op.command);
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
    // The modifier takes the selected faces or edges as addModifier creates it, once undo has put the selection back.
    else if (kind == "extrude")
        params = {{"thickness", value("thickness")}, {"offset", value("offset")}, {"individual", value("individual")}};

    // Back past every adjustment and the op's own run.
    st().rerunning = true;
    for (size_t i = 0; i <= st().at; ++i)
        UndoStack::get().undo();
    st().rerunning = false;
    components::setMode(op.modeBefore);
    std::vector<std::string> meshes = meshesOf(op);
    st().op.reset();
    st().entries.clear();
    notify(Change::Cleared);
    UndoChunk chunk("Make Non-Destructive");
    for (const std::string& mesh : meshes)
    {
        json call = {{"args", {mesh}}, {"kind", kind}};
        if (!params.empty())
            call["params"] = params;
        ops::execute("addModifier", call, CallSource::UI);
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
