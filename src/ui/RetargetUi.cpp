#include "ui/RetargetUi.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "retarget/SceneRetarget.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "ui/Actions.h"
#include "ui/FileDialog.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Panels.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <map>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::NodeId;

/** Reads without echoing to the Script Output, as the panel refreshes often. */
json query(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::Script);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

json run(const char* command, const json& call, std::string* error = nullptr)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        if (error)
            *error = e.what();
        log::warn("{}", e.what());
        return nullptr;
    }
}

/** Top joints of every skeleton in the scene, by path. */
QStringList skeletons()
{
    Graph& g = Graph::current();
    QStringList out;
    for (NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n || !scene::rig::isJoint(*n))
            continue;
        const NodeId up = g.parent(id);
        if (up == scene::kNoNode || !scene::rig::isJoint(*g.node(up)))
            out << QString::fromStdString(g.path(id));
    }
    return out;
}

QStringList nodesOfType(const char* type)
{
    Graph& g = Graph::current();
    QStringList out;
    for (NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->type().name == type)
            out << QString::fromStdString(g.path(id));
    return out;
}

/** The top joints of the selected skeletons, in selection order, each once. */
QStringList selectedSkeletons()
{
    Graph& g = Graph::current();
    QStringList out;
    for (NodeId id : g.selection())
        if (const scene::Node* n = g.node(id); n && scene::rig::isJoint(*n))
        {
            NodeId top = id;
            while (g.parent(top) != scene::kNoNode && scene::rig::isJoint(*g.node(g.parent(top))))
                top = g.parent(top);
            const QString p = QString::fromStdString(g.path(top));
            if (!out.contains(p))
                out << p;
        }
    return out;
}

QString titleCase(const std::string& key)
{
    QString out;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const char c = key[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (std::isupper(static_cast<unsigned char>(c)))
            out += QString(" ") + QChar(c).toLower();
        else
            out += QChar(c);
    }
    return out.replace(" fk", " FK").replace(" ik", " IK");
}

/** Choices of the enum-valued settings and rig fields, by key. */
const std::map<std::string, QStringList>& enums()
{
    static const std::map<std::string, QStringList> e = {
        {"rotation", {"interpolated", "oneToOne", "oneToOneReversed", "matchChain", "none"}},
        {"translation", {"none", "globallyScaled", "absolute", "stretchUniform", "stretchNonUniform"}},
        {"role", {"root", "spine", "neck", "head", "jaw", "clavicle", "arm", "leg", "toe", "finger", "tail", "wing", "antenna", "other"}},
        {"side", {"center", "left", "right"}},
    };
    return e;
}

QDoubleSpinBox* number(QWidget* parent, double v)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(-1e6, 1e6);
    s->setDecimals(3);
    s->setSingleStep(0.05);
    s->setValue(v);
    s->setKeyboardTracking(false);
    return s;
}

/**
 * An editor for one JSON value: a check box, a number, a choice, a line of text or three numbers. onEdit gets the
 * new value. Null for values it can't edit (nested objects).
 */
QWidget* valueEditor(QWidget* parent, const std::string& key, const json& v, std::function<void(json)> onEdit)
{
    if (v.is_boolean())
    {
        auto* c = new QCheckBox(parent);
        c->setChecked(v.get<bool>());
        QObject::connect(c, &QCheckBox::toggled, c, [onEdit](bool on) { onEdit(on); });
        return c;
    }
    if (v.is_number_integer())
    {
        auto* s = new QSpinBox(parent);
        s->setRange(-100000, 100000);
        s->setValue(v.get<int>());
        s->setKeyboardTracking(false);
        QObject::connect(s, &QSpinBox::valueChanged, s, [onEdit](int x) { onEdit(x); });
        return s;
    }
    if (v.is_number())
    {
        QDoubleSpinBox* s = number(parent, v.get<double>());
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [onEdit](double x) { onEdit(x); });
        return s;
    }
    if (v.is_string())
    {
        if (auto it = enums().find(key); it != enums().end())
        {
            auto* c = new QComboBox(parent);
            c->addItems(it->second);
            c->setCurrentText(QString::fromStdString(v.get<std::string>()));
            QObject::connect(c, &QComboBox::currentTextChanged, c, [onEdit](const QString& t) { onEdit(t.toStdString()); });
            return c;
        }
        auto* e = new QLineEdit(QString::fromStdString(v.get<std::string>()), parent);
        QObject::connect(e, &QLineEdit::editingFinished, e, [e, onEdit] { onEdit(e->text().toStdString()); });
        return e;
    }
    if (v.is_array() && v.size() == 3 && v[0].is_number())
    {
        auto* w = new QWidget(parent);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        auto boxes = std::make_shared<std::array<QDoubleSpinBox*, 3>>();
        for (int k = 0; k < 3; ++k)
        {
            (*boxes)[size_t(k)] = number(w, v[size_t(k)].get<double>());
            h->addWidget((*boxes)[size_t(k)]);
            QObject::connect((*boxes)[size_t(k)], &QDoubleSpinBox::valueChanged, w, [boxes, onEdit](double) {
                onEdit(json::array({(*boxes)[0]->value(), (*boxes)[1]->value(), (*boxes)[2]->value()}));
            });
        }
        return w;
    }
    return nullptr;
}

