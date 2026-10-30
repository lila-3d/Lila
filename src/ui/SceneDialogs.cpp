#include "ui/AchievementRules.h"
#include "ui/SceneDialogs.h"

#include "core/CrashHandler.h"
#include "core/CrashReport.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/Examples.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "ui/DocsUi.h"
#include "ui/Theme.h"
#include "ui/WelcomeNews.h"
#include "vcs/SourceControl.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QCheckBox>
#include <QDate>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace lila::ui
{
namespace fs = std::filesystem;
namespace history = io::history;
using json = nlohmann::json;

namespace
{
constexpr int kPathRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kRevisionRole = Qt::UserRole + 2;
constexpr int kDescriptionRole = Qt::UserRole + 3;

/** An example as a card: thumbnail, title and the start of its description. */
class ExampleCard : public QStyledItemDelegate
{
public:
    static constexpr QSize kSize{240, 210};
    static constexpr int kThumbHeight = 135;

    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return kSize; }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(opt.rect).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool selected = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
        p->setPen(QPen(themeColor(selected ? "highlight" : hover ? "hover" : "border"), selected ? 2.0 : 1.0));
        p->setBrush(themeColor("base"));
        p->drawRoundedRect(card, 4, 4);

        const QRect thumb(opt.rect.left() + 1, opt.rect.top() + 1, opt.rect.width() - 2, kThumbHeight);
        const QPixmap pix = index.data(Qt::DecorationRole).value<QPixmap>();
        if (pix.isNull())
            p->fillRect(thumb, themeColor("altBase"));
        else
        {
            const QPixmap scaled = pix.scaled(thumb.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p->drawPixmap(thumb, scaled, QRect((scaled.width() - thumb.width()) / 2, (scaled.height() - thumb.height()) / 2, thumb.width(), thumb.height()));
        }

        QRect text = opt.rect.adjusted(8, kThumbHeight + 6, -8, -6);
        QFont title = opt.font;
        title.setBold(true);
        p->setFont(title);
        p->setPen(themeColor("text"));
        const int titleHeight = QFontMetrics(title).height();
        p->drawText(text.left(), text.top(), text.width(), titleHeight, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(title).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text.width()));
        text.setTop(text.top() + titleHeight + 2);
        p->setFont(opt.font);
        p->setPen(themeColor("dimText"));
        // Whole lines only, with the last one elided, so the text never runs past the card.
        const QFontMetrics fm(opt.font);
        const int lines = std::max(1, text.height() / fm.lineSpacing());
        QString rest = index.data(kDescriptionRole).toString();
        for (int i = 0; i < lines && !rest.isEmpty(); ++i)
        {
            QString line;
            if (i == lines - 1)
                line = fm.elidedText(rest, Qt::ElideRight, text.width());
            else
            {
                int n = rest.size();
                while (n > 0 && fm.horizontalAdvance(rest.left(n)) > text.width())
                    n = rest.lastIndexOf(' ', n - 1) > 0 ? rest.lastIndexOf(' ', n - 1) : n - 1;
                line = rest.left(std::max(n, 1));
            }
            p->drawText(text.left(), text.top() + i * fm.lineSpacing(), text.width(), fm.lineSpacing(), Qt::AlignLeft | Qt::AlignVCenter, line);
            rest = rest.mid(line.size()).trimmed();
        }
        p->restore();
    }
};

QString qpath(const fs::path& p)
{
    return QString::fromStdWString(p.wstring());
}

QString when(int64_t unixSeconds)
{
    if (!unixSeconds)
        return "-";
    return QDateTime::fromSecsSinceEpoch(unixSeconds).toString("yyyy-MM-dd  hh:mm:ss");
}

QString kindLabel(history::Kind k)
{
    switch (k)
    {
    case history::Kind::Autosave:
        return "Autosave";
    case history::Kind::Emergency:
        return "Emergency save";
    case history::Kind::Backup:
        return "Backup";
    case history::Kind::Increment:
        return "Increment";
    case history::Kind::Revision:
        return "Source control";
    }
    return {};
}

