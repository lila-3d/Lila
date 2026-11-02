#include <doctest/doctest.h>

#include "ui/ColorPopup.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QToolButton>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

using namespace lila::ui;

namespace
{
bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-4) { return glm::all(glm::lessThan(glm::abs(a - b), glm::dvec3(eps))); }
} // namespace

TEST_CASE("color picker: hex reads and writes sRGB, short forms and alpha")
{
    CHECK(colorMath::hex(glm::dvec3(1, 0, 0)) == "#FF0000");
    CHECK(colorMath::hex(glm::dvec3(colorMath::toLinear(128 / 255.0))) == "#808080");
    CHECK(colorMath::hex(glm::dvec3(0.5)) == "#BCBCBC");
    CHECK(colorMath::hex(glm::dvec3(4.0, -1.0, 0.0)) == "#FF0000");

    auto red = colorMath::parseHex("#f00");
    REQUIRE(red);
    CHECK(near(glm::dvec3(*red), {1, 0, 0}));
    auto grey = colorMath::parseHex("808080");
    REQUIRE(grey);
    CHECK(grey->x == doctest::Approx(0.2158).epsilon(0.001));
    auto alpha = colorMath::parseHex("0x00FF0080");
    REQUIRE(alpha);
    CHECK(alpha->w == doctest::Approx(128 / 255.0));
    CHECK_FALSE(colorMath::parseHex("#12345"));
    CHECK_FALSE(colorMath::parseHex("#GGGGGG"));
}

TEST_CASE("color picker: HSV keeps hue through grey and allows HDR value")
{
    const glm::dvec3 hsv = colorMath::rgbToHsv({0.2, 0.2, 0.2}, 0.3, 0.7);
    CHECK(hsv.x == doctest::Approx(0.3));
    CHECK(hsv.y == doctest::Approx(0.0));
    CHECK(hsv.z == doctest::Approx(0.2));
    CHECK(near(colorMath::hsvToRgb({0.0, 1.0, 4.0}), {4, 0, 0}));
    for (const glm::dvec3 c : {glm::dvec3(0.1, 0.5, 0.9), glm::dvec3(0.9, 0.2, 0.4), glm::dvec3(2.0, 1.0, 0.5)})
        CHECK(near(colorMath::hsvToRgb(colorMath::rgbToHsv(c)), c));
}

TEST_CASE("color picker: OKLCh round trips and gamut-maps into sRGB")
{
    for (const glm::dvec3 c : {glm::dvec3(0.1, 0.5, 0.9), glm::dvec3(0.9, 0.2, 0.4), glm::dvec3(0.5)})
        CHECK(near(colorMath::oklchToLinear(colorMath::linearToOklch(c)), c, 1e-3));
    const glm::dvec3 white = colorMath::linearToOklch({1, 1, 1});
    CHECK(white.x == doctest::Approx(1.0).epsilon(0.001));
    CHECK(white.y < 1e-3);
    // Far more chroma than sRGB holds at this lightness: still a valid colour, same hue.
    const glm::dvec3 mapped = colorMath::oklchToLinear({0.6, 0.5, 140.0});
    CHECK(std::min({mapped.x, mapped.y, mapped.z}) >= 0.0);
    CHECK(std::max({mapped.x, mapped.y, mapped.z}) <= 1.0 + 1e-5);
    CHECK(colorMath::linearToOklch(mapped).z == doctest::Approx(140.0).epsilon(0.02));
}

TEST_CASE("color picker: GIMP palettes round trip")
{
    const auto file = std::filesystem::temp_directory_path() / "lila_color_popup_test.gpl";
    colorLibrary::Palette p{"Test Set", {{colorMath::linear(QColor(255, 0, 0)), 1.0, "Red"}, {colorMath::linear(QColor(18, 52, 86)), 1.0, {}}}};
    REQUIRE(colorLibrary::writeGpl(file, p));
    std::string error;
    auto back = colorLibrary::readGpl(file, &error);
    REQUIRE(back);
    CHECK(back->name == "Test Set");
    REQUIRE(back->swatches.size() == 2);
    CHECK(back->swatches[0].name == "Red");
    CHECK(colorMath::hex(back->swatches[1].color) == "#123456");
    CHECK(back->swatches[1].name.empty());

    {
        std::ofstream bad(file);
        bad << "not a palette\n";
    }
    CHECK_FALSE(colorLibrary::readGpl(file, &error));
    CHECK_FALSE(error.empty());
    std::filesystem::remove(file);
}

TEST_CASE("color picker: popup shows hex, edits report live, Esc restores")
{
    std::vector<colorPopup::Value> live;
    std::optional<std::pair<colorPopup::Value, bool>> closed;
    colorPopup::Options o;
    o.value.color = {1.0, 0.0, 0.0};
    o.title = "Test";
    o.changed = [&](const colorPopup::Value& v) { live.push_back(v); };
    o.closed = [&](const colorPopup::Value& v, bool accepted) { closed = {v, accepted}; };
    colorPopup::show({100, 100}, std::move(o));

    QWidget* popup = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->isVisible() && (w->windowFlags() & Qt::Popup) == Qt::Popup)
            popup = w;
    REQUIRE(popup);
    QLineEdit* hex = nullptr;
    for (QLineEdit* e : popup->findChildren<QLineEdit*>())
        if (e->text().startsWith('#'))
            hex = e;
    REQUIRE(hex);
    CHECK(hex->text() == "#FF0000");

    if (const char* out = std::getenv("LILA_TEST_GRAB"))
        popup->grab().save(QString::fromUtf8(out));

    hex->setText("#00ff00");
    Q_EMIT hex->editingFinished();
    REQUIRE_FALSE(live.empty());
    CHECK(colorMath::hex(live.back().color) == "#00FF00");

    // The old swatch swaps: green goes left, red comes back, and a second click returns to green.
    QWidget* compare = nullptr;
    for (QWidget* w : popup->findChildren<QWidget*>())
        if (w->toolTip().startsWith("Left:"))
            compare = w;
    REQUIRE(compare);
    auto clickLeft = [&] {
        const QPointF at(4, 4);
        QMouseEvent press(QEvent::MouseButtonPress, at, compare->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(compare, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, compare->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(compare, &release);
    };
    clickLeft();
    CHECK(colorMath::hex(live.back().color) == "#FF0000");
    clickLeft();
    CHECK(colorMath::hex(live.back().color) == "#00FF00");

    QApplication::clipboard()->setText("#0000ff");
    for (QToolButton* b : popup->findChildren<QToolButton*>())
        if (b->toolTip().startsWith("Paste"))
            b->click();
    CHECK(colorMath::hex(live.back().color) == "#0000FF");
    CHECK(hex->text() == "#0000FF");

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(popup, &esc);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(closed);
    CHECK_FALSE(closed->second);
    CHECK(colorMath::hex(closed->first.color) == "#FF0000");
    CHECK(colorMath::hex(live.back().color) == "#FF0000");
}
