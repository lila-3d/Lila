#include "ui/AttrAuthoring.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <set>

namespace lila::ui::attrAuthoring
{
namespace
{
using scene::Graph;

enum Role
{
    kKind = Qt::UserRole,
    kPayload,
};
constexpr int kNew = 1, kComponent = 2, kCopy = 3;

std::vector<std::string> pathsOf(const std::vector<scene::NodeId>& nodes)
{
    std::vector<std::string> out;
    for (scene::NodeId id : nodes)
        if (const scene::Node* n = Graph::current().node(id))
            out.push_back(n->path());
    return out;
}

void placeBelow(QWidget* popup, QWidget* anchor)
{
    popup->adjustSize();
    QPoint at = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos();
    if (QScreen* s = anchor ? anchor->screen() : QGuiApplication::screenAt(at))
    {
        const QRect r = s->availableGeometry();
        at.setX(std::clamp(at.x(), r.left(), std::max(r.left(), r.right() - popup->width())));
        if (at.y() + popup->height() > r.bottom() && anchor)
            at.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - popup->height());
        at.setY(std::clamp(at.y(), r.top(), std::max(r.top(), r.bottom() - popup->height())));
    }
    popup->move(at);
}

QLabel* statusLabel(QWidget* parent)
{
    auto* l = new QLabel(parent);
    l->setObjectName("status");
    l->setWordWrap(true);
    l->setStyleSheet("color: #e0a050;");
    l->hide();
    return l;
}

void fail(QLabel* status, const QString& why)
{
    status->setText(why);
    status->show();
}

QComboBox* typeCombo(QWidget* parent)
{
    auto* c = new QComboBox(parent);
    c->setObjectName("type");
    for (const std::string& t : typeNames())
        c->addItem(QString::fromStdString(t));
    return c;
}

QComboBox* categoryCombo(const std::vector<scene::NodeId>& nodes, const std::string& current, QWidget* parent)
{
    auto* c = new QComboBox(parent);
    c->setObjectName("category");
    c->setEditable(true);
    c->addItem(QString());
    for (const std::string& cat : categories(nodes))
        c->addItem(QString::fromStdString(cat));
    c->setCurrentText(QString::fromStdString(current));
    c->lineEdit()->setPlaceholderText("No heading");
    return c;
}

QLineEdit* field(const char* name, const QString& placeholder, QWidget* parent)
{
    auto* e = new QLineEdit(parent);
    e->setObjectName(name);
    e->setPlaceholderText(placeholder);
    return e;
}

bool numericType(const std::string& t) { return t == "float" || t == "int"; }
} // namespace

const std::vector<std::string>& typeNames()
{
    static const std::vector<std::string> names = {"float", "int", "bool", "enum", "string", "vector", "color", "matrix", "file", "text"};
    return names;
}

json addAttrFlags(const std::string& type, bool array)
{
    json f = json::object();
    if (type == "string" || type == "file" || type == "text")
    {
        f["dataType"] = array ? "stringArray" : "string";
        if (type == "file")
            f["usedAsFilename"] = true;
        return f;
    }
    static const std::map<std::string, const char*> at = {{"float", "double"}, {"int", "long"}, {"bool", "bool"}, {"enum", "enum"},
                                                          {"vector", "double3"}, {"color", "double3"}, {"matrix", "matrix"}};
    auto it = at.find(type);
    f["attributeType"] = it == at.end() ? "double" : it->second;
    if (type == "color")
        f["usedAsColor"] = true;
    if (array)
        f["multi"] = true;
    return f;
}

