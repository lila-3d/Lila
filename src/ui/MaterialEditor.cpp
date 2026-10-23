#include "ui/MaterialEditor.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/MaterialDraw.h"
#include "ui/SpinScrub.h"
#include "ui/TextureCache.h"
#include "ui/ValueReset.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleValidator>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
using namespace scene::material;
namespace fs = std::filesystem;

constexpr int kThumb = 40;
constexpr int kPreview = 112;

float toSrgb(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float toLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

QColor displayColor(const glm::dvec3& linear)
{
    return QColor::fromRgbF(toSrgb(float(linear.x)), toSrgb(float(linear.y)), toSrgb(float(linear.z)));
}

json run(const char* command, const json& call)
{
    try
    {
        return cmds::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return nullptr;
}

/** assignMaterial with no objects would find something to assign to: faces in face mode, else selected meshes. */
bool hasAssignTargets()
{
    scene::Graph& g = scene::Graph::current();
    if (scene::components::mode() == scene::ComponentMode::Face)
        for (const auto& [node, ids] : scene::components::selected(scene::ComponentMode::Face))
            if (!ids.empty())
                return true;
    auto isMesh = [&](scene::NodeId id) {
        const scene::Node* n = g.node(id);
        return n && n->type().isA("mesh");
    };
    for (scene::NodeId id : g.selection())
        if (isMesh(id) || std::ranges::any_of(g.descendants(id), isMesh))
            return true;
    return false;
}

/** Creates a material and, when there is something to assign it to (or `always`), assigns it, as one undo step. */
json createAndAssign(bool always)
{
    UndoChunk chunk("New Material");
    json name = run("createMaterial", {{"args", json::array({"M_Material"})}});
    if (name.is_string() && (always || hasAssignTargets()))
        run("assignMaterial", {{"args", json::array({name})}});
    return name;
}

QIcon swatchIcon(const QColor& c)
{
    QPixmap pm(36, 18);
    pm.fill(c);
    QPainter p(&pm);
    p.setPen(QColor(20, 20, 20));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}

QPixmap thumbPixmap(const QImage& img, int size)
{
    QPixmap pm(size, size);
    pm.fill(QColor(40, 40, 40));
    if (!img.isNull())
    {
        QPainter p(&pm);
        QImage s = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((size - s.width()) / 2, (size - s.height()) / 2, s);
    }
    return pm;
}

/** A lit ball, shaded on the CPU like the viewport's key light plus ambient; the base color map wraps it. */
QPixmap previewBall(const scene::Node& n)
{
    LILA_ZONE();

    glm::vec3 base(n.as<glm::dvec3>(kBaseColor));
    float metal = float(n.as<double>(kMetallic)), rough = std::clamp(float(n.as<double>(kRoughness)), 0.04f, 1.0f);
    glm::vec3 emissive = glm::vec3(n.as<glm::dvec3>(kEmissiveColor)) * float(n.as<double>(kEmissiveIntensity));
    QImage map = textures::thumbnail(scene::materials::resolvePath(n.as<std::string>(kBaseColorMap)), 128);
    QImage img(kPreview, kPreview, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const glm::vec3 l = glm::normalize(glm::vec3(-0.5f, 0.7f, 0.6f)), v(0, 0, 1);
    const glm::vec3 h = glm::normalize(l + v);
    float r = kPreview * 0.5f - 2.0f;
    for (int y = 0; y < kPreview; ++y)
        for (int x = 0; x < kPreview; ++x)
        {
            float px = (x + 0.5f - kPreview * 0.5f) / r, py = (kPreview * 0.5f - y - 0.5f) / r;
            float d2 = px * px + py * py;
            if (d2 > 1.0f)
                continue;
            glm::vec3 nrm(px, py, std::sqrt(1.0f - d2));
            glm::vec3 albedo = base;
            if (!map.isNull())
            {
                float u = 0.5f + std::atan2(nrm.x, nrm.z) / (2.0f * 3.14159265f), t = 0.5f - std::asin(nrm.y) / 3.14159265f;
                QRgb c = map.pixel(std::clamp(int(u * map.width()), 0, map.width() - 1), std::clamp(int(t * map.height()), 0, map.height() - 1));
                albedo = {toLinear(qRed(c) / 255.0f), toLinear(qGreen(c) / 255.0f), toLinear(qBlue(c) / 255.0f)};
            }
            glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedo, metal);
            float nl = std::max(glm::dot(nrm, l), 0.0f), nh = std::max(glm::dot(nrm, h), 0.0f);
            float a2 = rough * rough * rough * rough;
            float dd = nh * nh * (a2 - 1.0f) + 1.0f;
            float spec = a2 / (3.14159265f * dd * dd) * 0.25f;
            glm::vec3 c = (albedo * (1.0f - metal) / 3.14159265f + f0 * spec) * nl * 2.6f;
            c += (albedo * (1.0f - metal) + f0 * 0.5f) * (0.18f + 0.1f * nrm.y) + emissive;
            c = c / (c + glm::vec3(0.6f)) * 1.25f;
            float edge = std::clamp((1.0f - std::sqrt(d2)) * r, 0.0f, 1.0f);
            img.setPixel(x, y, qPremultiply(qRgba(int(toSrgb(c.r) * 255), int(toSrgb(c.g) * 255), int(toSrgb(c.b) * 255), int(edge * 255))));
        }
    return QPixmap::fromImage(img);
}

QPointer<MaterialEditor>& floatingEditor()
{
    static QPointer<MaterialEditor> e;
    return e;
}

QDockWidget* findDock(const char* name)
{
    for (QWidget* w : QApplication::topLevelWidgets())
    {
        if (auto* d = qobject_cast<QDockWidget*>(w); d && w->objectName() == name)
            return d;
        if (auto* d = w->findChild<QDockWidget*>(name))
            return d;
    }
    return nullptr;
}

MaterialEditor* showEditor()
{
    if (QDockWidget* d = findDock(kMaterialEditorDock))
    {
        revealDock(d);
        return qobject_cast<MaterialEditor*>(d->widget());
    }
    auto& e = floatingEditor();
    if (!e)
    {
        e = new MaterialEditor(QApplication::activeWindow());
        e->setWindowFlag(Qt::Tool);
        e->setWindowTitle("Material Editor");
        e->setAttribute(Qt::WA_DeleteOnClose);
        e->resize(475, 720);
    }
    e->show();
    e->raise();
    return e;
}
} // namespace

MaterialEditor::MaterialEditor(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    auto* top = new QHBoxLayout;
    mList = new QListWidget(this);
    mList->setMaximumHeight(140);
    mList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    top->addWidget(mList, 1);
    auto* buttons = new QVBoxLayout;
    auto* add = new QPushButton("New", this);
    auto* del = new QPushButton("Delete", this);
    mAssign = new QPushButton("Assign", this);
    mAssign->setToolTip("Assign to the selected objects, or the selected faces in face mode.");
    for (QPushButton* b : {add, del, mAssign})
        buttons->addWidget(b);
    buttons->addStretch();
    top->addLayout(buttons);
    mPreview = new QLabel(this);
    mPreview->setFixedSize(kPreview, kPreview);
    top->addWidget(mPreview);
    root->addLayout(top);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    mBody = new QWidget;
    mBodyLayout = new QVBoxLayout(mBody);
    mBodyLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(mBody);
    root->addWidget(scroll, 1);

    add->setToolTip("Create a material; with objects or faces selected, also assign it to them.");
    connect(add, &QPushButton::clicked, this, [this] {
        json name = createAndAssign(false);
        if (name.is_string())
            showMaterial(name.get<std::string>());
    });
    connect(del, &QPushButton::clicked, this, [this] {
        if (scene::Node* n = current())
            run("delete", {{"args", json::array({n->name()})}});
    });
    connect(mAssign, &QPushButton::clicked, this, [this] {
        if (scene::Node* n = current())
            run("assignMaterial", {{"args", json::array({n->name()})}});
    });
    connect(mList, &QListWidget::currentTextChanged, this, [this](const QString& name) {
        scene::Node* n = scene::materials::find(scene::Graph::current(), name.toStdString());
        scene::NodeId id = n ? n->id() : scene::kNoNode;
        if (id != mNode)
        {
            mNode = id;
            rebuild();
        }
    });
    connect(mList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        scene::Node* n = current();
        std::string wanted = item->text().toStdString();
        if (n && wanted != n->name())
            run("renameMaterial", {{"args", json::array({n->name(), wanted})}});
        scheduleRefresh();
    });

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c == scene::Change::Selection)
            QTimer::singleShot(0, this, [this] { followSelection(); });
        scheduleRefresh();
    });
    mTextureListener = textures::addListener([this] { scheduleRefresh(); });
    refreshList();
}

