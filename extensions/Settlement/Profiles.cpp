// Settlement: the built-in styles and their JSON form.

#include "SettlementCore.h"

#include <stdexcept>

namespace settlement
{
namespace
{
struct DoubleField
{
    const char* name;
    double Profile::*member;
};
struct IntField
{
    const char* name;
    int Profile::*member;
};

constexpr DoubleField kDoubles[] = {
    {"widthArterial", &Profile::widthArterial}, {"widthCollector", &Profile::widthCollector},
    {"widthLocal", &Profile::widthLocal},       {"widthAlley", &Profile::widthAlley},
    {"segment", &Profile::segment},             {"turn", &Profile::turn},
    {"minAngle", &Profile::minAngle},           {"spacing", &Profile::spacing},
    {"maxGrade", &Profile::maxGrade},           {"density", &Profile::density},
    {"falloff", &Profile::falloff},             {"maxBlock", &Profile::maxBlock},
    {"gridAngle", &Profile::gridAngle},         {"gridWidth", &Profile::gridWidth},
    {"gridDepth", &Profile::gridDepth},         {"coreRadius", &Profile::coreRadius},
    {"lotWidth", &Profile::lotWidth},           {"lotDepth", &Profile::lotDepth},
    {"minLot", &Profile::minLot},               {"frontSetback", &Profile::frontSetback},
    {"sideSetback", &Profile::sideSetback},     {"buildDepth", &Profile::buildDepth},
    {"floorHeight", &Profile::floorHeight},     {"foundation", &Profile::foundation},
    {"commercial", &Profile::commercial},
};

constexpr IntField kInts[] = {
    {"gates", &Profile::gates}, {"arterialEvery", &Profile::arterialEvery}, {"rings", &Profile::rings},
    {"spokes", &Profile::spokes}, {"minFloors", &Profile::minFloors},       {"maxFloors", &Profile::maxFloors},
};

/** Hamlets and villages: a few lanes, wide plots, detached houses. */
Profile hamlet()
{
    Profile p;
    p.name = "hamlet";
    p.pattern = Pattern::Organic;
    p.widthArterial = 600.0, p.widthCollector = 450.0, p.widthLocal = 350.0, p.widthAlley = 250.0;
    p.gates = 3, p.segment = 4000.0, p.turn = 22.0, p.spacing = 4000.0;
    p.density = 0.35, p.falloff = 2.2, p.maxBlock = 40000.0, p.plaza = false;
    p.lotWidth = 2200.0, p.lotDepth = 4500.0, p.minLot = 150.0;
    p.frontSetback = 400.0, p.sideSetback = 400.0, p.buildDepth = 1200.0;
    p.minFloors = 1, p.maxFloors = 2, p.floorHeight = 300.0, p.roof = "gable", p.commercial = 0.1;
    return p;
}

/** A medieval market town: streets converging on the market, narrow deep burgage plots, row houses. */
Profile medievalTown() { return Profile{}; }

/** A street village strung along one road. */
Profile streetVillage()
{
    Profile p = hamlet();
    p.name = "streetVillage";
    p.pattern = Pattern::Linear;
    p.gates = 2, p.density = 0.25;
    p.lotWidth = 1500.0, p.lotDepth = 6000.0, p.frontSetback = 200.0, p.sideSetback = 250.0;
    return p;
}

/** A small US town: a survey grid, alleys behind, detached houses set back from the street. */
Profile usTown()
{
    Profile p;
    p.name = "usTown";
    p.pattern = Pattern::Grid;
    p.widthArterial = 2400.0, p.widthCollector = 1800.0, p.widthLocal = 1500.0, p.widthAlley = 500.0;
    p.gridWidth = 9000.0, p.gridDepth = 20000.0, p.arterialEvery = 6, p.maxBlock = 25000.0;
    p.plaza = false, p.maxGrade = 0.08;
    p.lotWidth = 1500.0, p.lotDepth = 4000.0, p.minLot = 200.0;
    p.frontSetback = 600.0, p.sideSetback = 150.0, p.buildDepth = 1600.0;
    p.minFloors = 1, p.maxFloors = 3, p.floorHeight = 300.0, p.roof = "gable", p.commercial = 0.2;
    return p;
}

/** Manhattan: long thin blocks, avenues every few streets, towers rising to the centre. */
Profile manhattan()
{
    Profile p;
    p.name = "manhattan";
    p.pattern = Pattern::Grid;
    p.widthArterial = 3000.0, p.widthCollector = 2400.0, p.widthLocal = 1800.0, p.widthAlley = 600.0;
    p.gridWidth = 6000.0, p.gridDepth = 25000.0, p.arterialEvery = 3, p.maxBlock = 30000.0;
    p.plaza = false, p.maxGrade = 0.08, p.falloff = 1.2;
    p.lotWidth = 800.0, p.lotDepth = 3000.0, p.minLot = 80.0;
    p.frontSetback = 0.0, p.sideSetback = 0.0, p.buildDepth = 3000.0;
    p.minFloors = 5, p.maxFloors = 60, p.floorHeight = 380.0, p.roof = "flat", p.commercial = 0.7;
    return p;
}

/** A European capital: a medieval core inside a ring boulevard where the walls were, perimeter blocks beyond. */
Profile europeanCity()
{
    Profile p;
    p.name = "europeanCity";
    p.pattern = Pattern::OrganicGrid;
    p.widthArterial = 2400.0, p.widthCollector = 1500.0, p.widthLocal = 1000.0, p.widthAlley = 400.0;
    p.gates = 6, p.coreRadius = 0.35;
    p.gridWidth = 11300.0, p.gridDepth = 11300.0, p.arterialEvery = 5, p.gridAngle = 12.0;
    p.lotWidth = 1200.0, p.lotDepth = 2500.0, p.minLot = 100.0;
    p.frontSetback = 0.0, p.sideSetback = 0.0, p.buildDepth = 1600.0;
    p.minFloors = 3, p.maxFloors = 7, p.floorHeight = 340.0, p.roof = "mansard", p.commercial = 0.45;
    return p;
}

/** A modern city: big blocks, setback towers, wide roads. */
Profile modernCity()
{
    Profile p;
    p.name = "modernCity";
    p.pattern = Pattern::Grid;
    p.widthArterial = 4000.0, p.widthCollector = 2800.0, p.widthLocal = 2000.0, p.widthAlley = 800.0;
    p.gridWidth = 15000.0, p.gridDepth = 15000.0, p.arterialEvery = 3, p.maxBlock = 40000.0;
    p.plaza = false, p.maxGrade = 0.07, p.falloff = 2.4;
    p.lotWidth = 4000.0, p.lotDepth = 5000.0, p.minLot = 400.0;
    p.frontSetback = 800.0, p.sideSetback = 800.0, p.buildDepth = 4000.0;
    p.minFloors = 4, p.maxFloors = 80, p.floorHeight = 400.0, p.roof = "flat", p.commercial = 0.6;
    return p;
}

/** A sci-fi megacity: rings and spokes around a hub, enormous blocks, towers kilometres high. */
Profile scifiMegacity()
{
    Profile p;
    p.name = "scifiMegacity";
    p.pattern = Pattern::Radial;
    p.widthArterial = 8000.0, p.widthCollector = 5000.0, p.widthLocal = 3000.0, p.widthAlley = 1200.0;
    p.rings = 6, p.spokes = 12, p.maxBlock = 120000.0, p.plaza = true;
    p.maxGrade = 0.1, p.falloff = 1.4;
    p.lotWidth = 8000.0, p.lotDepth = 10000.0, p.minLot = 1500.0;
    p.frontSetback = 1500.0, p.sideSetback = 1500.0, p.buildDepth = 9000.0;
    p.minFloors = 20, p.maxFloors = 400, p.floorHeight = 450.0, p.roof = "flat", p.commercial = 0.5;
    return p;
}

struct Builtin
{
    const char* name;
    Profile (*make)();
};

constexpr Builtin kBuiltins[] = {
    {"hamlet", hamlet},           {"streetVillage", streetVillage}, {"medievalTown", medievalTown},
    {"usTown", usTown},           {"manhattan", manhattan},         {"europeanCity", europeanCity},
    {"modernCity", modernCity},   {"scifiMegacity", scifiMegacity},
};
} // namespace

const char* patternName(Pattern p)
{
    switch (p)
    {
    case Pattern::Organic: return "organic";
    case Pattern::Grid: return "grid";
    case Pattern::Radial: return "radial";
    case Pattern::Linear: return "linear";
    case Pattern::OrganicGrid: return "organicGrid";
    }
    return "organic";
}

bool parsePattern(std::string_view name, Pattern* out)
{
    for (Pattern p : {Pattern::Organic, Pattern::Grid, Pattern::Radial, Pattern::Linear, Pattern::OrganicGrid})
        if (name == patternName(p))
        {
            *out = p;
            return true;
        }
    return false;
}

std::vector<std::string> profileNames()
{
    std::vector<std::string> out;
    for (const Builtin& b : kBuiltins)
        out.emplace_back(b.name);
    return out;
}

bool builtinProfile(std::string_view name, Profile* out)
{
    for (const Builtin& b : kBuiltins)
        if (name == b.name)
        {
            *out = b.make();
            return true;
        }
    return false;
}

json toJson(const Profile& p)
{
    json j = {{"name", p.name}, {"pattern", patternName(p.pattern)}, {"plaza", p.plaza}, {"roof", p.roof}};
    for (const DoubleField& f : kDoubles)
        j[f.name] = p.*f.member;
    for (const IntField& f : kInts)
        j[f.name] = p.*f.member;
    return j;
}

void applyJson(Profile& p, const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("profile overrides must be a JSON object");
    for (auto& [k, v] : j.items())
    {
        bool done = false;
        for (const DoubleField& f : kDoubles)
            if (k == f.name)
            {
                if (!v.is_number())
                    throw std::runtime_error("profile field " + k + " takes a number");
                p.*f.member = v.get<double>();
                done = true;
            }
        for (const IntField& f : kInts)
            if (k == f.name)
            {
                if (!v.is_number())
                    throw std::runtime_error("profile field " + k + " takes a number");
                p.*f.member = int(v.get<double>());
                done = true;
            }
        if (done)
            continue;
        if (k == "name" && v.is_string())
            p.name = v.get<std::string>();
        else if (k == "roof" && v.is_string())
            p.roof = v.get<std::string>();
        else if (k == "plaza" && v.is_boolean())
            p.plaza = v.get<bool>();
        else if (k == "pattern" && v.is_string())
        {
            if (!parsePattern(v.get<std::string>(), &p.pattern))
                throw std::runtime_error("unknown pattern '" + v.get<std::string>() + "' (organic, grid, radial, linear, organicGrid)");
        }
        else
            throw std::runtime_error("unknown or mistyped profile field '" + k + "'");
    }
}

double streetWidth(const Profile& p, StreetClass c)
{
    switch (c)
    {
    case StreetClass::Arterial: return p.widthArterial;
    case StreetClass::Collector: return p.widthCollector;
    case StreetClass::Local: return p.widthLocal;
    case StreetClass::Alley: return p.widthAlley;
    case StreetClass::Boundary: return 0.0;
    }
    return p.widthLocal;
}
} // namespace settlement