json parseValue(const std::string& type, const QString& text, const std::vector<std::string>& enumNames)
{
    const QString t = text.trimmed();
    if (type == "string" || type == "file" || type == "text")
        return t.toStdString();
    if (t.isEmpty())
        return nullptr;
    bool ok = false;
    if (type == "float")
    {
        const double d = t.toDouble(&ok);
        return ok ? json(d) : json();
    }
    if (type == "int")
    {
        const double d = t.toDouble(&ok);
        return ok ? json(int64_t(std::llround(d))) : json();
    }
    if (type == "bool")
    {
        const QString l = t.toLower();
        if (l == "1" || l == "true" || l == "on" || l == "yes")
            return true;
        if (l == "0" || l == "false" || l == "off" || l == "no")
            return false;
        return nullptr;
    }
    if (type == "enum")
    {
        for (size_t i = 0; i < enumNames.size(); ++i)
            if (QString::fromStdString(enumNames[i]).compare(t, Qt::CaseInsensitive) == 0)
                return int64_t(i);
        const int i = t.toInt(&ok);
        return ok && i >= 0 && size_t(i) < std::max<size_t>(enumNames.size(), 1) ? json(int64_t(i)) : json();
    }
    if (type == "vector" || type == "color")
    {
        const QStringList parts = t.split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
        if (parts.size() == 1)
        {
            const double d = parts[0].toDouble(&ok);
            return ok ? json::array({d, d, d}) : json();
        }
        if (parts.size() != 3)
            return nullptr;
        json out = json::array();
        for (const QString& p : parts)
        {
            out.push_back(p.toDouble(&ok));
            if (!ok)
                return nullptr;
        }
        return out;
    }
    return nullptr;
}

QString valueText(const json& v)
{
    if (v.is_string())
        return QString::fromStdString(v.get<std::string>());
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_number())
        return QString::number(v.get<double>(), 'g', 10);
    if (v.is_array() && v.size() == 3)
        return QString("%1 %2 %3").arg(v[0].get<double>(), 0, 'g', 10).arg(v[1].get<double>(), 0, 'g', 10).arg(v[2].get<double>(), 0, 'g', 10);
    return {};
}

bool matches(const scene::AttrDef& def, const QString& query)
{
    const QString q = query.trimmed();
    if (q.isEmpty())
        return true;
    QString nice = QString::fromStdString(def.niceName);
    if (nice.isEmpty())
    {
        // The label the editor makes from a camelCase name, so "max health" finds maxHealth.
        for (char c : def.name)
        {
            if (std::isupper(static_cast<unsigned char>(c)) && !nice.isEmpty())
                nice += ' ';
            nice += QChar(c);
        }
    }
    for (const QString& s : {nice, QString::fromStdString(def.name), QString::fromStdString(def.shortName), QString::fromStdString(def.doc),
                             QString::fromStdString(def.category)})
        if (s.contains(q, Qt::CaseInsensitive))
            return true;
    return false;
}

std::vector<std::string> categories(const std::vector<scene::NodeId>& nodes)
{
    std::vector<std::string> out;
    for (scene::NodeId id : nodes)
        if (const scene::Node* n = Graph::current().node(id))
            for (const scene::AttrDef& d : n->type().attrs)
                if ((d.flags & scene::AttrUser) && !d.category.empty() && std::find(out.begin(), out.end(), d.category) == out.end())
                    out.push_back(d.category);
    return out;
}