MaterialEditor::~MaterialEditor()
{
    scene::Graph::current().removeListener(mListener);
    textures::removeListener(mTextureListener);
}

scene::Node* MaterialEditor::current() const
{
    scene::Node* n = scene::Graph::current().node(mNode);
    return n && n->type().isA("material") ? n : nullptr;
}

void MaterialEditor::showMaterial(const std::string& name)
{
    refreshList();
    auto items = mList->findItems(QString::fromStdString(name), Qt::MatchExactly);
    if (!items.isEmpty())
        mList->setCurrentItem(items.front());
}

void MaterialEditor::followSelection()
{
    scene::Graph& g = scene::Graph::current();
    if (g.selection().empty())
        return;
    scene::Node* n = g.node(g.selection().back());
    if (!n || !n->type().isA("mesh"))
        return;
    g.evaluate();
    const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh)
        return;
    for (const auto& [slot, faces] : scene::materials::slotUsage(*mesh))
        if (scene::materials::find(g, slot))
        {
            showMaterial(slot);
            return;
        }
}

void MaterialEditor::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        refreshList();
        if (!current() && mNode != scene::kNoNode)
        {
            mNode = scene::kNoNode;
            rebuild();
        }
        updateValues();
    });
}

void MaterialEditor::refreshList()
{
    std::vector<std::string> names;
    for (scene::Node* n : scene::materials::all(scene::Graph::current()))
        names.push_back(n->name());
    std::sort(names.begin(), names.end());
    bool same = int(names.size()) == mList->count();
    for (int i = 0; same && i < mList->count(); ++i)
        same = mList->item(i)->text().toStdString() == names[size_t(i)];
    if (same)
        return;
    std::string keep = current() ? current()->name() : std::string();
    QSignalBlocker block(mList);
    mList->clear();
    for (const std::string& n : names)
    {
        auto* item = new QListWidgetItem(QString::fromStdString(n), mList);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        if (n == keep)
            mList->setCurrentItem(item);
    }
    if (keep.empty() && mList->count() && mNode == scene::kNoNode)
    {
        mList->setCurrentRow(0);
        scene::Node* n = scene::materials::find(scene::Graph::current(), names[0]);
        mNode = n ? n->id() : scene::kNoNode;
        rebuild();
    }
}