/** A form over a JSON object's plain values; onEdit gets {key: value} for each change. */
QWidget* objectForm(QWidget* parent, const json& obj, const std::function<void(json)>& onEdit)
{
    auto* w = new QWidget(parent);
    auto* f = new QFormLayout(w);
    f->setContentsMargins(4, 2, 4, 2);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const auto& [key, value] : obj.items())
    {
        const std::string k = key;
        if (QWidget* e = valueEditor(w, k, value, [k, onEdit](json v) { onEdit(json{{k, std::move(v)}}); }))
            f->addRow(titleCase(k), e);
    }
    return w;
}

/**
 * An editable list of JSON objects shaped like `blank`: one row per object, a column per key, add and remove
 * buttons. onChange gets the whole new list.
 */
QWidget* listEditor(QWidget* parent, const json& list, const json& blank, const std::function<void(json)>& onChange)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* tree = new QTreeWidget(w);
    QStringList headers;
    std::vector<std::string> keys;
    for (const auto& [key, value] : blank.items())
    {
        keys.push_back(key);
        headers << titleCase(key);
    }
    tree->setColumnCount(int(keys.size()));
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setIndentation(0);
    Hotkeys::passThrough(tree);
    auto data = std::make_shared<json>(list.is_array() ? list : json::array());
    for (size_t r = 0; r < data->size(); ++r)
    {
        auto* item = new QTreeWidgetItem(tree);
        for (size_t c = 0; c < keys.size(); ++c)
        {
            const std::string k = keys[c];
            const json value = (*data)[r].contains(k) ? (*data)[r][k] : blank[k];
            if (QWidget* e = valueEditor(tree, k, value, [data, r, k, onChange](json nv) {
                    (*data)[r][k] = std::move(nv);
                    onChange(*data);
                }))
                tree->setItemWidget(item, int(c), e);
        }
    }
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    v->addWidget(tree, 1);
    auto* bar = new QHBoxLayout;
    auto* add = new QToolButton(w);
    add->setIcon(icon("plus"));
    add->setToolTip("Add a row");
    auto* remove = new QToolButton(w);
    remove->setIcon(icon("minus"));
    remove->setToolTip("Remove the selected row");
    bar->addWidget(add);
    bar->addWidget(remove);
    bar->addStretch(1);
    v->addLayout(bar);
    QObject::connect(add, &QToolButton::clicked, w, [data, blank, onChange] {
        data->push_back(blank);
        onChange(*data);
    });
    QObject::connect(remove, &QToolButton::clicked, w, [tree, data, onChange] {
        const int r = tree->indexOfTopLevelItem(tree->currentItem());
        if (r < 0 || r >= int(data->size()))
            return;
        data->erase(data->begin() + r);
        onChange(*data);
    });
    return w;
}

QWidget* scrolled(QWidget* inner)
{
    auto* s = new QScrollArea;
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setWidget(inner);
    return s;
}

// The Retargeter panel ---------------------------------------------------------------------------------------------

class RetargeterPanel : public QWidget
{
public:
    explicit RetargeterPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(2, 2, 2, 2);
        v->setSpacing(2);

