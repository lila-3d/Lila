#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/Theme.h"
#include "ui/ThemeEditor.h"
#include "ui/ThemeModel.h"

#include <QWidget>

#include <memory>

using namespace lila;
namespace themes = lila::ui::themes;

namespace
{
struct UseTheme
{
    explicit UseTheme(const char* name)
    {
        PrefStore::get().set("general", themes::kThemePref, std::string(name));
        themes::invalidate();
    }
    ~UseTheme()
    {
        PrefStore::get().remove("general", themes::kThemePref);
        themes::invalidate();
    }
};
} // namespace

TEST_CASE("Monotone maps the greys widgets were written with onto its ramp")
{
    UseTheme mono("Monotone");
    // Panels sit at window (written as 48), fields and buttons one step above it (written as 36 and 58).
    CHECK(ui::themeShade(0, 90) == QColor(0, 0, 0, 90));
    CHECK(ui::themeShade(48) == QColor(0x28, 0x28, 0x28));
    CHECK(ui::themeShade(36) == QColor(0x33, 0x33, 0x33));
    CHECK(ui::themeShade(58) == QColor(0x33, 0x33, 0x33));
    CHECK(ui::themeShade(150) == QColor(0x8c, 0x8c, 0x8c));
    QString sheet = "QToolBar { background: #303030; color: rgb(150,150,150); border-color: #ff0000; }";
    CHECK(ui::themedStyleSheet(sheet) == "QToolBar { background: #282828; color: #8c8c8c; border-color: #ff0000; }");
    const glm::vec4 legacy(0.92f, 0.92f, 0.92f, 1.0f);
    CHECK(ui::themeLinear("wireSelected", legacy) == legacy);
}

TEST_CASE("other themes map style sheet greys and leave the rest")
{
    UseTheme light("Light");
    const themes::Theme& t = *themes::builtIn("Light");
    QString out = ui::themedStyleSheet("#modCard { background: #303030; border: 1px solid #ff0000; color: rgb(212, 212, 212); }");
    CHECK(out.startsWith("#modCard {"));
    CHECK(out.contains(QString::fromStdString(themes::formatColor(t.color("window")))));
    CHECK(out.contains("#ff0000"));
    CHECK(out.contains(QString::fromStdString(themes::formatColor(t.color("text")))));
    CHECK_FALSE(out.contains("rgb("));
    CHECK(ui::themeColor("highlight") == QColor(QString::fromStdString(themes::formatColor(t.color("highlight")))));
}

TEST_CASE("the theme editor and first-run chooser build")
{
    std::unique_ptr<QWidget> editor(ui::makeThemeEditor());
    CHECK(editor != nullptr);
    std::unique_ptr<QWidget> chooser(ui::makeThemeChooser(nullptr));
    CHECK(chooser != nullptr);
    // applyTheme was never called here, as in tests and batch runs: no first-run chooser.
    CHECK_FALSE(ui::themeChooserPending());
}
