// Destruction's sound cue events: links breaking and pieces hitting, read from the simulation's cached frames.

#include "Destruction.h"
#include "PieceTransform.h"
#include "RigidState.h"

#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Media.h>
#include <lila/Node.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>

#include <cmath>
#include <span>
#include <vector>

using namespace lila;

namespace destruction
{
namespace
{
/** Speed lost between frames below this (cm/s) is solver jitter or resting contact, not a hit. */
constexpr double kMinImpact = 5.0;

glm::dvec3 soundBodyPosition(const BodyRecord& b)
{
    return glm::dvec3(b.position[0], b.position[1], b.position[2]) / kM;
}

glm::dvec3 soundBodyVelocity(const BodyRecord& b)
{
    return glm::dvec3(b.velocity[0], b.velocity[1], b.velocity[2]) / kM;
}

/**
 * One simulation's events over frames (f0, f1]: each link that broke, and each piece that lost speed between frames,
 * which is how a hit shows in per-frame state (falling only ever gains speed).
 */
void soundEventsOf(scene::Graph& g, scene::NodeId mesh, scene::NodeId modifier, int64_t f0, int64_t f1, double fps, std::vector<media::SimEvent>& out)
{
    const std::string object = g.node(mesh)->path();
    const glm::dmat4 world = g.node(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    // Link records follow the links' non-empty curves in order; each curve names the pieces it holds.
    std::vector<int> firstPiece;
    if (std::shared_ptr<const gn::SimInputs> in = gn::simulationInputs(modifier))
        if (gn::GeometryPtr links = in->geometry("links"); links && links->curves)
        {
            const gn::CurveSet& c = *links->curves;
            std::span<const int32_t> pa = c.curveAttrs.get<int32_t>("pieceA");
            for (size_t k = 0; k < c.numCurves(); ++k)
                if (c.curveSize(k) > 0)
                    firstPiece.push_back(k < pa.size() ? pa[k] : -1);
        }
    std::vector<uint8_t> prevBytes = gn::cachedSimulationState(g, modifier, f0);
    for (int64_t f = f0 + 1; f <= f1; ++f)
    {
        std::vector<uint8_t> curBytes = gn::cachedSimulationState(g, modifier, f);
        const StateView prev(prevBytes), cur(curBytes);
        if (prev.valid() && cur.valid() && prev.header().bodies == cur.header().bodies && prev.header().links == cur.header().links)
        {
            const double time = double(f) / fps;
            for (uint32_t k = 0; k < cur.header().links; ++k)
            {
                const LinkRecord now = cur.link(k);
                if (now.brokenStep < 0 || prev.link(k).brokenStep >= 0)
                    continue;
                media::SimEvent e;
                e.type = media::EventType::Break;
                e.time = time;
                e.object = object;
                e.strength = now.impulse;
                e.piece = k < firstPiece.size() ? firstPiece[k] : -1;
                if (e.piece >= 0 && uint32_t(e.piece) < cur.header().bodies)
                    e.position = glm::dvec3(world * glm::dvec4(soundBodyPosition(cur.body(uint32_t(e.piece))), 1.0));
                out.push_back(e);
            }
            for (uint32_t k = 0; k < cur.header().bodies; ++k)
            {
                const BodyRecord a = prev.body(k), b = cur.body(k);
                if (!a.active || !b.active)
                    continue;
                const double lost = glm::length(soundBodyVelocity(a)) - glm::length(soundBodyVelocity(b));
                if (lost < kMinImpact)
                    continue;
                media::SimEvent e;
                e.type = media::EventType::Contact;
                e.time = time;
                e.object = object;
                e.piece = int(k);
                e.strength = lost;
                e.position = glm::dvec3(world * glm::dvec4(soundBodyPosition(b), 1.0));
                out.push_back(e);
            }
        }
        // A frame missing from the cache leaves its events to the next cached one.
        if (cur.valid())
            prevBytes = std::move(curBytes);
    }
}

std::vector<media::SimEvent> soundEvents(scene::Graph& g, double t0, double t1)
{
    std::vector<media::SimEvent> out;
    const double fps = scene::sim::framesPerSecond();
    const int64_t f0 = int64_t(std::floor(t0 * fps + 1e-6)), f1 = int64_t(std::floor(t1 * fps + 1e-6));
    if (f1 <= f0)
        return out;
    for (scene::NodeId id : g.nodes())
        for (scene::NodeId m : scene::modifiers::stack(g, id))
            if (scene::Node* mod = g.node(m); mod && scene::sim::isSimulation(*mod) && gn::simulationInputs(m))
                soundEventsOf(g, id, m, f0, f1, fps, out);
    return out;
}
} // namespace

void registerSounds(ext::Context&)
{
    media::eventSources().put("destruction", {"destruction", soundEvents});
}
} // namespace destruction
