#pragma once

/**
 * The pin node and its commands (Pin.cpp): a world frame riding a curve, a vertex, an edge, a face or a mesh's surface,
 * which drives a pin locator that objects and joints are constrained to. Several pins on one object blend like a
 * weighted parent constraint. bakePin turns pins into joints for Unreal.
 */
namespace lila::scene::pins
{
void registerTypes();
void registerCommands();
} // namespace lila::scene::pins
