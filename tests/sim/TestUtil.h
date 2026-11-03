#pragma once

#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"

#include <string>

namespace simtest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
inline json v3(double x, double y, double z) { return json::array({x, y, z}); }

/** Keys of every curve on a layer, as text, in member order. */
inline std::string layerKeys(const std::string& layer)
{
    scene::Graph& g = scene::Graph::current();
    const scene::NodeId l = g.find(layer)->id();
    std::string out;
    for (const scene::PlugRef& m : scene::anim::layerMembers(g, l))
    {
        const scene::NodeId b = scene::anim::blendFor(g, m, l);
        for (int k = -1; k < 3; ++k)
            if (auto s = g.source({b, scene::anim::BlendNode::kInputB, k}))
            {
                const scene::Node& c = *g.node(s->node);
                out += g.plugName(m) + ":" + c.as<std::string>(c.type().attrIndex("keys")) + "\n";
            }
    }
    return out;
}
} // namespace simtest
