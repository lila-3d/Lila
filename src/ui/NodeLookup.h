#pragma once

#include "scene/Graph.h"

#include <exception>
#include <string_view>

namespace lila::ui
{
/** Graph::find for UI handlers: null when the path is ambiguous, so the CommandError never reaches Qt. */
inline scene::Node* findNode(std::string_view path)
{
    try
    {
        return scene::Graph::current().find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}
} // namespace lila::ui
