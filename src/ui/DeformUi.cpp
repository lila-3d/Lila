#include "ui/DeformUi.h"

#include "ui/Actions.h"
#include "ui/DeformHandles.h"
#include "ui/MorphPanel.h"

#include <tuple>

namespace lila::ui
{
namespace
{
void add(const char* id, const char* label, const char* command, nlohmann::json call, const char* python, const char* doc)
{
    actions::add({.id = id, .label = label, .category = "Deform", .doc = doc, .python = python, .command = command, .call = std::move(call)});
}
} // namespace

void registerDeformActions()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerMorphUi();
    registerDeformFoundationUi();
    add("deform.lattice", "Lattice", "lattice", nlohmann::json::object(), "lattice()",
        "Wraps the selected meshes in a lattice; edit its points in vertex mode to reshape them.");
    for (const char* kind : {"bend", "twist", "taper", "squash", "sine", "wave"})
    {
        std::string k = kind;
        actions::add({.id = "deform." + k, .label = std::string(1, char(kind[0] - 'a' + 'A')) + k.substr(1), .category = "Deform",
                      .doc = "Adds a " + k + " deformer to the selected meshes with a handle to place it.",
                      .python = "nonlinear(type=\"" + k + "\")", .command = "nonlinear", .call = {{"type", k}}});
    }
    add("deform.deltaMush", "Delta Mush", "deltaMush", nlohmann::json::object(), "deltaMush()",
        "Smooths skinning and deformer artefacts on the selected meshes while keeping their detail.");
    add("deform.wrap", "Wrap", "wrap", nlohmann::json::object(), "wrap()",
        "Select the meshes to deform, then the driver last: they follow the driver's surface.");
    add("deform.shrinkwrap", "Shrinkwrap", "shrinkwrapDeformer", nlohmann::json::object(), "shrinkwrapDeformer()",
        "Select the meshes, then the target last: they move onto the target's surface, live.");
    add("deform.surfaceDeform", "Surface Deform", "surfaceDeform", nlohmann::json::object(), "surfaceDeform()",
        "Select the meshes, then a driver of any topology last: they ride its surface.");
    add("deform.addWrapDriver", "Add Wrap Driver", "addWrapDriver", nlohmann::json::object(), "addWrapDriver()",
        "Select a wrap deformer, then more driver meshes: the wrap follows all of them.");
    add("deform.collision", "Collision", "collision", nlohmann::json::object(), "collision()",
        "Select the meshes, then a collider (a mesh, or a transform as a capsule) last: they are pushed out of it.");
    add("deform.addCollider", "Add Collider", "addCollider", nlohmann::json::object(), "addCollider()",
        "Select a collision deformer, then more colliders.");
    add("deform.rigidify", "Rigidify", "rigidify", {{"regions", "selection"}}, "rigidify(regions=\"selection\")",
        "Select components: each connected patch keeps its shape and moves rigidly with the deformation under it.");
    add("deform.skinSlide", "Skin Slide", "skinSlide", nlohmann::json::object(), "skinSlide()",
        "Makes the deformation below slide along the rest surface instead of stretching it.");
    add("deform.uvPin", "UV Pin", "surfacePin", {{"mode", "uv"}}, "surfacePin(mode=\"uv\")",
        "Select objects or joints, then a mesh last: each is pinned to the mesh's final surface at its nearest uv.");
    add("deform.proximityPin", "Proximity Pin", "surfacePin", {{"mode", "nearest"}}, "surfacePin(mode=\"nearest\")",
        "Select objects or joints, then a mesh last: each is pinned to the mesh's final surface at its nearest point.");
    add("deform.cage", "Cage", "cage", nlohmann::json::object(), "cage()",
        "Select the meshes, then a closed cage mesh last: editing the cage reshapes them smoothly.");
    add("deform.softMod", "Soft Modification", "softMod", nlohmann::json::object(), "softMod()",
        "Adds a handle that moves nearby points with a soft falloff.");
    add("deform.rebind", "Rebind Deformer", "rebindDeformer", nlohmann::json::object(), "rebindDeformer()",
        "Makes the selected deformers' current state their rest state.");
    add("deform.jiggle", "Jiggle", "jiggle", nlohmann::json::object(), "jiggle()",
        "Makes the selected meshes' points trail their motion on springs and settle; scrubbing matches playback.");
    add("deform.bakeSkinWeights", "Bake to Skin Weights", "bakeSkinWeights", nlohmann::json::object(), "bakeSkinWeights()",
        "Fits linear skin weights to everything the selected skinned mesh's stack does over a range of motion, then turns the replaced "
        "deformers off: what Unreal will run.");
    add("deform.bakeHelperJoints", "Bake to Helper Joints", "bakeSkinWeights", {{"source", "animation"}, {"helpers", 4}},
        "bakeSkinWeights(source=\"animation\", helpers=4)",
        "Fits linear weights plus keyed helper joints to the selected skinned mesh over the playback range: the route for jiggle and "
        "other motion.");
    add("deform.bakeCorrectiveMorphs", "Bake to Corrective Morphs", "bakeCorrectiveMorphs", nlohmann::json::object(), "bakeCorrectiveMorphs()",
        "Turns what the stack adds over linear skinning into corrective morph targets driven by pose drivers.");
    add("deform.rangeOfMotion", "Range of Motion Take", "rangeOfMotion", nlohmann::json::object(), "rangeOfMotion()",
        "Makes a take turning the selected skinned mesh's joints through their range of motion, one pose per frame.");
    add("deform.exportMLDeformer", "Send ML Deformer to Unreal", "exportMLDeformer", nlohmann::json::object(), "exportMLDeformer()",
        "Exports a range of motion of the selected skinned mesh as ML Deformer training data and creates the ML Deformer in Unreal.");
    for (auto [id, label, method] : {std::tuple{"rig.skinLinear", "Linear", "linear"}, std::tuple{"rig.skinDualQuaternion", "Dual Quaternion", "dualQuaternion"},
                                     std::tuple{"rig.skinCentresOfRotation", "Centres of Rotation", "centresOfRotation"},
                                     std::tuple{"rig.skinDirectDeltaMush", "Direct Delta Mush", "directDeltaMush"}})
        actions::add({.id = id, .label = label, .category = "Rig", .doc = std::string("Skins the selected meshes with ") + label + ".",
                      .python = std::string("skinningMethod(method=\"") + method + "\")", .command = "skinningMethod", .call = {{"method", method}}});
    add("deform.cageGreen", "Green", "cage", {{"coordinates", "green"}}, "cage(coordinates=\"green\")",
        "Select the meshes, then a closed cage last: a cage whose edits keep the detail's proportions (Green coordinates).");
    add("deform.cageSomigliana", "Somigliana", "cage", {{"coordinates", "somigliana"}}, "cage(coordinates=\"somigliana\")",
        "Select the meshes, then a closed cage last: an elastic cage with a bulge and Poisson ratio for volume.");
    add("deform.kelvinlet", "Kelvinlet", "kelvinlet", nlohmann::json::object(), "kelvinlet()",
        "Adds a handle that grabs, twists, scales or pinches the selected meshes like rubber as you move, rotate or scale it.");
    add("deform.arap", "ARAP", "arap", nlohmann::json::object(), "arap()",
        "Select vertices for a handle: they follow a new locator and the rest bends rigidly. Add anchors with Set Anchors.");
    add("deform.laplacian", "Laplacian", "laplacianDeform", nlohmann::json::object(), "laplacianDeform()",
        "Select vertices for a handle: they follow a new locator and the surface between follows smoothly.");
    add("deform.addHandle", "Add Handle", "addDeformHandle", nlohmann::json::object(), "addDeformHandle()",
        "Select vertices of a mesh with an ARAP or Laplacian deformer: they get their own handle.");
    add("deform.setAnchors", "Set Anchors", "setDeformAnchors", nlohmann::json::object(), "setDeformAnchors()",
        "Select vertices of a mesh with an ARAP or Laplacian deformer: they stay put.");
    add("deform.bbw", "Point Handles", "bbw", nlohmann::json::object(), "bbw()",
        "Select the meshes and the locators or joints to use as handles: bounded biharmonic weights blend them smoothly.");
    add("deform.bbwSkinWeights", "BBW Skin Weights", "bbwSkinWeights", nlohmann::json::object(), "bbwSkinWeights()",
        "Recomputes the selected skinned mesh's weights as bounded biharmonic weights between its joints.");
    add("deform.shear", "Shear", "shear", nlohmann::json::object(), "shear()", "Adds a shear deformer with a handle to place it.");
    add("deform.inflate", "Inflate", "inflate", nlohmann::json::object(), "inflate()", "Pushes the selected meshes along their normals.");
    add("deform.cast", "Cast", "cast", nlohmann::json::object(), "cast()",
        "Pulls the selected meshes toward a sphere, cylinder or box around a handle.");
    add("deform.smooth", "Smooth", "smoothDeformer", nlohmann::json::object(), "smoothDeformer()",
        "Smooths the selected meshes: Laplacian, Taubin or corrective (only what changed since binding).");
    add("deform.warp", "Warp", "warp", nlohmann::json::object(), "warp()",
        "Adds a from and a to handle: moving the to handle drags the points near the from handle.");
    add("deform.noise", "Noise", "noiseDeform", nlohmann::json::object(), "noiseDeform()", "Displaces the selected meshes by seeded noise.");
    add("deform.tension", "Tension", "tension", nlohmann::json::object(), "tension()",
        "Writes stretch and compression as a colour set and a point attribute, for wrinkle maps.");
    add("deform.textureDeformer", "Texture Displace", "textureDeformer", nlohmann::json::object(), "textureDeformer()",
        "Displaces the selected meshes by an image or a texture set's channel read through their uvs; set image or textureSet on it.");
    add("deform.bakeTension", "Bake Tension Mask", "bakeTension", nlohmann::json::object(), "bakeTension()",
        "Bakes the selected tension deformer (or mesh's) over the playback range into a tensionMask colour set: red stretch, green "
        "compression.");
    add("deform.bakeSurfacePin", "Bake Pins to Joints", "bakeSurfacePin", nlohmann::json::object(), "bakeSurfacePin()",
        "Turns the selected surface pins into joints under the joint that weights the surface there, so they export as bones for sockets.");
}
} // namespace lila::ui