QTreeWidget* makeList(QWidget* parent, const QStringList& headers)
{
    auto* list = new QTreeWidget(parent);
    list->setHeaderLabels(headers);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->header()->setStretchLastSection(true);
    return list;
}
} // namespace

void openCrashReport(const fs::path& dir)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(dir / "report.txt")));
}

void openCrashFolder(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(dir)));
}

void copyCrashReport(const fs::path& dir)
{
    std::string text = crash::readText(dir / "report.txt");
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text.data(), qsizetype(text.size())));
}

RecoveryDialog::RecoveryDialog(const std::vector<io::recovery::Candidate>& candidates, const fs::path& crashReport, QWidget* parent)
    : QDialog(parent), mCandidates(candidates)
{
    setWindowTitle(crashReport.empty() ? "Recover Unsaved Work" : "Lila Crashed");
    auto* layout = new QVBoxLayout(this);

    if (!crashReport.empty())
    {
        std::string summary = crash::reportSummary(crashReport);
        const std::string prefix = "Lila crashed: ";
        if (summary.starts_with(prefix))
            summary = summary.substr(prefix.size());
        auto* title = new QLabel("<b>Lila crashed last time.</b>", this);
        layout->addWidget(title);
        auto* what = new QLabel(QString::fromStdString(summary), this);
        what->setWordWrap(true);
        what->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(what);
        auto* row = new QHBoxLayout();
        auto* open = new QPushButton("Open Crash Report", this);
        auto* folder = new QPushButton("Open Crash Folder", this);
        auto* copy = new QPushButton("Copy Crash Report", this);
        copy->setToolTip("Everything a developer needs to see what crashed, ready to paste into a bug report or a chat");
        auto* copied = new QLabel(this);
        row->addWidget(open);
        row->addWidget(folder);
        row->addWidget(copy);
        row->addWidget(copied);
        row->addStretch(1);
        layout->addLayout(row);
        connect(open, &QPushButton::clicked, this, [crashReport] { openCrashReport(crashReport); });
        connect(folder, &QPushButton::clicked, this, [crashReport] { openCrashFolder(crashReport); });
        connect(copy, &QPushButton::clicked, this, [crashReport, copied] {
            copyCrashReport(crashReport);
            copied->setText("Copied.");
        });
        layout->addSpacing(8);
    }

    if (candidates.empty())
    {
        auto* buttons = new QHBoxLayout();
        auto* close = new QPushButton("Close", this);
        close->setDefault(true);
        buttons->addStretch(1);
        buttons->addWidget(close);
        layout->addLayout(buttons);
        connect(close, &QPushButton::clicked, this, [this] { finish(Choice::Discard); });
        resize(640, 0);
        return;
    }

    QString intro = candidates.size() == 1 ? "This scene has unsaved work:" : "These scenes have unsaved work:";
    if (crashReport.empty())
        intro = "Lila did not shut down cleanly. " + intro;
    layout->addWidget(new QLabel(intro, this));

    mList = makeList(this, {"Scene", "Folder", "Recovered from", "Recovered at", "Saved file at"});
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto& c = candidates[i];
        auto* item = new QTreeWidgetItem(mList);
        item->setText(0, c.scene.empty() ? QString("untitled") : qpath(c.scene.filename()));
        item->setText(1, c.scene.empty() ? QString() : qpath(c.scene.parent_path()));
        item->setText(2, kindLabel(c.kind));
        item->setText(3, when(c.fileTime));
        item->setText(4, c.savedTime ? when(c.savedTime) : QString("never saved"));
        item->setToolTip(2, qpath(c.file));
        item->setData(0, kPathRole, int(i));
    }
    for (int col = 0; col < 5; ++col)
        mList->resizeColumnToContents(col);
    mList->setCurrentItem(mList->topLevelItem(0));
    layout->addWidget(mList, 1);
    if (candidates.size() > 1)
        layout->addWidget(new QLabel("Lila opens one scene at a time. The others stay available under File > Versions.", this));

    auto* buttons = new QHBoxLayout();
    auto* discard = new QPushButton("Discard", this);
    discard->setToolTip("Keep the saved files as they are. The recovered versions stay under File > Recovered Scenes.");
    mOpenSaved = new QPushButton("Open Saved", this);
    mOpenSaved->setToolTip("Open the last saved version of the selected scene");
    auto* recover = new QPushButton("Recover", this);
    recover->setToolTip("Open the recovered version as unsaved changes to the scene; Save writes the real file");
    recover->setDefault(true);
    buttons->addWidget(discard);
    buttons->addStretch(1);
    buttons->addWidget(mOpenSaved);
    buttons->addWidget(recover);
    layout->addLayout(buttons);

    connect(discard, &QPushButton::clicked, this, [this] { finish(Choice::Discard); });
    connect(mOpenSaved, &QPushButton::clicked, this, [this] { finish(Choice::OpenSaved); });
    connect(recover, &QPushButton::clicked, this, [this] { finish(Choice::Recover); });
    connect(mList, &QTreeWidget::itemActivated, this, [this] { finish(Choice::Recover); }, Qt::QueuedConnection);
    connect(mList, &QTreeWidget::currentItemChanged, this, [this] { mOpenSaved->setEnabled(selected().savedTime != 0); });
    mOpenSaved->setEnabled(selected().savedTime != 0);
    resize(760, 300);
}

