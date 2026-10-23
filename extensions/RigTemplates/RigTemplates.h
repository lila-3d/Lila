#pragma once

#include "Template.h"

#include <lila/Extension.h>
#include <lila/Graph.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/** The extension's state and scene side, shared by its commands and its UI. */
namespace rigt
{
inline constexpr const char* kRootType = "rigTemplate";
inline constexpr const char* kMarkerType = "rigMarker";

struct Entry
{
    Template tmpl;
    std::filesystem::path file; // empty for a template made this session and not saved
    std::string tier;           // builtin, workspace, user, file, session
};

/** Templates by id. */
std::vector<const Entry*> templates();
const Entry* findTemplate(const std::string& id);
/** Adds or replaces, registering its poses, aliases and picker under the extension. */
const Entry& addTemplate(Template t, std::filesystem::path file, std::string tier);
/** Called after the template list changes. */
int addListener(std::function<void()> fn);
void removeListener(int id);

/** <user config>/rigTemplates and <workspace>/.lila/rigTemplates (empty with no workspace). */
std::filesystem::path userDir();
std::filesystem::path workspaceDir();

/** The instance root a node belongs to (the root itself, or a marker under it); kNoNode when none. */
lila::scene::NodeId instanceOf(lila::scene::Graph& g, lila::scene::NodeId node);

bool registerTypes(lila::ext::Context& ctx);
bool registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
void loadTemplates(lila::ext::Context& ctx);
void shutdown();
} // namespace rigt
