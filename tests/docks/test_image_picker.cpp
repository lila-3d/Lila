#include <doctest/doctest.h>

#include "scene/Node.h"
#include "ui/Environments.h"
#include "ui/ImagePicker.h"

#include <QComboBox>

#include <vector>

using namespace lila;
using namespace lila::ui;

TEST_CASE("image picker: dome texture lists the HDRIs with thumbnails and keeps the current one")
{
    scene::NodeType light;
    light.name = "light";
    scene::NodeType dome;
    dome.name = "myDomeLight";
    dome.base = &light;
    const imagePicker::Library* lib = imagePicker::libraryFor(dome, "texture");
    REQUIRE(lib);
    CHECK(lib->id == "hdri");

    const auto shipped = environments::builtIn();
    REQUIRE_FALSE(shipped.empty());
    CHECK_FALSE(imagePicker::thumbnail(shipped.front()).isNull());

    ImagePicker picker(lib, "Texture");
    const std::string name = shipped.front().filename().string();
    picker.setValue(name);
    auto* combo = picker.findChild<QComboBox*>();
    REQUIRE(combo);
    CHECK(combo->currentData().toString().toStdString() == name);
    CHECK_FALSE(combo->itemIcon(combo->currentIndex()).isNull());
    CHECK(combo->findText("Browse...") >= 0);

    std::vector<QString> chosen;
    QObject::connect(&picker, &ImagePicker::chosen, [&](const QString& v) { chosen.push_back(v); });
    const int other = combo->findData(QString::fromStdString(shipped.back().filename().string()));
    REQUIRE(other >= 0);
    Q_EMIT combo->activated(other);
    REQUIRE(chosen.size() == 1);
    CHECK(chosen[0].toStdString() == shipped.back().filename().string());

    picker.setValue("somewhere/missing.hdr");
    CHECK(combo->currentText().contains("missing"));
}