        auto* top = new QHBoxLayout;
        mPick = new QComboBox(this);
        mPick->setToolTip("The retargeter to edit");
        top->addWidget(mPick, 1);
        auto button = [&](const char* iconName, const QString& tip, std::function<void()> fn) {
            auto* b = new QToolButton(this);
            b->setIcon(icon(iconName));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, fn);
            top->addWidget(b);
        };
        button("plus", "New retargeter from the selection: select a joint of the source skeleton, then one of the target's", [this] {
            const QStringList sel = selectedSkeletons();
            if (sel.size() != 2)
            {
                log::warn("Retargeter: select a joint of the source skeleton, then one of the target skeleton");
                return;
            }
            const json r = run("retargeter", {{"source", sel[0].toStdString()}, {"target", sel[1].toStdString()}});
            if (r.is_object())
                mCurrent = r["retargeter"].get<std::string>();
            refresh();
        });
        button("player-play", "Retarget: key the target into a take", [this] {
            if (mCurrent.empty())
                return;
            const json r = run("retarget", {{"retargeter", mCurrent}, {"mode", mMode->currentText().toStdString()}});
            if (r.is_object())
                mReport->setText(reportText(r));
        });
        button("arrows-maximize", "Align the target's retarget pose to the source's", [this] {
            if (!mCurrent.empty())
                run("alignRetargetPose", {{"args", {mCurrent}}});
        });
        button("camera", "Capture both skeletons' current poses as this pair's retarget poses", [this] {
            if (!mCurrent.empty())
                run("alignRetargetPose", {{"args", {mCurrent}}, {"align", false}, {"captureSource", true}, {"captureTarget", true}});
        });
        button("restore", "Reset the retarget poses to each rig's own", [this] {
            if (!mCurrent.empty())
                run("alignRetargetPose", {{"args", {mCurrent}}, {"align", false}, {"reset", true}});
        });
        v->addLayout(top);

        auto* modeRow = new QHBoxLayout;
        modeRow->addWidget(new QLabel("Mode", this));
        mMode = new QComboBox(this);
        mMode->addItems({"auto", "fk", "semantic"});
        mMode->setToolTip("fk: chain to chain, the same kind of body. semantic: a body with other limbs. auto: semantic when the leg counts differ.");
        modeRow->addWidget(mMode);
        modeRow->addStretch(1);
        v->addLayout(modeRow);

        mTabs = new QTabWidget(this);
        v->addWidget(mTabs, 1);
        mReport = new QLabel(this);
        mReport->setWordWrap(true);
        mReport->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(mReport);

        connect(mPick, &QComboBox::currentTextChanged, this, [this](const QString& t) {
            if (mFilling)
                return;
            mCurrent = t.toStdString();
            rebuild();
        });
        QPointer<RetargeterPanel> self(this);
        mListener = Graph::current().addListener([self](scene::Change c, NodeId id) {
            if (!self || c == scene::Change::Selection)
                return;
            if (c == scene::Change::Value)
            {
                const scene::Node* n = Graph::current().node(id);
                if (!n || (n->type().name != retarget::kRetargeterType && n->type().name != retarget::kRigType))
                    return;
            }
            self->scheduleRefresh();
        });
        refresh();
    }

    ~RetargeterPanel() override { Graph::current().removeListener(mListener); }

    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        refresh();
    }

    void select(const std::string& retargeter)
    {
        mCurrent = retargeter;
        refresh();
    }

