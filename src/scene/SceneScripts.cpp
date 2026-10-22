#include "scene/SceneScripts.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Log.h"

#include <algorithm>
#include <map>

namespace lila::scene::scripts
{
namespace
{
struct State
{
    std::vector<Script> scripts;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    std::function<void(const std::vector<Script>&)> openHandler;
};

State& state()
{
    static State s;
    return s;
}

void notify()
{
    auto listeners = state().listeners;
    for (auto& [id, fn] : listeners)
        fn();
}

/** Swaps the whole list, so undo and redo are the same operation. */
struct SwapAction final : UndoAction
{
    std::vector<Script> other;
    explicit SwapAction(std::vector<Script> before) : other(std::move(before)) {}
    void undo() override
    {
        std::swap(state().scripts, other);
        notify();
    }
    void redo() override { undo(); }
    size_t bytes() const override
    {
        size_t n = sizeof(*this);
        for (const Script& s : other)
            n += sizeof(Script) + s.name.capacity() + s.text.capacity();
        return n;
    }
};

void replaceUndoably(std::vector<Script> next)
{
    State& s = state();
    if (next == s.scripts)
        return;
    UndoStack::get().record(std::make_unique<SwapAction>(s.scripts));
    s.scripts = std::move(next);
    notify();
}

json toJson(const Script& s)
{
    json j = {{"name", s.name}, {"text", s.text}};
    if (s.runOnOpen)
        j["runOnOpen"] = true;
    return j;
}
} // namespace

const std::vector<Script>& all()
{
    return state().scripts;
}

const Script* find(const std::string& name)
{
    for (const Script& s : state().scripts)
        if (s.name == name)
            return &s;
    return nullptr;
}

void set(const Script& script)
{
    std::vector<Script> next = state().scripts;
    auto it = std::find_if(next.begin(), next.end(), [&](const Script& s) { return s.name == script.name; });
    if (it != next.end())
        *it = script;
    else
        next.push_back(script);
    replaceUndoably(std::move(next));
}

bool remove(const std::string& name)
{
    std::vector<Script> next = state().scripts;
    if (std::erase_if(next, [&](const Script& s) { return s.name == name; }) == 0)
        return false;
    replaceUndoably(std::move(next));
    return true;
}

void clear()
{
    if (state().scripts.empty())
        return;
    state().scripts.clear();
    notify();
}

json toJson()
{
    json out = json::array();
    for (const Script& s : state().scripts)
        out.push_back(toJson(s));
    return out;
}

void fromJson(const json& j)
{
    std::vector<Script> next;
    if (j.is_array())
        for (const json& e : j)
        {
            if (!e.is_object() || !e.contains("name") || !e["name"].is_string() || e["name"].get<std::string>().empty())
                continue;
            Script s;
            s.name = e["name"].get<std::string>();
            if (e.contains("text") && e["text"].is_string())
                s.text = e["text"].get<std::string>();
            s.runOnOpen = e.value("runOnOpen", false);
            if (std::none_of(next.begin(), next.end(), [&](const Script& o) { return o.name == s.name; }))
                next.push_back(std::move(s));
        }
    state().scripts = std::move(next);
    notify();
}

int addListener(std::function<void()> fn)
{
    State& s = state();
    s.listeners.emplace(s.nextListener, std::move(fn));
    return s.nextListener++;
}

void removeListener(int id)
{
    state().listeners.erase(id);
}

void setOpenHandler(std::function<void(const std::vector<Script>&)> fn)
{
    state().openHandler = std::move(fn);
}

void sceneOpened()
{
    std::vector<Script> pending;
    for (const Script& s : state().scripts)
        if (s.runOnOpen)
            pending.push_back(s);
    if (pending.empty())
        return;
    if (state().openHandler)
        state().openHandler(pending);
    else
        log::info("{} scene script(s) set to run on open were not run", pending.size());
}

void registerCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "sceneScript",
        .doc = "Create, edit, query or remove a Python script stored in the scene file. With no name, lists the scene's "
               "scripts. runOnOpen scripts are offered to run when the scene opens, after a confirmation. Returns the "
               "script as {name, text, runOnOpen}, or the list.",
        .positionalDoc = "name",
        .flags = {
            {"text", "t", T::String, nullptr, "The script's source."},
            {"runOnOpen", "roo", T::Bool, nullptr, "Offer to run it when the scene opens."},
            {"rename", "rn", T::String, nullptr, "A new name for the script."},
            {"remove", "rm", T::Bool, false, "Delete the script."},
            {"query", "q", T::Bool, false, ""},
        },
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
            {
                json list = json::array();
                for (const Script& s : all())
                    list.push_back(toJson(s));
                return list;
            }
            std::string name = a.positional[0].get<std::string>();
            if (name.empty())
                throw CommandError("sceneScript: the name is empty");
            const Script* existing = find(name);
            if (a.get<bool>("query"))
            {
                if (!existing)
                    throw CommandError("sceneScript: no scene script '" + name + "'");
                return toJson(*existing);
            }
            if (a.get<bool>("remove"))
            {
                if (!remove(name))
                    throw CommandError("sceneScript: no scene script '" + name + "'");
                return nullptr;
            }
            Script s = existing ? *existing : Script{name, {}, false};
            if (a.has("text"))
                s.text = a.get<std::string>("text");
            if (a.has("runOnOpen"))
                s.runOnOpen = a.get<bool>("runOnOpen");
            if (a.has("rename"))
            {
                std::string to = a.get<std::string>("rename");
                if (to.empty())
                    throw CommandError("sceneScript: the new name is empty");
                if (to != name && find(to))
                    throw CommandError("sceneScript: a scene script '" + to + "' exists already");
                s.name = to;
                if (existing)
                {
                    std::vector<Script> next = all();
                    for (Script& o : next)
                        if (o.name == name)
                            o = s;
                    replaceUndoably(std::move(next));
                    return toJson(s);
                }
            }
            set(s);
            return toJson(s);
        },
    });
}
} // namespace lila::scene::scripts
