#pragma once

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"

#include <exception>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace lila::ui
{
/** Runs a command as the UI (echoed to the script editor). A failure is logged and returns null. */
inline json runUi(std::string_view command, const json& call = json::object())
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

/** runUi for callers that need to tell failure from a null result. The message goes to `error` too when given. */
inline bool tryRunUi(std::string_view command, const json& call, json* result = nullptr, std::string* error = nullptr)
{
    try
    {
        json r = ops::execute(command, call, CallSource::UI);
        if (result)
            *result = std::move(r);
        return true;
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        if (error)
            *error = e.what();
        return false;
    }
}

/** runUi with the command's remembered option box values filled in under `call`. */
inline void runUiWithOptions(std::string_view command, const json& call = json::object())
{
    runUi(command, ops::options::callWith(command, call));
}

/** A call with only positional arguments. */
inline json argsCall(std::initializer_list<json> args) { return {{"args", json(args)}}; }

/** Paths of the selected nodes, in selection order. */
inline json selectionNames()
{
    scene::Graph& g = scene::Graph::current();
    json out = json::array();
    for (scene::NodeId id : g.selection())
        out.push_back(g.node(id)->path());
    return out;
}

/** The node's path, or empty when it is gone. */
inline std::string nameOf(scene::NodeId id)
{
    scene::Node* n = scene::Graph::current().node(id);
    return n ? n->path() : std::string();
}
} // namespace lila::ui
