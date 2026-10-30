#pragma once

#include "TailorCore.h"

#include <lila/Command.h>

#include <string>

namespace lila::ext
{
class Context;
}

namespace tailor
{
/** The DG node holding a garment and producing its draped mesh. */
inline constexpr const char* kGarmentType = "tailorGarment";

const char* panelStateName(PanelState s);
/** False for an unknown name. */
bool parsePanelState(const std::string& name, PanelState* out);

nlohmann::json garmentToJson(const Garment& g);
/** Throws std::exception on malformed input. Missing fields keep their defaults. */
Garment garmentFromJson(const nlohmann::json& j);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace tailor
