#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace lila::mcp::agents
{
namespace fs = std::filesystem;

inline constexpr const char* kBlockBegin = "<!-- lila:begin (managed by Lila: Help > Set Up Agent Integration; edits inside are replaced) -->";
inline constexpr const char* kBlockEnd = "<!-- lila:end -->";

/**
 * The "lila" server entry merged into a workspace's .mcp.json. The URL and token come from the environment Lila's terminal
 * sets (LILA_MCP_URL, LILA_MCP_TOKEN), so the file holds no secret and is safe under source control.
 */
std::string mergeMcpJson(const std::string& existing, int port);

/** existing with the delimited Lila block replaced by block, or appended when it has none. User text is never touched. */
std::string upsertBlock(const std::string& existing, const std::string& block);

struct SetupOptions
{
    bool mcpJson = true;      // <root>/.mcp.json
    bool instructions = true; // Lila block in <root>/CLAUDE.md and <root>/AGENTS.md
    bool skill = true;        // <root>/.claude/skills/lila-modeling/SKILL.md
    int port = 8740;
};

struct SetupResult
{
    std::vector<std::string> written;
    std::vector<std::string> errors; // read-only (checked-in) files and the like
};

/** Writes what the options ask for into the workspace root. Reads templates from resources/agents. */
SetupResult setUp(const fs::path& root, const SetupOptions& options);

/** Variables for shells started from Lila: LILA, LILA_MCP_URL, LILA_MCP_TOKEN, LILA_WORKSPACE, LILA_BIN. */
std::vector<std::pair<std::string, std::string>> shellVariables();

/** Per workspace answer to the setup prompt, in prefs: "" (never asked), "done" or "declined". */
std::string workspaceState(const fs::path& root);
void setWorkspaceState(const fs::path& root, const std::string& state);
} // namespace lila::mcp::agents
