#include <doctest/doctest.h>

#include "ui/MayaHotkeys.h"

using namespace lila::ui;
using mayaHotkeys::MayaHotkey;
using nlohmann::json;

namespace
{
const char* kSet = R"MEL(//Maya Preference 2025 (Release 1)
//
//

runTimeCommand
	-annotation "Wraps move"
	-category "Custom Scripts"
	-commandLanguage "mel"
	-command ("MoveTool")
	MyMove;

nameCommand
	-annotation "MyMoveNameCommand"
	-sourceType "mel"
	-command ("MyMove")
	MyMoveNameCommand;

nameCommand
	-annotation "printHi"
	-sourceType "python"
	-command ("print(\"hi; there\")")
	printHiNameCommand;

hotkeySet -source "Maya_Default" -current Custom;
hotkeyCtx -type "Editor" -addClient "nodeEditorPanel";
hotkey -keyShortcut "w" -name ("TranslateToolWithSnapMarkingMenuNameCommand") -releaseName ("TranslateToolWithSnapMarkingMenuPopDownNameCommand");
hotkey -keyShortcut "q" -ctl -name ("MyMoveNameCommand");
hotkey -keyShortcut "D" -alt -name ("DuplicateWithTransformNameCommand");
hotkey -keyShortcut "x" -name ("SnapToGridPressNameCommand") -releaseName ("SnapToGridReleaseNameCommand");
hotkey -keyShortcut "Page_Up" -sht -name ("NextKeyNameCommand");
hotkey -keyShortcut "~" -name ("UndoNameCommand");
hotkey -keyShortcut "h" -alt -name ("");
hotkey -keyShortcut "p" -name ("printHiNameCommand");
hotkey -keyShortcut "k" -name ("SomethingMayaOnlyNameCommand");
hotkey -keyShortcut "f" -name ("FrameSelectedWithoutChildrenNameCommand") -ctxClient "nodeEditorPanel";
hotkey -keyShortcut "f" -name ("FrameSelectedWithoutChildrenNameCommand") -ctxClient "hyperGraphPanel";
)MEL";

QKeyCombination chord(int mods, int key)
{
    return QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(key));
}

const hotkeys::Assignment* findAssignment(const mayaHotkeys::Plan& p, const std::string& context, QKeyCombination c,
                                          hotkeys::Slot slot)
{
    for (const auto& a : p.assignments)
        if (a.context == context && a.chord == c && a.slot == slot)
            return &a;
    return nullptr;
}
} // namespace

TEST_CASE("Maya hotkey keys become Lila chords")
{
    auto c = [](std::string key, bool ctrl = false, bool alt = false, bool shift = false) {
        MayaHotkey h;
        h.key = std::move(key);
        h.ctrl = ctrl;
        h.alt = alt;
        h.shift = shift;
        return mayaHotkeys::chordFor(h);
    };
    CHECK(c("w") == chord(0, Qt::Key_W));
    CHECK(c("W") == chord(Qt::ShiftModifier, Qt::Key_W));
    CHECK(c("w", true, true, true) == chord(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, Qt::Key_W));
    CHECK(c("Page_Down") == chord(0, Qt::Key_PageDown));
    CHECK(c("F8") == chord(0, Qt::Key_F8));
    CHECK(c("F12", true) == chord(Qt::ControlModifier, Qt::Key_F12));
    CHECK(c("Return") == chord(0, Qt::Key_Return));
    CHECK(c(" ") == chord(0, Qt::Key_Space));
    CHECK(c("~") == chord(Qt::ShiftModifier, Qt::Key_QuoteLeft));
    CHECK(c(">") == chord(Qt::ShiftModifier, Qt::Key_Period));
    CHECK(c("\\") == chord(0, Qt::Key_Backslash));
    CHECK_FALSE(c("F13"));
    CHECK_FALSE(c("NumPad5"));
}

TEST_CASE("Maya hotkey sets parse hotkeys and custom commands")
{
    mayaHotkeys::Parsed parsed;
    mayaHotkeys::parse(kSet, parsed);
    REQUIRE(parsed.hotkeys.size() == 11);
    CHECK(parsed.hotkeys[0].key == "w");
    CHECK(parsed.hotkeys[0].release == "TranslateToolWithSnapMarkingMenuPopDownNameCommand");
    CHECK(parsed.hotkeys[1].ctrl);
    CHECK(parsed.hotkeys[2].alt);
    CHECK(parsed.hotkeys[4].shift);
    CHECK(parsed.hotkeys[6].hasPress);
    CHECK(parsed.hotkeys[6].press.empty());
    CHECK(parsed.hotkeys[9].editor == "nodeEditorPanel");
    REQUIRE(parsed.nameCommands.contains("printHiNameCommand"));
    CHECK(parsed.nameCommands["printHiNameCommand"].language == "python");
    CHECK(parsed.nameCommands["printHiNameCommand"].command == "print(\"hi; there\")");
    CHECK(parsed.runTimeCommands["MyMove"].command == "MoveTool");
}