const io::recovery::Candidate& RecoveryDialog::selected() const
{
    QTreeWidgetItem* item = mList->currentItem();
    int i = item ? item->data(0, kPathRole).toInt() : 0;
    return mCandidates[size_t(std::clamp(i, 0, int(mCandidates.size()) - 1))];
}

void RecoveryDialog::finish(Choice c)
{
    mChoice = c;
    accept();
}

namespace
{
constexpr int kRecentWidth = 340;
constexpr int kNewsWidth = 400;

/** The Essentials link as the welcome banner: the site's night-blue horizon behind the icon, title and call to action. */
class EssentialsBanner : public QPushButton
{
public:
    explicit EssentialsBanner(QWidget* parent) : QPushButton(parent)
    {
        setObjectName("startupEssentials");
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setStyleSheet(QString("QLabel { background: transparent; }"
                              "QLabel#startupHeroTitle { color: #f4f6fa; font-size: 22pt; font-weight: 600; }"
                              "QLabel#startupHeroSubtitle { color: %1; font-size: 12pt; }")
                          .arg(themeColor("highlight").lighter(130).name()));
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(24, 18, 24, 18);
        row->setSpacing(18);
        row->addStretch(1);
        auto* icon = new QLabel(this);
        icon->setPixmap(qApp->windowIcon().pixmap(QSize(60, 60), devicePixelRatioF()));
        row->addWidget(icon);
        auto* text = new QVBoxLayout;
        text->setSpacing(2);
        for (auto [label, name] : {std::pair{"Welcome to Lila", "startupHeroTitle"}, std::pair{"New to Lila? Read the Essentials  →", "startupHeroSubtitle"}})
        {
            auto* l = new QLabel(QString::fromUtf8(label), this);
            l->setObjectName(name);
            text->addWidget(l);
        }
        row->addLayout(text);
        row->addStretch(1);
        for (QLabel* l : findChildren<QLabel*>())
            l->setAttribute(Qt::WA_TransparentForMouseEvents);
        setMinimumHeight(row->sizeHint().height());
    }

protected:
    void enterEvent(QEnterEvent* e) override
    {
        QPushButton::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent* e) override
    {
        QPushButton::leaveEvent(e);
        update();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool hover = underMouse();
        const QColor accent = themeColor("highlight");
        QPainterPath shape;
        shape.addRoundedRect(r, 8, 8);

        QLinearGradient sky(r.topLeft(), r.bottomLeft());
        sky.setColorAt(0, QColor(0x10, 0x13, 0x1a));
        sky.setColorAt(1, QColor(0x1a, 0x24, 0x36));
        p.fillPath(shape, sky);

        p.save();
        p.setClipPath(shape);
        // An elliptical glow rising from the horizon: a radial gradient squashed vertically.
        p.translate(r.center().x(), r.bottom());
        p.scale(1.0, r.height() / (r.width() * 0.45));
        QRadialGradient glow(QPointF(0, 0), r.width() * 0.45);
        QColor c = accent;
        c.setAlphaF(hover ? 0.55f : 0.35f);
        glow.setColorAt(0, c);
        c.setAlphaF(0);
        glow.setColorAt(1, c);
        p.fillRect(QRectF(-r.width(), -r.width(), 2 * r.width(), 2 * r.width()), glow);
        p.restore();

        QLinearGradient horizon(r.topLeft(), r.topRight());
        QColor line = accent.lighter(140);
        line.setAlphaF(0);
        horizon.setColorAt(0, line);
        horizon.setColorAt(1, line);
        line.setAlphaF(hover ? 1.0f : 0.8f);
        horizon.setColorAt(0.5, line);
        p.setPen(QPen(QBrush(horizon), 1.5));
        p.drawLine(QPointF(r.left() + 8, r.bottom() - 0.75), QPointF(r.right() - 8, r.bottom() - 0.75));

        QColor border = accent;
        border.setAlphaF(hover ? 0.9f : 0.45f);
        p.setPen(QPen(border, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
    }
};

/** A news item: its 16:9 banner, then title and text; a click opens its link. */
class NewsCard : public QFrame
{
public:
    NewsCard(const news::Item& item, QWidget* parent) : QFrame(parent), mUrl(QString::fromStdString(item.url))
    {
        setObjectName("newsCard");
        if (!mUrl.isEmpty())
        {
            setCursor(Qt::PointingHandCursor);
            setToolTip(mUrl);
        }
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 10);
        layout->setSpacing(4);
        const int width = kNewsWidth - 2;
        auto* banner = new QLabel(this);
        banner->setFixedSize(width, width * 9 / 16);
        if (QPixmap pix(news::imageFile(item)); !pix.isNull())
            banner->setPixmap(pix.scaled(banner->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        else
            banner->setStyleSheet(QString("background: %1;").arg(themeColor("altBase").name()));
        layout->addWidget(banner);

        static const std::map<std::string, QString> kTags = {{"release", "Release"}, {"spotlight", "Community Spotlight"}, {"tutorial", "Tutorial"}};
        QStringList meta;
        if (auto it = kTags.find(item.type); it != kTags.end())
            meta << it->second;
        if (!item.date.empty())
            meta << QLocale().toString(QDate::fromString(QString::fromStdString(item.date), Qt::ISODate), QLocale::LongFormat);
        if (!item.author.empty())
            meta << QString::fromStdString(item.author);
        addText(meta.join("  /  "), "newsMeta");
        addText(QString::fromStdString(item.title), "newsTitle");
        addText(QString::fromStdString(item.body), "newsBody");
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && !mUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(mUrl));
    }

private:
    void addText(const QString& text, const char* name)
    {
        if (text.isEmpty())
            return;
        auto* label = new QLabel(text, this);
        label->setObjectName(name);
        label->setWordWrap(true);
        label->setContentsMargins(10, 0, 10, 0);
        layout()->addWidget(label);
    }

    QString mUrl;
};

QWidget* makeColumn(QWidget* parent, const QString& title, QVBoxLayout*& layout)
{
    auto* column = new QWidget(parent);
    layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* heading = new QLabel(title, column);
    heading->setObjectName("startupHeading");
    layout->addWidget(heading);
    return column;
}
} // namespace

StartupDialog::StartupDialog(Hooks hooks, bool welcome, QWidget* parent) : QDialog(parent), mHooks(std::move(hooks)), mWelcome(welcome)
{
    setObjectName("startupDialog");
    // A tool window rather than a popup: popups close whenever the app loses focus.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet(QString("QDialog#startupDialog { background: #2e2e2e; border: 1px solid #5a5a5a; }"
                          "QLabel#startupTitle { color: #e0e0e0; font-size: 13pt; }"
                          "QLabel#startupHeading { color: #e0e0e0; font-size: 11pt; }"
                          "QScrollArea#startupPage, QWidget#startupPageBody { background: transparent; }"
                          "QFrame#newsCard { background: %1; border: 1px solid %2; border-radius: 4px; }"
                          "QFrame#newsCard:hover { border-color: %3; }"
                          "QLabel#newsMeta { color: %4; }"
                          "QLabel#newsTitle { color: #f0f0f0; font-size: 11pt; font-weight: bold; }"
                          "QLabel#newsBody { color: %4; }")
                      .arg(themeColor("base").name(), themeColor("border").name(), themeColor("highlight").name(), themeColor("dimText").name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);

