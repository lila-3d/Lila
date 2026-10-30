#pragma once

#include <lila/Command.h>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * MaterialX networks Lila materials are written as, kept neutral so the same network goes into a USD stage (shaders
 * whose info:id is the nodedef, under the material's outputs:mtlx:surface) or a .mtlx document (UsdMaterialXDoc.cpp).
 */
namespace lila::usd::mtlx
{
/** The lit surface written; unlit materials always use surface_unlit. */
enum class Surface
{
    None,
    OpenPbr,         // ND_open_pbr_surface_surfaceshader
    StandardSurface, // ND_standard_surface_surfaceshader
};

/** "openPBR", "standardSurface" or "none". */
const char* surfaceName(Surface s);
/** Reads surfaceName's names (and "off"); false for anything else. */
bool parseSurface(std::string_view name, Surface* out);

struct Input
{
    std::string name;
    /** MaterialX type: float, integer, boolean, vector2, vector3, color3, color4, string, filename. */
    std::string type;
    std::vector<float> numbers;
    std::string text; // string and filename values
    /** A connection to another node of the network, and which of its outputs (empty is "out"). */
    std::string node, output;
    std::string colorSpace;
};

struct Node
{
    std::string name;
    std::string category; // "image"
    std::string type;     // output type, "multioutput" for several
    std::string nodedef;  // "ND_image_color3", the info:id in USD
    std::vector<Input> inputs;
    /** Output name and type. */
    std::vector<std::pair<std::string, std::string>> outputs;
};

struct Network
{
    std::vector<Node> nodes;
    /** The surface shader node's name. */
    std::string surface;
};
} // namespace lila::usd::mtlx

namespace lila::usd
{
/** The importMaterialX and exportMaterialX commands (UsdMaterialXDoc.cpp). */
nlohmann::json importMaterialX(const Args& a);
nlohmann::json exportMaterialX(const Args& a);
} // namespace lila::usd
