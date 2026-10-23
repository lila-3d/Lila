#pragma once

// The Spinner: Puppeteer's radial gizmo, and the breakdown sampling its slider shares with the puppeteerBreakdown
// command.

#include "Common.h"

#include <map>
#include <string>
#include <vector>

namespace puppeteer
{
/** One node's keyed channels at the previous key, now and the next key, for the breakdown slider. */
struct BreakdownSample
{
    NodeId id = lila::scene::kNoNode;
    std::string name;
    std::vector<std::pair<int, std::string>> channels; // Transform attr, "translate" / "rotate" / "scale"
    double previousTime = 0.0, nextTime = 0.0;
    std::map<int, glm::dvec3> previous, current, next;
};

/** The nodes the breakdown works on: the given transforms, or with all, every joint of their characters. */
std::vector<NodeId> breakdownNodes(Graph& g, const std::vector<std::string>& names, bool all);
/** Samples the nodes that have keys around `now`. Moves the graph's time and puts it back. */
std::vector<BreakdownSample> sampleBreakdown(Graph& g, const std::vector<NodeId>& nodes, double now);
/** The channel's value at the slider's weight (-1 previous key, 0 now, 1 next key); rotations go by slerp. */
glm::dvec3 breakdownValue(Graph& g, const BreakdownSample& s, int attr, double weight);
} // namespace puppeteer