    mList = makeList(this, {"Name", "Folder", "Modified"});
    json recent = PrefStore::get().value("general", "/recentFiles", json::array());
    std::error_code ec;
    for (const json& r : recent.is_array() ? recent : json::array())
    {
        if (!r.is_string())
            continue;
        fs::path p = fs::path(r.get<std::string>());
        if (!fs::is_regular_file(p, ec))
            continue;
        auto* item = new QTreeWidgetItem(mList);
        item->setText(0, qpath(p.filename()));
        item->setText(1, qpath(p.parent_path()));
        item->setText(2, when(history::unixTime(p)));
        item->setToolTip(0, qpath(p));
        item->setData(0, kPathRole, qpath(p));
    }
    const bool anyRecent = mList->topLevelItemCount() > 0;
    if (!anyRecent)
    {
        auto* none = new QTreeWidgetItem(mList, {"No recent scenes"});
        none->setFlags(Qt::NoItemFlags);
    }
    else
        mList->setCurrentItem(mList->topLevelItem(0));
    mList->resizeColumnToContents(0);
    mList->setColumnWidth(1, 300);

    if (welcome)
    {
        auto* essentials = new EssentialsBanner(this);
        connect(essentials, &QPushButton::clicked, this, [this] {
            reject();
            docs::open("essentials");
        });
        layout->addWidget(essentials);
        layout->addSpacing(10);

        auto* page = new QWidget;
        page->setObjectName("startupPageBody");
        auto* columns = new QHBoxLayout(page);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(20);
        // Zero-stretch ends: they only take the slack when Recent Scenes is hidden, centring the rest.
        columns->addStretch(0);

        // Too narrow for the folder; it is in each row's tooltip.
        mList->hideColumn(1);
        QVBoxLayout* recentLayout = nullptr;
        QWidget* recentColumn = makeColumn(page, "Recent Scenes", recentLayout);
        recentColumn->setMinimumWidth(kRecentWidth);
        recentLayout->addWidget(mList, 1);
        recentColumn->setVisible(anyRecent);
        columns->addWidget(recentColumn, 1);

        QVBoxLayout* newsLayout = nullptr;
        mNews = makeColumn(page, "News", newsLayout);
        mNews->setFixedWidth(kNewsWidth);
        mNewsCards = new QVBoxLayout;
        mNewsCards->setSpacing(12);
        newsLayout->addLayout(mNewsCards);
        newsLayout->addStretch(1);
        columns->addWidget(mNews);

        QVBoxLayout* examplesLayout = nullptr;
        QWidget* examplesColumn = makeColumn(page, "Examples", examplesLayout);
        mExamples = new QListWidget(examplesColumn);
        mExamples->setViewMode(QListView::IconMode);
        mExamples->setResizeMode(QListView::Adjust);
        mExamples->setMovement(QListView::Static);
        mExamples->setWrapping(true);
        mExamples->setUniformItemSizes(true);
        mExamples->setSpacing(6);
        mExamples->setGridSize(ExampleCard::kSize + QSize(12, 12));
        mExamples->setFrameShape(QFrame::NoFrame);
        mExamples->viewport()->setAutoFillBackground(false);
        mExamples->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mExamples->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mExamples->setItemDelegate(new ExampleCard(mExamples));
        for (const io::examples::Example& x : io::examples::list())
        {
            auto* item = new QListWidgetItem(mExamples);
            item->setText(QString::fromStdString(x.name));
            item->setData(kDescriptionRole, QString::fromStdString(x.description));
            item->setToolTip("<p>" + QString::fromStdString(x.description).toHtmlEscaped() + "</p>");
            item->setData(kKindRole, int(mExampleList.size()));
            if (!x.thumbnail.empty())
                item->setData(Qt::DecorationRole, QPixmap(qpath(x.thumbnail)));
            mExampleList.push_back(x);
        }
        examplesLayout->addWidget(mExamples);
        examplesLayout->addStretch(1);
        columns->addWidget(examplesColumn);
        columns->addStretch(0);
        showNews(news::cached());
        QPointer<StartupDialog> self = this;
        news::refresh([self](const std::vector<news::Item>& items) {
            if (self)
                self->showNews(items);
        });

        auto* scroll = new QScrollArea;
        scroll->setObjectName("startupPage");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        // A single tab shows no tab bar; a second one brings it up.
        auto* tabs = new QTabWidget(this);
        tabs->setDocumentMode(true);
        tabs->setTabBarAutoHide(true);
        tabs->addTab(scroll, "Home");
        layout->addWidget(tabs, 1);
    }
    else
    {
        auto* title = new QLabel("Recent Scenes", this);
        title->setObjectName("startupTitle");
        layout->addWidget(title);
        layout->addWidget(mList, 1);
    }