AddPopover::AddPopover(std::vector<scene::NodeId> nodes, std::string category, QWidget* parent)
    : QFrame(parent, Qt::Popup), mNodes(std::move(nodes))
{
    setObjectName("attrAddPopover");
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(320);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);
    mSearch = field("search", "New attribute name, or search components", this);
    mSearch->setClearButtonEnabled(true);
    mSearch->installEventFilter(this);
    v->addWidget(mSearch);
    mList = new QListWidget(this);
    mList->setObjectName("list");
    mList->setUniformItemSizes(true);
    mList->setMinimumHeight(180);
    v->addWidget(mList, 1);

    mForm = new QWidget(this);
    mForm->setObjectName("form");
    auto* f = new QFormLayout(mForm);
    f->setContentsMargins(0, 0, 0, 0);
    f->setSpacing(3);
    auto* typeRow = new QWidget(mForm);
    auto* th = new QHBoxLayout(typeRow);
    th->setContentsMargins(0, 0, 0, 0);
    mType = typeCombo(typeRow);
    mArray = new QCheckBox("Array", typeRow);
    mArray->setObjectName("array");
    th->addWidget(mType, 1);
    th->addWidget(mArray);
    f->addRow("Type", typeRow);
    mDefault = field("default", "Default", mForm);
    f->addRow("Default", mDefault);
    auto* range = new QWidget(mForm);
    auto* rh = new QHBoxLayout(range);
    rh->setContentsMargins(0, 0, 0, 0);
    mMin = field("min", "Min", range);
    mMax = field("max", "Max", range);
    rh->addWidget(mMin);
    rh->addWidget(mMax);
    f->addRow("Range", range);
    mEnum = field("enum", "low:medium:high", mForm);
    f->addRow("Options", mEnum);
    mCategory = categoryCombo(mNodes, category, mForm);
    f->addRow("Heading", mCategory);
    auto* create = new QPushButton("Add", mForm);
    create->setObjectName("create");
    f->addRow(QString(), create);
    v->addWidget(mForm);
    mStatus = statusLabel(this);
    v->addWidget(mStatus);
    mForm->hide();

    for (QLineEdit* e : {mDefault, mMin, mMax, mEnum})
        connect(e, &QLineEdit::returnPressed, this, [this] { commitNew(); });
    connect(mCategory->lineEdit(), &QLineEdit::returnPressed, this, [this] { commitNew(); });
    connect(create, &QPushButton::clicked, this, [this] { commitNew(); });
    connect(mType, &QComboBox::currentIndexChanged, this, [this] { updateFormForType(); });
    connect(mSearch, &QLineEdit::textChanged, this, [this] { refill(); });
    connect(mList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { activate(item); });
    connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item->data(kKind).toInt() == kNew)
            activate(item);
    });
    updateFormForType();
    refill();
}

void AddPopover::updateFormForType()
{
    const std::string t = mType->currentText().toStdString();
    mMin->parentWidget()->setEnabled(numericType(t));
    mEnum->setEnabled(t == "enum");
    mDefault->setEnabled(t != "matrix");
    mDefault->setPlaceholderText(t == "vector" || t == "color" ? "x y z" : t == "bool" ? "true or false" : "Default");
}

void AddPopover::refill()
{
    mList->clear();
    const QString text = mSearch->text().trimmed();
    const std::string name = text.toStdString();
    Graph& g = Graph::current();
    std::set<scene::NodeId> mine(mNodes.begin(), mNodes.end());

    if (!name.empty() && Graph::validName(name.substr(0, name.find('.'))))
    {
        bool taken = false;
        for (scene::NodeId id : mNodes)
            if (const scene::Node* n = g.node(id))
                taken |= n->type().attrIndex(name) >= 0;
        if (!taken)
        {
            auto* item = new QListWidgetItem(icon("plus"), QString("New attribute \"%1\"").arg(text), mList);
            item->setData(kKind, kNew);
            item->setData(kPayload, text);
        }
    }
    auto header = [&](const QString& title) {
        auto* h = new QListWidgetItem(title, mList);
        h->setFlags(Qt::NoItemFlags);
        QFont f = h->font();
        f.setBold(true);
        h->setFont(f);
    };
    bool headed = false;
    for (const scene::attrComponents::Component& c : scene::attrComponents::all())
    {
        const QString cname = QString::fromStdString(c.name);
        const QString shown = c.label.empty() ? cname : QString::fromStdString(c.label);
        if (!text.isEmpty() && !cname.contains(text, Qt::CaseInsensitive) && !shown.contains(text, Qt::CaseInsensitive) &&
            !QString::fromStdString(c.doc).contains(text, Qt::CaseInsensitive))
            continue;
        if (!headed)
            header("Components");
        headed = true;
        auto* item = new QListWidgetItem(icon("puzzle"), QString("%1   %2 · %3 attributes").arg(shown, QString::fromStdString(c.source)).arg(c.attrs.size()), mList);
        item->setToolTip(QString::fromStdString(c.doc));
        item->setData(kKind, kComponent);
        item->setData(kPayload, cname);
    }
    headed = false;
    int copies = 0;
    for (scene::NodeId id : g.nodes())
    {
        if (mine.contains(id) || copies >= 20)
            continue;
        const scene::Node* n = g.node(id);
        int user = 0;
        for (const scene::AttrDef& d : n->type().attrs)
            user += (d.flags & scene::AttrUser) && !(d.flags & scene::AttrHidden) ? 1 : 0;
        if (!user || (!text.isEmpty() && !QString::fromStdString(n->name()).contains(text, Qt::CaseInsensitive)))
            continue;
        if (!headed)
            header("Copy from");
        headed = true;
        auto* item = new QListWidgetItem(icon("copy"), QString("%1   %2 attributes").arg(QString::fromStdString(n->path())).arg(user), mList);
        item->setData(kKind, kCopy);
        item->setData(kPayload, QString::fromStdString(n->path()));
        ++copies;
    }
    for (int i = 0; i < mList->count(); ++i)
        if (mList->item(i)->flags() & Qt::ItemIsEnabled)
        {
            mList->setCurrentRow(i);
            break;
        }
    showForm(mList->currentItem() && mList->currentItem()->data(kKind).toInt() == kNew && mForm->isVisible());
}

