// The Attribute Editor sections of live stages, built offscreen for a real stage and grabbed to PNGs beside the test's
// files (lila_usd_tests/panels) for a look.

#include "UsdTestUtil.h"

#include "ui/AttributeEditor.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstdlib>
#include <memory>

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;

namespace
{
/** Made on first use, offscreen and themed: no other USD test needs a QApplication. */
void ensureApp()
{
    if (QApplication::instance())
        return;
#ifdef _WIN32
    _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
#endif
    static int argc = 1;
    static char name[] = "lila_usd_tests";
    static char* argv[] = {name, nullptr};
    static auto* app = new QApplication(argc, argv);
    // Lila's fonts and colours, so the grabs look as the app draws them.
    ui::applyTheme(*app);
}

fs::path panelDir()
{
    const fs::path dir = tempDir() / "panels";
    fs::create_directories(dir);
    return dir;
}

/** The section of that title, in a frame as wide as a docked Attribute Editor, laid out and shown offscreen. */
std::unique_ptr<QWidget> section(NodeId id, const std::string& title)
{
    std::unique_ptr<QWidget> frame;
    for (auto& [t, w] : ui::attributeEditor::buildSections(id))
    {
        if (t != title || frame)
        {
            delete w;
            continue;
        }
        frame = std::make_unique<QWidget>();
        auto* v = new QVBoxLayout(frame.get());
        v->addWidget(w);
        v->addStretch(1);
    }
    if (!frame)
        return frame;
    frame->setFixedWidth(360);
    frame->adjustSize();
    frame->show();
    QApplication::processEvents();
    return frame;
}

void grab(QWidget& w, const char* file)
{
    const fs::path out = panelDir() / file;
    CHECK(w.grab().save(QString::fromStdWString(out.wstring()), "PNG"));
    MESSAGE("panel grab: " << out.string());
}

/** The field the form row labelled label holds. */
template <class W> W* row(QWidget& w, const QString& label)
{
    for (QFormLayout* form : w.findChildren<QFormLayout*>())
        for (int r = 0; r < form->rowCount(); ++r)
        {
            QLayoutItem* l = form->itemAt(r, QFormLayout::LabelRole);
            QLayoutItem* f = form->itemAt(r, QFormLayout::FieldRole);
            const auto* text = l ? qobject_cast<QLabel*>(l->widget()) : nullptr;
            if (!text || text->text() != label || !f)
                continue;
            if (!f->layout())
                return qobject_cast<W*>(f->widget());
            for (int i = 0; i < f->layout()->count(); ++i)
                if (W* found = qobject_cast<W*>(f->layout()->itemAt(i)->widget()))
                    return found;
        }
    return nullptr;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool labelled(QWidget& w, const QString& part)
{
    for (QLabel* l : w.findChildren<QLabel*>())
        if (l->text().contains(part))
            return true;
    return false;
}

const std::string kMesh = "def Mesh \"Mesh\" (\n    variants = {\n        string look = \"a\"\n    }\n    prepend variantSets = \"look\"\n)\n{\n"
                          "    int[] faceVertexCounts = [3]\n    int[] faceVertexIndices = [0, 1, 2]\n    variantSet \"look\" = {\n"
                          "        \"a\" {\n            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n        }\n"
                          "        \"b\" {\n            point3f[] points = [(0, 0, 0), (5, 0, 0), (0, 1, 0)]\n        }\n    }\n}\n";

/** A stage with variant sets, LOD levels holding variant sets of their own, a payload, an instance and custom properties. */
std::string panelStage()
{
    std::string lod;
    for (const char* level : {"LOD0", "LOD1"})
    {
        lod += std::string("            \"") + level + "\" {\n";
        std::string line;
        for (char c : kMesh)
        {
            line += c;
            if (c == '\n')
            {
                lod += "                " + line;
                line.clear();
            }
        }
        lod += "            }\n";
    }
    return "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n"
           "over \"Protos\"\n{\n    def Mesh \"Tri\"\n    {\n        int[] faceVertexCounts = [3]\n        int[] faceVertexIndices = [0, 1, 2]\n"
           "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n    }\n}\n"
           "def Xform \"World\"\n{\n"
           "    def Xform \"Lamp\"\n    {\n        custom string studio:assetId = \"LA_001\"\n        custom int studio:rating = 3\n"
           "        double3 xformOp:translate = (0, 20, 0)\n        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n    }\n"
           "    def Xform \"Car\" (\n        variants = {\n            string color = \"red\"\n        }\n        prepend variantSets = \"color\"\n    )\n"
           "    {\n        variantSet \"color\" = {\n"
           "            \"blue\" {\n                def Xform \"Blue\"\n                {\n                }\n            }\n"
           "            \"red\" {\n                def Xform \"Red\"\n                {\n                }\n            }\n        }\n    }\n"
           "    def Xform \"Chair\" (\n        variants = {\n            string LOD = \"LOD0\"\n        }\n        prepend variantSets = \"LOD\"\n    )\n"
           "    {\n        variantSet \"LOD\" = {\n" +
           lod +
           "        }\n    }\n"
           "    def \"Payloaded\" (\n        prepend payload = @./part.usda@</Part>\n    )\n    {\n    }\n"
           "    def Xform \"Inst\" (\n        instanceable = true\n        prepend references = </Protos>\n    )\n    {\n    }\n"
           "}\n";
}
} // namespace

TEST_CASE("usd stage panels: the stage and prim sections show the stage and edit it")
{
    UpAxisScope axis(math::UpAxis::Z);
    ensureApp();
    fresh();
    const fs::path dir = tempDir() / "stage" / "panels";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    fs::copy_file(fixture("part.usda"), dir / "part.usda", fs::copy_options::overwrite_existing);
    {
        std::ofstream f(dir / "shop.usda", std::ios::binary);
        f << panelStage();
    }
    const std::string stage = run("usdStageOpen", {{"args", {(dir / "shop.usda").generic_string()}}, {"report", true}})["stage"];
    REQUIRE(graph().find("Car|Red"));
    REQUIRE(graph().find("Payloaded|Box"));
    // An edit inside the instance, so the last save leaves a warning.
    graph().setValue({node("Inst|Tri").id(), Transform::kTranslate, -1}, glm::dvec3(1, 2, 3));
    run("saveScene", {{"args", {(dir / "scene.lila").generic_string()}}});

    std::unique_ptr<QWidget> panel = section(node(stage).id(), "USD Stage");
    REQUIRE(panel);
    grab(*panel, "stage.png");
    CHECK(labelled(*panel, "Edits are kept inside the scene."));
    QComboBox* color = row<QComboBox>(*panel, "Car color");
    REQUIRE(color);
    CHECK(color->count() == 2);
    CHECK(color->currentText() == "red");
    QComboBox* look0 = row<QComboBox>(*panel, "Mesh (LOD0) look");
    QComboBox* look1 = row<QComboBox>(*panel, "Mesh (LOD1) look");
    REQUIRE(look0);
    REQUIRE(look1);
    CHECK(look1->currentText() == "a");
    QComboBox* lod = row<QComboBox>(*panel, "Chair LOD");
    REQUIRE(lod);
    CHECK(lod->count() == 2);
    QCheckBox* payload = nullptr;
    for (QCheckBox* c : panel->findChildren<QCheckBox*>())
        if (c->text() == "/World/Payloaded")
            payload = c;
    REQUIRE(payload);
    CHECK(payload->isChecked());
    CHECK(labelled(*panel, "At the last save:"));
    CHECK(labelled(*panel, "made non-instanceable"));
    for (QLabel* l : panel->findChildren<QLabel*>())
        CHECK_MESSAGE((l->width() >= l->sizeHint().width() || l->wordWrap()), l->text().toStdString());

    // Switching a variant runs usdStageVariant: the Car's objects follow.
    color->setCurrentText("blue");
    Q_EMIT color->textActivated("blue");
    CHECK(graph().find("Car|Blue"));
    CHECK_FALSE(graph().find("Car|Red"));
    // One level's variant, inside that level.
    look1->setCurrentText("b");
    Q_EMIT look1->textActivated("b");
    CHECK(has(run("usdStageInfo", {{"args", {stage}}, {"edits", true}})["edits"].get<std::string>(), "string look = \"b\""));
    // The payload unloaded.
    payload->setChecked(false);
    CHECK_FALSE(graph().find("Payloaded|Box"));

    panel = section(node("Lamp").id(), "USD Prim");
    REQUIRE(panel);
    grab(*panel, "prim.png");
    CHECK(labelled(*panel, "/World/Lamp"));
    // Unauthored schema properties holding nothing are left out.
    CHECK_FALSE(row<QLineEdit>(*panel, "proxyPrim"));
    // Custom properties are the object's own attributes, under Extra Attributes.
    CHECK_FALSE(row<QLineEdit>(*panel, "studio:assetId"));
    CHECK(node("Lamp").as<std::string>(node("Lamp").type().attrIndex("studio:assetId")) == "LA_001");
    QLineEdit* translate = row<QLineEdit>(*panel, "xformOp:translate");
    REQUIRE(translate);
    CHECK(translate->text() == "(0, 20, 0)");
    CHECK_FALSE(translate->isReadOnly());
    // Each property opens its opinions.
    QToolButton* stack = row<QToolButton>(*panel, "xformOp:translate");
    REQUIRE(stack);
    stack->click();
    QDialog* opinions = nullptr;
    for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<QDialog*>(top); d && d->windowTitle() == "Opinions: /World/Lamp.xformOp:translate")
            opinions = d;
    REQUIRE(opinions);
    QTreeWidget* tree = opinions->findChild<QTreeWidget*>();
    REQUIRE(tree);
    CHECK(tree->topLevelItemCount() == 1);
    CHECK(tree->topLevelItem(0)->text(2) == "(0, 20, 0)");
    CHECK(tree->topLevelItem(0)->font(0).bold());
    delete opinions;

    // Editing a field runs usdStageSetProperty.
    translate->setText("(4, 5, 6)");
    Q_EMIT translate->editingFinished();
    CHECK(near(node("Lamp").as<glm::dvec3>(Transform::kTranslate), {4, 5, 6}, 1e-9));

    // A mesh inside an LOD level.
    const std::vector<NodeId> levels = graph().children(node("Chair").id());
    REQUIRE(levels.size() == 2);
    panel = section(levels[1], "USD Prim");
    REQUIRE(panel);
    grab(*panel, "lodPrim.png");
    CHECK(labelled(*panel, "/World/Chair/Mesh in LOD1"));
    CHECK_FALSE(row<QLineEdit>(*panel, "normals"));
    QLineEdit* points = row<QLineEdit>(*panel, "points");
    REQUIRE(points);
    CHECK(points->text().contains("(5, 0, 0)"));

    // An object not yet in the stage says so.
    addMesh("Pebble", geo::makeCube({}), node("World").id());
    panel = section(node("Pebble").id(), "USD Prim");
    REQUIRE(panel);
    grab(*panel, "unlinked.png");
    CHECK(labelled(*panel, "Not linked to a prim"));
}