    auto* row = new QHBoxLayout();
    const char* showPref = welcome ? kShowWelcomePref : kShowRecentPref;
    auto* show = new QCheckBox(welcome ? "Show welcome at startup" : "Show on startup", this);
    show->setChecked(PrefStore::get().value<bool>("general", showPref, true));
    connect(show, &QCheckBox::toggled, this, [showPref](bool on) { PrefStore::get().set("general", showPref, on); });
    auto* newScene = new QPushButton("New Scene", this);
    auto* browse = new QPushButton("Open...", this);
    row->addWidget(show);
    row->addStretch(1);
    row->addWidget(newScene);
    row->addWidget(browse);
    layout->addLayout(row);

    // Queued: opening a scene tears this dialog down, and Qt's double-click handler still uses the list after emitting.
    connect(mList, &QTreeWidget::itemActivated, this, [this] { openSelected(mList); }, Qt::QueuedConnection);
    if (mExamples)
    {
        connect(mExamples, &QListWidget::itemActivated, this, [this] { openExampleSelected(); }, Qt::QueuedConnection);
        // One selection across the two lists, so Enter opens what is highlighted.
        connect(mExamples, &QListWidget::itemPressed, mList, [this] { mList->setCurrentItem(nullptr); });
        connect(mList, &QTreeWidget::itemPressed, mExamples, [this] { mExamples->setCurrentItem(nullptr); });
    }
    connect(newScene, &QPushButton::clicked, this, [this] {
        auto create = mHooks.newScene;
        reject();
        if (create)
            create();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        auto open = mHooks.open;
        reject();
        if (open)
            open({});
    });
    resize(welcome ? QSize(kRecentWidth + kNewsWidth + examplesWidth(2) + 4 * 20 + 2 * 16, 900) : QSize(680, 380));
    qApp->installEventFilter(this);
}

void StartupDialog::showNews(const std::vector<news::Item>& items)
{
    while (QLayoutItem* old = mNewsCards->takeAt(0))
    {
        delete old->widget();
        delete old;
    }
    for (const news::Item& item : items)
        mNewsCards->addWidget(new NewsCard(item, mNews));
    mNews->setVisible(!items.empty());
    // Without news, the examples take its space.
    const int across = items.empty() ? 3 : 2;
    const int rows = std::max(1, (int(mExampleList.size()) + across - 1) / across);
    mExamples->setFixedSize(examplesWidth(across), rows * mExamples->gridSize().height() + 2 * mExamples->frameWidth() + 2 * mExamples->spacing());
}

int StartupDialog::examplesWidth(int across) const
{
    return across * mExamples->gridSize().width() + 2 * mExamples->frameWidth() + 2 * mExamples->spacing() + 2;
}

void StartupDialog::present()
{
    QWidget* anchor = parentWidget();
    QScreen* screen = anchor ? anchor->screen() : QGuiApplication::primaryScreen();
    QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1600, 900);
    // Never bigger than the screen: the page scrolls instead.
    resize(size().boundedTo(area.size() * 0.92));
    move(area.center() - QPoint(width() / 2, height() / 2));
    show();
    raise();
    activateWindow();
    mList->setFocus();
    mReclaimFocus.start();
}

