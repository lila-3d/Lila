// Sample UI extension: an action in a menu of its own, a dockable panel, a preferences page, and a mode that takes
// Space's tap while Space's hold stays the hotbox. Its marking-menu set and shelf come from resources/.
#include <lila/Extension.h>
#include <lila/UI.h>

#include <QLabel>
#include <QVBoxLayout>

using namespace lila;
using namespace lila::ui;

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    actions::add(ctx, {.id = "uiMode.hello",
                       .label = "Say Hello",
                       .category = "UI Mode Example",
                       .run = [&ctx] { ctx.log(LogLevel::Info, "hello from the UI mode example"); },
                       .doc = "Logs a greeting: the action the example's menu, mode and marking menus run."});

    menus::addMenu(ctx, {.id = "UiModeExample", .title = "UI Example"});
    menus::addItem(ctx, {.menu = "UiModeExample", .action = "uiMode.hello"});
    menus::addItem(ctx, {.menu = "UiModeExample", .action = "mode.uiMode"});
    menus::addItem(ctx, {.menu = "Windows", .action = "window.uiModePanel"});

    panels::add(ctx, {.id = "uiModePanel", .title = "UI Example", .create = [](QWidget* parent) -> QWidget* {
                          auto* w = new QWidget(parent);
                          auto* v = new QVBoxLayout(w);
                          v->addWidget(new QLabel("A panel registered by an extension."));
                          v->addStretch(1);
                          return w;
                      },
                      .open = false});

    prefPages::add(ctx, {.id = "uiModeExample", .title = "UI Example", .file = "uiModeExample", .build = [](prefPages::PageBuilder& b) {
                             b.section("Greeting");
                             b.toggle("Greet on entering the mode", "/greetOnEnter", true);
                         }});

    modes::add(ctx, {.id = "uiMode",
                     .label = "UI Example",
                     .doc = "The sample extension's mode: Space taps say hello, Space held is still the hotbox.",
                     .hotkeys = {{"viewport", {{"Space", {{"press", "uiMode.hello"}}}}}},
                     .markingMenuSet = "uiMode",
                     .shelf = "UI Example",
                     .onEnter = [&ctx] { ctx.log(LogLevel::Info, "entered the UI example mode"); },
                     .onExit = [&ctx] { ctx.log(LogLevel::Info, "left the UI example mode"); }});
    return 0;
}
