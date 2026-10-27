#pragma once

#include "core/Export.h"

#include "gn/Group.h"

#include <string>
#include <string_view>
#include <vector>

/**
 * Nodes as shareable text: JSON with a format header, readable enough to post on a forum and paste back.
 *   { "format": "lila.geometryNodes", "version": 1,
 *     "nodes": [{name, type, position, values}], "links": [[fromNode, fromSocket, toNode, toSocket]],
 *     "inputs": [{name, type, default}] }
 * Positions are relative to the copied nodes' centre. Links are only those between copied nodes, and "ties" (when any)
 * only the zip ties among them. "inputs" holds the
 * group parameters a copied Group Input feeds, so pasting into another group can add them.
 */
namespace lila::gn
{
inline constexpr const char* kClipboardFormat = "lila.geometryNodes";
inline constexpr int kClipboardVersion = 1;

/** The named nodes as text. Group Output is left out. Throws std::invalid_argument for a name not in the group. */
LILA_SCENE_API std::string copyNodesText(const Group& g, const std::vector<std::string>& names);

/**
 * Adds the nodes in text to g, centred on `at` and renamed where taken; returns their new names in order. Also takes
 * a saved group definition, and text around the JSON (a forum post, a code fence). Validates everything first: on
 * any problem it throws std::invalid_argument naming each one and leaves g untouched.
 */
LILA_SCENE_API std::vector<std::string> pasteNodesText(Group& g, std::string_view text, glm::dvec2 at);
} // namespace lila::gn
