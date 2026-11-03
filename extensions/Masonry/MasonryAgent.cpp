// What agents read about masonry over MCP: the guide, the building spec schema and the make_ruin prompt.

#include "Masonry.h"
#include "MasonryBuilding.h"

#include <lila/Mcp.h>

namespace masonry
{
namespace
{
const char* kGuide = R"(# Masonry guide

Masonry builds walls and whole buildings stone by stone, ruins and ages them, and ships them to Unreal. Lengths are cm, Y is up. Every command returns stats; read them instead of capturing the viewport.

## Wall from a line
- `masonryWall(points=[[x, y, z], ...], closed=False, height=, thickness=, bond=)`: each point's y is the ground there, so the wall follows terrain and its courses step over slopes. `heights=[...]` (one per point) slopes or breaks the top. `ground='Terrain'` drops the footprint onto a mesh.
- `batter=` leans both faces in (degrees). `bands='[300, 600]'` adds string courses, `cornice=2` corbels the top courses out.
- `openings=[{'at': cm along the points, 'width', 'height', 'sill', 'arch': lintel|round|pointed|segmental|flat, 'kind': door|window|niche, 'name'}]`. Arches lay voussoirs and a keystone, lintels and sills are single stones, the coursing stops cleanly at the jambs.
- Change any setting later with `setAttr('<modifier>.<flag>', value)`.

## Walls over a blockout
- `masonryFromMesh('Blockout', bond='ashlar')`: the blockout is sliced into bands and walls laid on each section's centre line; a hollow blockout gives its own wall thickness. Bonds: running, stack, english, flemish, header, gardenWall, herringbone, ashlar, rubble, dryStone. `chips` knocks chips out of stone edges, more on old and damaged stones.

## Building from a spec
- `masonryBuilding(spec={...})`: walls (with openings, buttresses, bands, cornices), columns, colonnades, stairs, barrel vaults, domes, freestanding arches and floors. Read lila://masonry/spec-schema for every key.
- Every part is a named region: `north_wall`, `north_wall/door_1`, `portico/column_3`. masonryInfo lists them with stone counts.
- Edit and rebuild: `s = masonrySpec('Temple')`, change `s['spec']`, `masonryBuilding(node='Temple', spec=s['spec'])`.
- Scale: `'scale': 8` multiplies every length, stones included. For creatures far larger than people, scale the stones with the building: stones a metre or more, courses a metre high, doors tens of metres. A building at human proportions with only the walls scaled reads as human-sized.

## Ruin
- `ruin('Wall', left=0.5)`: left is the share of stone left standing (0.75, 0.5, 0.25). `collapse=` (degrees) is the side it fell toward, `slope=` how much lower it is there, `gaps=` breaches, `survivors=`, `loose=`, `tip=`, `rubble=` share of fallen stones heaped at the foot, `settle=True` drops them with the rigid-body solver.
- `ruin('Temple', regions=['north_wall'], left=0.66)` ruins only that region; the rest stands and holds it up. Floors break into pieces; pieces with nothing under them fall.
- On objects that are not masonry (stones placed by hand), ruin moves and deletes them in place.
- `ruinPaint('Temple', points=[[x, y, z], ...], radius=120)` paints damage along a stroke (strength=-1 holds stones up); the Ruin Brush does the same in the viewport. `structural=True` also drops what cannot stand under the rigid-body solver, such as an arch without its keystone.
- `masonryWedge('Temple', count=9, vary={'left': [0.75, 0.5, 0.25]})` builds variants side by side into a contact sheet PNG; `apply=k` keeps variant k.
- `makeDestructible('Temple', strength=30, impact=[x, y, z], blast=1500)` hands the stones to the Destruction solver, linked where they touch.

## Age
- `age('Wall', age=0.7, moss=0.6, plaster=0.5, ivy=0.6)`: per stone age, wet and moss for the material; damage comes from the ruin. plaster leaves a peeling render coat, ivy climbs from the ground.
- Floors take `surface`: cobbles, flags or tiles (a pitched roof); vaults take kind groin.

## Check and ship
- `masonryCheck('Wall')` -> ok, issues (floating stones, interpenetration, missing collision, budget). Fix and re-check before shipping.
- `masonryShip('Wall', wait=True)`: HISM stones per chunk (default), `mode='merged'`, `mode='lowPoly'` (shells following the walls as they stand), or `mode='assembly'` (UE 5.8 Nanite Assembly, opt-in). Nanite is never on by default.
- Instances carry custom data random, height, age, damage, wet, moss; the generated M_LilaMasonry (on MF_LilaMasonry) reads them.
- Python: `import lila.masonry as masonry` wraps all of this with typed parameters and dataclass results.

## Recipes
- Ruined chapel: masonryBuilding with closed nave walls (pointed windows on the long sides, a door in the west wall, buttresses), a floor as the roof, then `ruin(left=0.5, collapse=0, gaps=1)`, `age(age=0.8)`, `masonryCheck`, `masonryShip`.
- Field wall on a hill: `masonryWall(points=..., ground='Terrain', bond='running', stoneLength=40, stoneHeight=20, thickness=50)`, then `ruin(left=0.75, rubble=0.8)`.
)";

const char* kPrompt = R"(Build {subject} in Lila with the Masonry tools, then ruin, age, check and ship it.

Style: {style}. Condition: {condition}. Budget: {budget}.

1. Read lila://guide/masonry and lila://masonry/spec-schema.
2. Write one masonryBuilding spec: name every wall and opening the way you would talk about them (north_wall, west_door). Pick stone sizes and scale to suit the subject's size; for very large builders scale the stones too.
3. Build it, read the stats (stones, regions, triangles) and adjust the spec until the shape is right.
4. Ruin it to the condition: left (0.75, 0.5, 0.25), collapse direction, gaps, regions for parts that fell further.
5. Age it, then masonryCheck until ok is true and the stats fit the budget.
6. masonryShip it with wait=True and report what Unreal placed.
)";
} // namespace

