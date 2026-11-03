#pragma once

#include "MasonryBuilding.h"
#include "MasonryCore.h"
#include "MasonryRuin.h"

#include <lila/GeometryNodes.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace masonry
{
/** One setting of the masonry nodes: a node input, a command flag and an exposed group input, all by the same name. */
struct Input
{
    enum Type : uint8_t
    {
        Float,
        Int,
        String,
        Bool,
    };
    const char* name;
    Type type;
    double def;          // Float, Int, Bool
    const char* text;    // String default
    double min, max;     // hard limits, max <= min for none
    double softMax;      // slider end in the attribute editor, <= min for none
    const char* panel;
    const char* doc;
};

/** Which inputs a node or command takes. */
enum class InputSet : uint8_t
{
    Wall,     // wall shape, stones, look, ruin, age
    Building, // spec, look, ruin, age
    Ruin,     // ruin only
    Age,      // age only
};
std::vector<const Input*> inputsOf(InputSet set);
const Input* findInput(std::string_view name);

/** A value as the input's type (numbers rounded for ints, clamped to limits); the input's default for null. */
nlohmann::json typedValue(const Input& in, const nlohmann::json& v);

/** Footprints in a geometry: its curves (evaluated), then its mesh's loose edges. */
std::vector<Footprint> footprintsOf(const lila::gn::Geometry& g);

/** Build options, ruin and age from named values (typed by typedValue). Throws std::runtime_error for a bad bond or opening list. */
BuildOptions optionsFromValues(const nlohmann::json& v);
RuinParams ruinFromValues(const nlohmann::json& v);
AgeParams ageFromValues(const nlohmann::json& v);

/** Openings from a JSON list (the masonryWall openings flag and input), lengths cm. Throws std::runtime_error. */
std::vector<Opening> openingsFromJson(const nlohmann::json& list);

/** A finished masonry build and what its ruin did. */
struct Result
{
    Built built;
    RuinStats ruin;
};

/**
 * Builds walls (footprints and values) or a building (values["spec"]), ruined and aged per the values, shared through a
 * small cache keyed by every input, so a command and the node evaluating the same wall build it once.
 */
std::shared_ptr<const Result> buildMasonry(std::span<const Footprint> footprints, const nlohmann::json& values, bool lods);

/** The per-stone instance attributes the masonry nodes write. */
void writeStoneAttrs(geo::AttributeSet& attrs, const Built& b);

/** Stones read back from instances: transforms and the bounds of what they reference. */
std::vector<Stone> stonesFromInstances(const lila::gn::Instances& inst, std::vector<glm::dvec3>* offset);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** The guide, spec schema and make_ruin prompt over MCP, owned by ctx. */
void registerAgentDocs(lila::ext::Context& ctx);
} // namespace masonry