bool StartupDialog::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::KeyPress && isVisible() && qobject_cast<QWidget*>(obj) && static_cast<QWidget*>(obj)->window() == this)
    {
        auto* ke = static_cast<QKeyEvent*>(e);
        // The main window's shortcuts don't reach a tool window, and these two replace what the dialog is for.
        if (ke->matches(QKeySequence::Open) || ke->matches(QKeySequence::New))
        {
            auto run = ke->matches(QKeySequence::Open) ? std::function<void()>([open = mHooks.open] {
                if (open)
                    open({});
            })
                                                       : mHooks.newScene;
            reject();
            if (run)
                run();
            return true;
        }
    }
    // The main window finishes activating after the dialog is shown at startup and would take the keyboard. Only then:
    // later activations are the user going back to the window.
    if (e->type() == QEvent::WindowActivate && obj == parentWidget() && isVisible() && mReclaimFocus.isValid() &&
        mReclaimFocus.elapsed() < 2000)
        QTimer::singleShot(0, this, [this] {
            activateWindow();
            mList->setFocus();
        });
    if (e->type() == QEvent::MouseButtonPress && isVisible() && !mWelcome)
    {
        auto* me = static_cast<QMouseEvent*>(e);
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w->window() != this && !geometry().contains(me->globalPosition().toPoint()))
            reject();
    }
    return QDialog::eventFilter(obj, e);
}

