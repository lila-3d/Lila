#include "ui/Actions.h"

#include "cmds/Command.h"
#include "cmds/CommandOptions.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "python/Python.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "ui/Hotkeys.h"
#include "ui/ViewportDisplay.h"

#include <map>

namespace lila::ui::actions
{
namespace
{
std::map<std::string, Action>& registry()
{
    static std::map<std::string, Action> r;
    return r;
}

std::vector<json>& recentList()
{
    static std::vector<json> r;
    return r;
}

void remember(const json& target)
{
    // Opening the hotbox is how Recent Commands is reached, so it isn't one of them.
    if (target.is_string() && target.get<std::string>().rfind("hotbox.", 0) == 0)
        return;
    std::vector<json>& r = recentList();
    std::erase(r, target);
    r.insert(r.begin(), target);
    if (r.size() > 20)
        r.resize(20);
}

void runCommand(const char* name, const json& call = json::object())
{
    try
    {
        cmds::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** "weightedNormals" -> "Weighted Normals". */
std::string modifierLabel(const std::string& kind)
{
    std::string out;
    for (size_t i = 0; i < kind.size(); ++i)
    {
        char c = kind[i];
        if (i == 0)
            out += char(std::toupper(static_cast<unsigned char>(c)));
        else
        {
            if (std::isupper(static_cast<unsigned char>(c)))
                out += ' ';
            out += c;
        }
    }
    return out;
}

/** Queries don't echo. */
json runCommandResult(const char* name, const json& call)
{
    try
    {
        return cmds::execute(name, call, CallSource::Script);
    }
    catch (const CommandError&)
    {
        return nullptr;
    }
}
} // namespace

void add(Action a)
{
    std::string id = a.id;
    if (a.doc.empty())
        log::warn("action {} has no doc", id);
    if (!a.command.empty() && !a.run)
        a.run = [command = a.command, call = a.call.is_object() ? a.call : json::object()] { runWithOptions(command, call); };
    registry()[id] = std::move(a);
}

void bindCommand(const std::string& id, std::string command, json call)
{
    auto it = registry().find(id);
    if (it == registry().end())
        return;
    it->second.command = std::move(command);
    it->second.call = call.is_object() ? std::move(call) : json::object();
    it->second.run = [command = it->second.command, c = it->second.call] { runWithOptions(command, c); };
}

void runWithOptions(const std::string& command, const json& call)
{
    try
    {
        cmds::execute(command, cmds::options::callWith(command, call), CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

std::string tooltip(const std::string& id)
{
    const Action* a = find(id);
    if (!a)
        return id;
    std::string key = Hotkeys::get().keyFor(id).toStdString();
    std::string tip = a->label + (key.empty() ? "" : "  (" + key + ")");
    return a->doc.empty() ? tip : tip + "\n" + a->doc;
}

const Action* find(const std::string& id)
{
    auto it = registry().find(id);
    return it == registry().end() ? nullptr : &it->second;
}

void setEnabledWhen(const std::string& id, std::function<bool()> enabled)
{
    if (auto it = registry().find(id); it != registry().end())
        it->second.enabled = std::move(enabled);
}

bool isEnabled(const json& target)
{
    if (!target.is_string())
        return true;
    const Action* a = find(target.get<std::string>());
    return !a || !a->enabled || a->enabled();
}

bool hasSelection()
{
    return !scene::Graph::current().selection().empty();
}

std::vector<const Action*> all()
{
    std::vector<const Action*> out;
    for (auto& [id, a] : registry())
        out.push_back(&a);
    return out;
}

bool run(const json& target)
{
    if (target.is_string())
    {
        const Action* a = find(target.get<std::string>());
        if (!a)
        {
            log::warn("no action named '{}'", target.get<std::string>());
            return false;
        }
        if (a->enabled && !a->enabled())
            return false;
        a->run();
        remember(target);
        return true;
    }
    if (!target.is_object())
        return false;
    if (target.contains("command"))
    {
        try
        {
            cmds::execute(target["command"].get<std::string>(), target.value("args", json::object()), CallSource::UI);
            remember(target);
            return true;
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            return false;
        }
    }
    if (target.contains("python"))
        return python::run(target["python"].get<std::string>(), "<hotkey>").ok;
    return false;
}

std::vector<json> recent()
{
    return recentList();
}

std::string labelFor(const json& target)
{
    if (target.is_string())
    {
        const Action* a = find(target.get<std::string>());
        return a ? a->label : target.get<std::string>();
    }
    if (target.is_object())
    {
        if (target.contains("label"))
            return target["label"].get<std::string>();
        if (target.contains("command"))
            return target["command"].get<std::string>();
        if (target.contains("markingMenu"))
            return target["markingMenu"].get<std::string>() + " menu";
        if (target.contains("python"))
            return "Python";
    }
    return "?";
}

std::string pythonFor(const json& target)
{
    if (target.is_string())
    {
        const Action* a = find(target.get<std::string>());
        if (a && !a->python.empty())
            return a->python;
        return "lila.ui.run_action(" + json(target.get<std::string>()).dump() + ")";
    }
    if (target.is_object())
    {
        if (target.contains("python"))
            return target["python"].get<std::string>();
        if (target.contains("command"))
            return cmds::toPython(target["command"].get<std::string>(), target.value("args", json::object()));
        if (target.contains("markingMenu"))
            return "# opens the '" + target["markingMenu"].get<std::string>() + "' marking menu";
    }
    return "";
}

void registerBuiltins()
{
    auto cmd = [](std::string id, std::string label, std::string category, const char* command, json call, std::string doc) {
        std::string py = cmds::toPython(command, call);
        add({.id = std::move(id), .label = std::move(label), .category = std::move(category), .doc = std::move(doc), .python = py,
             .command = command, .call = call});
    };
    cmd("edit.undo", "Undo", "Edit", "undo", json::object(), "Steps back through the undo history.");
    cmd("edit.redo", "Redo", "Edit", "redo", json::object(), "Reapplies the last change you undid.");
    cmd("edit.flushUndo", "Flush Undo", "Edit", "flushUndo", json::object(), "Clears the undo history so undo cannot go back past this point.");
    cmd("edit.flushRedo", "Flush Redo", "Edit", "flushUndo", {{"which", "future"}}, "Clears the redo steps, keeping the undo history.");
    cmd("edit.flushAll", "Flush All Undo", "Edit", "flushUndo", {{"which", "all"}}, "Clears both the undo and the redo history.");
    // Delete removes components in component mode, objects otherwise.
    add({"edit.delete", "Delete", "Edit", [] {
             bool components = runCommandResult("selectMode", json::object()) != "object" &&
                               !runCommandResult("componentSelection", json::object()).empty();
             runCommand(components ? "deleteComponents" : "delete", json::object());
         },
         nullptr, "Removes the selected objects, or the selected components in component mode.",
         "delete()  # or deleteComponents() in component mode"});
    cmd("select.hierarchy", "Select Hierarchy", "Select", "selectHierarchy", json::object(), "Adds everything below the selected objects to the selection.");
    cmd("select.pickWalkUp", "Pick Walk Up (Parent)", "Select", "pickWalk", {{"direction", "up"}}, "Moves the selection to each object's parent.");
    cmd("select.pickWalkDown", "Pick Walk Down (Child)", "Select", "pickWalk", {{"direction", "down"}}, "Moves the selection to each object's first child.");
    cmd("select.pickWalkLeft", "Pick Walk Left (Previous Sibling)", "Select", "pickWalk", {{"direction", "left"}},
        "Moves the selection to the object above it in the outliner.");
    cmd("select.pickWalkRight", "Pick Walk Right (Next Sibling)", "Select", "pickWalk", {{"direction", "right"}},
        "Moves the selection to the object below it in the outliner.");
    cmd("edit.group", "Group", "Edit", "group", json::object(), "Parents the selected objects under a new group transform.");
    cmd("edit.duplicate", "Duplicate", "Edit", "duplicate", json::object(), "Copies the selected objects in place and selects the copies.");
    cmd("modify.centerPivot", "Center Pivot", "Modify", "centerPivot", json::object(), "Moves each selected object's pivot to the center of its bounds.");
    cmd("display.hide", "Hide Selection", "Display", "hide", json::object(), "Hides the selected objects in every viewport.");
    cmd("display.toggleVisibility", "Toggle Visibility", "Display", "toggleVisibility", json::object(),
        "Hides the selected objects that are shown and shows the ones that are hidden.");
    cmd("display.smoothOff", "Smooth Preview Off", "Display", "smoothPreview", {{"level", 0}}, "Draws the selected meshes as their plain cage.");
    cmd("display.smoothCage", "Smooth Preview (1 level)", "Display", "smoothPreview", {{"level", 1}},
        "Previews the selected meshes subdivided once. Display only; the mesh is unchanged.");
    cmd("display.smoothOn", "Smooth Preview", "Display", "smoothPreview", {{"level", 2}},
        "Previews the selected meshes subdivided and smooth. Display only; the mesh is unchanged.");
    cmd("display.show", "Show Selection", "Display", "show", json::object(), "Unhides the selected objects.");
    cmd("display.showAll", "Show All", "Display", "show", {{"all", true}}, "Unhides every hidden object.");
    for (const char* id : {"display.hide", "display.show", "display.smoothOff", "display.smoothCage", "display.smoothOn"})
        setEnabledWhen(id, hasSelection);
    cmd("modify.freezeTransforms", "Freeze Transformations", "Modify", "freezeTransforms", json::object(),
        "Bakes the selected objects' transforms into their geometry, leaving translate and rotate at 0 and scale at 1.");
    cmd("modify.resetTransforms", "Reset Transformations", "Modify", "resetTransforms", json::object(),
        "Puts translate, rotate and scale back to their defaults, moving the object.");
    cmd("modify.resetPivot", "Reset Pivot", "Modify", "resetPivot", json::object(), "Moves the pivot back to the object's origin.");
    cmd("edit.parent", "Parent", "Edit", "parent", json::object(), "Parents the selected objects under the one selected last.");
    cmd("edit.unparent", "Unparent", "Edit", "parent", {{"world", true}}, "Moves the selected objects to the top of the hierarchy, keeping where they are.");
    cmd("edit.ungroup", "Ungroup", "Edit", "ungroup", json::object(), "Moves the selected groups' children up a level and deletes the groups.");
    cmd("edit.parentChain", "Parent Chain", "Edit", "parent", {{"chain", true}}, "Parents the selected objects into a chain, in selection order.");
    add({"edit.selectAll", "Select All", "Edit", [] {
              try
              {
                  json all = cmds::execute("ls", {{"dag", true}});
                  cmds::execute("select", {{"args", all}}, CallSource::UI);
              }
              catch (const CommandError& e)
              {
                  log::error("{}", e.what());
              }
          }, nullptr, "Selects every object in the scene.", "select(ls(dag=True))"});
    cmd("edit.deselect", "Deselect", "Edit", "select", {{"clear", true}}, "Clears the selection.");
    cmd("file.new", "New Scene", "File", "newScene", json::object(), "Clears the scene and starts an empty one.");
    cmd("create.locator", "Locator", "Create", "spaceLocator", json::object(), "Creates a cross-hair transform to mark a spot, snap to or parent under.");
    cmd("create.camera", "Camera", "Create", "createNode", {{"args", {"camera"}}},
        "Creates a camera at the origin looking down -Z; look through it from the view's camera menu or by MMB-dragging it from the outliner onto a view.");
    cmd("create.emptyGroup", "Empty Group", "Create", "group", {{"empty", true}}, "Creates an empty transform to group objects under.");
    for (auto [mode, doc] : {std::pair{"object", "Selects and transforms whole objects."},
                             {"vertex", "Picks and edits the selected meshes' vertices."},
                             {"edge", "Picks and edits the selected meshes' edges."},
                             {"face", "Picks and edits the selected meshes' faces."}})
    {
        std::string label = std::string(1, char(std::toupper(mode[0]))) + (mode + 1) + " Mode";
        cmd(std::string("select.") + mode + "Mode", label, "Select", "selectMode", {{"args", {mode}}}, doc);
    }
    cmd("select.grow", "Grow Selection", "Select", "growSelection", json::object(), "Adds the components bordering the selection.");
    cmd("select.loop", "Select Edge Loop", "Select", "selectLoop", json::object(), "Extends the selected edges along their loops.");
    cmd("select.ring", "Select Edge Ring", "Select", "selectLoop", {{"ring", true}}, "Selects the parallel edges around each selected edge's ring.");
    cmd("select.allComponents", "Select All Components", "Select", "selectAllComponents", json::object(),
        "Selects every component of the current type on the selected meshes.");
    cmd("select.toVertices", "Convert to Vertices", "Select", "convertSelection", {{"args", {"vertex"}}}, "Turns the selection into the vertices it touches.");
    cmd("select.toEdges", "Convert to Edges", "Select", "convertSelection", {{"args", {"edge"}}}, "Turns the selection into the edges it touches.");
    cmd("select.toFaces", "Convert to Faces", "Select", "convertSelection", {{"args", {"face"}}}, "Turns the selection into the faces it touches.");

    cmd("mesh.extrude", "Extrude", "Mesh", "polyExtrude", json::object(), "Pulls the selected faces or edges out, adding side faces.");
    cmd("mesh.inset", "Inset", "Mesh", "polyInset", json::object(), "Shrinks a new face inside each selected face, ringed by quads.");
    cmd("mesh.bevel", "Bevel", "Mesh", "polyBevel", json::object(), "Cuts the selected edges into chamfers or rounded strips, once. The Bevel modifier stays live.");
    cmd("mesh.insertLoop", "Insert Edge Loop", "Mesh", "polyInsertLoop", json::object(), "Adds an edge loop across the ring of the selected edge.");
    cmd("mesh.merge", "Merge", "Mesh", "polyMerge", json::object(), "Welds selected vertices that lie within the merge distance.");
    cmd("mesh.mergeCenter", "Merge to Center", "Mesh", "polyMergeCenter", json::object(), "Collapses the selection to a single point at its center.");
    cmd("mesh.collapse", "Collapse", "Mesh", "polyCollapse", json::object(), "Collapses each selected edge or face to a point.");
    cmd("mesh.dissolve", "Dissolve", "Mesh", "polyDissolve", json::object(), "Removes the selected edges or vertices, joining the faces around them.");
    cmd("mesh.bridge", "Bridge", "Mesh", "polyBridge", json::object(), "Joins two selected borders or face sets with a band of faces.");
    cmd("mesh.poke", "Poke", "Mesh", "polyPoke", json::object(), "Splits each selected face into triangles around a new center vertex.");
    cmd("mesh.spin", "Spin Edge", "Mesh", "polySpinEdge", json::object(), "Turns the selected edges to join the next corners of their two faces.");
    cmd("mesh.fillHole", "Fill Hole", "Mesh", "polyFillHole", json::object(), "Caps the selected open borders with a face.");
    cmd("mesh.flip", "Flip Normals", "Mesh", "polyFlip", json::object(), "Reverses the selected faces so they point the other way.");
    cmd("mesh.conform", "Conform Normals", "Mesh", "polyConform", json::object(), "Turns faces to agree with their neighbours, so the whole shell faces one way.");
    cmd("mesh.triangulate", "Triangulate", "Mesh", "polyTriangulate", json::object(), "Splits the selected faces into triangles.");
    cmd("mesh.quadrangulate", "Quadrangulate", "Mesh", "polyQuadrangulate", json::object(), "Joins pairs of triangles into quads where they make good ones.");
    cmd("mesh.reduce", "Reduce", "Mesh", "polyReduce", json::object(), "Lowers the polygon count, keeping the shape, UVs and borders.");
    cmd("mesh.subdivide", "Subdivide", "Mesh", "polySubdivide", json::object(), "Splits the selected faces into smaller ones.");
    cmd("mesh.mirror", "Mirror", "Mesh", "polyMirror", json::object(), "Adds a mirrored, welded copy of the mesh, once. The Mirror modifier stays live.");
    cmd("mesh.separate", "Separate", "Mesh", "polySeparate", json::object(), "Splits a mesh into one object per disconnected piece.");
    cmd("mesh.combine", "Combine", "Mesh", "polyCombine", json::object(), "Merges the selected meshes into one object.");
    cmd("mesh.deleteHistory", "Delete History", "Mesh", "deleteHistory", json::object(),
        "Bakes the generator and modifier stack into the selected meshes, leaving plain geometry.");
    cmd("mesh.transferAttributes", "Transfer Attributes", "Mesh", "transferAttributes", json::object(),
        "Copies UVs, normals or other attributes from the first selected mesh onto the others.");
    cmd("mesh.quadRemesh", "Quad Remesh", "Mesh", "quadRemesh", json::object(), "Rebuilds the mesh as evenly sized, flowing quads.");
    cmd("mesh.repair", "Repair Mesh", "Mesh", "meshRepair", json::object(),
        "Fixes coincident points, lamina and zero-area faces, non-manifold geometry and flipped faces; the option box picks the checks.");
    cmd("mesh.selectIssues", "Select Mesh Issues", "Mesh", "meshRepair", {{"mode", "select"}},
        "Selects what Repair Mesh would fix, without changing anything, and logs the counts per check.");
    cmd("mesh.voxelRemesh", "Voxel Remesh", "Mesh", "voxelRemesh", json::object(),
        "Rebuilds the mesh as one closed surface from a voxel grid, closing holes and fusing shells.");
    cmd("mesh.relax", "Relax", "Mesh", "relaxMesh", json::object(), "Evens out the spacing of the selected vertices, keeping the shape.");
    cmd("mesh.booleanDifference", "Boolean Difference", "Mesh", "polyBoolean", {{"operation", "difference"}},
        "Cuts the other selected meshes out of the first.");
    cmd("mesh.booleanUnion", "Boolean Union", "Mesh", "polyBoolean", {{"operation", "union"}},
        "Merges the selected meshes into one solid, removing the parts inside.");
    cmd("mesh.booleanIntersection", "Boolean Intersection", "Mesh", "polyBoolean", {{"operation", "intersection"}},
        "Keeps only the volume the selected meshes share.");
    for (const std::string& kind : scene::modifiers::kinds())
        cmd("modifier." + kind, modifierLabel(kind), "Modifiers", "addModifier", {{"kind", kind}}, scene::modifiers::kindDoc(kind));
    cmd("mesh.symmetryX", "Symmetry X", "Mesh", "setSymmetry", {{"axis", "x"}}, "Mirrors selections and edits across the object's X plane.");
    cmd("mesh.symmetryOff", "Symmetry Off", "Mesh", "setSymmetry", {{"axis", "off"}}, "Stops mirroring selections and edits.");
    cmd("mesh.symmetryY", "Symmetry Y", "Mesh", "setSymmetry", {{"axis", "y"}}, "Mirrors selections and edits across the object's Y plane.");
    cmd("mesh.symmetryZ", "Symmetry Z", "Mesh", "setSymmetry", {{"axis", "z"}}, "Mirrors selections and edits across the object's Z plane.");
    cmd("mesh.toggleSymmetry", "Toggle Symmetry", "Mesh", "toggleSymmetry", json::object(),
        "Turns symmetry on for the selected meshes (on the axis last used), or off when they all have it on.");
    cmd("mesh.symmetryCheck", "Symmetry Check", "Mesh", "symmetryCheck", json::object(), "Selects the vertices that have no mirror partner.");
    cmd("mesh.symmetrizePositive", "Symmetrize (Keep + Side)", "Mesh", "symmetrize", {{"side", "positive"}},
        "Copies the + side of the mesh onto the - side.");
    cmd("mesh.symmetrizeNegative", "Symmetrize (Keep - Side)", "Mesh", "symmetrize", {{"side", "negative"}},
        "Copies the - side of the mesh onto the + side.");
    cmd("mesh.symmetryRebuild", "Rebuild Symmetry", "Mesh", "symmetryRebuild", json::object(), "Pairs mirror vertices again from their positions.");
    cmd("mesh.symmetryRebuildTopological", "Rebuild Symmetry from Edge", "Mesh", "symmetryRebuild", {{"topological", true}},
        "Pairs mirror vertices by topology, outward from the selected center edge. Works on posed or uneven meshes.");
    cmd("mesh.extract", "Extract", "Mesh", "polyExtract", json::object(), "Moves the selected faces into a new object.");
    cmd("mesh.connect", "Connect", "Mesh", "polyConnect", json::object(), "Splits faces with new edges between the selected vertices or edges.");
    cmd("select.shrink", "Shrink Selection", "Select", "shrinkSelection", json::object(), "Drops the components on the selection's border.");
    cmd("select.border", "Select Border Edges", "Select", "selectBorder", json::object(), "Selects the open border edges of the selected meshes.");
    cmd("select.byAngle", "Select by Angle", "Select", "selectByAngle", json::object(), "Selects edges whose faces meet sharper than an angle.");
    actions::add({.id = "select.toggleCameraBased", .label = "Camera-Based Selection", .category = "Select", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/select/cameraBased", false);
                      PrefStore::get().set("general", "/select/cameraBased", !on);
                  },
                  .doc = "Marquee selection only picks components the camera can see.",
                  .python = "lila.prefs.set(\"general\", \"/select/cameraBased\", True)"});

    for (auto [id, label, command, doc] : {
             std::tuple{"create.cube", "Cube", "polyCube", "Creates a box."},
             {"create.sphere", "Sphere", "polySphere", "Creates a UV sphere, quads with triangles at the poles."},
             {"create.cylinder", "Cylinder", "polyCylinder", "Creates a capped cylinder."},
             {"create.cone", "Cone", "polyCone", "Creates a cone with a capped base."},
             {"create.plane", "Plane", "polyPlane", "Creates a flat grid of quads."},
             {"create.torus", "Torus", "polyTorus", "Creates a ring-shaped tube."},
             {"create.disc", "Disc", "polyDisc", "Creates a flat round disc."},
             {"create.pipe", "Pipe", "polyPipe", "Creates a hollow tube with wall thickness."},
             {"create.quadSphere", "Quad Sphere", "polyQuadSphere", "Creates an all-quad sphere from a subdivided cube, with no poles."},
             {"create.icoSphere", "Ico Sphere", "polyIcoSphere", "Creates an evenly triangulated sphere from a subdivided icosahedron."},
         })
        cmd(id, label, "Create", command, json::object(), std::string(doc) + " Its size and subdivisions stay editable in the channel box INPUTS.");
    auto display = [](const char* id, const char* label, const char* mode, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Display",
                      .run = [mode] { PrefStore::get().set("general", "/viewport/displayMode", mode); },
                      .doc = doc,
                      .python = std::string("lila.prefs.set(\"general\", \"/viewport/displayMode\", \"") + mode + "\")"});
    };
    display("viewport.wireframe", "Wireframe", "wireframe", "Draws meshes as their edges only.");
    display("viewport.shaded", "Shaded", "shaded", "Draws meshes as lit surfaces.");
    actions::add({.id = "viewport.wireOnShaded", .label = "Wireframe on Shaded", .category = "Display", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/viewport/wireOnShaded", false);
                      PrefStore::get().set("general", "/viewport/wireOnShaded", !on);
                  },
                  .doc = "Draws mesh edges over the shaded surfaces."});
    actions::add({.id = "viewport.toggleAO", .label = "Ambient Occlusion", .category = "Display", .run = [] {
                      bool on = PrefStore::get().value<bool>("general", "/viewport/ao/enabled", true);
                      PrefStore::get().set("general", "/viewport/ao/enabled", !on);
                  },
                  .doc = "Darkens creases and contact areas, so forms read better.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/ao/enabled\", not lila.prefs.get(\"general\", \"/viewport/ao/enabled\", True))"});

    namespace vd = viewportDisplay;
    auto togglePref = [](const char* id, const char* label, const char* key, bool fallback, const char* doc) {
        std::string k = key;
        std::string fb = fallback ? "True" : "False";
        actions::add({.id = id, .label = label, .category = "Display",
                      .run = [k, fallback] { PrefStore::get().set("general", k, !PrefStore::get().value<bool>("general", k, fallback)); },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"" + k + "\", not lila.prefs.get(\"general\", \"" + k + "\", " + fb + "))"});
    };
    togglePref("viewport.toggleAA", "Antialiasing", vd::kAaEnabled, true, "Multisample antialiasing in every viewport.");
    togglePref("viewport.toggleGrid", "Grid", vd::kGrid, true, "The ground grid in every viewport.");
    togglePref("viewport.toggleXray", "X-Ray", vd::kXray, false, "Shaded meshes become see-through.");
    togglePref("viewport.toggleBackfaceCulling", "Backface Culling", vd::kBackfaceCulling, false, "Hides the back faces of shaded meshes.");
    togglePref("viewport.toggleShadows", "Shadows", vd::kShadows, true, "The soft ground shadow under meshes.");
    togglePref("viewport.defaultMaterial", "Use Default Material", vd::kDefaultMaterial, false, "Shades every mesh with the plain default material.");
    togglePref("viewport.toggleEnvBackground", "Environment Background", "/viewport/env/background", true, "Shows the HDRI behind the scene.");
    togglePref("viewport.toggleToolbar", "Show Toolbar", vd::kToolbar, true, "The icon row at the top of each viewport panel.");
    togglePref("viewport.togglePreselectObjects", "Highlight Objects Under Cursor", "/viewport/preselect/objects", true,
               "In object mode, the object under the cursor highlights before it is clicked.");
    for (int n : {2, 4, 8})
    {
        std::string label = "Antialiasing " + std::to_string(n) + "x";
        actions::add({.id = "viewport.aaSamples" + std::to_string(n), .label = label, .category = "Display", .run = [n] {
                          PrefStore::get().set("general", vd::kAaSamples, n);
                          PrefStore::get().set("general", vd::kAaEnabled, true);
                      },
                      .doc = std::to_string(n) + " samples per pixel. More is smoother and costs more GPU time.",
                      .python = "lila.prefs.set(\"general\", \"/viewport/aa/samples\", " + std::to_string(n) + ")"});
    }
    for (auto [mode, label, doc] : {std::tuple{"environment", "Environment Lighting", "Lights the scene with the HDRI environment."},
                                    {"studio", "Studio Lighting", "Lights the scene with a fixed studio rig, whatever the HDRI."},
                                    {"flat", "Flat Lighting", "Unlit base colors, for checking textures and silhouettes."}})
    {
        std::string m = mode;
        actions::add({.id = "viewport.lighting." + m, .label = label, .category = "Display",
                      .run = [m] { PrefStore::get().set("general", vd::kLighting, m); },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"/viewport/lighting\", \"" + m + "\")"});
    }
    for (auto [mode, label, doc] : {std::tuple{"off", "Hide Vertex Colors", "Meshes draw without their vertex colors."},
                                    {"shaded", "Vertex Colors (Shaded)", "Multiplies each mesh's current color set into its lit base color."},
                                    {"unlit", "Vertex Colors (Unlit)", "Shows each mesh's current color set flat, without lighting."}})
    {
        std::string m = mode;
        actions::add({.id = "viewport.vertexColors." + m, .label = label, .category = "Display",
                      .run = [m] { PrefStore::get().set("general", vd::kVertexColors, m); },
                      .doc = doc,
                      .python = "lila.prefs.set(\"general\", \"/viewport/vertexColors\", \"" + m + "\")"});
    }
    actions::add({.id = "viewport.toggleVertexColors", .label = "Vertex Colors", .category = "Display", .run = [] {
                      bool on = PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") != "off";
                      PrefStore::get().set("general", vd::kVertexColors, on ? "off" : "shaded");
                  },
                  .doc = "Shows or hides every mesh's current color set (shaded)."});
    for (auto [channel, label, doc] : {std::tuple{"rgb", "All Channels", "Vertex colors show in full color."},
                                       {"r", "Red Channel", "Shows only the red channel, as grayscale."},
                                       {"g", "Green Channel", "Shows only the green channel, as grayscale."},
                                       {"b", "Blue Channel", "Shows only the blue channel, as grayscale."},
                                       {"a", "Alpha Channel", "Shows only the alpha channel, as grayscale."}})
    {
        std::string c = channel;
        actions::add({.id = "viewport.colorChannel." + c, .label = label, .category = "Display",
                      .run = [c] {
                          PrefStore::get().set("general", vd::kColorChannel, c);
                          if (PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") == "off")
                              PrefStore::get().set("general", vd::kVertexColors, "unlit");
                      },
                      .doc = std::string(doc) + " Unreal materials often read single channels as masks.",
                      .python = "lila.prefs.set(\"general\", \"/viewport/colorChannel\", \"" + c + "\")"});
    }
    togglePref("viewport.grayscaleWeights", "Grayscale Weight Display", vd::kGrayscaleWeights, false,
               "Skin weights and the soft select falloff draw as a gray ramp instead of colours.");
    {
        namespace sm = scene::selectionMask;
        for (auto [kind, label, doc] : {std::tuple{sm::Kind::Mesh, "Select Polygon Objects", "Meshes can be picked in object mode."},
                                        {sm::Kind::Joint, "Select Joints", "Joints can be picked in object mode."},
                                        {sm::Kind::Locator, "Select Locators", "Locators can be picked in object mode."},
                                        {sm::Kind::Other, "Select Other Objects", "Cameras, image planes and measurements can be picked in object mode."}})
        {
            sm::Kind k = kind;
            std::string key = sm::prefKey(k);
            actions::add({.id = std::string("select.mask.") + sm::name(k), .label = label, .category = "Select",
                          .run = [k] { sm::setEnabled(k, !sm::enabled(k)); },
                          .doc = std::string(doc) + " Off masks them out of clicks, marquees and the highlight under the cursor.",
                          .python = "lila.prefs.set(\"general\", \"" + key + "\", not lila.prefs.get(\"general\", \"" + key + "\", True))"});
        }
    }
    actions::add({.id = "viewport.resetExposure", .label = "Reset Exposure and Gamma", .category = "Display", .run = [] {
                      PrefStore::get().set("general", vd::kExposure, 0.0);
                      PrefStore::get().set("general", vd::kGamma, 1.0);
                  },
                  .doc = "Puts the viewport exposure back to 0 and gamma to 1."});
}
} // namespace lila::ui::actions
