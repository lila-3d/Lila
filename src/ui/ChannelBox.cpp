#include "ui/ChannelBox.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Spaces.h"
#include "ui/Animation.h"
#include "ui/Deletion.h"
#include "ui/NodeLookup.h"
#include "ui/NumberExpr.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QKeyEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui
{
namespace
{
using scene::Graph;

/** The plug "Set as Driver" picked, for Drive From. */
std::string gDriverPlug;

double currentNumber(Graph& g, const scene::PlugRef& p)
{
    scene::PlugValue v = g.value(p);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

/** "translateX" -> "Translate X". */
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

/** An enum channel's choices: the node's spaces for its space channel, else the attr's names. */
std::vector<std::string> enumChoices(const Graph& g, scene::NodeId owner, int attrIndex)
{
    if (attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, owner))
        return scene::spaces::names(g, owner);
    const scene::Node* n = g.node(owner);
    return n && attrIndex >= 0 && attrIndex < int(n->type().attrs.size()) ? n->type().attrs[size_t(attrIndex)].enumNames
                                                                           : std::vector<std::string>();
}

/**
 * Enum values edit through a dropdown of their choices, which writes the chosen name back as the cell's text; the
 * cell shows a chevron so it reads as one. Other cells keep the line edit.
 */
class EnumDelegate : public QStyledItemDelegate
{
public:
    EnumDelegate(std::function<QStringList(int row)> choices, QObject* parent) : QStyledItemDelegate(parent), mChoices(std::move(choices)) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStringList choices = mChoices(index.row());
        if (choices.isEmpty())
            return QStyledItemDelegate::createEditor(parent, option, index);
        auto* combo = new QComboBox(parent);
        combo->addItems(choices);
        connect(combo, &QComboBox::activated, combo, [this, combo] {
            auto* self = const_cast<EnumDelegate*>(this);
            emit self->commitData(combo);
            emit self->closeEditor(combo);
        });
        QTimer::singleShot(0, combo, [combo] { combo->showPopup(); });
        return combo;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        if (auto* combo = qobject_cast<QComboBox*>(editor))
            combo->setCurrentIndex(std::max(0, combo->findText(index.data(Qt::DisplayRole).toString())));
        else
            QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        if (auto* combo = qobject_cast<QComboBox*>(editor))
            model->setData(index, combo->currentText());
        else
            QStyledItemDelegate::setModelData(editor, model, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (mChoices(index.row()).isEmpty())
            return;
        const QRectF r = option.rect;
        const double cx = r.right() - 7.0, cy = r.center().y() + 0.5;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QColor c = index.data(Qt::ForegroundRole).value<QBrush>().color();
        if (!index.data(Qt::ForegroundRole).isValid())
            c = option.palette.color(QPalette::Text);
        painter->setPen(QPen(c, 1.2));
        painter->drawPolyline(QPolygonF({QPointF(cx - 3.0, cy - 1.5), QPointF(cx, cy + 1.5), QPointF(cx + 3.0, cy - 1.5)}));
        painter->restore();
    }

private:
    std::function<QStringList(int row)> mChoices;
};

QString formatNumber(double v)
{
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 3);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

double numericOf(const scene::PlugValue& v, int component)
{
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    if (auto* v3 = std::get_if<glm::dvec3>(&v); v3 && component >= 0)
        return (*v3)[component];
    return 0.0;
}

std::vector<ChannelBox*> gBoxes;
ChannelBox* gSliderBox = nullptr; // the box a viewport middle drag is scrubbing

/** One channel from Copy Selected Values. Input rows (polyCube1.width) remember their node type to find it again. */
struct CopiedValue
{
    std::string attr;
    std::string inputType;
    json value;
};
std::vector<CopiedValue> gCopied;
QString gCopiedText; // what the copy put on the clipboard; a different clipboard means someone else copied since

/** A value held inside the attr's hard limits. */
double clampToLimits(const scene::AttrDef& def, double v)
{
    if (def.minValue)
        v = std::max(v, *def.minValue);
    if (def.maxValue)
        v = std::min(v, *def.maxValue);
    return v;
}

/**
 * The setAttr value for text typed into a channel: on/off for bools, an enum's name or index, otherwise a number or
 * an expression on the channel's current value. nullopt when the text doesn't fit the channel.
 */
std::optional<json> parseValue(Graph& g, const std::string& plug, scene::PlugType type, const QString& text)
{
    if (type == scene::PlugType::Bool)
    {
        std::optional<bool> b = parseBool(text);
        return b ? std::optional<json>(*b) : std::nullopt;
    }
    if (type == scene::PlugType::Enum)
    {
        const scene::PlugRef ref = g.plug(plug);
        const auto& names = ref.attr == scene::Transform::kSpace && scene::spaces::hasSpaces(g, ref.node) ? scene::spaces::names(g, ref.node)
                                                                                                     : g.attr(ref).enumNames;
        auto it = std::find(names.begin(), names.end(), text.trimmed().toStdString());
        if (it != names.end())
            return json(int64_t(it - names.begin()));
        bool ok = false;
        int idx = text.toInt(&ok);
        return ok ? std::optional<json>(idx) : std::nullopt;
    }
    double current = numericOf(g.value(g.plug(plug)), -1);
    auto v = evalExpression(text, current);
    if (!v)
        return std::nullopt;
    *v = clampToLimits(g.attr(g.plug(plug)), *v);
    if (type == scene::PlugType::Int)
        return json(int64_t(std::llround(*v)));
    return json(*v);
}

/** How a channel's default reads in its tooltip, or empty when it has none. */
QString defaultText(const scene::AttrDef& def, int component)
{
    std::optional<json> v = valueReset::plugDefault(def, component);
    return v ? QString::fromStdString(valueReset::describe(*v, def.enumNames)) : QString();
}

bool hasPlug(Graph& g, const std::string& plug)
{
    try
    {
        g.plug(plug);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace

namespace channelSlider
{
bool begin(QPoint globalPos)
{
    for (ChannelBox* box : gBoxes)
    {
        if (!box->isVisible())
            continue;
        box->beginScrub(globalPos);
        if (box->mScrub)
        {
            gSliderBox = box;
            return true;
        }
    }
    return false;
}

void drag(QPoint globalPos, Qt::KeyboardModifiers mods)
{
    if (gSliderBox && gSliderBox->mScrub)
        gSliderBox->scrub(globalPos, mods);
}

void end()
{
    if (gSliderBox && gSliderBox->mScrub)
        gSliderBox->endScrub(gSliderBox->mScrub->moved);
    gSliderBox = nullptr;
}

bool active() { return gSliderBox != nullptr; }
} // namespace channelSlider

ChannelBox::ChannelBox(QWidget* parent) : QWidget(parent)
{
    gBoxes.push_back(this);
    mClock.start();
    mScrubTimer = new QTimer(this);
    mScrubTimer->setSingleShot(true);
    connect(mScrubTimer, &QTimer::timeout, this, &ChannelBox::applyScrub);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    mName = new QLineEdit(this);
    mName->setPlaceholderText("Nothing selected");
    QFont f = mName->font();
    f.setWeight(QFont::DemiBold);
    mName->setFont(f);
    layout->addWidget(mName);
    connect(mName, &QLineEdit::editingFinished, this, [this] {
        scene::Node* n = Graph::current().node(mNode);
        if (!n || mName->text().toStdString() == n->name() || mName->text().isEmpty())
            return;
        try
        {
            ops::execute("rename", {{"args", {n->path(), mName->text().toStdString()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            scheduleRefresh();
        }
    });

    mTable = new QTableWidget(0, 2, this);
    // The rows are the scene selection's channels, so Delete removes the selection as in Maya.
    deletion::claimScene(this);
    mTable->horizontalHeader()->hide();
    mTable->verticalHeader()->hide();
    mTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    mTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    mTable->setColumnWidth(1, 90);
    mTable->verticalHeader()->setDefaultSectionSize(20);
    mTable->setShowGrid(false);
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    mTable->setProperty("numericEntry", true);
    mTable->viewport()->installEventFilter(this);
    mTable->installEventFilter(this);
    layout->addWidget(mTable, 1);
    connect(mTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!mUpdating)
            commitCell(item);
    });
    mTable->setItemDelegateForColumn(1, new EnumDelegate(
                                            [this](int r) {
                                                QStringList out;
                                                if (r < 0 || r >= int(mRows.size()) || mRows[r].header || mRows[r].type != scene::PlugType::Enum)
                                                    return out;
                                                for (const std::string& c : enumChoices(Graph::current(), mRows[r].node ? mRows[r].node : mNode, mRows[r].attrIndex))
                                                    out << QString::fromStdString(c);
                                                return out;
                                            },
                                            mTable));

    mListener = Graph::current().addListener([this](scene::Change, scene::NodeId) { scheduleRefresh(); });
    onTimeChanged(this, [this] { scheduleRefresh(); });

    mTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTable, &QTableWidget::customContextMenuRequested, this, [this](QPoint pos) {
        QModelIndex idx = mTable->indexAt(pos);
        if (!idx.isValid() || idx.column() != 0 || idx.row() >= int(mRows.size()) || mRows[idx.row()].header)
            return;
        if (!mTable->item(idx.row(), 0)->isSelected())
            mTable->selectRow(idx.row());
        json plugs = json::array();
        Graph& g = Graph::current();
        for (QTableWidgetItem* item : mTable->selectedItems())
        {
            if (item->column() != 0 || item->row() >= int(mRows.size()) || mRows[item->row()].header)
                continue;
            const Row& row = mRows[item->row()];
            for (const std::string& name : row.node ? std::vector<std::string>{g.node(row.node)->path()} : targets())
                plugs.push_back(name + "." + row.attr);
        }
        auto runEach = [plugs](const char* label, auto fn) {
            UndoChunk chunk(label);
            for (const json& p : plugs)
            {
                try
                {
                    fn(p.get<std::string>());
                }
                catch (const std::exception& e)
                {
                    log::error("{}", e.what());
                }
            }
        };
        QMenu menu(this);
        menu.addAction("Copy Selected Values", [this] { copyValues(); });
        QAction* paste = menu.addAction("Paste Values", [this] { pasteValues(); });
        paste->setEnabled(!QApplication::clipboard()->text().trimmed().isEmpty());
        menu.addSeparator();
        menu.addAction("Reset to Default", [this] { resetRows(highlightedRows()); });
        menu.addAction("Key Selected", [runEach] {
            runEach("Key Selected", [](const std::string& p) { ops::execute("setKeyframe", {{"args", {p}}}, CallSource::UI); });
        });
        menu.addAction("Break Connections", [runEach] {
            runEach("Break Connections", [](const std::string& p) {
                Graph& g = Graph::current();
                scene::PlugRef ref = g.plug(p);
                // An anim curve goes with its connection, as in Maya.
                if (scene::anim::curveFor(g, ref))
                    ops::execute("cutKey", {{"args", {p}}, {"clear", true}}, CallSource::UI);
                else if (g.source(ref))
                    ops::execute("disconnectAttr", {{"args", {p}}}, CallSource::UI);
            });
        });
        menu.addAction("Delete Keys", [runEach] {
            runEach("Delete Keys", [](const std::string& p) { ops::execute("cutKey", {{"args", {p}}, {"clear", true}}, CallSource::UI); });
        });
        menu.addSeparator();
        if (plugs.size() == 1)
            menu.addAction("Set as Driver", [p = plugs[0].get<std::string>()] {
                gDriverPlug = p;
                log::info("driver: {}; right-click a channel and choose Drive From to drive it", p);
            });
        if (!gDriverPlug.empty())
            menu.addAction(QString::fromStdString("Drive From " + gDriverPlug), [runEach] {
                runEach("Drive From", [](const std::string& p) {
                    Graph& g = Graph::current();
                    const double d = currentNumber(g, g.plug(gDriverPlug)), v = currentNumber(g, g.plug(p));
                    ops::execute("driveChannel",
                                 {{"args", {p}}, {"driver", gDriverPlug}, {"keys", json::array({json::array({d, v}), json::array({d + 1.0, v + 1.0})})},
                                  {"force", true}},
                                 CallSource::UI);
                });
            });
        menu.addSeparator();
        for (bool lock : {true, false})
            menu.addAction(lock ? "Lock Selected" : "Unlock Selected", [runEach, lock] {
                runEach(lock ? "Lock Selected" : "Unlock Selected", [lock](const std::string& p) {
                    ops::execute("setAttr", {{"args", {p}}, {"lock", lock}}, CallSource::UI);
                });
            });
        if (Graph& sg = Graph::current(); sg.node(mNode) && scene::spaces::hasSpaces(sg, mNode))
        {
            menu.addSeparator();
            QMenu* spaces = menu.addMenu("Switch Space");
            const std::string node = sg.node(mNode)->path();
            const std::vector<std::string> names = scene::spaces::names(sg, mNode);
            const int now = scene::spaces::active(sg, mNode);
            for (int i = 0; i < int(names.size()); ++i)
            {
                QAction* act = spaces->addAction(QString::fromStdString(names[size_t(i)]), [node, i] {
                    try
                    {
                        ops::execute("switchSpace", {{"args", {node}}, {"space", i}}, CallSource::UI);
                    }
                    catch (const std::exception& e)
                    {
                        log::error("{}", e.what());
                    }
                });
                act->setCheckable(true);
                act->setChecked(i == now);
            }
        }
        menu.exec(mTable->viewport()->mapToGlobal(pos));
    });
    refresh();
}

ChannelBox::~ChannelBox()
{
    if (mScrub)
        endScrub(false);
    if (gSliderBox == this)
        gSliderBox = nullptr;
    std::erase(gBoxes, this);
    Graph::current().removeListener(mListener);
}

void ChannelBox::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        refresh();
    });
}

std::vector<std::string> ChannelBox::targets() const
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

std::vector<ChannelBox::Row> ChannelBox::buildRows(scene::NodeId lead) const
{
    std::vector<Row> rows;
    Graph& g = Graph::current();
    auto addChannels = [&](scene::Node* n, scene::NodeId owner) {
        const auto& attrs = n->type().attrs;
        for (int i = 0; i < int(attrs.size()); ++i)
        {
            const scene::AttrDef& a = attrs[i];
            const bool space = i == scene::Transform::kSpace && scene::spaces::hasSpaces(g, n->id());
            if (!(a.flags & (scene::AttrKeyable | scene::AttrChannelBox)) || ((a.flags & scene::AttrHidden) && !space))
                continue;
            if (a.flags & scene::AttrArray)
            {
                // One row per element of a number array; a vector array's elements stay in the attribute editor.
                if (a.type != scene::PlugType::Vec3)
                    for (const scene::PlugRef& e : g.elements({n->id(), i}))
                    {
                        std::string plug = g.plugName(e);
                        rows.push_back({plug.substr(plug.find('.') + 1), i, -1, a.type, owner, false, {}, e.element});
                    }
                continue;
            }
            if (a.type == scene::PlugType::Vec3)
                for (int c = 0; c < 3; ++c)
                    rows.push_back({a.childNames[c], i, c, scene::PlugType::Float, owner});
            else
                rows.push_back({a.name, i, -1, a.type, owner});
        }
    };
    scene::Node* n = g.node(lead);
    if (!n)
        return rows;
    addChannels(n, scene::kNoNode);
    if (!n->type().isA("mesh"))
        return rows;

    std::vector<scene::NodeId> inputs = scene::modifiers::stack(g, lead);
    std::reverse(inputs.begin(), inputs.end());
    // The node feeding the bottom of the stack (a generator such as polyCube) comes last, like Maya's INPUTS.
    scene::PlugRef bottom = inputs.empty() ? scene::PlugRef{lead, scene::MeshNode::kInMesh, -1}
                                           : scene::PlugRef{inputs.back(), 0, -1};
    if (auto src = g.source(bottom); src && g.node(src->node) && !g.node(src->node)->type().dag)
        inputs.push_back(src->node);
    for (scene::NodeId id : inputs)
    {
        scene::Node* in = g.node(id);
        if (!in)
            continue;
        rows.push_back({in->name(), -1, -1, scene::PlugType::Float, id, true});
        addChannels(in, id);
        if (in->type().isA("morph"))
            for (scene::NodeId t : scene::morphs::targetNodes(g, id))
                rows.push_back({"weight", scene::morphs::kWeight, -1, scene::PlugType::Float, t, false,
                                g.node(t)->as<std::string>(scene::morphs::kMorphName)});
    }
    return rows;
}

void ChannelBox::refresh()
{
    LILA_ZONE();

    // Don't yank a cell out from under the user's cursor.
    QWidget* focus = QApplication::focusWidget();
    if (focus && focus != mTable && mTable->isAncestorOf(focus))
        return;
    Graph& g = Graph::current();
    g.evaluate();
    scene::NodeId lead = g.selection().empty() ? scene::kNoNode : g.selection().back();
    scene::Node* n = g.node(lead);

    mUpdating = true;
    mNode = lead;
    mName->setText(n ? QString::fromStdString(n->name()) : QString());
    mName->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    mName->setEnabled(n != nullptr);

    std::vector<Row> rows = buildRows(lead);
    bool sameLayout = rows.size() == mRows.size() &&
                      std::equal(rows.begin(), rows.end(), mRows.begin(), [](const Row& a, const Row& b) {
                          return a.attr == b.attr && a.node == b.node && a.header == b.header && a.component == b.component && a.label == b.label &&
                                 a.element == b.element;
                      });
    // A scrub holds row indices, so it only ever sees values change.
    if (!sameLayout && mScrub)
    {
        mUpdating = false;
        return;
    }
    if (!sameLayout)
    {
        mRows = std::move(rows);
        mTable->clearSpans();
        mTable->setRowCount(int(mRows.size()));
        for (int r = 0; r < int(mRows.size()); ++r)
        {
            const Row& row = mRows[r];
            if (row.header)
            {
                auto* head = new QTableWidgetItem(QString::fromStdString(row.attr));
                head->setFlags(Qt::ItemIsEnabled);
                QFont hf = head->font();
                hf.setWeight(QFont::DemiBold);
                head->setFont(hf);
                head->setForeground(palette().color(QPalette::BrightText));
                if (scene::Node* in = g.node(row.node))
                    head->setToolTip(QString::fromStdString(in->type().name + (in->type().doc.empty() ? "" : "\n" + in->type().doc)));
                mTable->setItem(r, 0, head);
                mTable->setItem(r, 1, new QTableWidgetItem());
                mTable->item(r, 1)->setFlags(Qt::NoItemFlags);
                mTable->setSpan(r, 0, 1, 2);
                continue;
            }
            scene::Node* owner = g.node(row.node ? row.node : lead);
            const scene::AttrDef& def = owner->type().attrs[row.attrIndex];
            QString shown = row.label.empty() ? niceName(row.attr) : QString::fromStdString(row.label);
            if (row.label.empty() && !def.niceName.empty())
            {
                const char* axes = (def.flags & scene::AttrColor) ? "RGB" : "XYZ";
                shown = QString::fromStdString(def.niceName) + (row.component >= 0 && row.component < 3 ? QString(" ") + axes[row.component] : QString());
            }
            auto* name = new QTableWidgetItem(shown);
            name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            name->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            QString tip = QString::fromStdString(row.attr + (def.doc.empty() ? "" : "\n" + def.doc));
            if (QString text = defaultText(def, row.component); !text.isEmpty())
                tip += "\n" + resetHint(text);
            name->setToolTip(tip);
            mTable->setItem(r, 0, name);
            auto* value = new QTableWidgetItem();
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            mTable->setItem(r, 1, value);
        }
    }

    for (int r = 0; n && r < int(mRows.size()); ++r)
    {
        const Row& row = mRows[r];
        if (row.header)
            continue;
        scene::NodeId owner = row.node ? row.node : lead;
        scene::PlugRef ref{owner, row.attrIndex, row.component, row.element};
        const scene::AttrDef& def = g.node(owner)->type().attrs[row.attrIndex];
        scene::PlugValue v = g.value(scene::PlugRef{owner, row.attrIndex, -1, row.element});
        QString text;
        if (row.type == scene::PlugType::Bool)
            text = std::get<bool>(v) ? "on" : "off";
        else if (row.type == scene::PlugType::Enum)
        {
            int64_t idx = std::get<int64_t>(v);
            const auto& names = row.attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, owner) ? scene::spaces::names(g, owner)
                                                                                                           : def.enumNames;
            text = idx >= 0 && idx < int64_t(names.size()) ? QString::fromStdString(names[size_t(idx)]) : QString::number(idx);
        }
        else
            text = formatNumber(numericOf(v, row.component));
        QTableWidgetItem* item = mTable->item(r, 1);
        item->setText(text);
        // Keyed channels get a gray tint, brighter on a key; other driven channels read as locked.
        scene::NodeId curve = scene::anim::curveFor(g, ref);
        bool driven = !curve && (g.source(ref).has_value() || g.source(scene::PlugRef{owner, row.attrIndex, -1, row.element}).has_value());
        QBrush tint;
        if (curve)
            tint = scene::anim::hasKeyAt(*g.node(curve), g.time()) ? themeShade(84) : themeShade(60);
        // Locked fields read as a flat light grey, Maya's blue-grey in grayscale.
        const bool locked = g.isLocked(ref);
        item->setBackground(locked ? QBrush(themeShade(112)) : tint);
        mTable->item(r, 0)->setBackground(tint);
        item->setForeground(locked ? themeShade(28) : driven ? themeShade(130) : palette().color(QPalette::Text));
        QFont f = item->font();
        f.setItalic(driven);
        item->setFont(f);
        item->setFlags(driven || locked ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                                        : (Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
    }
    mUpdating = false;
}

void ChannelBox::commitCell(QTableWidgetItem* item)
{
    if (item->column() != 1 || item->row() >= int(mRows.size()))
        return;
    // Apply the same text to every selected channel row, like Maya.
    std::vector<int> rows;
    for (QTableWidgetItem* sel : mTable->selectedItems())
        if (std::find(rows.begin(), rows.end(), sel->row()) == rows.end())
            rows.push_back(sel->row());
    if (std::find(rows.begin(), rows.end(), item->row()) == rows.end())
        rows = {item->row()};
    QString text = item->text();
    UndoChunk chunk("Set Channels");
    for (int r : rows)
        if (!mRows[r].header)
            apply(mRows[r], text);
    scheduleRefresh();
}

void ChannelBox::toggleBool(int row)
{
    QTableWidgetItem* cell = mTable->item(row, 1);
    if (!cell || !(cell->flags() & Qt::ItemIsEditable))
        return;
    std::vector<int> rows;
    if (mTable->item(row, 0)->isSelected())
        for (int r : highlightedRows())
            if (mRows[r].type == scene::PlugType::Bool && mTable->item(r, 1) && (mTable->item(r, 1)->flags() & Qt::ItemIsEditable))
                rows.push_back(r);
    if (std::find(rows.begin(), rows.end(), row) == rows.end())
        rows.push_back(row);
    // Every channel takes the opposite of the clicked one, like Maya, rather than each flipping on its own.
    const QString text = cell->text() == "on" ? "off" : "on";
    UndoChunk chunk("Toggle Channels");
    for (int r : rows)
        apply(mRows[r], text);
    scheduleRefresh();
}

void ChannelBox::apply(const Row& row, const QString& text)
{
    Graph& g = Graph::current();
    std::vector<std::string> names = row.node ? std::vector<std::string>{g.node(row.node)->path()} : targets();
    for (const std::string& name : names)
    {
        scene::Node* n = findNode(name);
        std::string plug = name + "." + row.attr;
        // Skip selected nodes that don't have this channel.
        try
        {
            if (!n)
                continue;
            g.plug(plug);
        }
        catch (const std::exception&)
        {
            continue;
        }
        try
        {
            std::optional<json> value = parseValue(g, plug, row.type, text);
            if (!value)
            {
                if (row.type == scene::PlugType::Enum)
                    continue;
                log::warn("channel box: can't read '{}'", text.toStdString());
                return;
            }
            // Changing space keeps the pose: the node is re-keyed in the new frame.
            if (row.attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, n->id()))
                ops::execute("switchSpace", {{"args", {name}}, {"space", *value}}, CallSource::UI);
            else
                ops::execute("setAttr", {{"args", {plug, *value}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    }
}

bool ChannelBox::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mTable && (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride))
    {
        auto* k = static_cast<QKeyEvent*>(e);
        bool copy = k->matches(QKeySequence::Copy), paste = k->matches(QKeySequence::Paste);
        if (!copy && !paste)
            return QWidget::eventFilter(obj, e);
        e->accept();
        if (e->type() == QEvent::KeyPress)
            copy ? copyValues() : pasteValues();
        return true;
    }
    if (obj != mTable->viewport())
        return QWidget::eventFilter(obj, e);
    auto* me = static_cast<QMouseEvent*>(e);
    switch (e->type())
    {
    case QEvent::MouseButtonDblClick:
        if (me->button() == Qt::LeftButton)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            if (idx.isValid() && idx.row() < int(mRows.size()) && !mRows[idx.row()].header && mRows[idx.row()].type == scene::PlugType::Bool)
            {
                toggleBool(idx.row());
                return true;
            }
        }
        break;
    case QEvent::MouseButtonPress:
        if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            if (idx.isValid() && idx.column() == 1 && idx.row() < int(mRows.size()) && !mRows[idx.row()].header &&
                mRows[idx.row()].type == scene::PlugType::Enum)
            {
                const int row = idx.row();
                QTimer::singleShot(0, this, [this, row] {
                    if (QTableWidgetItem* cell = mTable->item(row, 1); cell && (cell->flags() & Qt::ItemIsEditable))
                        mTable->editItem(cell);
                });
            }
        }
        // Left button is the view's own selection (drag a range, Ctrl toggle, Shift extend); middle drag is the virtual slider.
        if (me->button() == Qt::MiddleButton)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            std::vector<int> highlighted = highlightedRows();
            QPoint at = me->globalPosition().toPoint();
            mMidClick.press(at.x(), at.y(), mClock.elapsed());
            bool channel = idx.isValid() && idx.row() < int(mRows.size()) && !mRows[idx.row()].header;
            mMidRow = channel && idx.column() == 0 ? idx.row() : -1;
            if (idx.isValid() || !highlighted.empty())
            {
                // Pressing a channel outside the highlight makes it the one dragged; on a highlighted one, all of them drag.
                bool inHighlight = std::binary_search(highlighted.begin(), highlighted.end(), idx.row());
                if (idx.isValid() && (highlighted.empty() || (channel && !inHighlight)))
                    mTable->selectionModel()->setCurrentIndex(mTable->model()->index(idx.row(), 0),
                                                              QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                beginScrub(at);
                return true;
            }
        }
        break;
    case QEvent::MouseMove:
        if (mMidClick.down())
            mMidClick.move(me->globalPosition().toPoint().x(), me->globalPosition().toPoint().y());
        if (mScrub)
        {
            scrub(me->globalPosition().toPoint(), me->modifiers());
            return !mScrub || mScrub->moved;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (me->button() == Qt::MiddleButton && mMidClick.down())
        {
            QPoint at = me->globalPosition().toPoint();
            bool click = mMidClick.release(at.x(), at.y(), mClock.elapsed());
            bool moved = mScrub && mScrub->moved;
            if (mScrub)
                endScrub(moved);
            if (click && !moved && mMidRow >= 0 && mMidRow < int(mRows.size()))
                resetRows(resetTargets(mMidRow));
            mMidRow = -1;
            return true;
        }
        if (mScrub)
        {
            bool moved = mScrub->moved;
            endScrub(moved);
            return moved;
        }
        break;
    default: break;
    }
    return QWidget::eventFilter(obj, e);
}

void ChannelBox::beginScrub(QPoint pos)
{
    Scrub s;
    s.start = s.last = pos;
    for (QTableWidgetItem* item : mTable->selectedItems())
        if (item->column() == 0 && item->row() < int(mRows.size()))
        {
            const Row& r = mRows[item->row()];
            if (!r.header && (r.type == scene::PlugType::Float || r.type == scene::PlugType::Int))
                s.rows.push_back(item->row());
        }
    if (s.rows.empty())
        return;
    Graph& g = Graph::current();
    for (int r : s.rows)
        for (const std::string& name : mRows[r].node ? std::vector<std::string>{g.node(mRows[r].node)->path()} : targets())
        {
            try
            {
                scene::PlugRef p = g.plug(name + "." + mRows[r].attr);
                if (!g.isLocked(p))
                    s.saved.push_back({p, g.value(p)});
            }
            catch (const std::exception&)
            {
            }
        }
    mScrub = std::move(s);
}

void ChannelBox::scrub(QPoint pos, Qt::KeyboardModifiers mods)
{
    Scrub& s = *mScrub;
    // The release can go missing (focus stolen mid-drag); a move with no button held ends the drag.
    if (QApplication::mouseButtons() == Qt::NoButton)
    {
        endScrub(s.moved);
        return;
    }
    int dx = pos.x() - s.last.x();
    s.last = pos;
    if (!s.moved && std::abs(pos.x() - s.start.x()) < valueReset::kMaxTravel)
        return;
    if (!s.moved)
    {
        UndoStack::get().openChunk("Drag Channels");
        s.chunk = true;
        s.moved = true;
    }
    double step = (mods & Qt::ControlModifier) ? 0.01 : (mods & Qt::ShiftModifier) ? 1.0 : 0.1;
    s.accum += dx * step;
    if (mScrubTimer->isActive())
        return;
    qint64 since = mLastScrubApply.isValid() ? mLastScrubApply.elapsed() : 1000;
    mScrubTimer->start(int(std::max<qint64>(0, 16 - since)));
}

void ChannelBox::applyScrub()
{
    LILA_ZONE();

    if (!mScrub)
        return;
    mLastScrubApply.start();
    Scrub& s = *mScrub;
    Graph& g = Graph::current();
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    for (auto& [plug, value] : s.saved)
    {
        try
        {
            double v = clampToLimits(g.attr(plug), numericOf(value, -1) + s.accum);
            if (std::holds_alternative<int64_t>(value))
                g.setValue(plug, int64_t(std::llround(v)));
            else
                g.setValue(plug, v);
        }
        catch (const std::exception&)
        {
        }
    }
    u.setEnabled(was);
    refresh();
}

void ChannelBox::endScrub(bool commit)
{
    mScrubTimer->stop();
    Scrub s = std::move(*mScrub);
    mScrub.reset();
    if (!s.moved)
        return;
    Graph& g = Graph::current();
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    for (auto& [plug, value] : s.saved)
        g.setValue(plug, value);
    u.setEnabled(was);
    if (commit)
    {
        // Lands in the chunk opened when the drag started, so the drag undoes as one step.
        UndoChunk chunk("Drag Channels");
        for (auto& [plug, value] : s.saved)
        {
            double v = clampToLimits(g.attr(plug), numericOf(value, -1) + s.accum);
            json jv = std::holds_alternative<int64_t>(value) ? json(int64_t(std::llround(v))) : json(std::round(v * 1e4) / 1e4);
            try
            {
                ops::execute("setAttr", {{"args", {g.plugName(plug), jv}}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        }
    }
    if (s.chunk)
        UndoStack::get().closeChunk();
    scheduleRefresh();
}

std::vector<int> ChannelBox::resetTargets(int clicked) const
{
    std::vector<int> rows = highlightedRows();
    if (std::find(rows.begin(), rows.end(), clicked) == rows.end())
        rows = {clicked};
    return rows;
}

void ChannelBox::resetRows(const std::vector<int>& rows)
{
    Graph& g = Graph::current();
    std::vector<int> done;
    {
        UndoChunk chunk("Reset Channels");
        for (int r : rows)
        {
            if (r < 0 || r >= int(mRows.size()) || mRows[r].header)
                continue;
            const Row& row = mRows[r];
            bool any = false;
            for (const std::string& name : row.node ? std::vector<std::string>{g.node(row.node)->path()} : targets())
            {
                try
                {
                    std::string plug = name + "." + row.attr;
                    if (!hasPlug(g, plug))
                        continue;
                    scene::PlugRef ref = g.plug(plug);
                    std::optional<json> def = valueReset::plugDefault(g.attr(ref), ref.component);
                    if (!def)
                        continue;
                    ops::execute("setAttr", {{"args", {plug, *def}}}, CallSource::UI);
                    any = true;
                }
                catch (const std::exception& e)
                {
                    log::error("{}", e.what());
                }
            }
            if (any)
                done.push_back(r);
        }
    }
    for (int r : done)
        if (QTableWidgetItem* cell = mTable->item(r, 1))
            flashReset(mTable->viewport(), mTable->visualItemRect(cell));
    scheduleRefresh();
}

std::vector<int> ChannelBox::highlightedRows() const
{
    std::vector<int> rows;
    for (QTableWidgetItem* item : mTable->selectedItems())
        if (item->column() == 0 && item->row() < int(mRows.size()) && !mRows[item->row()].header)
            rows.push_back(item->row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void ChannelBox::copyValues()
{
    Graph& g = Graph::current();
    if (!g.node(mNode))
        return;
    std::vector<CopiedValue> copied;
    QStringList lines;
    for (int r : highlightedRows())
    {
        const Row& row = mRows[r];
        scene::NodeId owner = row.node ? row.node : mNode;
        scene::Node* n = g.node(owner);
        if (!n)
            continue;
        scene::PlugValue v = g.value(scene::PlugRef{owner, row.attrIndex, -1, row.element});
        CopiedValue c{row.attr, row.node ? n->type().name : std::string(), json()};
        QString text;
        if (row.type == scene::PlugType::Bool)
        {
            c.value = std::get<bool>(v);
            text = std::get<bool>(v) ? "on" : "off";
        }
        else if (row.type == scene::PlugType::Enum || row.type == scene::PlugType::Int)
        {
            c.value = std::get<int64_t>(v);
            text = QString::number(std::get<int64_t>(v));
        }
        else
        {
            double d = numericOf(v, row.component);
            c.value = d;
            text = formatNumber(d);
        }
        lines << QString::fromStdString(row.attr) + " " + text;
        copied.push_back(std::move(c));
    }
    if (copied.empty())
        return;
    gCopied = std::move(copied);
    gCopiedText = lines.join('\n');
    QApplication::clipboard()->setText(gCopiedText);
}

void ChannelBox::pasteValues()
{
    Graph& g = Graph::current();
    QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty())
        return;

    // Text from elsewhere: "attr value" lines, or a single value (or expression) for every highlighted channel.
    std::vector<std::pair<std::string, QString>> lines;
    bool ours = text == gCopiedText.trimmed() && !gCopied.empty();
    if (!ours)
        for (const QString& line : text.split('\n', Qt::SkipEmptyParts))
        {
            QStringList parts = line.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (parts.size() != 2)
            {
                lines.clear();
                break;
            }
            lines.emplace_back(parts[0].toStdString(), parts[1]);
        }
    if (!ours && lines.empty())
    {
        UndoChunk chunk("Paste Channels");
        for (int r : highlightedRows())
            apply(mRows[r], text);
        scheduleRefresh();
        return;
    }

    auto set = [&](const std::string& plug, const json& value) {
        try
        {
            ops::execute("setAttr", {{"args", {plug, value}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    };
    auto parsed = [&](const std::string& plug, const QString& value) -> std::optional<json> {
        try
        {
            const scene::AttrDef& a = g.attr(g.plug(plug));
            scene::PlugRef ref = g.plug(plug);
            scene::PlugType type = ref.component >= 0 ? scene::PlugType::Float : a.type;
            return parseValue(g, plug, type, value);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    };

    UndoChunk chunk("Paste Channels");
    if (ours)
        for (const CopiedValue& c : gCopied)
        {
            if (c.inputType.empty())
            {
                for (const std::string& name : targets())
                    if (hasPlug(g, name + "." + c.attr))
                        set(name + "." + c.attr, c.value);
                continue;
            }
            // Input nodes are per object: paste onto the matching input shown here.
            for (const Row& row : mRows)
                if (row.node && !row.header && row.attr == c.attr && g.node(row.node) && g.node(row.node)->type().name == c.inputType)
                    set(g.node(row.node)->path() + "." + c.attr, c.value);
        }
    else
        for (auto& [attr, value] : lines)
        {
            bool any = false;
            for (const std::string& name : targets())
                if (std::string plug = name + "." + attr; hasPlug(g, plug))
                    if (auto v = parsed(plug, value))
                    {
                        set(plug, *v);
                        any = true;
                    }
            if (any)
                continue;
            for (const Row& row : mRows)
                if (row.node && !row.header && row.attr == attr && g.node(row.node))
                {
                    std::string plug = g.node(row.node)->path() + "." + attr;
                    if (auto v = parsed(plug, value))
                        set(plug, *v);
                }
        }
    scheduleRefresh();
}
} // namespace lila::ui
