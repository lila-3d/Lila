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

TEST_CASE("Monotone leaves greys and style sheets exactly as written")
{
    UseTheme mono("Monotone");
    for (int v : {0, 18, 34, 58, 105, 150, 212, 255})
        CHECK(ui::themeShade(v, 90) == QColor(v, v, v, 90));
    QString sheet = "QToolBar { background: #2c2c2c; border: 1px solid #3a3a3a; color: rgb(150,150,150); }";
    CHECK(ui::themedStyleSheet(sheet) == sheet);
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
