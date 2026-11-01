#pragma once

#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Mesh.h>

#include <glm/mat4x4.hpp>

#include <string>

namespace lila::ext
{
class Context;
}

namespace tailor
{
/** The DG node holding a garment and producing its draped mesh. */
inline constexpr const char* kGarmentType = "tailorGarment";
/** Garments are solved in metres; the scene is in centimetres. */
inline constexpr double kCmPerM = 100.0;

/** A collider mesh in garment space (metres) as triangles. */
void colliderTriangles(const lila::geo::Mesh& m, const glm::dmat4& world, std::vector<Vec3>& points,
                       std::vector<std::array<int, 3>>& tris);

const char* panelStateName(PanelState s);
/** False for an unknown name. */
bool parsePanelState(const std::string& name, PanelState* out);

nlohmann::json garmentToJson(const Garment& g);
/** Throws std::exception on malformed input. Missing fields keep their defaults. */
Garment garmentFromJson(const nlohmann::json& j);

/** While one exists, garments drape inside their evaluation instead of on a worker (batch runs always do). */
struct DrapeNow
{
    DrapeNow();
    ~DrapeNow();
    DrapeNow(const DrapeNow&) = delete;
    DrapeNow& operator=(const DrapeNow&) = delete;
};
/** Cancels running drapes and waits for them; for unloading. */
void stopDrapes();

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace tailor