TEST_CASE("Maya hotkey sets map to Lila actions and contexts")
{
    mayaHotkeys::Parsed parsed;
    mayaHotkeys::parse(kSet, parsed);
    mayaHotkeys::Plan p = mayaHotkeys::plan(parsed);
    using hotkeys::Slot;

    auto target = [&](const std::string& ctx, QKeyCombination c, Slot slot = Slot::Press) {
        const hotkeys::Assignment* a = findAssignment(p, ctx, c, slot);
        return a ? a->target : json("<none>");
    };
    CHECK(target("global", chord(0, Qt::Key_W)) == "tool.move");
    // The marking menu's pop-down has no Lila counterpart and is dropped quietly.
    CHECK(target("global", chord(0, Qt::Key_W), Slot::Release) == "<none>");
    // A named command wrapping a runtime command that wraps MoveTool.
    CHECK(target("global", chord(Qt::ControlModifier, Qt::Key_Q)) == "tool.move");
    CHECK(target("global", chord(Qt::AltModifier | Qt::ShiftModifier, Qt::Key_D)) == "edit.duplicateWithTransform");
    CHECK(target("global", chord(0, Qt::Key_X)) == "snap.gridOn");
    CHECK(target("global", chord(0, Qt::Key_X), Slot::Release) == "snap.gridOff");
    CHECK(target("global", chord(Qt::ShiftModifier, Qt::Key_PageUp)) == "anim.nextKey");
    CHECK(target("global", chord(Qt::ShiftModifier, Qt::Key_QuoteLeft)) == "edit.undo");
    CHECK(target("global", chord(Qt::AltModifier, Qt::Key_H)).is_null());
    CHECK(target("nodeEditor", chord(0, Qt::Key_F)) == "nodeEditor.frameSelected");

    REQUIRE(p.skipped.size() == 3);
    CHECK(p.skipped[0].key == "P");
    CHECK(p.skipped[0].reason == "custom script");
    CHECK(p.skipped[1].command == "SomethingMayaOnly");
    CHECK(p.skipped[1].reason == "no Lila equivalent");
    CHECK(p.skipped[2].reason == "Maya editor hyperGraphPanel");
}

TEST_CASE("older Maya hotkey files use Maya's own named commands")
{
    const char* mel = R"MEL(
nameCommand -annotation "Wireframe Display"
	-command "DisplayWireframe"
	"NameComMyWire" ;
hotkey -k "4" -name "NameComMyWire";
hotkey -k "A" -ctl -name "NameComSelect_All";
hotkey -k "a" -name "NameComFit_All_in_Active_Panel_MMenu" -releaseName "NameComFit_All_in_Active_Panel_MMenu_release";
hotkey -k "Space" -name "NameComPop_hotBox" -releaseName "NameComUnpop_hotBox";
hotkey -k "x" -name "NameComSnap_to_Grid_modifierON" -releaseName "NameComSnap_to_Grid_modifierOFF";
)MEL";
    mayaHotkeys::Parsed parsed;
    mayaHotkeys::parse(mel, parsed);
    CHECK(parsed.nameCommands["NameComMyWire"].command == "DisplayWireframe");
    mayaHotkeys::Plan p = mayaHotkeys::plan(parsed);
    using hotkeys::Slot;
    auto target = [&](QKeyCombination c, Slot slot = Slot::Press) {
        const hotkeys::Assignment* a = findAssignment(p, "global", c, slot);
        return a ? a->target : json("<none>");
    };
    CHECK(target(chord(0, Qt::Key_4)) == "viewport.wireframe");
    CHECK(target(chord(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_A)) == "edit.selectAll");
    CHECK(target(chord(0, Qt::Key_A)) == "viewport.frameAll");
    CHECK(target(chord(0, Qt::Key_Space), Slot::Hold) == "hotbox.show");
    CHECK(target(chord(0, Qt::Key_Space)) == "<none>");
    CHECK(target(chord(0, Qt::Key_X), Slot::Release) == "snap.gridOff");
    CHECK(p.skipped.empty());
}
