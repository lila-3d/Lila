#pragma once

#include "core/Export.h"

#include "gn/Group.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

/**
 * Zones: a pair of nodes on the canvas (Repeat Input and Repeat Output, ...) with the nodes between them as the body,
 * drawn as one region. The input node names its output in its "output" value; the items that travel through the zone
 * are a list on the output node ([{name, type}]).
 *
 * Evaluation never runs zone nodes. compile() first lowers each zone, innermost first, into a subgroup holding its body
 * and a node that runs that subgroup (Repeat, For Each Element, a simulation or a closure). Values the body reads from
 * outside the zone become extra inputs of the subgroup.
 */
namespace lila::gn
{
struct ZoneKind
{
    const char* input;
    const char* output;
    /** The node the zone is lowered to. */
    const char* runner;
    const char* label;
    /** Value keys on the output node holding the items the body receives and the items it gives back. */
    const char* inputItems = "items";
    const char* outputItems = "items";
    /** Whether a geometry lane runs through the zone ahead of the items. */
    bool geometry = true;
    /** Values the zone input hands its body besides the items, read by the runner: (name, type). */
    std::vector<std::pair<std::string, SocketType>> driven;
};

LILA_SCENE_API const std::vector<ZoneKind>& zoneKinds();
LILA_SCENE_API const ZoneKind* zoneOfInput(std::string_view type);
LILA_SCENE_API const ZoneKind* zoneOfOutput(std::string_view type);

/** A list of items a node carries as a value: [{name, type}]. */
struct ZoneItem
{
    std::string name;
    SocketType type = SocketType::Float;
};
/** The items in n.values[key]; malformed entries are skipped. */
LILA_SCENE_API std::vector<ZoneItem> itemsOf(const GNode& n, std::string_view key = "items");
LILA_SCENE_API json itemsToJson(const std::vector<ZoneItem>& items);
/** Why an item list can't be used (a bad or repeated name, one reserved by the node), or empty. */
LILA_SCENE_API std::string itemsProblem(const std::vector<ZoneItem>& items, const std::vector<std::string>& reserved = {});
/** Socket names an item can't take on a node of this type (its own sockets). */
LILA_SCENE_API std::vector<std::string> reservedItemNames(std::string_view type);
/** Node types that carry item lists, with the value keys holding them. */
LILA_SCENE_API std::vector<std::string> itemListsOf(std::string_view type);

/** The output node paired with a zone input, or null. */
LILA_SCENE_API const GNode* zoneOutputOf(const Group& g, const GNode& input);
/** The zone input paired with an output node, or null. */
LILA_SCENE_API const GNode* zoneInputOf(const Group& g, const GNode& output);

/** names plus the other half of each zone they hold one half of, so copies keep zones whole. */
LILA_SCENE_API std::vector<std::string> withZonePartners(const Group& g, std::vector<std::string> names);
/** A copied zone input pairs with its output's copy (renamed maps old names to new), or with nothing when that wasn't copied. */
LILA_SCENE_API void retargetZoneCopy(GNode& copy, const std::map<std::string, std::string>& renamed);
/** Values a node keeps that are neither sockets nor properties: a zone input's pairing and item lists. */
LILA_SCENE_API bool isStructuralValue(std::string_view type, std::string_view key);

/** A zone as the canvas draws it. */
struct ZoneRegion
{
    std::string input, output;
    const ZoneKind* kind = nullptr;
    /** Nodes between the two: downstream of the input and upstream of the output. */
    std::vector<std::string> body;
};
LILA_SCENE_API std::vector<ZoneRegion> zoneRegions(const Group& g);

/** Lowers every zone into a subgroup and its runner (see above). Returns an error, or empty when it worked. */
LILA_SCENE_API std::string lowerZones(Group& g);
} // namespace lila::gn
