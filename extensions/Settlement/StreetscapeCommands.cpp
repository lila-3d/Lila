// settlementNames names a settlement's streets and districts.

#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Profile.h>

#include <algorithm>
#include <map>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using T = ArgType;

std::string settlementOf(const Args& a, const char* cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one settlement");
    return names[0];
}

json namesJson(const Layout& l)
{
    std::map<uint64_t, json> streets;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
        {
            const StreetEdge& x = l.graph.edges[e];
            if (!streets.contains(x.street))
                streets[x.street] = {{"id", x.street}, {"name", streetName(l, x.street)}, {"class", className(x.cls)}};
        }
    json s = json::array(), d = json::array();
    for (auto& [id, j] : streets)
        s.push_back(std::move(j));
    for (const District& x : l.districts)
        d.push_back({{"id", x.id}, {"name", x.name}, {"areaM2", x.area}, {"center", json::array({x.center.x, x.center.y})}});
    return {{"culture", l.culture}, {"cultures", cultureNames()}, {"streets", s}, {"districts", d}};
}
} // namespace

void registerStreetscapeCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "settlementNames",
        .doc = "Street and district names. Every street through its junctions has one name from the naming culture "
               "(oldEnglish, latin, french, spanish, usGrid with numbered streets and avenues, sector codes), stable by the "
               "ids of the streets in it; districts are the quarters between arterials. culture= sets the settlement's "
               "culture (default from its style); district= with name= renames a district. Street names change with "
               "settlementEditStreet(name=). Names save with the scene and go out in settlementExport. Returns the culture, "
               "every street's name by stable id and the districts.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"culture", "cu", T::String, nullptr, "Naming culture; empty for the style's.", {"oldEnglish", "latin", "french", "spanish", "usGrid", "sector", ""}},
                  {"district", "d", T::Int, nullptr, "A district's id, with name=."},
                  {"name", "n", T::String, nullptr, "The district's new name; empty gives it back to the culture."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementNames";
            Graph& g = Graph::current();
            const std::string target = settlementOf(a, cmd);
            Rebuilt r = rebuildSettlement(g, target, cmd);
            if (a.has("culture") || a.has("district"))
            {
                const json raw = ops::execute("getAttr", {{"args", {r.modifier + ".edits"}}});
                json edits;
                try
                {
                    edits = parseEdits(raw.is_string() ? raw.get<std::string>() : std::string());
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string(cmd) + ": " + e.what());
                }
                json& naming = edits["naming"];
                if (!naming.is_object())
                    naming = json::object();
                if (a.has("culture"))
                {
                    const std::string c = a.get<std::string>("culture");
                    const std::vector<std::string> known = cultureNames();
                    if (!c.empty() && std::ranges::find(known, c) == known.end())
                        throw CommandError(std::string(cmd) + ": unknown culture '" + c + "'");
                    naming["culture"] = c;
                }
                if (a.has("district"))
                {
                    if (!a.has("name"))
                        throw CommandError(std::string(cmd) + ": district= needs name=");
                    const uint64_t id = uint64_t(a.get<int64_t>("district"));
                    if (std::ranges::find_if(r.layout.districts, [&](const District& d) { return d.id == id; }) == r.layout.districts.end())
                        throw CommandError(std::string(cmd) + ": no district with id " + std::to_string(id));
                    json& districts = naming["districts"];
                    if (!districts.is_object())
                        districts = json::object();
                    if (a.get<std::string>("name").empty())
                        districts.erase(std::to_string(id));
                    else
                        districts[std::to_string(id)] = a.get<std::string>("name");
                }
                ops::execute("setAttr", {{"args", {r.modifier + ".edits", edits.dump()}}});
                r = rebuildSettlement(g, target, cmd);
            }
            return namesJson(r.layout);
        },
        .examples = {"settlementNames('Settlement', culture='french')['streets'][0]['name']",
                     "d = settlementNames('Settlement')['districts'][0]['id']\nsettlementNames('Settlement', district=d, name='Old Town')"},
    });

}
} // namespace settlement
