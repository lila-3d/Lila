// The agent guide lila://guide/contacts: recipes for covering where things meet the ground.

#include "Seamwork.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Mcp.h>
#include <lila/SpecSteps.h>

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
- In a terrain spec, `seams` does steps 1 and 2 once the terrain is built: `{'objects': ['Wall_N', 'Rock_01'], 'skirt': {'width': 40}, 'decal': True, 'scatter': {'preset': 'rubble'}}` (or `True` for every visible mesh, no cover).
- Mobile forward safe by default: strips are meshes, the skirt samples both materials itself, no GBuffer reads. `seamDecal(decals='dbuffer')` is desktop only; `decals='actor'` needs ForwardLevelUp clustered decals.
)";
using nlohmann::json;

/** The terrain spec's "seams" key: seamFind against the built terrain, then the covers it names. */
json runSpecSeams(const std::string& terrain, const json& value)
{
    json flags = value.is_object() ? value : json::object();
    const json objects = flags.value("objects", json::array());
    json cover = json::object();
    for (const char* k : {"skirt", "decal", "scatter"})
        if (flags.contains(k))
        {
            cover[k] = flags[k];
            flags.erase(k);
        }
    flags.erase("objects");
    flags["args"] = objects;
    flags["terrain"] = terrain;
    json out = lila::ops::execute("seamFind", flags);
    const std::string seam = out.value("seam", std::string());
    const std::pair<const char*, const char*> commands[] = {{"skirt", "seamSkirt"}, {"decal", "seamDecal"}, {"scatter", "seamScatter"}};
    for (const auto& [key, command] : commands)
    {
        if (!cover.contains(key) || (cover[key].is_boolean() && !cover[key].get<bool>()))
            continue;
        json call = cover[key].is_object() ? cover[key] : json::object();
        call["args"] = json::array({seam});
        out[key] = lila::ops::execute(command, call);
    }
    return out;
}

const json& specSchema()
{
    static const json s = {
        {"type", {"object", "boolean"}},
        {"description", "Contacts after the terrain is built (Seamwork): seamFind flags ({objects: meshes, default every visible one; gap, "
                        "crease, ...}) plus skirt, decal and scatter (true, or seamSkirt / seamDecal / seamScatter flags) to cover them; "
                        "or true to find every visible mesh's contacts. Needs the Seamwork extension."}};
    return s;
}
} // namespace

void registerAgent(lila::ext::Context& ctx)
{
    ctx.add(lila::specs::steps(), "specStep", "terrain.seams",
            lila::specs::Step{.spec = "terrain", .key = "seams", .schema = specSchema(), .run = runSpecSeams});
    lila::mcp::Server::get().addResource(ctx, {.uri = "lila://guide/contacts",
                                               .name = "Contacts guide",
                                               .description = "Covering where walls, rocks and props meet the ground: find contacts, scatter debris, "
                                                              "strips, skirts, materials, check and ship. Read before covering building bases.",
                                               .read = [] { return std::string(kGuide); }});
}
} // namespace seamwork
