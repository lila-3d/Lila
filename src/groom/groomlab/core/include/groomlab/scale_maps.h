// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scales baked into the skin's textures, for scales too small or too far away to be worth their
// geometry (a distant creature, the fine scales between the big ones, a game's lower levels of
// detail): a tangent-space normal map, a height map and a color map on the scalp's UV layout.
// Each texel of the skin is cast from above, along the skin's normal at that texel, down onto
// the scales' bent mesh (scales.h; through a ScalpMesh's ray tree of its triangles): the first
// scale met gives the texel its height above the skin, its normal (in the skin's tangent space:
// x along the uv's u, y along v, z the skin's normal; OpenGL's +Y as Maya, Arnold and Unity read
// it, Unreal flips green) and its color. Texels between scales are the bare skin: height 0, a
// flat normal, the color map's alpha 0 (its coverage) over `background`.
//
// A scalp whose uvs leave the 0-1 square bakes a UDIM tile set: one image of each map per tile
// a triangle lies on (1001 + floor(u) + 10 floor(v)), named <prefix><Map>.<tile>.png; uvs in the
// 0-1 square bake one image of each, <prefix><Map>.png. Texels past the triangles' edges take
// their neighbors' values (`padding` texels), so filtering never reads the empty space round a
// uv shell.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace groomlab {

class ScalpMesh;
struct ScaleMesh;

struct ScaleMapOptions {
    int resolution = 2048;  // texels across each tile (16 to 8192)
    // The height the height map's white stands for, above the skin (scene units). 0: the
    // scales' highest point above the skin, measured.
    float maxHeight = 0.0f;
    int padding = 8;  // texels each uv shell's edges grow by
    std::array<float, 3> background{0.0f, 0.0f, 0.0f};  // the color map between scales (linear)
};

struct ScaleMaps {
    struct Tile {
        int udim = 1001;
        std::vector<std::uint8_t> normal;   // RGB, rows top to bottom (v = 1 first)
        std::vector<std::uint16_t> height;  // 0 the skin, 65535 maxHeight
        std::vector<std::uint8_t> color;    // RGBA, sRGB; alpha the scales' coverage
    };
    int resolution = 0;
    float maxHeight = 0.0f;  // what the height map's 65535 stands for
    bool udim = false;       // a tile set (the uvs leave the 0-1 square)
    std::vector<Tile> tiles;
    std::size_t texels = 0, covered = 0;  // the skin's texels, and those a scale covers
};

// The maps of `scales` (bent onto `skin`, as buildScaleMesh makes them) on `skin`'s uvs. Throws
// std::invalid_argument when the skin has no uvs or the resolution is out of range.
ScaleMaps bakeScaleMaps(const ScalpMesh& skin, const ScaleMesh& scales, const ScaleMapOptions& options = {});

// The highest any point of `scales` stands above `skin` (along the skin's normal at the closest
// point): the default maxHeight.
float scalesMaxHeight(const ScalpMesh& skin, const ScaleMesh& scales);

// The maps' file names: <prefix>ScaleNormal.png, <prefix>ScaleHeight.png (16-bit gray) and
// <prefix>ScaleColor.png, or <prefix><Map>.<tile>.png for each tile of a tile set.
inline const char* const kScaleMapNames[3] = {"ScaleNormal", "ScaleHeight", "ScaleColor"};
// The path of a map (one of kScaleMapNames) as a texture names it: the file, or the tile set's
// pattern with <UDIM> where the tile goes.
std::string scaleMapPath(const std::string& prefix, const std::string& map, bool udim);
// Writes every map of every tile; returns the files written, normal, height and color per tile.
// Throws std::runtime_error when one cannot be written.
std::vector<std::string> writeScaleMaps(const ScaleMaps& maps, const std::string& prefix);

}  // namespace groomlab
