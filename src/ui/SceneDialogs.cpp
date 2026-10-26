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
#include "ui/Theme.h"
#include "vcs/SourceControl.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

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

StartupDialog::StartupDialog(Hooks hooks, bool welcome, QWidget* parent) : QDialog(parent), mHooks(std::move(hooks))
{
    setObjectName("startupDialog");
    // A tool window rather than a popup: popups close whenever the app loses focus.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet("QDialog#startupDialog { background: #2e2e2e; border: 1px solid #5a5a5a; }"
                  "QLabel#startupTitle { color: #e0e0e0; font-size: 13pt; }"
                  "QLabel#startupHeading { color: #e0e0e0; font-size: 11pt; }");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    auto heading = [&](const QString& text, const char* name) {
        auto* label = new QLabel(text, this);
        label->setObjectName(name);
        layout->addWidget(label);
    };

    if (welcome)
    {
        heading("Welcome to Lila", "startupTitle");
        layout->addSpacing(6);
        heading("Examples", "startupHeading");
        mExamples = makeList(this, {"Example", "About"});
        mExamples->setIconSize(QSize(64, 36));
        for (const io::examples::Example& x : io::examples::list())
        {
            auto* item = new QTreeWidgetItem(mExamples);
            item->setText(0, QString::fromStdString(x.name));
            item->setText(1, QString::fromStdString(x.description));
            item->setToolTip(1, "<p>" + QString::fromStdString(x.description).toHtmlEscaped() + "</p>");
            item->setToolTip(0, qpath(x.scene));
            item->setData(0, kPathRole, qpath(x.scene));
            item->setData(0, kKindRole, int(mExampleList.size()));
            if (!x.thumbnail.empty())
                item->setIcon(0, QIcon(qpath(x.thumbnail)));
            mExampleList.push_back(x);
        }
        if (mExamples->topLevelItemCount() == 0)
            (new QTreeWidgetItem(mExamples, {"No examples installed"}))->setFlags(Qt::NoItemFlags);
        mExamples->resizeColumnToContents(0);
        layout->addWidget(mExamples, 1);
        layout->addSpacing(6);
        heading("Recent Scenes", "startupHeading");
    }
    else
        heading("Recent Scenes", "startupTitle");

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
    if (mList->topLevelItemCount() == 0)
    {
        auto* none = new QTreeWidgetItem(mList, {"No recent scenes"});
        none->setFlags(Qt::NoItemFlags);
    }
    else
        mList->setCurrentItem(mList->topLevelItem(0));
    mList->resizeColumnToContents(0);
    mList->setColumnWidth(1, 300);
    layout->addWidget(mList, 1);

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
        connect(mExamples, &QTreeWidget::itemActivated, this, [this] { openSelected(mExamples); }, Qt::QueuedConnection);
        // One selection across the two lists, so Enter opens what is highlighted.
        connect(mExamples, &QTreeWidget::itemPressed, mList, [this] { mList->setCurrentItem(nullptr); });
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
    resize(welcome ? 820 : 680, welcome ? 620 : 380);
    qApp->installEventFilter(this);
}

void StartupDialog::present()
{
    QWidget* anchor = parentWidget();
    QScreen* screen = anchor ? anchor->screen() : QGuiApplication::primaryScreen();
    QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1600, 900);
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
    if (e->type() == QEvent::MouseButtonPress && isVisible())
    {
        auto* me = static_cast<QMouseEvent*>(e);
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w->window() != this && !geometry().contains(me->globalPosition().toPoint()))
            reject();
    }
    return QDialog::eventFilter(obj, e);
}

void StartupDialog::openSelected(QTreeWidget* list)
{
    QTreeWidgetItem* item = list->currentItem();
    if (!item || item->data(0, kPathRole).toString().isEmpty())
        return;
    if (list == mExamples)
    {
        io::examples::Example x = mExampleList[size_t(item->data(0, kKindRole).toInt())];
        auto open = mHooks.openExample;
        accept();
        if (open)
            open(x);
        return;
    }
    fs::path p = item->data(0, kPathRole).toString().toStdWString();
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
