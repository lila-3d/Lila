#include "ui/Issues.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui::issues
{
namespace dx = scene::diagnostics;
using dx::Severity;

namespace
{
constexpr int kIssueRole = Qt::UserRole + 1;

void run(const char* name, const json& call)
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** Every issue, worst first, then in the order they came. */
class ValidationPanel final : public QWidget
{
public:
    explicit ValidationPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(4);
        auto* bar = new QHBoxLayout;
        auto* validate = new QToolButton(this);
        validate->setText("Validate");
        validate->setIcon(ui::icon("checklist"));
        validate->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        validate->setToolTip("Run every validator again.");
        connect(validate, &QToolButton::clicked, this, [] { run("validate", json::object()); });
        mShow = new QComboBox(this);
        mShow->addItem("All", 0);
        mShow->addItem("Warnings and errors", 1);
        mShow->addItem("Errors", 2);
        mShow->setToolTip("The least severity listed.");
        connect(mShow, &QComboBox::currentIndexChanged, this, [this](int) { refill(); });
        mFix = new QToolButton(this);
        mFix->setText("Fix");
        mFix->setIcon(ui::icon("tool"));
        mFix->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mFix->setToolTip("Run the selected issues' fixes.");
        connect(mFix, &QToolButton::clicked, this, [this] {
            json ids = json::array();
            for (QTreeWidgetItem* it : mList->selectedItems())
                if (const dx::Issue* i = dx::find(it->data(0, kIssueRole).toULongLong()); i && !i->fix.empty())
                    ids.push_back(i->id);
            if (!ids.empty())
                run("fixIssue", {{"args", ids}});
        });
        auto* clear = new QToolButton(this);
        clear->setText("Clear");
        clear->setToolTip("Remove the selected issues, or every issue when none are selected.");
        connect(clear, &QToolButton::clicked, this, [this] {
            const QList<QTreeWidgetItem*> sel = mList->selectedItems();
            if (sel.isEmpty())
                return run("clearIssues", json::object());
            for (QTreeWidgetItem* it : sel)
                run("clearIssues", {{"id", it->data(0, kIssueRole).toULongLong()}});
        });
        mCount = new QLabel(this);
        bar->addWidget(validate);
        bar->addWidget(mShow);
        bar->addWidget(mFix);
        bar->addWidget(clear);
        bar->addStretch(1);
        bar->addWidget(mCount);
        v->addLayout(bar);

        mList = new QTreeWidget(this);
        mList->setRootIsDecorated(false);
        mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        mList->setHeaderLabels({"Object", "Where", "Message", "Source"});
        mList->header()->setStretchLastSection(false);
        mList->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        mList->setUniformRowHeights(true);
        v->addWidget(mList, 1);
        connect(mList, &QTreeWidget::itemSelectionChanged, this, [this] { updateFix(); });
        connect(mList, &QTreeWidget::itemActivated, this, [](QTreeWidgetItem* it, int) { reveal(it->data(0, kIssueRole).toULongLong()); });
        connect(mList, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* it, int) { reveal(it->data(0, kIssueRole).toULongLong()); });

        mListener = dx::addListener([this] { schedule(); });
        refill();
    }
    ~ValidationPanel() override { dx::removeListener(mListener); }

private:
    /** Selects the issue's object (and its components, when it says which) and shows its component or attribute in the Attribute Editor. */
    static void reveal(uint64_t id)
    {
        const dx::Issue* i = dx::find(id);
        scene::Graph& g = scene::Graph::current();
        if (!i || !g.node(i->node))
            return;
        attributeEditor::revealIssue(id);
        run("selectIssue", {{"args", {id}}});
    }

    void schedule()
    {
        if (mQueued)
            return;
        mQueued = true;
        QTimer::singleShot(0, this, [this] {
            mQueued = false;
            refill();
        });
    }

    void updateFix()
    {
        bool any = false;
        for (QTreeWidgetItem* it : mList->selectedItems())
            if (const dx::Issue* i = dx::find(it->data(0, kIssueRole).toULongLong()); i && !i->fix.empty())
                any = true;
        mFix->setEnabled(any);
    }

    void refill()
    {
        std::vector<uint64_t> selected;
        for (QTreeWidgetItem* it : mList->selectedItems())
            selected.push_back(it->data(0, kIssueRole).toULongLong());
        mList->clear();
        const Severity least = Severity(mShow->currentData().toInt());
        std::vector<const dx::Issue*> shown;
        for (const dx::Issue& i : dx::issues())
            if (i.severity >= least)
                shown.push_back(&i);
        std::stable_sort(shown.begin(), shown.end(), [](const dx::Issue* a, const dx::Issue* b) {
            return std::pair(a->severity, a->blocking()) > std::pair(b->severity, b->blocking());
        });
        scene::Graph& g = scene::Graph::current();
        int counts[3] = {};
        for (const dx::Issue& i : dx::issues())
            ++counts[int(i.severity)];
        for (const dx::Issue* i : shown)
        {
            auto* it = new QTreeWidgetItem(mList);
            it->setData(0, kIssueRole, QVariant::fromValue<qulonglong>(i->id));
            it->setIcon(0, icon(i->severity, i->blocking()));
            const bool resolved = g.node(i->node) != nullptr;
            it->setText(0, QString::fromStdString(resolved ? g.node(i->node)->name() : i->object.empty() ? "(scene)" : i->object));
            it->setToolTip(0, QString::fromStdString(resolved ? g.fullPath(i->node) : i->object.empty() ? "The scene as a whole" : i->object + "\nNo object here yet"));
            if (!resolved && !i->object.empty())
                it->setForeground(0, themeShade(110));
            it->setText(1, where(*i));
            it->setText(2, QString::fromStdString(i->message));
            QString tip = QString::fromStdString(std::string(dx::severityName(i->severity)) + ": " + i->message);
            if (i->blocking())
            {
                std::string b;
                for (const std::string& c : i->blocks)
                    b += (b.empty() ? "" : ", ") + c;
                tip += QString::fromStdString("\nBlocks " + b);
            }
            if (!i->fix.empty())
                tip += "\nHas a fix";
            it->setToolTip(2, tip);
            it->setText(3, QString::fromStdString(i->source));
            if (std::ranges::find(selected, i->id) != selected.end())
                it->setSelected(true);
        }
        for (int c : {0, 1, 3})
            mList->resizeColumnToContents(c);
        mCount->setText(QString("%1 errors, %2 warnings, %3 info").arg(counts[2]).arg(counts[1]).arg(counts[0]));
        updateFix();
    }

    QTreeWidget* mList = nullptr;
    QComboBox* mShow = nullptr;
    QToolButton* mFix = nullptr;
    QLabel* mCount = nullptr;
    int mListener = 0;
    bool mQueued = false;
};
} // namespace

