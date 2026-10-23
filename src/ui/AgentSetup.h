#pragma once

#include "core/Export.h"

class QWidget;

namespace lila::ui::agentSetup
{
/**
 * "Set Up Agent Integration" (help.agentSetup): turns on the MCP server and writes .mcp.json, a Lila block in CLAUDE.md
 * and AGENTS.md, and the lila-modeling skill into the open workspace, each optional.
 */
LILA_UI_API void open(QWidget* parent);

/** The same dialog, once per workspace, the first time a terminal starts in it. The answer is remembered either way. */
LILA_UI_API void maybePrompt(QWidget* parent);

LILA_UI_API void registerActions();
} // namespace lila::ui::agentSetup
