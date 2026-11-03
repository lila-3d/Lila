#pragma once

#include "groom/Groom.h"

#include <memory>

namespace lila::scene
{
class Graph;
class Node;
} // namespace lila::scene

/** Graph edits the groom commands share. Internal to src/groom. */
namespace lila::groom
{
/** The guides' scalp in the guides' object space, or null. */
std::unique_ptr<Surface> scalpOf(scene::Graph& g, scene::Node& guides);
/** The guides' own splines, object space. */
Splines guideSplines(scene::Graph& g, scene::Node& guides);
/** Stores splines as the guides' own, dropping any history feeding them. */
void storeGuides(scene::Graph& g, scene::Node& guides, Splines s);
/** Wires a style node in at position (-1 the end) of the groom's stack. */
void insertStyle(scene::Graph& g, scene::Node& groom, scene::Node& style, int position);
/** Takes a style node out of the stack and deletes it. */
void removeFromStack(scene::Graph& g, scene::Node& groom, scene::Node& style);
} // namespace lila::groom
