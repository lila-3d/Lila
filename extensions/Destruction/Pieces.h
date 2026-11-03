#pragma once

#include "RigidWorld.h"

#include <lila/GeometryNodes.h>

namespace destruction
{
/** What pieces without density, friction or bounce attributes get. */
struct Material
{
    double density = 2400.0;
    double friction = 0.6;
    double bounce = 0.1;
};

/**
 * A simulation's scene from geometry: pieces (instances), links (two-point curves), colliders and blasts (points with
 * frame, speed and radius). shapes = false skips the hulls and collider meshes (steps after the first). Throws
 * std::invalid_argument when pieces aren't instances.
 */
SceneDesc describeGeometry(const lila::gn::GeometryPtr& pieces, const lila::gn::GeometryPtr& links, const lila::gn::GeometryPtr& colliders,
                           const lila::gn::GeometryPtr& blasts, const Settings& settings, const Material& material, bool shapes);

/** Registers Damage Pieces and Debris From Events. */
bool registerDamageNodes(lila::ext::Context& ctx);
} // namespace destruction