private:
    void scheduleRefresh()
    {
        if (mPending || !isVisible() || mEditing)
            return;
        mPending = true;
        QTimer::singleShot(0, this, [this] {
            mPending = false;
            refresh();
        });
    }

    void refresh()
    {
        const QStringList all = nodesOfType(retarget::kRetargeterType);
        mFilling = true;
        mPick->clear();
        mPick->addItems(all);
        if (mCurrent.empty() || !all.contains(QString::fromStdString(mCurrent)))
            mCurrent = all.isEmpty() ? std::string() : all.front().toStdString();
        mPick->setCurrentText(QString::fromStdString(mCurrent));
        mFilling = false;
        rebuild();
    }

    /** Applies a settings patch and keeps the panel as it is (the edit came from it). */
    void edit(const json& call)
    {
        mEditing = true;
        json c = call;
        c["args"] = {mCurrent};
        run("editRetargeter", c);
        mEditing = false;
    }

    void editRig(const std::string& rig, const json& call)
    {
        mEditing = true;
        json c = call;
        c["args"] = {rig};
        run("editRetargetRig", c);
        mEditing = false;
    }

    void rebuild()
    {
        const int tab = mTabs->currentIndex();
        while (mTabs->count() > 0)
        {
            QWidget* w = mTabs->widget(0);
            mTabs->removeTab(0);
            w->deleteLater();
        }
        if (mCurrent.empty())
        {
            auto* hint = new QLabel("No retargeter yet. Select a joint of the source skeleton, then one of the target's, and press +. "
                                    "Animate > Retarget Animation does it all in one step.");
            hint->setWordWrap(true);
            mTabs->addTab(hint, "Retargeter");
            return;
        }
        const json s = query("editRetargeter", {{"args", {mCurrent}}, {"query", true}});
        if (!s.is_object())
            return;
        mTabs->addTab(chainsTab(s), "Chains");
        mTabs->addTab(rootTab(s), "Root");
        mTabs->addTab(listsTab(s), "Pins and Contacts");
        mTabs->addTab(rigsTab(), "Rigs");
        mTabs->setCurrentIndex(std::max(tab, 0));
    }

    QWidget* chainsTab(const json& s)
    {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        auto* tree = new QTreeWidget(w);
        tree->setHeaderLabels({"Target", "Source", "FK", "IK", "Plant"});
        tree->setRootIsDecorated(false);
        tree->setIndentation(0);
        Hotkeys::passThrough(tree);
        QStringList sources{""};
        Graph& g = Graph::current();
        const scene::Node* r = g.find(mCurrent);
        if (r)
            if (const NodeId rn = retarget::rigNodeFor(g, retarget::sourceTopOf(g, r->id())); rn != scene::kNoNode)
                for (const retarget::Chain& c : retarget::rigOf(g, rn).chains)
                    sources << QString::fromStdString(c.name);
        for (const json& c : s["chains"])
        {
            const std::string target = c["target"];
            auto* item = new QTreeWidgetItem(tree);
            item->setText(0, QString::fromStdString(target));
            item->setData(0, Qt::UserRole, QString::fromStdString(target));
            auto* src = new QComboBox(tree);
            src->addItems(sources);
            src->setCurrentText(QString::fromStdString(c.value("source", std::string())));
            connect(src, &QComboBox::currentTextChanged, this,
                    [this, target](const QString& t) { edit({{"chain", {{"target", target}, {"source", t.toStdString()}}}}); });
            tree->setItemWidget(item, 1, src);
            int col = 2;
            for (const char* part : {"fk", "ik", "plant"})
            {
                auto* on = new QCheckBox(tree);
                on->setChecked(c[part].value("enabled", true));
                connect(on, &QCheckBox::toggled, this, [this, target, part](bool b) { edit({{"chain", {{"target", target}, {part, {{"enabled", b}}}}}}); });
                tree->setItemWidget(item, col++, on);
            }
        }
        tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
        v->addWidget(tree, 1);

        auto* details = new QWidget(w);
        auto* dv = new QVBoxLayout(details);
        dv->setContentsMargins(0, 0, 0, 0);
        v->addWidget(scrolled(details), 1);
        connect(tree, &QTreeWidget::currentItemChanged, this, [this, s, details, dv](QTreeWidgetItem* item) {
            while (QLayoutItem* old = dv->takeAt(0))
            {
                if (old->widget())
                    old->widget()->deleteLater();
                delete old;
            }
            if (!item)
                return;
            const std::string target = item->data(0, Qt::UserRole).toString().toStdString();
            for (const json& c : s["chains"])
                if (c["target"] == target)
                    for (const char* part : {"fk", "ik", "plant"})
                    {
                        auto* title = new QLabel(QString("<b>%1</b>").arg(QString(part).toUpper()), details);
                        dv->addWidget(title);
                        dv->addWidget(objectForm(details, c[part], [this, target, part](json patch) {
                            edit({{"chain", {{"target", target}, {part, patch}}}});
                        }));
                    }
            dv->addStretch(1);
        });
        if (tree->topLevelItemCount() > 0)
            tree->setCurrentItem(tree->topLevelItem(0));
        return w;
    }

    QWidget* rootTab(const json& s)
    {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        json globals = json::object();
        for (const char* k : {"enableRoot", "enableFk", "enableIk", "fullBody", "scale", "copyCurves"})
            globals[k] = s[k];
        v->addWidget(new QLabel("<b>Passes</b>", w));
        v->addWidget(objectForm(w, globals, [this](json patch) { edit(patch); }));
        v->addWidget(new QLabel("<b>Root</b>", w));
        v->addWidget(objectForm(w, s["root"], [this](json patch) { edit({{"root", patch}}); }));
        v->addWidget(new QLabel("<b>Stride warping</b>", w));
        v->addWidget(objectForm(w, s["warp"], [this](json patch) { edit({{"warp", patch}}); }));
        v->addStretch(1);
        return scrolled(w);
    }

    QWidget* listsTab(const json& s)
    {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->addWidget(new QLabel("<b>Pinned bones</b>: a bone copying another's transform last", w));
        v->addWidget(listEditor(w, s["pins"], {{"bone", ""}, {"to", ""}, {"translate", true}, {"rotate", true}, {"maintainOffset", true}},
                                [this](json l) { edit({{"pins", l}}); }));
        v->addWidget(new QLabel("<b>Contacts</b>: a hand held on the body where the source's is", w));
        v->addWidget(listEditor(w, s["contacts"], {{"effector", ""}, {"body", ""}, {"bodyParam", 1.0}, {"distance", 8.0}},
                                [this](json l) { edit({{"contacts", l}}); }));
        v->addWidget(new QLabel("<b>Curves</b>: animated attributes copied renamed", w));
        v->addWidget(listEditor(w, s["curves"], {{"source", ""}, {"target", ""}}, [this](json l) { edit({{"curves", l}}); }));
        return scrolled(w);
    }

    QWidget* rigsTab()
    {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        Graph& g = Graph::current();
        const scene::Node* r = g.find(mCurrent);
        if (!r)
            return w;
        for (bool target : {false, true})
        {
            const NodeId top = target ? retarget::targetTopOf(g, r->id()) : retarget::sourceTopOf(g, r->id());
            const NodeId rn = top == scene::kNoNode ? scene::kNoNode : retarget::rigNodeFor(g, top);
            if (rn == scene::kNoNode)
                continue;
            const std::string rig = g.path(rn);
            auto* head = new QHBoxLayout;
            head->addWidget(new QLabel(QString("<b>%1</b>: %2").arg(target ? "Target" : "Source", QString::fromStdString(rig)), w), 1);
            auto* again = new QPushButton("Characterize Again", w);
            again->setToolTip("Find the chains again from the skeleton as it is posed now (the retarget pose becomes this pose)");
            const std::string topPath = g.path(top);
            connect(again, &QPushButton::clicked, this, [topPath] { run("characterize", {{"args", {topPath}}}); });
            head->addWidget(again);
            v->addLayout(head);
            const json def = query("editRetargetRig", {{"args", {rig}}, {"query", true}});
            if (!def.is_object())
                continue;
            for (const std::string& note : def.value("notes", std::vector<std::string>()))
                v->addWidget(new QLabel("Note: " + QString::fromStdString(note), w));
            v->addWidget(objectForm(w, {{"pelvis", def["pelvis"]}, {"ground", def["ground"]}}, [this, rig](json patch) { editRig(rig, patch); }));
            v->addWidget(listEditor(w, def["chains"],
                                    {{"name", ""}, {"role", "other"}, {"side", "center"}, {"index", 0}, {"start", ""}, {"end", ""}, {"goal", ""}},
                                    [this, rig, def](json chains) {
                                        json d = def;
                                        d["chains"] = chains;
                                        editRig(rig, {{"definition", d}});
                                    }));
        }
        v->addStretch(1);
        return scrolled(w);
    }

