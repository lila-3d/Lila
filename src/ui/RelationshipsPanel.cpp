#include "ui/RelationshipsPanel.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/LightingChannels.h"
#include "ui/AttrView.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <set>

namespace lila::ui
{
namespace
{
using scene::Graph;

json runQuiet(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call);
    }
    catch (const std::exception&)
    {
        return json();
    }
}

const char* kindIcon(const std::string& kind)
{
    if (kind == "channel")
        return "bulb";
    if (kind == "set")
        return "list-details";
    if (kind == "layer")
        return "stack-2";
    return "stack";
}

const char* kindLabel(const std::string& kind)
{
    if (kind == "channel")
        return "Lighting Channel";
    if (kind == "set")
        return "Set";
    if (kind == "layer")
        return "Display Layer";
    return "Anim Layer";
}
} // namespace

RelationshipsPanel::RelationshipsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* top = new QHBoxLayout;
    mSearch = new QLineEdit(this);
    mSearch->setObjectName("relSearch");
    mSearch->setPlaceholderText("Search channels, sets and layers");
    mSearch->setClearButtonEnabled(true);
    connect(mSearch, &QLineEdit::textChanged, this, [this] { scheduleRefresh(); });
    top->addWidget(mSearch, 1);
    auto* add = new QToolButton(this);
    add->setObjectName("relNewChannel");
    add->setIcon(icon("plus"));
    add->setAutoRaise(true);
    add->setToolTip("New lighting channel, with the selection in it");
    connect(add, &QToolButton::clicked, this, [this] {
        // Typed in place: the search field doubles as the name when it names no group yet.
        std::string name = mSearch->text().trimmed().toStdString();
        if (name.empty())
        {
            const std::vector<std::string> taken = scene::lightingChannels::names();
            name = "channel1";
            for (int i = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++i)
                name = "channel" + std::to_string(i);
        }
        try
        {
            ops::execute("lightingChannels", {{"args", mRows}, {"add", {name}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    });
    top->addWidget(add);
    v->addLayout(top);
    mTable = new QTableWidget(this);
    mTable->setObjectName("relTable");
    mTable->horizontalHeader()->setDefaultSectionSize(28);
    mTable->horizontalHeader()->setMinimumSectionSize(24);
    mTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    mTable->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(mTable, 1);
    connect(mTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (mUpdating || item->row() >= int(mRows.size()) || item->column() >= int(mColumns.size()))
            return;
        setMember(mRows[size_t(item->row())], mColumns[size_t(item->column())], item->checkState() == Qt::Checked);
    });
    mListener = Graph::current().addListener([this](scene::Change, scene::NodeId) { scheduleRefresh(); });
    refresh();
}

RelationshipsPanel::~RelationshipsPanel() { Graph::current().removeListener(mListener); }

void RelationshipsPanel::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        refresh();
    });
}

bool RelationshipsPanel::setMember(const std::string& object, const Column& c, bool in)
{
    try
    {
        if (c.kind == "channel")
            ops::execute("lightingChannels", {{"args", {object}}, {in ? "add" : "remove", {c.name}}}, CallSource::UI);
        else if (c.kind == "set")
            ops::execute(in ? "addToSet" : "removeFromSet", {{"args", {c.name, object}}}, CallSource::UI);
        else if (c.kind == "layer")
        {
            if (in)
                ops::execute("editDisplayLayerMembers", {{"args", {c.name, object}}}, CallSource::UI);
            else
                ops::execute("removeFromDisplayLayer", {{"args", {object}}}, CallSource::UI);
        }
        else
            ops::execute(in ? "addAnimLayerMembers" : "removeAnimLayerMembers", {{"args", {object}}, {"layer", c.name}}, CallSource::UI);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        scheduleRefresh();
        return false;
    }
}

