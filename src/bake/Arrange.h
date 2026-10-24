#pragma once

#include "core/Export.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>

namespace lila::scene
{
class Graph;
}

/**
 * The Arrange display (bakeArrange): a project's groups drawn side by side for inspection. Session state only: scene
 * meshes never move, the scene file never changes and bakes always run where the meshes are. The viewport draws and
 * picks each group's lows, scene highs and cage at its group's offset. Main thread.
 */
namespace lila::bake::arrange
{
/** Shows the project's groups at these offsets (by group node), replacing what it had. */
LILA_SCENE_API void set(scene::Graph& g, scene::NodeId project, const std::map<scene::NodeId, glm::dvec3>& offsets);
/** The project back where its meshes are. */
LILA_SCENE_API void clear(scene::NodeId project);
LILA_SCENE_API void clearAll();
LILA_SCENE_API bool active(scene::Graph& g, scene::NodeId project);
/** The group offsets an arranged project shows, empty when it isn't arranged. */
LILA_SCENE_API std::map<scene::NodeId, glm::dvec3> offsets(scene::Graph& g, scene::NodeId project);

/**
 * The view-only offset a node is drawn and picked at: its group's when it is a low, scene high or cage of a group in an
 * arranged project (the first such group), else zero.
 */
LILA_SCENE_API glm::dvec3 displayOffset(scene::Graph& g, scene::NodeId node);
/** world moved by displayOffset. */
LILA_SCENE_API glm::dmat4 displayWorld(scene::Graph& g, scene::NodeId node, const glm::dmat4& world);

/** Called whenever a project is arranged or cleared, so views redraw. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);
} // namespace lila::bake::arrange