void registerAgentDocs(lila::ext::Context& ctx)
{
    using lila::mcp::Server;
    Server& s = Server::get();
    s.addResource(ctx, {.uri = "lila://guide/masonry",
                   .name = "Masonry guide",
                   .description = "Walls and buildings stone by stone: wall from a line, building from a spec, regions, ruin, age, "
                                  "check and ship to Unreal. Read before building masonry.",
                   .read = [] { return std::string(kGuide); }});
    s.addResource(ctx, {.uri = "lila://masonry/spec-schema",
                   .name = "Masonry building spec",
                   .description = "Every key of the masonryBuilding spec with its type, default and meaning.",
                   .mimeType = "application/json",
                   .read = [] { return specSchema().dump(2); }});
    s.addPrompt(ctx, {.name = "make_ruin",
                 .description = "Build a masonry building or ruin from one description: spec, ruin, age, check, ship to Unreal.",
                 .arguments = nlohmann::json::array({
                     {{"name", "subject"}, {"description", "What to build, e.g. \"a ruined Gothic chapel with a bell tower\""}, {"required", true}},
                     {{"name", "style"}, {"description", "Masonry style, e.g. \"ashlar, pointed arches\""}, {"required", false}},
                     {{"name", "condition"}, {"description", "How ruined, e.g. \"half the roof gone, north wall down to a third\""}, {"required", false}},
                     {{"name", "budget"}, {"description", "Stones or triangles, e.g. \"50000 stones\""}, {"required", false}},
                 }),
                 .get = [](const nlohmann::json& args) {
                     std::string text = kPrompt;
                     const std::pair<const char*, const char*> keys[] = {
                         {"subject", "the requested building"}, {"style", "ashlar"}, {"condition", "half ruined"}, {"budget", "100000 stones"}};
                     for (const auto& [key, fallback] : keys)
                     {
                         const std::string value = args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : fallback;
                         const std::string tag = std::string("{") + key + "}";
                         for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
                             text.replace(at, tag.size(), value);
                     }
                     return text;
                 }});
}
} // namespace masonry
