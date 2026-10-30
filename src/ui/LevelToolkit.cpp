#include "ui/LevelToolkit.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/DocsUi.h"
#include "ui/AdjustLastOp.h"
#include "ui/FixedRowLayout.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Shelf.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr const char* kLevelCollapsedPref = "/levelToolkit/collapsed";

QWidget* levelSection(const QString& title, QWidget* content, QWidget* parent, const std::string& docsPage = {})
{
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* header = new QToolButton(box);
    header->setText(title);
    header->setCheckable(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setStyleSheet("QToolButton { text-align: left; font-weight: 600; padding: 3px; }");
    std::string key = std::string(kLevelCollapsedPref) + "/" + title.toStdString();
    bool open = !PrefStore::get().value<bool>("general", key, false);
    header->setChecked(open);
    header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    content->setVisible(open);
    QObject::connect(header, &QToolButton::toggled, content, [header, content, key](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        PrefStore::get().set("general", key, !on);
    });
    // The docs button sits inside the header bar, on its right; it takes its own clicks, so it doesn't fold the section.
    if (!docsPage.empty())
    {
        auto* row = new QHBoxLayout(header);
        row->setContentsMargins(0, 0, 3, 0);
        row->addStretch(1);
        row->addWidget(docs::helpButton(docsPage, header));
    }
    v->addWidget(header);
    v->addWidget(content, 1);
    return box;
}

std::vector<const levelToolkit::Section*> sectionsInOrder()
{
    std::vector<const levelToolkit::Section*> out;
    for (const levelToolkit::Section* s : levelToolkit::registry().allInOrder())
        out.push_back(s);
    std::stable_sort(out.begin(), out.end(), [](const auto* a, const auto* b) { return a->order < b->order; });
    return out;
}
} // namespace

Registry<levelToolkit::Section>& levelToolkit::registry()
{
    static Registry<Section> r;
    return r;
}

void levelToolkit::add(Section s, std::string owner)
{
    std::string id = s.id;
    registry().put(std::move(id), std::move(s), std::move(owner));
}

uint64_t levelToolkit::add(ext::Context& ctx, Section s)
{
    if (const std::string* owner = registry().ownerOf(s.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "level toolkit section '" + s.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = s.id;
    add(std::move(s), ctx.owner());
    return ctx.track("levelToolkit:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool levelToolkit::remove(const std::string& id)
{
    return registry().remove(id);
}

void levelToolkit::registerShelf()
{
    auto sync = [] {
        json items = json::array();
        for (const Section* s : sectionsInOrder())
            for (const Item& item : s->items)
                if (!item.shelf.empty() && !item.action.empty())
                    items.push_back({{"label", item.shelf}, {"icon", item.icon}, {"tooltip", ""}, {"target", item.action}});
        if (items.empty())
        {
            shelves::registry().remove("Level");
            return;
        }
        items.insert(items.begin(), json{{"label", "Toolkit"}, {"icon", "map"}, {"tooltip", "Show or hide the Level Toolkit"}, {"target", "window.levelToolkit"}});
        shelves::add({"Level", std::move(items)});
    };
    static const int listener = registry().addListener([sync](RegistryChange, const std::string&, const std::string&) { sync(); });
    (void)listener;
    sync();
}

LevelToolkit::LevelToolkit(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    mScroll = new QScrollArea(this);
    mScroll->setWidgetResizable(true);
    mScroll->setFrameShape(QFrame::NoFrame);
    mScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(mScroll);
    mBody = new QWidget(mScroll);
    new QVBoxLayout(mBody);
    mScroll->setWidget(mBody);

    // Extensions register sections and actions in bursts while they load and unload, so the rebuild waits for the burst.
    mRebuild = new QTimer(this);
    mRebuild->setSingleShot(true);
    mRebuild->setInterval(0);
    connect(mRebuild, &QTimer::timeout, this, [this] { rebuild(); });
    mSectionListener = levelToolkit::registry().addListener([this](RegistryChange, const std::string&, const std::string&) { mRebuild->start(); });
    mActionListener = actions::registry().addListener([this](RegistryChange, const std::string&, const std::string&) { mRebuild->start(); });
    rebuild();
}

LevelToolkit::~LevelToolkit()
{
    levelToolkit::registry().removeListener(mSectionListener);
    actions::registry().removeListener(mActionListener);
}

void LevelToolkit::rebuild()
{
    mButtons.clear();
    QLayout* root = mBody->layout();
    while (QLayoutItem* item = root->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    auto* v = static_cast<QVBoxLayout*>(root);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    int shown = 0;
    for (const levelToolkit::Section* s : sectionsInOrder())
    {
        auto* box = new QWidget(mBody);
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(2);
        auto* rows = new QWidget(box);
        auto* flow = new FixedRowLayout(4, 3);
        flow->setContentsMargins(2, 0, 2, 2);
        rows->setLayout(flow);
        for (const levelToolkit::Item& item : s->items)
            if (item.action.empty())
                flow->endRow();
            else if (QToolButton* b = actionButton(item.action, QString::fromStdString(item.icon), QString::fromStdString(item.text), rows))
                flow->addWidget(optionBox::withOptionBox(b, item.action, rows));
        if (flow->count())
            boxLayout->addWidget(rows);
        else
            delete rows;
        if (s->create)
            if (QWidget* extra = s->create(box))
                boxLayout->addWidget(extra);
        if (boxLayout->count() == 0)
        {
            delete box;
            continue;
        }
        v->addWidget(levelSection(QString::fromStdString(s->title), box, mBody, s->docs));
        ++shown;
    }

    if (!shown)
    {
        auto* empty = new QLabel("No level tools are loaded. Terrain, Settlement, Masonry, Stagehand and other extensions add theirs here.", mBody);
        empty->setWordWrap(true);
        empty->setContentsMargins(4, 4, 4, 4);
        v->addWidget(empty);
    }
    else
    {
        auto* optionsBox = new QWidget(mBody);
        auto* optionsLayout = new QVBoxLayout(optionsBox);
        optionsLayout->setContentsMargins(4, 0, 4, 4);
        optionsLayout->addWidget(new LastOpEditor(optionsBox));
        v->addWidget(levelSection("Tool Options", optionsBox, mBody));
    }
    v->addStretch(1);
    // The panel stops at its narrowest fixed row instead of clipping it.
    mScroll->setMinimumWidth(mBody->minimumSizeHint().width() + mScroll->verticalScrollBar()->sizeHint().width());
}

QToolButton* LevelToolkit::actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent)
{
    const Action* action = actions::find(id);
    if (!action)
        return nullptr;
    auto* b = new QToolButton(parent);
    if (!iconName.isEmpty())
        b->setIcon(icon(iconName));
    b->setText(text.isEmpty() ? QString::fromStdString(action->label) : text);
    b->setToolButtonStyle(iconName.isEmpty() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
    b->setProperty("lilaAction", QString::fromStdString(id));
    b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    connect(b, &QToolButton::clicked, this, [id] {
        if ((QApplication::keyboardModifiers() & Qt::ShiftModifier) && optionBox::available(id))
            optionBox::open(id);
        else
            actions::run(json(id));
    });
    mButtons.push_back(b);
    return b;
}

void LevelToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // Hotkeys are live-editable, so tooltips pick up the current keys whenever the panel shows.
    for (QToolButton* b : mButtons)
        if (std::string id = b->property("lilaAction").toString().toStdString(); actions::find(id))
            b->setToolTip(QString::fromStdString(actions::tooltip(id)));
}
} // namespace lila::ui