public:
    static QString reportText(const json& r)
    {
        QStringList lines;
        lines << QString("Keyed %1 frames into take %2 (%3), scale %4, IK miss %5 cm.")
                     .arg(r.value("frames", 0))
                     .arg(QString::fromStdString(r.value("take", std::string())))
                     .arg(QString::fromStdString(r.value("mode", std::string())))
                     .arg(r.value("scale", 1.0), 0, 'f', 3)
                     .arg(r.value("ikError", 0.0), 0, 'f', 2);
        for (const json& n : r.value("notes", json::array()))
            lines << QString::fromStdString(n.get<std::string>());
        return lines.join("\n");
    }

private:
    QComboBox* mPick = nullptr;
    QComboBox* mMode = nullptr;
    QTabWidget* mTabs = nullptr;
    QLabel* mReport = nullptr;
    std::string mCurrent;
    int mListener = -1;
    bool mPending = false, mFilling = false, mEditing = false;
};

QPointer<RetargeterPanel> gPanel;

// The Retarget Animation dialog ------------------------------------------------------------------------------------

class RetargetDialog : public QDialog
{
public:
    explicit RetargetDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle("Retarget Animation");
        setAttribute(Qt::WA_DeleteOnClose);
        auto* v = new QVBoxLayout(this);
        auto* form = new QFormLayout;

