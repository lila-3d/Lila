#pragma once

#include "app/Host.h"
#include "core/Export.h"

#include <filesystem>

namespace lila::ui
{
/** Commands the UI library adds in batch mode too (offscreen captures). Stops the renderer at events::Event::Shutdown. */
LILA_UI_API void registerBatchCommands();

/** The GUI: QApplication, UI registrations, the main window and its event loop. Returns the exit code. */
LILA_UI_API int runApp(int argc, char** argv, const host::Options& opts, const std::filesystem::path& startupScene);
} // namespace lila::ui

/** registerBatchCommands for lilabatch, which loads this library at run time. */
extern "C" LILA_UI_API void lilaUiRegisterBatchCommands();
