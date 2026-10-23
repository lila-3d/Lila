#pragma once

#include <lila/Extension.h>

#include <string>
#include <vector>

/** Shared by the command and UI halves of the Terrain Tools extension. */
namespace terraintools
{
/** One deformer kind the tools add: what the panel calls it, the Lila deformer behind it and the amount it varies. */
struct DeformerSpec
{
    const char* kind;   // "bend"
    const char* label;  // "Bend"
    const char* amount; // "Curvature"
    const char* type;   // the nonlinear type: "bend", or "taper" for flare
    std::vector<const char*> attrs;
    double low, high;   // default amount range
    bool bounded;
};

const std::vector<DeformerSpec>& deformerSpecs();
/** Null when unknown. Accepts the kind ("flare") or the label ("Flare"). */
const DeformerSpec* findSpec(const std::string& kindOrLabel);

inline constexpr const char* kPrefsFile = "terrainGenerator";
inline constexpr const char* kPrefsKey = "/Tools";

void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace terraintools
