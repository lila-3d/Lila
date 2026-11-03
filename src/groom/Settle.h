#pragma once

#include "core/Export.h"
#include "groom/Groom.h"

#include <vector>

/**
 * Settle: guides as rods (position based dynamics) with roots pinned on the scalp, stretch, bend and hold-shape
 * stiffness, gravity, and collisions with the scalp and between locks sized by their thickness. Runs to rest for the
 * Settle command and brush; stepped, so the same solver can drive animated hair.
 */
namespace lila::groom
{
struct SettleOptions
{
    /** Gravity in the curves' space, cm/s^2. */
    D3 gravity{0.0, -981.0, 0.0};
    int steps = 90;
    int iterations = 8;
    double dt = 1.0 / 60.0;
    /** Bend stiffness, 0 (rope) to 1 (stiff wire). */
    double bend = 0.5;
    /** Pull back toward the styled shape, carried with the root, 0 to 1. Keeps a style from collapsing. */
    double holdShape = 0.15;
    /** How firmly the first segment keeps its direction out of the scalp, 0 to 1. */
    double rootStiffness = 0.9;
    /** Velocity kept per step, 0 to 1 (1 - damping). */
    double damping = 0.08;
    /** Tangential slip kept on contact, 0 (sticks) to 1 (slides freely). */
    double slip = 0.25;
    /** Collisions between locks (the scalp always collides). */
    bool lockCollisions = true;
    /** Points at most this far apart, cm; long segments are subdivided for the solve. 0 picks from the radii. */
    double spacing = 0.0;
    /** Per curve, per point, 0 to 1: how much each point may move (the settle brush); empty for all. */
    std::vector<std::vector<double>> weights;
};

struct SettleStats
{
    int curves = 0;
    int contacts = 0;
    /** Largest move of a point in the last step, cm. */
    double lastMotion = 0.0;
    /** Deepest a point still sits inside the scalp or another lock, cm. */
    double maxPenetration = 0.0;
};

/** Settles the curves in place. radii: collision radius per point of each curve (collisionRadii in groom/Locks.h). */
LILA_SCENE_API void settle(Splines& curves, const std::vector<std::vector<double>>& radii, const Surface* scalp, const SettleOptions& o, SettleStats* stats = nullptr);
} // namespace lila::groom
