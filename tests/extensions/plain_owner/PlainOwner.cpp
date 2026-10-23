// Test extension that registers through the plain overloads, which take no context and name no owner (or someone
// else's). Lila must still file everything under this extension and remove it before the library unloads.
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/UI.h>

#include <QLabel>

#include <memory>

using namespace lila;
using namespace lila::ui;

namespace
{
// Captured by every callback, so a callback outliving the library would run or destroy code that is gone.
std::shared_ptr<int> gAlive = std::make_shared<int>(42);
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context&)
{
    std::shared_ptr<int> alive = gAlive;
    actions::add({.id = "plainOwner.go", .label = "Go", .category = "Test", .run = [alive] { ++*alive; }, .doc = "Test action."});
    actions::add({.id = "plainOwner.claimsLila", .label = "Claims Lila", .run = [alive] {}, .doc = "Names Lila as its owner."}, kLilaOwner);
    menus::addItem({.menu = "File", .action = "plainOwner.go"});
    panels::add({.id = "plainOwnerPanel", .title = "Plain", .create = [alive](QWidget* p) -> QWidget* { return new QLabel("plain", p); }});
    markingMenus::add("plainOwnerMenu", {{"N", {{"label", "Go"}, {"action", "plainOwner.go"}}}});
    modes::add({.id = "plainOwnerMode", .label = "Plain", .onEnter = [alive] {}, .onExit = [alive] {}});
    optionBox::addCustom("plainOwner.go", [alive] {});
    cmds::registerCommand({.name = "plainOwnerCommand",
                           .doc = "Test command registered without a context.",
                           .undoable = false,
                           .run = [alive](const Args&) -> json { return *alive; }});
    return 0;
}