bool badgeInfo() { return PrefStore::get().value<bool>("general", kBadgeInfoPref, false); }

std::optional<Severity> tintFrom()
{
    const std::string v = PrefStore::get().value<std::string>("general", kTintRowsPref, "off");
    if (v == "off")
        return std::nullopt;
    return dx::parseSeverity(v);
}

QColor color(Severity s)
{
    switch (s)
    {
    case Severity::Error: return themeColor("issueError");
    case Severity::Warning: return themeColor("issueWarning");
    case Severity::Info: break;
    }
    return themeColor("issueInfo");
}

QPixmap badge(Severity s, bool blocking, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color(s));
    const QRectF r(0.5, 0.5, size - 1.0, size - 1.0);
    if (blocking)
    {
        // A stop sign: refuses commands until it is cleared.
        QPolygonF oct;
        for (int k = 0; k < 8; ++k)
        {
            const double a = (k * 45.0 + 22.5) * M_PI / 180.0;
            oct << r.center() + QPointF(std::cos(a), std::sin(a)) * (r.width() / 2.0 / std::cos(M_PI / 8.0));
        }
        QPainterPath clip;
        clip.addRect(r);
        p.setClipPath(clip);
        p.drawPolygon(oct);
        p.setClipping(false);
    }
    else
        p.drawEllipse(r);
    // ! for warnings and errors, i for info: a bar and a dot, drawn so they stay crisp at 6 px.
    p.setBrush(QColor(24, 24, 24));
    const double w = std::max(1.0, size / 7.0), cx = size / 2.0;
    const QRectF dot(cx - w / 2, s == Severity::Info ? size * 0.2 : size * 0.68, w, w);
    const QRectF bar(cx - w / 2, s == Severity::Info ? size * 0.4 : size * 0.2, w, size * 0.4);
    p.drawRect(bar);
    p.drawRect(dot);
    return pm;
}

QIcon icon(Severity s, bool blocking)
{
    static QHash<int, QIcon> cache;
    const int key = int(s) * 2 + (blocking ? 1 : 0);
    if (auto it = cache.find(key); it != cache.end())
        return *it;
    QIcon out;
    for (int size : {16, 32})
        out.addPixmap(badge(s, blocking, size));
    cache.insert(key, out);
    return out;
}

std::optional<Worst> worst(const std::vector<const dx::Issue*>& list, bool includeInfo)
{
    std::optional<Worst> w;
    for (const dx::Issue* i : list)
    {
        if (!includeInfo && i->severity == Severity::Info)
            continue;
        if (!w || i->severity > w->severity)
            w = Worst{i->severity, i->blocking()};
        else if (i->severity == w->severity)
            w->blocking = w->blocking || i->blocking();
    }
    return w;
}

QString where(const dx::Issue& i)
{
    if (i.component.empty())
        return QString::fromStdString(i.attr);
    return QString::fromStdString(i.component + (i.attr.empty() ? "" : " › " + i.attr));
}

QString lines(const std::vector<const dx::Issue*>& list)
{
    QStringList out;
    for (const dx::Issue* i : list)
    {
        QString at = where(*i);
        out << QString::fromStdString(std::string(dx::severityName(i->severity)) + (i->blocking() ? " (blocking)" : "") + ": ") +
                   (at.isEmpty() ? QString() : at + ": ") + QString::fromStdString(i->message);
    }
    return out.join('\n');
}

void registerUi()
{
    panels::add({.id = kValidationDock,
                 .title = "Validation",
                 .create = [](QWidget* p) -> QWidget* { return new ValidationPanel(p); },
                 .area = Qt::BottomDockWidgetArea,
                 .icon = "checklist",
                 .open = false,
                 .toggleAction = "window.validation"});
    actions::add({.id = "validate.run",
                  .label = "Validate Scene",
                  .category = "File",
                  .run = [] { run("validate", json::object()); },
                  .doc = "Runs every validator, listing what they find in the Validation panel and badging the outliner.",
                  .python = "lila.ops.validate()"});
}
} // namespace lila::ui::issues
