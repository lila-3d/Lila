#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>

/** Direct calls into the embedded interpreter for the UI panels. Kept apart because Python.h and Qt clash on `slots`. */
namespace lila::ui::pybridge
{
/** JSON from lila._apidoc.collect_json(), or empty when Python is down or the walk failed (traceback goes to the log). */
LILA_UI_API std::string apiDocJson();

/** sys.base_prefix of the embedded interpreter, else $LILA_PYTHON_HOME; empty when neither is known. */
LILA_UI_API std::filesystem::path pythonHome();
} // namespace lila::ui::pybridge