void StartupDialog::openExampleSelected()
{
    QListWidgetItem* item = mExamples->currentItem();
    if (!item)
        return;
    io::examples::Example x = mExampleList[size_t(item->data(kKindRole).toInt())];
    auto open = mHooks.openExample;
    accept();
    if (open)
        open(x);
}

void StartupDialog::openSelected(QTreeWidget* list)
{
    QTreeWidgetItem* item = list->currentItem();
    if (!item || item->data(0, kPathRole).toString().isEmpty())
        return;
    fs::path p = item->data(0, kPathRole).toString().toStdWString();
    if (list->indexOfTopLevelItem(item) == 0)
        achievementRules::lastSceneReopened();
    auto open = mHooks.open;
    accept();
    if (open)
        open(p);
}

VersionsDialog::VersionsDialog(const fs::path& scene, Hooks hooks, QWidget* parent)
    : QDialog(parent), mScene(scene), mHooks(std::move(hooks))
{
    setWindowTitle("Versions of " + QString::fromStdString(history::stemFor(scene)) +
                   (scene.empty() ? QString() : qpath(scene.extension())));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    mList = makeList(this, {"Kind", "Date", "Size", "Details"});
    mList->setSortingEnabled(false);
    layout->addWidget(mList, 1);

    auto* row = new QHBoxLayout();
    auto* note = new QLabel("Open loads a version as a new untitled scene. Restore replaces the current scene; "
                            "nothing is written until you save.",
                            this);
    note->setWordWrap(true);
    row->addWidget(note, 1);
    mOpen = new QPushButton("Open", this);
    mRestore = new QPushButton("Restore", this);
    auto* close = new QPushButton("Close", this);
    row->addWidget(mOpen);
    row->addWidget(mRestore);
    row->addWidget(close);
    layout->addLayout(row);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(mOpen, &QPushButton::clicked, this, [this] { act(false); });
    connect(mRestore, &QPushButton::clicked, this, [this] { act(true); });
    connect(mList, &QTreeWidget::itemActivated, this, [this] { act(false); }, Qt::QueuedConnection);
    auto refresh = [this] {
        QTreeWidgetItem* item = mList->currentItem();
        bool ok = item && item->data(0, kKindRole).isValid();
        mOpen->setEnabled(ok);
        mRestore->setEnabled(ok);
    };
    connect(mList, &QTreeWidget::currentItemChanged, this, refresh);
    populate();
    refresh();
    resize(820, 420);
}

