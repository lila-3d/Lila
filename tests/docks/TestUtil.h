#pragma once

#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "tex/GraphFiles.h"
#include "tex/TextureSet.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/NodeEditor.h"
#include "ui/TextureGraphUi.h"

#include <QApplication>

namespace dockstest
{
inline nlohmann::json run(const char* name, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(name, call); }

inline void pump()
{
    for (int i = 0; i < 4; ++i)
    {
        lila::mainq::drain();
        QApplication::processEvents();
    }
}

/** Commands, actions and the texture graph backend the node editor needs. */
inline void registerTexGraphUi()
{
    using namespace lila;
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    gn::registerTypes();
    gn::registerCommands();
    if (!scene::types::find(tex::kTextureSetType))
        tex::registerTypes();
    if (!ops::find("texCreateSet"))
        tex::registerCommands();
    if (!ops::find("texExportGraph"))
        tex::registerFileCommands();
    ui::actions::registerBuiltins();
    ui::registerNodeEditorActions();
    ui::registerTexGraphBackend();
    ui::Hotkeys::get().install();
}
} // namespace dockstest
