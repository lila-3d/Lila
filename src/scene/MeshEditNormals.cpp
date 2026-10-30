#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "scene/MeshEditInternal.h"

namespace lila::scene::detail
{
void registerMeshEditNormalsCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    ops::registerCommand({
        .name = "polyWeightedNormals",
        .doc = "Face-weighted normals on the selected faces (whole meshes in object mode): each smooth fan takes the normal of "
               "its biggest faces, so chamfered or bevelled low poly shades its large faces flat and only the chamfers bend. "
               "Written as locked split normals, which export to FBX. polyUnlockNormals goes back to computed ones.",
        .flags = {{"mode", "m", T::String, "area", "What weighs a face: area, angle (its corner angle) or areaAngle.", {"area", "angle", "areaAngle"}},
                  {"dominance", "d", T::Float, 0.5, "Faces under this fraction of the heaviest face in a fan are left out; 0 keeps all.", {}, {}, 0.0, 1.0},
                  {"keepSharp", "ks", T::Bool, true, "Keep hard edges split."},
                  {"faceStrength", "fs", T::Bool, true, "Only the faces with the highest face int faceStrength count, when the mesh has it."}},
        .run = [](const Args& a) -> json {
            geo::WeightedNormalOptions o;
            const std::string mode = a.get<std::string>("mode", "area");
            o.mode = mode == "angle" ? geo::WeightedNormalMode::CornerAngle
                     : mode == "areaAngle" ? geo::WeightedNormalMode::AreaAngle
                                           : geo::WeightedNormalMode::FaceArea;
            o.dominance = a.get<double>("dominance", 0.5);
            o.keepSharp = a.get<bool>("keepSharp", true);
            o.useFaceStrength = a.get<bool>("faceStrength", true);
            return editSelected("Weighted Normals", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::applyWeightedNormals(m, o, withMirror(m, s, sel, K::Face));
                return EditResult{};
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyLockNormals",
        .doc = "Store the current shading normals of the selected meshes as split normals, so later edits keep them.",
        .run = [](const Args&) -> json {
            return editSelected("Lock Normals", K::Face, [&](geo::Mesh& m, const Ids&, const geo::SymmetryMap*) {
                if (!m.cornerAttrs().has(geo::attr::kNormal))
                    geo::updateCornerNormals(m);
                return EditResult{};
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyUnlockNormals",
        .doc = "Drop the stored split normals of the selected meshes, so shading comes from the hard and soft edges again.",
        .run = [](const Args&) -> json {
            return editSelected("Unlock Normals", K::Face, [&](geo::Mesh& m, const Ids&, const geo::SymmetryMap*) {
                m.cornerAttrs().remove(geo::attr::kNormal);
                return EditResult{};
            }, true);
        },
    });
}
} // namespace lila::scene::detail
