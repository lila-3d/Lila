#include "ui/GnSpreadsheet.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Panels.h"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;

constexpr int kPage = 2000;

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString cellText(const json& v)
{
    if (v.is_null())
        return "-";
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer())
        return QString::number(v.get<int64_t>());
    if (v.is_number())
        return QString::number(v.get<double>(), 'g', 5);
    if (v.is_string())
        return qs(v.get<std::string>());
    QStringList parts;
    for (const json& x : v)
        parts << cellText(x);
    return parts.join("  ");
}

/** gnSpreadsheet's rows, fetched a page at a time as the view scrolls. */
class SheetModel final : public QAbstractTableModel
{
public:
    std::string target, component, domain;
    size_t total = 0;
    std::vector<std::string> names;
    std::vector<std::vector<json>> columns;

    void load(json first)
    {
        beginResetModel();
        names.clear();
        columns.clear();
        total = first.value("rows", size_t(0));
        for (const json& c : first.value("columns", json::array()))
        {
            names.push_back(c.value("name", ""));
            columns.emplace_back(c["values"].begin(), c["values"].end());
        }
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() || columns.empty() ? 0 : int(columns[0].size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(names.size()); }

    QVariant data(const QModelIndex& i, int role) const override
    {
        if (role == Qt::DisplayRole && size_t(i.column()) < columns.size() && size_t(i.row()) < columns[size_t(i.column())].size())
            return cellText(columns[size_t(i.column())][size_t(i.row())]);
        if (role == Qt::TextAlignmentRole)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }

    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (role != Qt::DisplayRole)
            return {};
        if (o == Qt::Vertical)
            return section;
        return size_t(section) < names.size() ? qs(names[size_t(section)]) : QVariant();
    }

    bool canFetchMore(const QModelIndex& parent) const override { return !parent.isValid() && !columns.empty() && columns[0].size() < total; }

    void fetchMore(const QModelIndex& parent) override
    {
        if (!canFetchMore(parent))
            return;
        json page;
        try
        {
            page = ops::execute("gnSpreadsheet", {{"args", {target}}, {"component", component}, {"domain", domain}, {"start", columns[0].size()}, {"count", kPage}},
                                CallSource::Script);
        }
        catch (const std::exception&)
        {
            return;
        }
        const json& cols = page["columns"];
        size_t added = cols.empty() ? 0 : cols[0]["values"].size();
        if (!added || cols.size() != columns.size())
            return;
        beginInsertRows({}, int(columns[0].size()), int(columns[0].size() + added) - 1);
        for (size_t c = 0; c < columns.size(); ++c)
            columns[c].insert(columns[c].end(), cols[c]["values"].begin(), cols[c]["values"].end());
        endInsertRows();
    }
};

const char* const kComponents[] = {"mesh", "points", "curves", "instances", "volume"};

class Spreadsheet final : public QWidget
{
public:
    explicit Spreadsheet(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(4, 4, 4, 4);
        auto* bar = new QHBoxLayout();
        mSource = new QLabel(this);
        mComponent = new QComboBox(this);
        mDomain = new QComboBox(this);
        mComponent->setToolTip("The part of the geometry to list.");
        mDomain->setToolTip("What each row is.");
        bar->addWidget(mSource, 1);
        bar->addWidget(mComponent);
        bar->addWidget(mDomain);
        v->addLayout(bar);
        mTable = new QTableView(this);
        mTable->setModel(&mModel);
        mTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        mTable->verticalHeader()->setDefaultSectionSize(20);
        mTable->setWordWrap(false);
        v->addWidget(mTable, 1);
        mStatus = new QLabel(this);
        v->addWidget(mStatus);
        QObject::connect(mComponent, &QComboBox::activated, this, [this](int) {
            mModel.component = mComponent->currentData().toString().toStdString();
            refresh(true);
        });
        QObject::connect(mDomain, &QComboBox::activated, this, [this](int) {
            mModel.domain = mDomain->currentData().toString().toStdString();
            refresh(true);
        });
        mModel.component = "mesh";
        mModel.domain = "point";
        // Follows the selection and the scene as it evaluates; cheap enough to poll while showing.
        auto* timer = new QTimer(this);
        timer->setInterval(300);
        QObject::connect(timer, &QTimer::timeout, this, [this] {
            if (isVisible())
                refresh(false);
        });
        timer->start();
    }

private:
    std::string selectedTarget() const
    {
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id))
                return n->path();
        return {};
    }

    void refresh(bool force)
    {
        scene::Graph& g = scene::Graph::current();
        std::string target = selectedTarget();
        if (!force && target == mModel.target && g.version() == mVersion && g.time() == mTime)
            return;
        mVersion = g.version();
        mTime = g.time();
        mModel.target = target;
        json first;
        if (!target.empty())
            try
            {
                first = ops::execute("gnSpreadsheet",
                                     {{"args", {target}}, {"component", mModel.component}, {"domain", mModel.domain}, {"count", kPage}},
                                     CallSource::Script);
            }
            catch (const std::exception&)
            {
            }
        if (!first.is_object())
        {
            mSource->setText("Select a mesh to see its geometry.");
            mComponent->clear();
            mDomain->clear();
            mModel.load(json::object());
            mStatus->clear();
            return;
        }
        mSource->setText(qs(first.value("source", "")));
        const json& comps = first["components"];
        mComponent->clear();
        for (const char* c : kComponents)
            if (comps.contains(c))
                mComponent->addItem(QString(c).replace(0, 1, QString(c).left(1).toUpper()), c);
        if (!comps.contains(mModel.component) && !comps.empty())
        {
            mModel.component = comps.begin().key();
            mModel.domain = comps.begin().value().begin().key();
            refresh(true);
            return;
        }
        mComponent->setCurrentIndex(std::max(0, mComponent->findData(qs(mModel.component))));
        mDomain->clear();
        if (comps.contains(mModel.component))
            for (auto& [domain, count] : comps[mModel.component].items())
                mDomain->addItem(QString("%1 (%2)").arg(qs(domain)).arg(count.get<int64_t>()), qs(domain));
        if (mDomain->findData(qs(mModel.domain)) < 0 && mDomain->count())
        {
            mModel.domain = mDomain->itemData(0).toString().toStdString();
            refresh(true);
            return;
        }
        mDomain->setCurrentIndex(std::max(0, mDomain->findData(qs(mModel.domain))));
        mModel.load(first);
        mStatus->setText(QString("%1 rows").arg(qulonglong(mModel.total)));
    }

    SheetModel mModel;
    QLabel* mSource = nullptr;
    QLabel* mStatus = nullptr;
    QComboBox* mComponent = nullptr;
    QComboBox* mDomain = nullptr;
    QTableView* mTable = nullptr;
    uint64_t mVersion = ~0ull;
    double mTime = -1e300;
};
} // namespace

void registerGnSpreadsheet()
{
    panels::add({.id = "gnSpreadsheet",
                 .title = "Spreadsheet",
                 .create = [](QWidget* p) -> QWidget* { return new Spreadsheet(p); },
                 .area = Qt::BottomDockWidgetArea,
                 .icon = "table",
                 .open = false,
                 .toggleAction = "window.spreadsheet"});
}
} // namespace lila::ui
