#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class Graph;

/**
 * Surface materials, modeled on Unreal's default lit material.
 *
 * A `material` node (non-DAG) holds the inputs. Mesh faces pick one through their material slot: the face int
 * "material" indexes geo::Mesh::materialNames(), and a slot binds to the material node whose name equals the slot
 * name, exactly as FBX and Unreal match material slots by name. A slot with no node of that name renders with the
 * neutral default (mid gray, roughness 0.55). Renaming a material through renameMaterial rewrites the slots too.
 *
 * Conventions, all Unreal's:
 *   - baseColor and emissive are linear; color maps (base color, emissive) are sRGB, data maps are linear.
 *   - A connected map replaces its constant (emissive maps are scaled by emissiveIntensity).
 *   - Normal maps are tangent space, DirectX style by default (green points down the texture, Unreal's convention);
 *     normalFormat "opengl" (Flip Green) is for maps baked green-up (Blender, Substance's OpenGL preset).
 *   - Packed maps hold occlusion in R, roughness in G, metallic in B (ORM, Unreal's packing), or with packedLayout
 *     "mro" metallic in R and occlusion in B. A packed map wins over separate ones.
 *   - Lila UVs are V-up like Maya and FBX (v = 0 at the bottom of the image); the viewport samples at (u, 1 - v),
 *     which is what Unreal's FBX import does when it flips V, so a texture lands the same way in both.
 */
namespace material
{
enum Attr
{
    kBaseColor,
    kBaseColorMap,
    kMetallic,
    kMetallicMap,
    kSpecular,
    kRoughness,
    kRoughnessMap,
    kNormalMap,
    kNormalFormat,
    kAmbientOcclusionMap,
    kOrmMap,
    kPackedLayout,
    kEmissiveColor,
    kEmissiveMap,
    kEmissiveIntensity,
    kOpacity,
    kOpacityMaskClip,
    kBlendMode,
    kTwoSided,
    kUvSet,
    kShadingModel,
    // Painterly
    kPainterlyStrength,
    kStrokeNormal,
    kStrokeScale,
    kStrokeLayerRatio,
    kStrokeMapping,
    kStrokeMap,
    kPaletteCount,
    kPaletteShadow,
    kPaletteLight,
    kPaletteAccent,
    kPaletteContrast,
    kPaletteSpread,
    kSpecularTint,
    kBoil,
    kBoilFps,
    // Toon
    kToonShadowColor,
    kToonLightTint,
    kToonThreshold,
    kToonSoftness,
    kToonOuterOffset,
    kToonOuterColor,
    kRimColor,
    kRimWidth,
    kRimStrength,
    // Foliage
    kFoliageBottomColor,
    kFoliageTopColor,
    kFoliageGradient,
    kFoliageBottomShadow,
    kFoliageTopHighlight,
    kFoliageTranslucency,
    kFoliageWrap,
    kCount
};

/** The viewport (and Unreal master material) lighting model; DefaultLit is Unreal's default lit. */
enum class ShadingModel
{
    DefaultLit,
    Painterly,
    Toon,
    Foliage,
};

/** Where Painterly stroke sheets are projected. */
enum class StrokeMapping
{
    Object, // triplanar in object space: strokes ride along with the object
    World,  // triplanar in world space
    Screen, // triplanar, rescaled by camera distance in octaves so strokes keep their size on screen
    Uv,     // the mesh's UVs, for output that bakes to textures
};

enum class BlendMode
{
    Opaque,
    Masked,
    Translucent,
};

enum class NormalFormat
{
    DirectX,
    OpenGL,
};

enum class PackedLayout
{
    ORM,
    MRO,
};
} // namespace material

namespace materials
{
/** Slot that faces fall back to when a mesh first gets a second material; the FBX exporter uses the same name. */
inline constexpr const char* kDefaultSlot = "Default";

/** The material node a slot binds to, or null. */
LILA_SCENE_API Node* find(Graph& g, std::string_view slot);
LILA_SCENE_API std::vector<Node*> all(Graph& g);

/** Creates a material node named exactly `name` when it is free (else made unique). Undoable. */
LILA_SCENE_API Node* create(Graph& g, std::string_view name);

/** Source channel (0 R, 1 G, 2 B) of occlusion, roughness and metallic in a packed map of this layout. */
LILA_SCENE_API std::array<int, 3> packedChannels(material::PackedLayout layout);

/** Texture channel names for setMaterialTexture ("baseColor", "normal", "orm", ...) and their attrs. */
LILA_SCENE_API const std::vector<std::pair<std::string, int>>& textureChannels();

/** The stroke sheets shipped in resources/strokes, by file stem; a material's strokeMap may name one instead of a path. */
LILA_SCENE_API std::vector<std::string> builtinStrokeSheets();
/** The stroke sheet file a strokeMap value means: the default sheet when empty, a built-in by stem, else resolvePath. */
LILA_SCENE_API std::string strokeSheetPath(const std::string& strokeMap);

/** paths::resolvePath of a stored texture path, as UTF-8. */
LILA_SCENE_API std::string resolvePath(const std::string& path);

/** Slot index for name, added when missing. */
LILA_SCENE_API int32_t slotIndex(geo::Mesh& mesh, const std::string& name);
/** Points every face (faces empty) or the given faces at the named slot, then drops slots no face uses. */
LILA_SCENE_API void assign(geo::Mesh& mesh, const std::string& slot, const std::vector<uint32_t>& faces);
/** Drops slots no face uses and renumbers the rest, keeping their order. */
LILA_SCENE_API void pruneSlots(geo::Mesh& mesh);
/** Slot name per face id; ids outside the names get "Material<id>", like the FBX exporter. */
LILA_SCENE_API std::string slotName(const geo::Mesh& mesh, int32_t id);
/** Faces per slot name, in slot order. */
LILA_SCENE_API std::vector<std::pair<std::string, uint32_t>> slotUsage(const geo::Mesh& mesh);

LILA_SCENE_API void registerTypes();
/** createMaterial, assignMaterial, listMaterials, materialInfo, setMaterialTexture, renameMaterial. */
LILA_SCENE_API void registerCommands();
} // namespace materials
} // namespace lila::scene