void RelationshipsPanel::refresh()
{
    Graph& g = Graph::current();
    const QString q = mSearch->text().trimmed();
    auto shown = [&](const std::string& kind, const std::string& name) {
        return q.isEmpty() || QString::fromStdString(name).contains(q, Qt::CaseInsensitive) ||
               QString(kindLabel(kind)).contains(q, Qt::CaseInsensitive);
    };
    mRows.clear();
    for (scene::NodeId id : attrView::selectionLeadFirst())
        if (const scene::Node* n = g.node(id))
            mRows.push_back(n->path());

    // Each column with the objects in it.
    std::vector<std::pair<Column, std::set<std::string>>> cols;
    for (const std::string& c : scene::lightingChannels::names())
        if (shown("channel", c))
        {
            std::set<std::string> members;
            for (const std::string& row : mRows)
                if (const scene::Node* n = g.find(row))
                    if (const auto ch = scene::lightingChannels::of(*n); std::find(ch.begin(), ch.end(), c) != ch.end())
                        members.insert(row);
            cols.push_back({{"channel", c}, members});
        }
    if (json sets = runQuiet("listSets", json::object()); sets.is_array())
        for (const json& s : sets)
            if (s.is_string() && shown("set", s.get<std::string>()))
            {
                std::set<std::string> members;
                if (json m = runQuiet("setMembers", {{"args", {s}}, {"nodes", true}}); m.is_array())
                    for (const json& x : m)
                        if (x.is_string())
                            if (const scene::Node* n = g.find(x.get<std::string>()))
                                members.insert(n->path());
                cols.push_back({{"set", s.get<std::string>()}, members});
            }
    if (json layers = runQuiet("listDisplayLayers", json::object()); layers.is_array())
        for (const json& l : layers)
            if (l.is_object() && shown("layer", l.value("name", "")))
            {
                std::set<std::string> members;
                for (const json& x : l.value("members", json::array()))
                    if (x.is_string())
                        if (const scene::Node* n = g.find(x.get<std::string>()))
                            members.insert(n->path());
                cols.push_back({{"layer", l.value("name", "")}, members});
            }
    if (json layers = runQuiet("listAnimLayers", json::object()); layers.is_array())
        for (const json& l : layers)
        {
            const std::string name = l.is_object() ? l.value("name", "") : l.is_string() ? l.get<std::string>() : std::string();
            if (name.empty() || !shown("animLayer", name))
                continue;
            std::set<std::string> members;
            json info = l.is_object() && l.contains("members") ? l : runQuiet("editAnimLayer", {{"args", {name}}});
            for (const json& x : info.is_object() ? info.value("members", json::array()) : json::array())
                if (x.is_string())
                {
                    // Members are plugs; an object is in when any of its plugs is.
                    const std::string m = x.get<std::string>();
                    if (const scene::Node* n = g.find(m.substr(0, m.find('.'))))
                        members.insert(n->path());
                }
            cols.push_back({{"animLayer", name}, members});
        }

    mUpdating = true;
    mColumns.clear();
    mTable->clear();
    mTable->setRowCount(int(mRows.size()));
    mTable->setColumnCount(int(cols.size()));
    for (size_t c = 0; c < cols.size(); ++c)
    {
        mColumns.push_back(cols[c].first);
        auto* head = new QTableWidgetItem(icon(kindIcon(cols[c].first.kind)), QString::fromStdString(cols[c].first.name));
        head->setToolTip(QString("%1: %2").arg(kindLabel(cols[c].first.kind), QString::fromStdString(cols[c].first.name)));
        mTable->setHorizontalHeaderItem(int(c), head);
    }
    for (size_t r = 0; r < mRows.size(); ++r)
    {
        const scene::Node* n = g.find(mRows[r]);
        auto* head = new QTableWidgetItem(icon(QString::fromStdString(scene::types::iconFor(n->type(), "box"))), QString::fromStdString(n->name()));
        mTable->setVerticalHeaderItem(int(r), head);
        for (size_t c = 0; c < cols.size(); ++c)
        {
            auto* cell = new QTableWidgetItem;
            cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            cell->setCheckState(cols[c].second.contains(mRows[r]) ? Qt::Checked : Qt::Unchecked);
            mTable->setItem(int(r), int(c), cell);
        }
    }
    mTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    mUpdating = false;
}

void registerRelationshipsPanel()
{
    panels::add({.id = "relationships",
                 .title = "Relationships",
                 .create = [](QWidget* p) -> QWidget* { return new RelationshipsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "affiliate"});
}
} // namespace lila::ui
