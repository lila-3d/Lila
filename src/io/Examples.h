#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lila::io::examples
{
namespace fs = std::filesystem;

/** A scene shipped in resources/examples/<Folder>/, described by the folder's README.md. */
struct Example
{
    std::string name;
    std::string description; // the README's first paragraph
    fs::path scene;
    fs::path thumbnail; // <scene stem>.png or thumb.png beside it; empty when there is none
};

LILA_SCENE_API fs::path examplesDir();
/** Every example scene, by folder then file name. */
LILA_SCENE_API std::vector<Example> list(const fs::path& dir = examplesDir());

/**
 * Opens an example as an untitled scene named after it, its references resolving in its folder, so saving asks for
 * a file and the shipped copy is never overwritten.
 */
LILA_SCENE_API void open(const Example& example);

/** examples, openExample. */
LILA_SCENE_API void registerCommands();
} // namespace lila::io::examples