void AddPopover::showForm(bool on)
{
    mForm->setVisible(on);
    adjustSize();
}

bool AddPopover::activate(QListWidgetItem* item)
{
    if (!item)
        return false;
    const QString payload = item->data(kPayload).toString();
    switch (item->data(kKind).toInt())
    {
    case kNew:
        if (!mForm->isVisible())
        {
            showForm(true);
            mType->setFocus();
            return true;
        }
        return commitNew();
    case kComponent:
    case kCopy:
        try
        {
            if (item->data(kKind).toInt() == kComponent)
                ops::execute("addComponent", {{"args", pathsOf(mNodes)}, {"name", payload.toStdString()}}, CallSource::UI);
            else
            {
                std::vector<std::string> args{payload.toStdString()};
                for (const std::string& p : pathsOf(mNodes))
                    args.push_back(p);
                ops::execute("copyAttr", {{"args", args}}, CallSource::UI);
            }
        }
        catch (const CommandError& e)
        {
            fail(mStatus, e.what());
            return false;
        }
        Q_EMIT committed();
        close();
        return true;
    }
    return false;
}

bool AddPopover::commitNew()
{
    const std::string name = mSearch->text().trimmed().toStdString();
    const std::string type = mType->currentText().toStdString();
    if (name.empty())
    {
        fail(mStatus, "Type the new attribute's name first.");
        return false;
    }
    json call = addAttrFlags(type, mArray->isChecked());
    call["args"] = pathsOf(mNodes);
    call["longName"] = name;
    if (!mCategory->currentText().trimmed().isEmpty())
        call["category"] = mCategory->currentText().trimmed().toStdString();
    std::vector<std::string> enumNames;
    if (type == "enum")
    {
        std::string names = mEnum->text().trimmed().toStdString();
        if (names.empty())
            names = "off:on";
        call["enumName"] = names;
        for (const QString& s : QString::fromStdString(names).split(':', Qt::SkipEmptyParts))
            enumNames.push_back(s.toStdString());
    }
    if (!mDefault->text().trimmed().isEmpty() && type != "matrix")
    {
        json v = parseValue(type, mDefault->text(), enumNames);
        if (v.is_null())
        {
            fail(mStatus, "The default doesn't read as a " + QString::fromStdString(type) + ".");
            return false;
        }
        call["defaultValue"] = v;
    }
    if (numericType(type))
        for (auto [box, flag] : {std::pair{mMin, "minValue"}, {mMax, "maxValue"}})
            if (!box->text().trimmed().isEmpty())
            {
                bool ok = false;
                const double d = box->text().trimmed().toDouble(&ok);
                if (!ok)
                {
                    fail(mStatus, "The range takes numbers.");
                    return false;
                }
                call[flag] = d;
            }
    try
    {
        UndoChunk chunk("Add Attribute");
        json plugs = ops::execute("addAttr", call, CallSource::UI);
        if (type == "text")
            ops::execute("editAttr", {{"args", plugs}, {"type", "text"}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        fail(mStatus, e.what());
        return false;
    }
    Q_EMIT committed();
    close();
    return true;
}

bool AddPopover::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mSearch && event->type() == QEvent::KeyPress)
    {
        auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up)
        {
            QApplication::sendEvent(mList, event);
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)
        {
            activate(mList->currentItem());
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

EditPopover::EditPopover(std::vector<scene::NodeId> nodes, std::string attr, QWidget* parent)
    : QFrame(parent, Qt::Popup), mNodes(std::move(nodes)), mAttr(std::move(attr))
{
    setObjectName("attrEditPopover");
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(320);
    installEventFilter(this);
    const std::vector<std::string> paths = pathsOf(mNodes);
    try
    {
        mBefore = ops::execute("editAttr", {{"args", {paths.at(0) + "." + mAttr}}, {"query", true}});
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        mBefore = json::object();
    }
    auto* f = new QFormLayout(this);
    f->setContentsMargins(8, 8, 8, 8);
    f->setSpacing(4);
    auto text = [&](const char* key) { return QString::fromStdString(mBefore.value(key, std::string())); };
    mName = field("name", "", this);
    mName->setText(QString::fromStdString(mAttr));
    f->addRow("Name", mName);
    mNice = field("niceName", "Made from the name", this);
    mNice->setText(text("niceName"));
    f->addRow("Label", mNice);
    auto* typeRow = new QWidget(this);
    auto* th = new QHBoxLayout(typeRow);
    th->setContentsMargins(0, 0, 0, 0);
    mType = typeCombo(typeRow);
    mType->setCurrentText(text("type"));
    mArray = new QCheckBox("Array", typeRow);
    mArray->setObjectName("array");
    mArray->setChecked(mBefore.value("array", false));
    th->addWidget(mType, 1);
    th->addWidget(mArray);
    f->addRow("Type", typeRow);
    mDefault = field("default", "", this);
    mDefault->setText(valueText(mBefore.value("default", json())));
    if (mBefore.contains("enum") && mBefore["default"].is_number_integer())
    {
        const int64_t i = mBefore["default"].get<int64_t>();
        if (i >= 0 && size_t(i) < mBefore["enum"].size())
            mDefault->setText(QString::fromStdString(mBefore["enum"][size_t(i)].get<std::string>()));
    }
    f->addRow("Default", mDefault);
    auto rangeRow = [&](const char* label, QLineEdit*& lo, QLineEdit*& hi, const char* loName, const char* hiName, const char* loKey, const char* hiKey) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        lo = field(loName, "None", w);
        hi = field(hiName, "None", w);
        if (mBefore.contains(loKey))
            lo->setText(valueText(mBefore[loKey]));
        if (mBefore.contains(hiKey))
            hi->setText(valueText(mBefore[hiKey]));
        h->addWidget(lo);
        h->addWidget(hi);
        f->addRow(label, w);
    };
    rangeRow("Range", mMin, mMax, "min", "max", "min", "max");
    rangeRow("Slider", mSoftMin, mSoftMax, "softMin", "softMax", "softMin", "softMax");

    auto* enumBox = new QWidget(this);
    auto* ev = new QHBoxLayout(enumBox);
    ev->setContentsMargins(0, 0, 0, 0);
    mEnum = new QListWidget(enumBox);
    mEnum->setObjectName("enum");
    mEnum->setDragDropMode(QAbstractItemView::InternalMove);
    mEnum->setMaximumHeight(110);
    for (const json& e : mBefore.value("enum", json::array()))
    {
        auto* item = new QListWidgetItem(QString::fromStdString(e.get<std::string>()), mEnum);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    auto* buttons = new QVBoxLayout;
    auto* plus = new QToolButton(enumBox);
    plus->setObjectName("enumAdd");
    plus->setIcon(icon("plus"));
    plus->setToolTip("Add an option");
    auto* minus = new QToolButton(enumBox);
    minus->setIcon(icon("minus"));
    minus->setToolTip("Remove the selected option");
    buttons->addWidget(plus);
    buttons->addWidget(minus);
    buttons->addStretch(1);
    ev->addWidget(mEnum, 1);
    ev->addLayout(buttons);
    connect(plus, &QToolButton::clicked, this, [this] {
        auto* item = new QListWidgetItem(QString("option%1").arg(mEnum->count()), mEnum);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        mEnum->editItem(item);
    });
    connect(minus, &QToolButton::clicked, this, [this] { delete mEnum->currentItem(); });
    f->addRow("Options", enumBox);
    auto showFor = [this, enumBox, f] {
        const std::string t = mType->currentText().toStdString();
        f->setRowVisible(enumBox, t == "enum");
        mMin->parentWidget()->setEnabled(numericType(t));
        mSoftMin->parentWidget()->setEnabled(numericType(t));
        adjustSize();
    };
    connect(mType, &QComboBox::currentIndexChanged, this, showFor);

    auto* flags = new QWidget(this);
    auto* fh = new QHBoxLayout(flags);
    fh->setContentsMargins(0, 0, 0, 0);
    mKeyable = new QCheckBox("Keyable", flags);
    mKeyable->setObjectName("keyable");
    mKeyable->setChecked(mBefore.value("keyable", false));
    mChannelBox = new QCheckBox("Channel Box", flags);
    mChannelBox->setObjectName("channelBox");
    mChannelBox->setChecked(mBefore.value("channelBox", false));
    mHidden = new QCheckBox("Hidden", flags);
    mHidden->setObjectName("hidden");
    mHidden->setChecked(mBefore.value("hidden", false));
    fh->addWidget(mKeyable);
    fh->addWidget(mChannelBox);
    fh->addWidget(mHidden);
    f->addRow(QString(), flags);
    mDoc = field("doc", "What it is for", this);
    mDoc->setText(text("doc"));
    f->addRow("Doc", mDoc);
    mCategory = categoryCombo(mNodes, mBefore.value("category", std::string()), this);
    f->addRow("Heading", mCategory);

    auto* row = new QHBoxLayout;
    auto* del = new QPushButton(icon("trash"), "Delete", this);
    del->setObjectName("delete");
    auto* ok = new QPushButton("Apply", this);
    ok->setObjectName("apply");
    ok->setDefault(true);
    row->addWidget(del);
    row->addStretch(1);
    row->addWidget(ok);
    f->addRow(row);
    mStatus = statusLabel(this);
    f->addRow(mStatus);
    connect(ok, &QPushButton::clicked, this, [this] { apply(); });
    connect(del, &QPushButton::clicked, this, [this] {
        try
        {
            std::vector<std::string> plugs;
            for (const std::string& p : pathsOf(mNodes))
                plugs.push_back(p + "." + mAttr);
            ops::execute("deleteAttr", {{"args", plugs}}, CallSource::UI);
            Q_EMIT committed();
            close();
        }
        catch (const CommandError& e)
        {
            fail(mStatus, e.what());
        }
    });
    for (QLineEdit* e : {mName, mNice, mDefault, mMin, mMax, mSoftMin, mSoftMax, mDoc})
        connect(e, &QLineEdit::returnPressed, this, [this] { apply(); });
    connect(mCategory->lineEdit(), &QLineEdit::returnPressed, this, [this] { apply(); });
    showFor();
}

bool EditPopover::apply()
{
    json call = json::object();
    std::vector<std::string> plugs;
    for (const std::string& p : pathsOf(mNodes))
        plugs.push_back(p + "." + mAttr);
    call["args"] = plugs;
    const std::string type = mType->currentText().toStdString();
    auto changed = [&](const char* flag, const json& now, const json& before) {
        if (now != before)
            call[flag] = now;
    };
    changed("longName", mName->text().trimmed().toStdString(), mAttr);
    changed("niceName", mNice->text().trimmed().toStdString(), mBefore.value("niceName", std::string()));
    changed("type", type, mBefore.value("type", std::string()));
    changed("array", mArray->isChecked(), mBefore.value("array", false));
    std::vector<std::string> enumNames;
    for (int i = 0; i < mEnum->count(); ++i)
        if (!mEnum->item(i)->text().trimmed().isEmpty())
            enumNames.push_back(mEnum->item(i)->text().trimmed().toStdString());
    if (type == "enum")
    {
        std::string joined;
        for (const std::string& e : enumNames)
            joined += (joined.empty() ? "" : ":") + e;
        if (enumNames != mBefore.value("enum", std::vector<std::string>{}))
            call["enumName"] = joined;
    }
    if (type != "matrix" && mDefault->isModified())
    {
        json v = parseValue(type, mDefault->text(), enumNames);
        if (v.is_null())
        {
            fail(mStatus, "The default doesn't read as a " + QString::fromStdString(type) + ".");
            return false;
        }
        call["defaultValue"] = v;
    }
    if (numericType(type))
        for (auto [box, flag, key] : {std::tuple{mMin, "minValue", "min"}, {mMax, "maxValue", "max"}, {mSoftMin, "softMinValue", "softMin"},
                                      {mSoftMax, "softMaxValue", "softMax"}})
        {
            const QString t = box->text().trimmed();
            bool ok = true;
            const json now = t.isEmpty() ? json(false) : json(t.toDouble(&ok));
            if (!ok)
            {
                fail(mStatus, "Ranges take numbers.");
                return false;
            }
            const json before = mBefore.contains(key) ? mBefore[key] : json(false);
            if (now != before && !(now.is_number() && before.is_number() && now.get<double>() == before.get<double>()))
                call[flag] = now;
        }
    changed("keyable", mKeyable->isChecked(), mBefore.value("keyable", false));
    changed("channelBox", mChannelBox->isChecked(), mBefore.value("channelBox", false));
    changed("hidden", mHidden->isChecked(), mBefore.value("hidden", false));
    changed("doc", mDoc->text().trimmed().toStdString(), mBefore.value("doc", std::string()));
    changed("category", mCategory->currentText().trimmed().toStdString(), mBefore.value("category", std::string()));
    if (call.size() > 1)
        try
        {
            ops::execute("editAttr", call, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            fail(mStatus, e.what());
            return false;
        }
    Q_EMIT committed();
    close();
    return true;
}

bool EditPopover::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
    {
        close();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

AddPopover* showAdd(std::vector<scene::NodeId> nodes, QWidget* anchor, std::string category)
{
    auto* p = new AddPopover(std::move(nodes), std::move(category), anchor ? anchor->window() : nullptr);
    p->setAttribute(Qt::WA_DeleteOnClose);
    placeBelow(p, anchor);
    p->show();
    p->findChild<QLineEdit*>("search")->setFocus();
    return p;
}

EditPopover* showEdit(std::vector<scene::NodeId> nodes, const std::string& attr, QWidget* anchor)
{
    auto* p = new EditPopover(std::move(nodes), attr, anchor ? anchor->window() : nullptr);
    p->setAttribute(Qt::WA_DeleteOnClose);
    placeBelow(p, anchor);
    p->show();
    p->findChild<QLineEdit*>("name")->setFocus();
    return p;
}
} // namespace lila::ui::attrAuthoring
