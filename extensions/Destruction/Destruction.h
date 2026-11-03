#pragma once

#include <string>

namespace lila::ext
{
class Context;
}

namespace destruction
{
/** The defaults breakApart starts a material from. */
struct Preset
{
    std::string material;
    int pieces = 30;
    double roughness = 1.0;    // cm
    double strength = 20.0;    // N per cm2 of shared area
    double blast = 800.0;      // cm/s
    double density = 2400.0;   // kg/m3
    double friction = 0.6;
    double bounce = 0.1;
    double impactFocus = 0.5;  // how much smaller the pieces get around the impact
};

/** Throws CommandError for an unknown material. */
const Preset& preset(const std::string& material);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
/** freezeDestruction: pieces at rest to static meshes with collision. */
void registerFreeze(lila::ext::Context& ctx);
/** exportChaos: pieces and motion for a Chaos Geometry Collection and cache in Unreal. */
void registerChaosExport(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** Breaks and hits as sound cue events (media::eventSources). */
void registerSounds(lila::ext::Context& ctx);
} // namespace destruction
