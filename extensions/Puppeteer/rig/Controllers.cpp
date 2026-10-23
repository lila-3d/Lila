// What the Spinner does to IK and reverse foot controllers, as commands over base Lila's: swing the pole (optionally
// turning the foot or hand with it), roll/twist/bank within the foot's limits, and lock a key so the planted foot or
// hand holds until the chain's next key. Each one poses the joints' rotations.

#include "Puppeteer.h"
#include "rig/Rig.h"

using namespace lila;
using namespace lila::scene;

namespace puppeteer
{
namespace
{
json forwarded(const Args& a)
{
    json call = a.flags;
    call["args"] = objectArgs(a);
    return call;
}
} // namespace

void registerControllers(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerPole",
        .doc = "Swing IK chains' pole by degrees (ikPole): the knee or elbow turns around the line from hip to foot. turn "
               "also turns the reverse foot or IK controller with it, so the foot follows the knee.",
        .positionalDoc = "IK controllers, reverse feet or joints in IK chains (default the selection)",
        .flags = {
            {"by", "by", T::Float, 0.0, "Degrees added to the pole angle."},
            {"turn", "tu", T::Bool, false, "Turn the controller with the pole."},
        },
        .run = [](const Args& a) -> json { return ops::execute("ikPole", forwarded(a)); },
    });
    ctx.registerCommand({
        .name = "puppeteerFootChannels",
        .doc = "Roll, twist and bank reverse feet by degrees (or to them, with absolute), within each foot's limits "
               "(footChannels).",
        .positionalDoc = "reverse feet, their chains or ankles (default the selection)",
        .flags = {
            {"roll", "r", T::Float, nullptr, "Roll: positive onto the toe (heel up), negative onto the heel."},
            {"twist", "tw", T::Float, nullptr, "Twist around the ball."},
            {"bank", "b", T::Float, nullptr, "Bank: positive onto the outer edge."},
            {"absolute", "abs", T::Bool, false, "Set the values instead of adding to them."},
        },
        .run = [](const Args& a) -> json { return ops::execute("footChannels", forwarded(a)); },
    });
    ctx.registerCommand({
        .name = "puppeteerFootPivot",
        .doc = "Move one of reverse feet's pivots (heel, ball, toe, inner or outer edge) to a world position on the sole "
               "(footPivot).",
        .positionalDoc = "reverse feet, their chains or ankles (default the selection)",
        .flags = {
            {"pivot", "pv", T::String, nullptr, "Which pivot.", {"heel", "ball", "toe", "inner", "outer"}},
            {"position", "p", T::Vec3, nullptr, "World position; its height is dropped onto the sole."},
        },
        .run = [](const Args& a) -> json { return ops::execute("footPivot", forwarded(a)); },
    });
    ctx.registerCommand({
        .name = "puppeteerFootLock",
        .doc = "Lock (plant) or unlock the key at a time on IK and reverse foot controllers (ikLock): a locked key holds the "
               "controller, and the foot or hand with it, until the chain's next key. Toggles without state. Returns each "
               "chain's new state.",
        .positionalDoc = "IK controllers, reverse feet or joints in IK chains (default the selection)",
        .flags = {
            {"time", "t", T::Float, nullptr, "Seconds; default the current time."},
            {"state", "s", T::Bool, nullptr, "Locked or not; default the opposite of now."},
        },
        .run = [](const Args& a) -> json { return ops::execute("ikLock", forwarded(a)); },
    });
}
} // namespace puppeteer