        auto* srcRow = new QHBoxLayout;
        mSource = new QComboBox(this);
        mSource->setToolTip("The skeleton whose animation is copied");
        srcRow->addWidget(mSource, 1);
        auto* files = new QToolButton(this);
        files->setText("Files...");
        files->setToolTip("Retarget .bvh or .fbx files instead: each becomes a take named after it");
        srcRow->addWidget(files);
        form->addRow("Source", srcRow);
        mTarget = new QComboBox(this);
        mTarget->setToolTip("The skeleton that gets keyed");
        form->addRow("Target", mTarget);
        mTake = new QLineEdit(this);
        mTake->setPlaceholderText("Automatic (the source's take or name)");
        form->addRow("Take", mTake);
        mMode = new QComboBox(this);
        mMode->addItems({"Automatic", "Same kind of body", "Different body"});
        mMode->setToolTip("Different body: a clip on a body with other limbs (a person's walk on a dog or a spider), kept as a gait.");
        form->addRow("Bodies", mMode);
        mGait = new QComboBox(this);
        mGait->addItems({"auto", "walk", "trot", "pace", "canter", "gallop", "tripod", "tetrapod", "wave"});
        mGait->setToolTip("For a different body: the gait its legs keep");
        form->addRow("Gait", mGait);
        v->addLayout(form);

        mReport = new QLabel(this);
        mReport->setWordWrap(true);
        mReport->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(mReport);

        auto* buttons = new QHBoxLayout;
        auto* open = new QPushButton("Open Retargeter", this);
        open->setToolTip("Every setting, chain by chain, as in Unreal's IK Retargeter");
        auto* preview = new QPushButton("Preview", this);
        preview->setToolTip("Key the target and keep this open; Preview again replaces it, Close takes it back");
        auto* apply = new QPushButton("Retarget", this);
        apply->setDefault(true);
        auto* close = new QPushButton("Close", this);
        buttons->addWidget(open);
        buttons->addStretch(1);
        buttons->addWidget(preview);
        buttons->addWidget(apply);
        buttons->addWidget(close);
        v->addLayout(buttons);

        const QStringList all = skeletons();
        mSource->addItems(all);
        mTarget->addItems(all);
        const QStringList sel = selectedSkeletons();
        if (!sel.isEmpty())
            mSource->setCurrentText(sel[0]);
        if (sel.size() > 1)
            mTarget->setCurrentText(sel[1]);
        else if (all.size() > 1 && mTarget->currentText() == mSource->currentText())
            mTarget->setCurrentIndex(mSource->currentIndex() == 0 ? 1 : 0);

        connect(files, &QToolButton::clicked, this, [this] {
            const QStringList picked = fileDialog::getOpenFileNames(this, "Animation to Retarget", {}, "Animation (*.bvh *.fbx)");
            if (picked.isEmpty())
                return;
            mFiles = picked;
            mSource->clear();
            mSource->addItem(QString("%1 file(s)").arg(picked.size()));
            mSource->setEnabled(false);
        });
        connect(preview, &QPushButton::clicked, this, [this] { go(); });
        connect(apply, &QPushButton::clicked, this, [this] {
            if (!mPreviewed)
                go();
            mPreviewed = false;
            accept();
        });
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        connect(open, &QPushButton::clicked, this, [this] {
            if (mFiles.isEmpty() && !mSource->currentText().isEmpty() && !mTarget->currentText().isEmpty())
            {
                Graph& g = Graph::current();
                const scene::Node* s = g.find(mSource->currentText().toStdString());
                const scene::Node* t = g.find(mTarget->currentText().toStdString());
                if (s && t)
                {
                    NodeId r = retarget::retargeterFor(g, s->id(), t->id());
                    if (r == scene::kNoNode)
                    {
                        const json made = run("retargeter", {{"source", g.path(s->id())}, {"target", g.path(t->id())}});
                        if (made.is_object())
                            r = g.find(made["retargeter"].get<std::string>())->id();
                    }
                    panels::show(kRetargeterDock);
                    if (gPanel && r != scene::kNoNode)
                        gPanel->select(g.path(r));
                }
            }
            else
                panels::show(kRetargeterDock);
        });
    }

    void reject() override
    {
        // Closing after a preview takes it back.
        if (mPreviewed)
            run("undo", json::object());
        mPreviewed = false;
        QDialog::reject();
    }