void VersionsDialog::populate()
{
    for (const history::Version& v : history::list(mScene))
    {
        auto* item = new QTreeWidgetItem(mList);
        item->setText(0, kindLabel(v.kind));
        item->setText(1, when(v.time));
        item->setText(2, QLocale().formattedDataSize(qint64(v.size)));
        item->setText(3, qpath(v.path.filename()));
        item->setToolTip(3, qpath(v.path));
        item->setData(0, kPathRole, qpath(v.path));
        item->setData(0, kKindRole, int(v.kind));
    }
    if (mList->topLevelItemCount())
        mList->setCurrentItem(mList->topLevelItem(0));
    for (int col = 0; col < 3; ++col)
        mList->resizeColumnToContents(col);

    if (mScene.empty())
        return;
    QPointer<VersionsDialog> self = this;
    fs::path scene = mScene;
    vcs::SourceControl::get().async(
        [scene]() -> json {
            auto provider = vcs::SourceControl::get().providerFor(scene.parent_path());
            json out = json::array();
            if (!provider)
                return out;
            for (const vcs::Revision& r : provider->history(scene, 50))
                out.push_back(r.toJson());
            return out;
        },
        [self](const vcs::AsyncResult& r) {
            if (!self)
                return;
            if (r.ok)
                self->addRevisions(r.value);
            else
                log::warn("versions: {}", r.error);
        });
}

void VersionsDialog::addRevisions(const json& revisions)
{
    for (const json& r : revisions)
    {
        auto* item = new QTreeWidgetItem(mList);
        std::string rev = r.value("revision", "");
        std::string change = r.value("change", "");
        std::string desc = r.value("description", "");
        if (size_t nl = desc.find('\n'); nl != std::string::npos)
            desc.resize(nl);
        item->setText(0, kindLabel(history::Kind::Revision));
        item->setText(1, when(r.value("time", int64_t(0))));
        item->setText(3, QString::fromStdString((change.empty() ? "#" + rev : change) + "  " + r.value("user", "") + ": " + desc));
        item->setData(0, kKindRole, int(history::Kind::Revision));
        item->setData(0, kRevisionRole, QString::fromStdString(rev));
    }
    if (!mList->currentItem() && mList->topLevelItemCount())
        mList->setCurrentItem(mList->topLevelItem(0));
}

void VersionsDialog::act(bool restore)
{
    QTreeWidgetItem* item = mList->currentItem();
    if (!item || !item->data(0, kKindRole).isValid())
        return;
    auto kind = history::Kind(item->data(0, kKindRole).toInt());
    QString label = kindLabel(kind).toLower() + " " + item->text(1).simplified();
    if (restore)
    {
        QString text = "Replace the current scene with the " + label + "?\n\nThe scene file is only changed when you save; "
                       "reopen it to go back.";
        if (io::modified())
            text += "\n\nUnsaved changes to the current scene will be lost.";
        if (QMessageBox::question(this, "Restore Version", text) != QMessageBox::Yes)
            return;
    }
    else if (mHooks.confirmDiscard && !mHooks.confirmDiscard())
        return;

    if (kind != history::Kind::Revision)
    {
        load(fs::path(item->data(0, kPathRole).toString().toStdWString()), restore, label);
        return;
    }
    // Source control revisions are fetched to a cache next to the autosaves first.
    std::string rev = item->data(0, kRevisionRole).toString().toStdString();
    fs::path scene = mScene;
    std::string safeRev = rev.substr(0, 12);
    fs::path dest = history::baseDir("versions") / (scene.stem().string() + "." + safeRev + scene.extension().string());
    QPointer<VersionsDialog> self = this;
    vcs::SourceControl::get().async(
        [scene, rev, dest]() -> json {
            auto provider = vcs::SourceControl::get().providerFor(scene.parent_path());
            if (!provider)
                throw vcs::VcsError("the scene is no longer under source control");
            provider->fetchRevision(scene, rev, dest);
            return dest.string();
        },
        [self, restore, label, dest](const vcs::AsyncResult& r) {
            if (!self)
                return;
            if (!r.ok)
            {
                QMessageBox::warning(self, "Versions", QString::fromStdString(r.error));
                return;
            }
            self->load(dest, restore, label);
        });
}

void VersionsDialog::load(const fs::path& file, bool restore, const QString& label)
{
    try
    {
        std::string title = history::stemFor(mScene) + (mScene.empty() ? "" : mScene.extension().string()) + " (" +
                            label.toStdString() + ")";
        io::loadVersion(file, mScene, restore, title);
        if (mHooks.opened)
            mHooks.opened();
        accept();
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "Versions", QString::fromStdString(e.what()));
    }
}
} // namespace lila::ui
