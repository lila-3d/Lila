#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

class QMenu;
class QWidget;

/**
 * One-click delivery of animated meshes (geometry nodes, deformers) to files or to Unreal, as static mesh frames
 * (a Niagara flipbook), an Alembic Geometry Cache or a vertex animation texture. Each mesh remembers its last
 * destination, kind and options in the scene, so sending again is a single click.
 */
namespace lila::ui::animatedSend
{
enum class Dest
{
    Files,
    Unreal,
};

enum class Kind
{
    Frames,
    Cache,
    Vat,
};

/** Display name, like "Geometry Cache". */
LILA_UI_API const char* kindLabel(Kind k);
/** False when the command behind it is not registered. */
LILA_UI_API bool available(Kind k);

/** The mesh's stored settings with defaults filled in. */
LILA_UI_API nlohmann::json settings(const std::string& mesh);

/** Exports or maps and syncs each mesh with its stored options, recording dest and kind as its last choice. */
LILA_UI_API void send(const std::vector<std::string>& meshes, Dest dest, Kind kind);
/** Sends each mesh to its last destination and kind; meshes never sent go to Unreal as a Geometry Cache. */
LILA_UI_API void sendAgain(const std::vector<std::string>& meshes);
/** Edits the first mesh's options in a dialog whose Send button sends them all. */
LILA_UI_API void openOptions(QWidget* parent, const std::vector<std::string>& meshes);

/** "Files > ..., Unreal > ..., Options..." entries for `meshes()`, evaluated when an entry fires. */
LILA_UI_API void fillMenu(QMenu* menu, QWidget* parent, std::function<std::vector<std::string>()> meshes);
/** Short text for the repeat button: "Unreal: Geometry Cache". */
LILA_UI_API std::string lastLabel(const std::vector<std::string>& meshes);

/** animated.sendAgain, animated.sendOptions and one action per destination and kind, on the selected meshes. */
LILA_UI_API void registerActions(QWidget* parent);
} // namespace lila::ui::animatedSend