private:
    void go()
    {
        if (mPreviewed)
            run("undo", json::object());
        mPreviewed = false;
        json call = {{"target", mTarget->currentText().toStdString()}};
        if (mFiles.isEmpty())
            call["source"] = mSource->currentText().toStdString();
        else
        {
            json f = json::array();
            for (const QString& p : mFiles)
                f.push_back(p.toStdString());
            call["files"] = f;
        }
        if (!mTake->text().isEmpty())
            call["take"] = mTake->text().toStdString();
        const char* modes[] = {"auto", "fk", "semantic"};
        call["mode"] = modes[std::clamp(mMode->currentIndex(), 0, 2)];
        call["gait"] = mGait->currentText().toStdString();
        std::string error;
        const json r = run("retarget", call, &error);
        if (r.is_null())
        {
            mReport->setText(QString::fromStdString(error));
            return;
        }
        mPreviewed = true;
        if (r.contains("files"))
        {
            QStringList lines;
            for (const json& one : r["files"])
                lines << QString::fromStdString(one.value("file", std::string())) + ": " +
                             (one.contains("error") ? QString::fromStdString(one["error"].get<std::string>()) : RetargeterPanel::reportText(one));
            mReport->setText(lines.join("\n"));
        }
        else
            mReport->setText(RetargeterPanel::reportText(r));
    }

    QComboBox *mSource = nullptr, *mTarget = nullptr, *mMode = nullptr, *mGait = nullptr;
    QLineEdit* mTake = nullptr;
    QLabel* mReport = nullptr;
    QStringList mFiles;
    bool mPreviewed = false;
};

std::string selectedJoint()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
        if (const scene::Node* n = g.node(id); n && scene::rig::isJoint(*n))
            return g.path(id);
    return {};
}
} // namespace

void showRetargetDialog()
{
    auto* d = new RetargetDialog(QApplication::activeWindow());
    d->show();
}

void registerRetargetUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    actions::add({.id = "anim.retarget",
                  .label = "Retarget Animation...",
                  .category = "Animation",
                  .run = showRetargetDialog,
                  .doc = "Copy one skeleton's animation (or .bvh and .fbx files) onto another, in one step: chains are found and "
                         "matched, poses aligned, feet planted. Works between different kinds of bodies too.",
                  .python = "retarget(source='Hips', target='pelvis')",
                  .icon = "run"});
    actions::add({.id = "anim.characterize",
                  .label = "Characterize Skeleton",
                  .category = "Animation",
                  .run =
                      [] {
                          const std::string j = selectedJoint();
                          if (j.empty())
                          {
                              log::warn("Characterize Skeleton: select a joint of the skeleton");
                              return;
                          }
                          const json r = run("characterize", {{"args", {j}}});
                          if (r.is_object())
                              for (const json& n : r.value("notes", json::array()))
                                  log::warn("Characterize Skeleton: {}", n.get<std::string>());
                      },
                  .enabled = [] { return !selectedJoint().empty(); },
                  .doc = "Find the pelvis, spine, limbs, fingers and tail of the selected skeleton for retargeting, as it is posed now.",
                  .python = "characterize('pelvis')",
                  .icon = "bone"});
    panels::add({.id = kRetargeterDock,
                 .title = "Retargeter",
                 .create = [](QWidget* p) -> QWidget* {
                     auto* panel = new RetargeterPanel(p);
                     gPanel = panel;
                     return panel;
                 },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "replace-user",
                 .open = false,
                 .toggleAction = "window.retargeter",
                 .docs = "retargeting"});
}
} // namespace lila::ui
