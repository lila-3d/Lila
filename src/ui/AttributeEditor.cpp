#include "ui/AttributeEditor.h"

#include "ops/Command.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Graph.h"
#include "ui/Icons.h"
#include "ui/NumberExpr.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/ValueReset.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::PlugType;

QString niceName(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (std::isupper(static_cast<unsigned char>(c)))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

QString fmt(double v)
{
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 4);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

/** Folder a file browser opens in for a stored path. */
QString browseStart(const std::string& stored)
{
    namespace fs = std::filesystem;
    fs::path p = paths::resolvePath(stored);
    std::error_code ec;
    if (!stored.empty() && fs::is_directory(p.parent_path(), ec))
        return QString::fromStdU16String(p.parent_path().u16string());
    return Workspace::get().isOpen() ? QString::fromStdU16String(Workspace::get().root().u16string()) : QString();
}

/** Collapsible section header, Maya frameLayout style. */
QWidget* section(const QString& title, QWidget* content)
{
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* header = new QToolButton;
    header->setText(title);
    header->setCheckable(true);
    header->setChecked(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(Qt::DownArrow);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setStyleSheet("QToolButton { text-align: left; font-weight: 600; padding: 3px; }");
    QObject::connect(header, &QToolButton::toggled, content, [header, content](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    v->addWidget(header);
    v->addWidget(content);
    return box;
}

double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

std::vector<attributeEditor::Section*> extraSections()
{
    return attributeEditor::registry().allInOrder();
}
} // namespace

Registry<attributeEditor::Section>& attributeEditor::registry()
{
    static Registry<Section> r;
    return r;
}

void attributeEditor::addSection(std::string typeName, std::string title, SectionFactory fn, std::string owner)
{
    std::string id = typeName + "/" + title;
    registry().put(std::move(id), {std::move(typeName), std::move(title), std::move(fn)}, std::move(owner));
}

uint64_t attributeEditor::addSection(ext::Context& ctx, std::string typeName, std::string title, SectionFactory fn)
{
    std::string id = typeName + "/" + title;
    if (const std::string* owner = registry().ownerOf(id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "Attribute Editor section '" + id + "' already belongs to " + *owner);
        return 0;
    }
    addSection(std::move(typeName), std::move(title), std::move(fn), ctx.owner());
    return ctx.track("attributeEditorSection:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool attributeEditor::removeSection(const std::string& typeName, const std::string& title)
{
    return registry().remove(typeName + "/" + title);
}

std::vector<std::pair<std::string, QWidget*>> attributeEditor::buildSections(scene::NodeId id)
{
    std::vector<std::pair<std::string, QWidget*>> out;
    scene::Node* n = Graph::current().node(id);
    if (!n)
        return out;
    for (const attributeEditor::Section* extra : extraSections())
        if (n->type().isA(extra->typeName))
            if (QWidget* w = extra->make(id))
                out.emplace_back(extra->title, w);
    return out;
}

AttributeEditor::AttributeEditor(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    auto* top = new QHBoxLayout;
    mType = new QLabel(this);
    mType->setStyleSheet("color: #9a9a9a;");
    mName = new QLineEdit(this);
    top->addWidget(new QLabel("Name", this));
    top->addWidget(mName, 1);
    top->addWidget(mType);
    outer->addLayout(top);
    connect(mName, &QLineEdit::editingFinished, this, [this] {
        scene::Node* n = Graph::current().node(mNode);
        if (!n || mName->text().isEmpty() || mName->text().toStdString() == n->name())
            return;
        try
        {
            ops::execute("rename", {{"args", {n->path(), mName->text().toStdString()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    });

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    mBody = new QWidget;
    mBodyLayout = new QVBoxLayout(mBody);
    mBodyLayout->setContentsMargins(0, 0, 0, 0);
    mBodyLayout->setSpacing(6);
    scroll->setWidget(mBody);
    outer->addWidget(scroll, 1);

    mSectionListener = attributeEditor::registry().addListener([this](RegistryChange, const std::string&, const std::string&) {
        mRebuild = true;
        scheduleRefresh();
    });
    mListener = Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Value)
            mRebuild = true;
        scheduleRefresh();
    });
    rebuild();
}

AttributeEditor::~AttributeEditor()
{
    Graph::current().removeListener(mListener);
    attributeEditor::registry().removeListener(mSectionListener);
}

void AttributeEditor::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        Graph& g = Graph::current();
        scene::NodeId lead = g.selection().empty() ? scene::kNoNode : g.selection().back();
        if (mRebuild || lead != mNode)
            rebuild();
        else
            updateValues();
        mRebuild = false;
    });
}

void AttributeEditor::set(const std::string& attr, const json& value)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    try
    {
        ops::execute("setAttr", {{"args", {n->path() + "." + attr, value}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        updateValues();
    }
}

QWidget* AttributeEditor::colorSwatch(const scene::AttrDef& def, scene::NodeId id, int index, bool readOnly)
{
    // A data display: the swatch shows the stored linear colour in sRGB.
    auto* b = new QToolButton;
    b->setMinimumWidth(36);
    b->setEnabled(!readOnly);
    std::string attr = def.name;
    auto current = [id, index] {
        const scene::PlugValue& v = Graph::current().value({id, index});
        auto* c = std::get_if<glm::dvec3>(&v);
        return c ? *c : glm::dvec3(0.0);
    };
    mUpdaters.push_back([b, current] {
        glm::dvec3 c = current();
        QColor q = QColor::fromRgbF(float(toSrgb(c.x)), float(toSrgb(c.y)), float(toSrgb(c.z)));
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #141414; }").arg(q.name()));
    });
    connect(b, &QToolButton::clicked, this, [this, b, attr, current] {
        glm::dvec3 c = current();
        QColor q = QColorDialog::getColor(QColor::fromRgbF(float(toSrgb(c.x)), float(toSrgb(c.y)), float(toSrgb(c.z))), b, "Color");
        if (q.isValid())
            set(attr, json::array({toLinear(q.redF()), toLinear(q.greenF()), toLinear(q.blueF())}));
    });
    return b;
}

void AttributeEditor::rebuild()
{
    LILA_ZONE();

    mUpdaters.clear();
    while (QLayoutItem* item = mBodyLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    Graph& g = Graph::current();
    mNode = g.selection().empty() ? scene::kNoNode : g.selection().back();
    scene::Node* n = g.node(mNode);
    mName->setText(n ? QString::fromStdString(n->name()) : QString());
    mName->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    mName->setEnabled(n != nullptr);
    mType->setText(n ? QString::fromStdString(n->type().name) : QString());
    mType->setToolTip(n ? QString::fromStdString(n->type().doc) : QString());
    if (!n)
    {
        mBodyLayout->addStretch(1);
        return;
    }

    // One section per type in the inheritance chain, base first.
    std::vector<const scene::NodeType*> chain;
    for (const scene::NodeType* t = &n->type(); t; t = t->base)
        chain.insert(chain.begin(), t);

    scene::NodeId id = mNode;
    /** Adds one attr's row to form; false when the attr shows no editor. */
    auto addRow = [&](QFormLayout* form, const scene::AttrDef& def) -> bool {
        if (def.flags & scene::AttrHidden)
            return false;
        int index = n->type().attrIndex(def.name);
        bool readOnly = !(def.flags & scene::AttrInput);
        std::string attr = def.name;
        QWidget* field = nullptr;

        auto numberEdit = [&](int component) {
            auto* e = new QLineEdit;
            e->setAlignment(Qt::AlignRight);
            e->setReadOnly(readOnly);
            e->setMinimumWidth(36);
            e->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            std::string target = component >= 0 ? def.childNames[component] : attr;
            connect(e, &QLineEdit::editingFinished, this, [this, e, target, def, id, index, component] {
                if (!e->isModified())
                    return;
                e->setModified(false);
                double current = 0.0;
                if (Graph::current().node(id))
                {
                    const scene::PlugValue& pv = Graph::current().value({id, index});
                    if (auto* v3 = std::get_if<glm::dvec3>(&pv); v3 && component >= 0)
                        current = (*v3)[component];
                    else if (auto* f = std::get_if<double>(&pv))
                        current = *f;
                    else if (auto* i = std::get_if<int64_t>(&pv))
                        current = double(*i);
                }
                std::optional<double> v = evalExpression(e->text(), current);
                if (!v)
                {
                    updateValues();
                    return;
                }
                if (def.minValue)
                    v = std::max(*v, *def.minValue);
                if (def.maxValue)
                    v = std::min(*v, *def.maxValue);
                set(target, def.type == PlugType::Int ? json(int64_t(std::llround(*v))) : json(*v));
            });
            mUpdaters.push_back([e, id, index, component] {
                if (e->hasFocus())
                    return;
                scene::Node* node = Graph::current().node(id);
                if (!node)
                    return;
                const scene::PlugValue& v = Graph::current().value({id, index});
                double d = 0.0;
                if (auto* v3 = std::get_if<glm::dvec3>(&v))
                    d = (*v3)[component];
                else if (auto* f = std::get_if<double>(&v))
                    d = *f;
                else if (auto* i = std::get_if<int64_t>(&v))
                    d = double(*i);
                e->setText(fmt(d));
            });
            return e;
        };

        switch (def.type)
        {
        case PlugType::Float:
        case PlugType::Int: field = numberEdit(-1); break;
        case PlugType::Vec3:
        {
            field = new QWidget;
            auto* h = new QHBoxLayout(field);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            for (int c = 0; c < 3; ++c)
                h->addWidget(numberEdit(c));
            if (def.flags & scene::AttrColor)
                h->addWidget(colorSwatch(def, id, index, readOnly));
            break;
        }
        case PlugType::Bool:
        {
            auto* cb = new QCheckBox;
            cb->setEnabled(!readOnly);
            connect(cb, &QCheckBox::clicked, this, [this, attr](bool on) { set(attr, on); });
            mUpdaters.push_back([cb, id, index] {
                QSignalBlocker block(cb);
                cb->setChecked(std::get<bool>(Graph::current().value({id, index})));
            });
            field = cb;
            break;
        }
        case PlugType::Enum:
        {
            auto* combo = new QComboBox;
            for (const std::string& e : def.enumNames)
                combo->addItem(QString::fromStdString(e));
            combo->setEnabled(!readOnly);
            connect(combo, &QComboBox::activated, this, [this, attr](int i) { set(attr, int64_t(i)); });
            mUpdaters.push_back([combo, id, index] {
                QSignalBlocker block(combo);
                combo->setCurrentIndex(int(std::get<int64_t>(Graph::current().value({id, index}))));
            });
            field = combo;
            break;
        }
        case PlugType::String:
        {
            auto* e = new QLineEdit;
            e->setReadOnly(readOnly);
            connect(e, &QLineEdit::editingFinished, this, [this, e, attr] {
                if (e->isModified())
                {
                    e->setModified(false);
                    set(attr, e->text().toStdString());
                }
            });
            mUpdaters.push_back([e, id, index] {
                if (!e->hasFocus())
                    e->setText(QString::fromStdString(std::get<std::string>(Graph::current().value({id, index}))));
            });
            field = e;
            if ((def.flags & scene::AttrFile) && !readOnly)
            {
                field = new QWidget;
                auto* h = new QHBoxLayout(field);
                h->setContentsMargins(0, 0, 0, 0);
                h->setSpacing(3);
                auto* browse = new QToolButton;
                browse->setIcon(icon("folder-open"));
                browse->setToolTip("Browse...");
                connect(browse, &QToolButton::clicked, this, [this, e, attr, label = niceName(def.name)] {
                    QString f = QFileDialog::getOpenFileName(this, label, browseStart(e->text().toStdString()),
                                                             "Images (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.webp);;All Files (*)");
                    if (!f.isEmpty())
                        set(attr, paths::storedPath(std::filesystem::path(f.toStdU16String())));
                });
                h->addWidget(e, 1);
                h->addWidget(browse);
            }
            break;
        }
        case PlugType::Matrix:
        {
            auto* label = new QLabel;
            label->setFont(monoFont());
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            mUpdaters.push_back([label, id, index] {
                const auto& m = std::get<glm::dmat4>(Graph::current().value({id, index}));
                QString text;
                // Rows of the matrix as written on paper (column-major storage).
                for (int r = 0; r < 4; ++r)
                {
                    for (int c = 0; c < 4; ++c)
                        text += QString("%1").arg(fmt(m[c][r]), 9);
                    if (r < 3)
                        text += "\n";
                }
                label->setText(text);
            });
            field = label;
            break;
        }
        }
        if (!field)
            return false;
        field->setToolTip(QString::fromStdString(def.doc));
        auto* label = new QLabel(def.niceName.empty() ? niceName(def.name) : QString::fromStdString(def.niceName));
        if (def.flags & scene::AttrKeyable)
        {
            label->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(label, &QWidget::customContextMenuRequested, this, [this, label, attr](const QPoint& at) {
                QMenu menu;
                menu.addAction("Set Key", [this, attr] {
                    scene::Node* node = Graph::current().node(mNode);
                    if (!node)
                        return;
                    try
                    {
                        ops::execute("setKeyframe", {{"args", {node->path() + "." + attr}}}, CallSource::UI);
                    }
                    catch (const CommandError& e)
                    {
                        log::error("{}", e.what());
                    }
                });
                menu.exec(label->mapToGlobal(at));
            });
        }
        label->setToolTip(QString::fromStdString(def.name + (def.shortName.empty() ? "" : " (" + def.shortName + ")") +
                                                 (def.doc.empty() ? "" : "\n" + def.doc)));
        if (readOnly)
            label->setStyleSheet("color: #8a8a8a;");
        else if (std::optional<json> d = valueReset::plugDefault(def))
            onMiddleClickReset(label, [this, attr, v = *d] { set(attr, v); },
                               QString::fromStdString(valueReset::describe(*d, def.enumNames)), field);
        form->addRow(label, field);
        return true;
    };
    auto newForm = [](QWidget* content) {
        auto* form = new QFormLayout(content);
        form->setLabelAlignment(Qt::AlignRight);
        form->setContentsMargins(8, 2, 4, 2);
        form->setSpacing(3);
        return form;
    };
    auto subHeading = [](QFormLayout* form, const std::string& title) {
        auto* l = new QLabel(QString::fromStdString(title));
        l->setStyleSheet("color: #c8c8c8; font-weight: 600; padding-top: 4px; border-bottom: 1px solid #474747;");
        form->addRow(l);
    };

    // Attrs with a category show together under its heading in a Parameters section, in attr order.
    std::vector<const scene::AttrDef*> parameters;
    for (const scene::NodeType* t : chain)
        for (const scene::AttrDef& def : t->ownAttrs)
            if (!def.category.empty() || (!def.niceName.empty() && !(def.flags & scene::AttrHidden)))
                parameters.push_back(&def);
    if (!parameters.empty())
    {
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        std::vector<std::string> categories{""};
        for (const scene::AttrDef* d : parameters)
            if (std::find(categories.begin(), categories.end(), d->category) == categories.end())
                categories.push_back(d->category);
        int added = 0;
        for (const std::string& c : categories)
        {
            bool headed = c.empty();
            for (const scene::AttrDef* d : parameters)
            {
                if (d->category != c)
                    continue;
                if (!headed)
                {
                    subHeading(form, c);
                    headed = true;
                }
                added += addRow(form, *d) ? 1 : 0;
            }
        }
        if (added)
            mBodyLayout->addWidget(section("Parameters", content));
        else
            delete content;
    }

    for (const scene::NodeType* t : chain)
    {
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        int added = 0;
        for (const scene::AttrDef& def : t->ownAttrs)
        {
            if (std::find(parameters.begin(), parameters.end(), &def) != parameters.end())
                continue;
            added += addRow(form, def) ? 1 : 0;
        }
        if (added && !t->doc.empty())
        {
            auto* about = new QLabel(QString::fromStdString(t->doc));
            about->setWordWrap(true);
            about->setStyleSheet("color: #9a9a9a;");
            form->insertRow(0, about);
        }
        if (added)
            mBodyLayout->addWidget(section(niceName(t->name) + " Attributes", content));
        else
            delete content;
    }
    for (const attributeEditor::Section* extra : extraSections())
        if (n->type().isA(extra->typeName))
            if (QWidget* w = extra->make(id))
                mBodyLayout->addWidget(section(QString::fromStdString(extra->title), w));
    mBodyLayout->addStretch(1);
    updateValues();
}

void AttributeEditor::updateValues()
{
    LILA_ZONE();

    Graph::current().evaluate();
    if (!Graph::current().node(mNode))
        return;
    for (auto& fn : mUpdaters)
        fn();
}
} // namespace lila::ui
