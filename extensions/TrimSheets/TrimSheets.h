#pragma once

#include "TrimSheet.h"

#include <filesystem>
#include <string>

namespace lila::ext
{
class Context;
}

namespace trims
{
/** The Lila material (and material slot) a sheet's faces use: M_<name without T_>. */
std::string materialName(const Sheet& sheet);
/** A palette's material, M_<name without T_>_<palette>; the sheet's own for the empty palette. */
std::string materialName(const Sheet& sheet, std::string_view palette);
/** Every material slot the sheet's palettes use, the sheet's own first. */
std::vector<std::string> paletteMaterials(const Sheet& sheet);

/** Node type of the live hotspot modifier (addModifier kind trimHotspot). */
inline constexpr const char* kLiveModifier = "trimHotspotModifier";

/** The sheet trim tools use when none is given; kept in prefs. */
std::string currentSheet();
void setCurrentSheet(const std::string& path);

/** <workspace>/TrimSheets, or beside the link cache without a workspace. */
std::filesystem::path defaultSheetFolder();

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** The Trim Sheet Editor panel. */
void registerEditor(lila::ext::Context& ctx);
} // namespace trims
