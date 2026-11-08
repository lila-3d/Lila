// The agent guide lila://guide/contacts: recipes for covering where things meet the ground.

#include "Seamwork.h"

#include <lila/Extension.h>
#include <lila/Mcp.h>

namespace seamwork
{
namespace
{
constexpr const char* kGuide = R"(# Contacts in Lila, for agents

Contacts are where meshes meet the ground and each other. UI says "contacts"; commands are seam*. Lengths are cm.

## Recipe: cover building bases

1. `r = seamFind(['Wall_N', 'Wall_S', 'Rock_01'], terrain='Terrain')` (no names: every visible mesh). Read `r['curves']`, `r['length']`, `r['pairs']`. A `warning` names what was read differently (a terrain read as a mesh).
2. Pick the cover by renderer and budget, any mix:
   - Debris: `seamScatter(r['seam'], preset='rubble', settle=True)`; presets rubble, gravel, grassTufts, mossClumps, roots, snowDrift, leafLitter; or `items=['Rock_A']`. Check `dressCheck.ok` in the result.
   - Grime strips: `seamDecal(r['seam'], side='both', width=30)` (meshes, every renderer).
   - Blend mesh: `seamSkirt(r['seam'], width=40)` (one skirt per object; result lists the method per contact and why).
   - Material only: the field modifiers seamFind added already carry the contact weight (vertex colour R by default) for MF_LilaContactMask; the terrain got a `contact` mask: `terrainMaterialRules(t, set={'name': 'dirt', 'when': {'contact': [0.3, 1]}})`.
3. `seamCheck()` until `ok`: `floating` names an object with nothing under it (its gap), `open` a contact nothing covers.
4. Ship: `seamSend(r['seam'])` (materials, tagged splines, PCG data with LilaLinkPCG, blend ids) or `seamExport(r['seam'], file='contacts.json')` for other engines.

## Notes

- Everything stays live: move a wall and its contacts, strips and skirt follow; batch scripts call `seamUpdate()` after moves to refresh fields and the terrain mask.
- `seamTag(names, ignore=True)` leaves intended overlaps out; `ground=True` makes a floor slab act as ground; pieces sharing `group=n` never join each other. Run seamFind again after tagging.
- `seamInfo(seam, points=True)` gives every contact point's angle (90 = wall on flat ground), depth, gap, upFacing, exposure and terrain layer.
- Mobile forward safe by default: strips are meshes, the skirt samples both materials itself, no GBuffer reads. `seamDecal(decals='dbuffer')` is desktop only; `decals='actor'` needs ForwardLevelUp clustered decals.
)";
} // namespace

void registerAgent(lila::ext::Context& ctx)
{
    lila::mcp::Server::get().addResource(ctx, {.uri = "lila://guide/contacts",
                                               .name = "Contacts guide",
                                               .description = "Covering where walls, rocks and props meet the ground: find contacts, scatter debris, "
                                                              "strips, skirts, materials, check and ship. Read before covering building bases.",
                                               .read = [] { return std::string(kGuide); }});
}
} // namespace seamwork