void MaterialEditor::setAttr(int attr, const json& value)
{
    scene::Node* n = current();
    if (!n)
        return;
    run("setAttr", {{"args", json::array({n->name() + "." + n->type().attrs[size_t(attr)].name, value})}});
}

void MaterialEditor::rebuild()
{
    LILA_ZONE();

    mUpdaters.clear();
    while (QLayoutItem* item = mBodyLayout->takeAt(0))
    {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    scene::Node* n = current();
    mAssign->setEnabled(n != nullptr);
    if (!n)
    {
        mPreview->clear();
        auto* hint = new QLabel("No material. New creates one; faces bind to a material through the slot of the same name.", mBody);
        hint->setWordWrap(true);
        mBodyLayout->addWidget(hint);
        mBodyLayout->addStretch();
        return;
    }

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;
    auto label = [&](const char* text) {
        auto* l = new QLabel(text, mBody);
        grid->addWidget(l, row, 0);
        return l;
    };
    // A middle click on the row's label restores the attribute's default.
    auto resettable = [&](QWidget* l, int attr, QWidget* field) {
        const scene::AttrDef& def = n->type().attrs[size_t(attr)];
        if (std::optional<json> d = valueReset::plugDefault(def))
            onMiddleClickReset(l, [this, attr, v = *d] { setAttr(attr, v); },
                               QString::fromStdString(valueReset::describe(*d, def.enumNames)), field);
    };
    auto section = [&](const char* text) {
        auto* l = new QLabel(QString("<b>%1</b>").arg(text), mBody);
        grid->addWidget(l, row++, 0, 1, 3);
    };

    auto color = [&](const char* text, int attr) {
        QLabel* l = label(text);
        auto* b = new QToolButton(mBody);
        b->setIconSize(QSize(36, 18));
        resettable(l, attr, b);
        grid->addWidget(b, row++, 1, 1, 2, Qt::AlignLeft);
        connect(b, &QToolButton::clicked, this, [this, attr] {
            scene::Node* m = current();
            if (!m)
                return;
            const scene::NodeId id = m->id();
            const glm::dvec3 start = m->as<glm::dvec3>(attr);
            auto linear = [](const QColor& c) { return glm::dvec3(toLinear(float(c.redF())), toLinear(float(c.greenF())), toLinear(float(c.blueF()))); };
            // Preview without undo while the picker moves; OK commits once, as one undo step, Cancel puts the start back.
            auto preview = [id, attr](const glm::dvec3& v) {
                scene::Graph& g = scene::Graph::current();
                if (!g.node(id))
                    return;
                UndoStack::get().setEnabled(false);
                g.setValue({id, attr, -1}, v);
                UndoStack::get().setEnabled(true);
            };
            QColorDialog dlg(displayColor(start), this);
            dlg.setWindowTitle("Color");
            dlg.setOption(QColorDialog::DontUseNativeDialog);
            connect(&dlg, &QColorDialog::currentColorChanged, this, [preview, linear](const QColor& c) {
                if (c.isValid())
                    preview(linear(c));
            });
            const bool ok = dlg.exec() == QDialog::Accepted;
            preview(start);
            QColor c = dlg.selectedColor();
            if (ok && c.isValid() && current() && current()->id() == id && c.rgb() != displayColor(start).rgb())
                setAttr(attr, json::array({linear(c).x, linear(c).y, linear(c).z}));
        });
        mUpdaters.push_back([this, b, attr] {
            if (scene::Node* m = current())
                b->setIcon(swatchIcon(displayColor(m->as<glm::dvec3>(attr))));
        });
    };

    auto slider = [&](const char* text, int attr, double lo, double hi) {
        QLabel* l = label(text);
        auto* s = new QSlider(Qt::Horizontal, mBody);
        resettable(l, attr, s);
        s->setRange(0, 1000);
        auto* value = new QLineEdit(mBody);
        value->setFixedWidth(56);
        auto* validator = new QDoubleValidator(value);
        validator->setLocale(QLocale::c());
        value->setValidator(validator);
        grid->addWidget(s, row, 1);
        grid->addWidget(value, row++, 2);
        auto toValue = [lo, hi](int v) { return lo + (hi - lo) * v / 1000.0; };
        // Live while dragging without undo; the release commits once, as one undo step.
        connect(s, &QSlider::sliderPressed, this, [this] { mDragging = true; });
        connect(s, &QSlider::valueChanged, this, [this, s, attr, toValue, value](int v) {
            value->setText(QString::number(toValue(v), 'f', 3));
            scene::Node* m = current();
            if (!m || !s->isSliderDown())
                return;
            UndoStack::get().setEnabled(false);
            scene::Graph::current().setValue({m->id(), attr, -1}, toValue(v));
            UndoStack::get().setEnabled(true);
        });
        connect(s, &QSlider::sliderReleased, this, [this, s, attr, toValue] {
            mDragging = false;
            if (scene::Node* m = current())
            {
                double v = toValue(s->value());
                UndoStack::get().setEnabled(false);
                scene::Graph::current().setValue({m->id(), attr, -1}, s->property("start").toDouble());
                UndoStack::get().setEnabled(true);
                setAttr(attr, v);
            }
        });
        // A typed value may go past the slider's range; Enter or leaving the field commits it as one undo step.
        connect(value, &QLineEdit::editingFinished, this, [this, value, attr] {
            scene::Node* m = current();
            if (!m)
                return;
            bool ok = false;
            double v = QLocale::c().toDouble(value->text(), &ok);
            // Focus leaving an untouched field must not round the stored value to the three shown decimals.
            if (ok && value->isModified() && v != m->as<double>(attr))
                setAttr(attr, v);
            value->setText(QString::number(m->as<double>(attr), 'f', 3));
        });
        connect(s, &QSlider::actionTriggered, this, [this, s, attr, toValue](int action) {
            if (action == QAbstractSlider::SliderMove)
                return;
            // Clicks on the groove and keys step without a drag.
            QTimer::singleShot(0, this, [this, s, attr, toValue] {
                if (!s->isSliderDown())
                    setAttr(attr, toValue(s->value()));
            });
        });
        mUpdaters.push_back([this, s, attr, lo, hi, value] {
            scene::Node* m = current();
            if (!m || s->isSliderDown())
                return;
            double v = m->as<double>(attr);
            s->setProperty("start", v);
            QSignalBlocker block(s);
            s->setValue(int(std::lround((std::clamp(v, lo, hi) - lo) / (hi - lo) * 1000.0)));
            if (!value->hasFocus())
                value->setText(QString::number(v, 'f', 3));
        });
    };

    // Items show `labels` when given (one per enum name), else the stored enum names.
    auto combo = [&](const char* text, int attr, QStringList labels = {}, const char* tip = nullptr) {
        QLabel* l = label(text);
        auto* c = new QComboBox(mBody);
        // Long enum labels would otherwise set the panel's minimum width.
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(8);
        if (tip)
            c->setToolTip(tip);
        resettable(l, attr, c);
        const auto& names = n->type().attrs[size_t(attr)].enumNames;
        for (size_t i = 0; i < names.size(); ++i)
            c->addItem(i < size_t(labels.size()) ? labels[qsizetype(i)] : QString::fromStdString(names[i]));
        grid->addWidget(c, row++, 1, 1, 2);
        connect(c, &QComboBox::activated, this, [this, attr](int i) { setAttr(attr, i); });
        mUpdaters.push_back([this, c, attr] {
            if (scene::Node* m = current())
            {
                QSignalBlocker block(c);
                c->setCurrentIndex(int(m->as<int64_t>(attr)));
            }
        });
    };

    auto texture = [&](const char* text, const char* channel, int attr) {
        QLabel* l = label(text);
        auto* box = new QWidget(mBody);
        auto* h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto* thumb = new QLabel(box);
        thumb->setFixedSize(kThumb, kThumb);
        auto* path = new QLineEdit(box);
        path->setPlaceholderText("none");
        auto* browse = new QToolButton(box);
        browse->setText("...");
        auto* clear = new QToolButton(box);
        clear->setText("x");
        h->addWidget(thumb);
        h->addWidget(path, 1);
        h->addWidget(browse);
        h->addWidget(clear);
        grid->addWidget(box, row++, 1, 1, 2);
        auto apply = [this, channel](const std::string& p) {
            if (scene::Node* m = current())
                run("setMaterialTexture", {{"args", json::array({m->name()})}, {"channel", channel}, {"path", p}});
        };
        connect(path, &QLineEdit::editingFinished, this, [path, apply, this, attr] {
            scene::Node* m = current();
            if (m && path->text().toStdString() != m->as<std::string>(attr))
                apply(path->text().toStdString());
        });
        connect(browse, &QToolButton::clicked, this, [this, apply] {
            QString start = Workspace::get().isOpen() ? QString::fromStdU16String(Workspace::get().root().u16string()) : QString();
            QString f = QFileDialog::getOpenFileName(this, "Texture", start, "Images (*.png *.jpg *.jpeg *.tga *.bmp *.hdr);;All files (*)");
            if (!f.isEmpty())
                apply(paths::storedPath(fs::path(f.toStdU16String())));
        });
        connect(clear, &QToolButton::clicked, this, [apply] { apply(""); });
        onMiddleClickReset(l, [apply] { apply(""); }, "none", path);
        mUpdaters.push_back([this, thumb, path, attr] {
            scene::Node* m = current();
            if (!m)
                return;
            const std::string& p = m->as<std::string>(attr);
            if (!path->hasFocus())
                path->setText(QString::fromStdString(p));
            std::string resolved = scene::materials::resolvePath(p);
            thumb->setPixmap(thumbPixmap(textures::thumbnail(resolved, kThumb * 2), kThumb));
            std::string err = p.empty() ? std::string() : textures::error(resolved);
            thumb->setToolTip(QString::fromStdString(err.empty() ? p : err));
        });
    };

    auto check = [&](const char* text, int attr) {
        auto* c = new QCheckBox(text, mBody);
        grid->addWidget(c, row++, 1, 1, 2);
        resettable(c, attr, c);
        connect(c, &QCheckBox::toggled, this, [this, attr](bool on) { setAttr(attr, on); });
        mUpdaters.push_back([this, c, attr] {
            if (scene::Node* m = current())
            {
                QSignalBlocker block(c);
                c->setChecked(m->as<bool>(attr));
            }
        });
    };

    section("Surface");
    color("Base Color", kBaseColor);
    texture("Base Color Map", "baseColor", kBaseColorMap);
    slider("Metallic", kMetallic, 0.0, 1.0);
    texture("Metallic Map", "metallic", kMetallicMap);
    slider("Specular", kSpecular, 0.0, 1.0);
    slider("Roughness", kRoughness, 0.0, 1.0);
    texture("Roughness Map", "roughness", kRoughnessMap);
    texture("Occlusion Map", "ambientOcclusion", kAmbientOcclusionMap);
    texture("Packed Map", "orm", kOrmMap);
    combo("Packed Layout", kPackedLayout, {"ORM", "MRO"},
          "ORM: R occlusion, G roughness, B metallic (Unreal's packing).\n"
          "MRO: R metallic, G roughness, B occlusion.\n"
          "CRM maps (cavity in R) use ORM: the channels are laid out the same.");
    texture("Normal Map", "normal", kNormalMap);
    auto* flip = new QCheckBox("Flip Green", mBody);
    flip->setToolTip("The normal map is OpenGL style (green up: Blender, Substance's OpenGL preset). Off is DirectX "
                     "style (green down), Unreal's convention. Sent to Unreal as the texture's Flip Green Channel.");
    grid->addWidget(flip, row++, 1, 1, 2);
    resettable(flip, kNormalFormat, flip);
    connect(flip, &QCheckBox::toggled, this, [this](bool on) { setAttr(kNormalFormat, on ? 1 : 0); });
    mUpdaters.push_back([this, flip] {
        if (scene::Node* m = current())
        {
            QSignalBlocker block(flip);
            flip->setChecked(NormalFormat(m->as<int64_t>(kNormalFormat)) == NormalFormat::OpenGL);
        }
    });
    section("Emissive");
    color("Emissive Color", kEmissiveColor);
    texture("Emissive Map", "emissive", kEmissiveMap);
    slider("Intensity", kEmissiveIntensity, 0.0, 20.0);
    section("Blending");
    combo("Blend Mode", kBlendMode);
    slider("Opacity", kOpacity, 0.0, 1.0);
    slider("Mask Clip", kOpacityMaskClip, 0.0, 1.0);
    check("Two Sided", kTwoSided);
    QLabel* uvLabel = label("UV Set");
    auto* uv = new QSpinBox(mBody);
    uv->setRange(0, 1);
    resettable(uvLabel, kUvSet, uv);
    if (std::optional<json> d = valueReset::plugDefault(n->type().attrs[size_t(kUvSet)]))
        setSpinDefault(uv, d->get<double>());
    grid->addWidget(uv, row++, 1, 1, 2, Qt::AlignLeft);
    connect(uv, &QSpinBox::valueChanged, this, [this](int v) { setAttr(kUvSet, v); });
    mUpdaters.push_back([this, uv] {
        if (scene::Node* m = current())
        {
            QSignalBlocker block(uv);
            uv->setValue(int(m->as<int64_t>(kUvSet)));
        }
    });

    auto* gridHost = new QWidget(mBody);
    gridHost->setLayout(grid);
    mBodyLayout->addWidget(gridHost);
    mBodyLayout->addStretch();
    updateValues();
}

void MaterialEditor::updateValues()
{
    scene::Node* n = current();
    if (!n)
        return;
    for (auto& fn : mUpdaters)
        fn();
    mPreview->setPixmap(previewBall(*n));
}

void registerMaterialUi()
{
    scene::materials::registerTypes();
    if (!cmds::find("createMaterial"))
        scene::materials::registerCommands();

    actions::add({.id = "window.materialEditor", .label = "Material Editor", .category = "Windows", .run = [] {
                      if (QDockWidget* d = findDock(kMaterialEditorDock))
                          toggleDock(d);
                      else if (auto& e = floatingEditor(); e && e->isVisible())
                          e->close();
                      else
                          showEditor();
                  },
                  .doc = "Create and edit materials and assign them to the selection.",
                  .python = "lila.ui.run_action(\"window.materialEditor\")"});
    actions::add({.id = "material.assignNew", .label = "Assign New Material", .category = "Materials", .run = [] {
                      json name = createAndAssign(true);
                      if (!name.is_string())
                          return;
                      if (MaterialEditor* e = showEditor())
                          e->showMaterial(name.get<std::string>());
                  },
                  .doc = "Create a material and assign it to the selected objects or faces.",
                  .python = "assignMaterial(createMaterial('M_Material'))"});
    actions::add({.id = "material.assignDefault", .label = "Assign Default Material", .category = "Materials",
                  .run = [] { run("assignMaterial", {{"args", json::array({scene::materials::kDefaultSlot})}}); },
                  .enabled = actions::hasSelection,
                  .doc = "Put the selected objects or faces back on the neutral default material.",
                  .python = "assignMaterial('Default')"});
    if (!cmds::find("editMaterial"))
        cmds::registerCommand({
            .name = "editMaterial",
            .doc = "Show a material in the Material Editor, without assigning it.",
            .positionalDoc = "material",
            .undoable = false,
            .run = [](const Args& a) -> json {
                if (a.positional.empty() || !a.positional[0].is_string())
                    throw CommandError("editMaterial: give a material name");
                std::string name = a.positional[0].get<std::string>();
                if (!scene::materials::find(scene::Graph::current(), name))
                    throw CommandError("editMaterial: " + name + " is not a material");
                if (MaterialEditor* e = showEditor())
                    e->showMaterial(name);
                return name;
            },
        });
    actions::add({.id = "viewport.textured", .label = "Textured", .category = "Display", .run = [] { setMaterialsShown(!materialsShown()); },
                  .doc = "Toggle materials and textures in the viewport; off draws flat gray for form checks.",
                  .python = "lila.prefs.set(\"general\", \"/viewport/materials\", not lila.prefs.get(\"general\", \"/viewport/materials\", True))"});
}
} // namespace lila::ui
