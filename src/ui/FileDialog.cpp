#include "ui/FileDialog.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "vcs/SourceControl.h"

#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr const char* kFdPrefs = "general";
constexpr const char* kFdBookmarksPref = "/fileDialog/bookmarks";
constexpr const char* kFdRecentPref = "/fileDialog/recent";
constexpr const char* kFdLastDirPref = "/fileDialog/lastDir";
constexpr const char* kFdStatePref = "/fileDialog/state";
constexpr int kFdRecentMax = 12;

enum FdRole
{
    FdPathRole = Qt::UserRole,
    FdKindRole,
    FdTotalRole,
    FdFreeRole,
};

enum class FdKind
{
    Header,
    Project,
    Bookmark,
    Recent,
    Drive,
    Place,
};

QStringList fdPrefList(const char* key)
{
    QStringList out;
    const json v = PrefStore::get().value(kFdPrefs, key);
    if (v.is_array())
        for (const json& s : v)
            if (s.is_string())
                out << QString::fromStdString(s.get<std::string>());
    return out;
}

void fdSetPrefList(const char* key, const QStringList& list)
{
    json arr = json::array();
    for (const QString& s : list)
        arr.push_back(s.toStdString());
    PrefStore::get().set(kFdPrefs, key, arr);
}

QString fdClean(const QString& dir) { return dir.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(dir)); }

/** "Projects (X:)" for a drive root, the folder name otherwise. */
QString fdDisplayName(const QString& dir)
{
    if (dir.isEmpty())
        return "This PC";
    const QDir d(dir);
    if (d.isRoot())
    {
        QString root = QDir::toNativeSeparators(dir);
        while (root.endsWith('\\') && root.size() > 1)
            root.chop(1);
        const QString label = QStorageInfo(dir).name();
        return label.isEmpty() ? root : label + " (" + root + ")";
    }
    return d.dirName();
}

/** The patterns of one filter entry: "Images (*.png *.jpg)" gives *.png and *.jpg; no parentheses means "*". */
QStringList fdPatterns(const QString& entry)
{
    static const QRegularExpression parens(R"(\(([^)]*)\))");
    const auto match = parens.match(entry);
    const QString inner = match.hasMatch() ? match.captured(1) : entry;
    QStringList out = inner.split(QRegularExpression(R"([\s;]+)"), Qt::SkipEmptyParts);
    if (out.isEmpty())
        out << "*";
    return out;
}

/** Folders first, names in natural order ("2" before "10"), other columns by value. */
class FdProxy : public QSortFilterProxyModel
{
public:
    explicit FdProxy(QFileSystemModel* fs) : mFs(fs)
    {
        setSourceModel(fs);
        mCollator.setNumericMode(true);
        mCollator.setCaseSensitivity(Qt::CaseInsensitive);
        setDynamicSortFilter(true);
    }

    void setRoot(const QString& dir)
    {
        mRoot = dir;
        invalidateFilter();
    }

    void setText(const QString& text)
    {
        mText = text;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        // Only the listed folder's children filter; its ancestors must stay for the view's root index.
        if (mText.isEmpty() || mFs->filePath(parent) != mRoot)
            return true;
        return mFs->fileName(mFs->index(row, 0, parent)).contains(mText, Qt::CaseInsensitive);
    }

    bool lessThan(const QModelIndex& a, const QModelIndex& b) const override
    {
        const bool dirA = mFs->isDir(a), dirB = mFs->isDir(b);
        if (dirA != dirB)
            return sortOrder() == Qt::AscendingOrder ? dirA : dirB;
        switch (a.column())
        {
        case 1:
            if (mFs->size(a) != mFs->size(b))
                return mFs->size(a) < mFs->size(b);
            break;
        case 2:
            if (int c = mCollator.compare(mFs->type(a), mFs->type(b)))
                return c < 0;
            break;
        case 3:
            if (mFs->lastModified(a) != mFs->lastModified(b))
                return mFs->lastModified(a) < mFs->lastModified(b);
            break;
        default:
            break;
        }
        return mCollator.compare(mFs->fileName(a), mFs->fileName(b)) < 0;
    }

private:
    QFileSystemModel* mFs;
    QCollator mCollator;
    QString mRoot;
    QString mText;
};

/** Renames typed into the list go through fileOps, so tracked files are renamed in source control. */
class FdFsModel : public QFileSystemModel
{
public:
    using QFileSystemModel::QFileSystemModel;
    std::function<void(const QString&)> onError;

    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (role != Qt::EditRole || index.column() != 0 || isReadOnly())
            return QFileSystemModel::setData(index, value, role);
        const QString name = value.toString().trimmed();
        const QString from = filePath(index);
        if (name.isEmpty() || name == fileName(index))
            return false;
        if (name.contains('/') || name.contains('\\'))
        {
            onError("A name can't contain / or \\");
            return false;
        }
        const QString error = fileOps::move(from, QFileInfo(from).absolutePath() + '/' + name);
        if (!error.isEmpty())
            onError(error);
        return error.isEmpty();
    }
};

/** A drive's usage bar under its name. */
class FdSidebarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(std::max(s.height(), 24) + (index.data(FdTotalRole).toLongLong() > 0 ? 8 : 0));
        return s;
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const qint64 total = index.data(FdTotalRole).toLongLong();
        if (total <= 0)
        {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        // The selection and hover fill span the whole row, the bar strip included.
        QStyleOptionViewItem full = option;
        initStyleOption(&full, index);
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &full, p, widget);
        QStyleOptionViewItem top = option;
        top.rect.setHeight(option.rect.height() - 8);
        QStyledItemDelegate::paint(p, top, index);

        const int left = option.rect.left() + option.decorationSize.width() + 10;
        const QRect bar(left, option.rect.bottom() - 7, std::min(option.rect.right() - 12 - left, 90), 4);
        const double used = 1.0 - double(index.data(FdFreeRole).toLongLong()) / double(total);
        p->fillRect(bar, themeShade(80));
        p->fillRect(QRect(bar.left(), bar.top(), int(bar.width() * used), bar.height()),
                    used > 0.9 ? themeColor("statusError") : themeColor("accent"));
    }
};
} // namespace

struct FileDialog::Impl
{
    FileDialog* q = nullptr;
    Mode mode = Mode::OpenFile;
    QStringList filters;

    FdFsModel* fs = nullptr;
    FdProxy* proxy = nullptr;
    QTreeView* view = nullptr;
    QTreeWidget* sidebar = nullptr;
    QLineEdit* sidebarFilter = nullptr;
    QSplitter* splitter = nullptr;

    QToolButton* back = nullptr;
    QToolButton* forward = nullptr;
    QToolButton* up = nullptr;
    QToolButton* recentButton = nullptr;
    QToolButton* bookmarkButton = nullptr;
    QStackedWidget* pathStack = nullptr;
    QWidget* crumbs = nullptr;
    QLineEdit* pathEdit = nullptr;

    QLineEdit* filter = nullptr;
    QLabel* counts = nullptr;
    QLineEdit* name = nullptr;
    QComboBox* types = nullptr;
    QPushButton* ok = nullptr;
    QLabel* status = nullptr;

    QString dir;
    QStringList history;
    int historyPos = -1;
    QStringList result;
    int folders = 0;
    int files = 0;
    bool countsQueued = false;
    bool switchToNative = false;

    void build(const QString& caption);
    void buildSidebar();
    void rebuildCrumbs();
    void show(const QString& d, bool record);
    void applyTypeFilter();
    void queueCounts();
    void updateCounts();
    void syncNameFromSelection();
    QStringList typedNames() const;
    void editPath();
    void commitPathEdit();
    void bookmarkFolder(const QString& d, bool on);
    void pushRecent(const QString& d);
    void showListMenu(const QPoint& pos);
    void newFolder();
    void fail(const QString& message);
    QStringList selectedPaths() const;
    /** The folder a drop or paste lands in: the folder under pos, else the one shown. */
    QString targetDir(const QPoint& viewportPos) const;
    void copyToClipboard(bool cut);
    void paste(const QString& into);
    void deleteSelection();
    bool handleDrag(QEvent* event);
    void offerNative();
    void saveState();
    void restoreState();
};

void FileDialog::Impl::build(const QString& caption)
{
    q->setWindowTitle(!caption.isEmpty()                ? caption
                      : mode == Mode::SaveFile          ? "Save As"
                      : mode == Mode::Directory         ? "Select Folder"
                      : mode == Mode::OpenFiles         ? "Open Files"
                                                        : "Open");
    q->resize(1000, 620);

    fs = new FdFsModel(q);
    fs->setReadOnly(!fileOps::allowed(fileOps::kAllowEditPref));
    fs->onError = [this](const QString& message) { fail(message); };
    fs->setNameFilterDisables(false);
    fs->setFilter(mode == Mode::Directory ? QDir::AllDirs | QDir::Drives | QDir::NoDotAndDotDot
                                          : QDir::AllDirs | QDir::Drives | QDir::Files | QDir::NoDotAndDotDot);
    proxy = new FdProxy(fs);

    auto* root = new QVBoxLayout(q);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Navigation row: back, forward, up, recent, then the breadcrumb path and the bookmark toggle.
    auto* nav = new QHBoxLayout;
    nav->setSpacing(2);
    auto tool = [&](const char* iconName, const QString& tip) {
        auto* b = new QToolButton;
        b->setIcon(icon(iconName));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        nav->addWidget(b);
        return b;
    };
    back = tool("arrow-left", "Back (Alt+Left)");
    forward = tool("arrow-right", "Forward (Alt+Right)");
    up = tool("arrow-up", "Up (Alt+Up, Backspace)");
    recentButton = tool("chevron-down", "Recent folders");
    recentButton->setPopupMode(QToolButton::InstantPopup);
    auto* recentMenu = new QMenu(recentButton);
    recentButton->setMenu(recentMenu);
    QObject::connect(recentMenu, &QMenu::aboutToShow, q, [this, recentMenu] {
        recentMenu->clear();
        const QStringList recent = fdPrefList(kFdRecentPref);
        if (recent.isEmpty())
            recentMenu->addAction("No recent folders")->setEnabled(false);
        for (const QString& d : recent)
            recentMenu->addAction(icon("folder"), QDir::toNativeSeparators(d), q, [this, d] { show(d, true); });
    });
    auto* newDir = tool("folder-plus", "New folder (Ctrl+Shift+N)");
    newDir->setVisible(fileOps::allowed(fileOps::kAllowEditPref));
    QObject::connect(newDir, &QToolButton::clicked, q, [this] { newFolder(); });

    pathStack = new QStackedWidget;
    pathStack->setFixedHeight(28);
    crumbs = new QWidget;
    crumbs->setLayout(new QHBoxLayout);
    crumbs->layout()->setContentsMargins(4, 0, 4, 0);
    crumbs->layout()->setSpacing(0);
    crumbs->setCursor(Qt::IBeamCursor);
    crumbs->setToolTip("Click empty space or press Ctrl+L to type a path");
    crumbs->installEventFilter(q);
    pathEdit = new QLineEdit;
    pathEdit->installEventFilter(q);
    auto* pathCompleter = new QCompleter(pathEdit);
    auto* completerModel = new QFileSystemModel(pathCompleter);
    completerModel->setFilter(QDir::AllDirs | QDir::Drives | QDir::NoDotAndDotDot);
    completerModel->setRootPath("");
    pathCompleter->setModel(completerModel);
    pathCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    pathEdit->setCompleter(pathCompleter);
    pathStack->addWidget(crumbs);
    pathStack->addWidget(pathEdit);
    pathStack->setStyleSheet(themedStyleSheet("QStackedWidget { border: 1px solid #3a3a3a; border-radius: 3px; }"));
    nav->addSpacing(6);
    nav->addWidget(pathStack, 1);
    bookmarkButton = tool("bookmark", "Bookmark this folder");
    bookmarkButton->setCheckable(true);
    root->addLayout(nav);

    QObject::connect(back, &QToolButton::clicked, q, [this] {
        if (historyPos > 0)
            show(history[--historyPos], false);
    });
    QObject::connect(forward, &QToolButton::clicked, q, [this] {
        if (historyPos + 1 < history.size())
            show(history[++historyPos], false);
    });
    QObject::connect(up, &QToolButton::clicked, q, [this] {
        if (dir.isEmpty())
            return;
        QDir d(dir);
        show(d.isRoot() ? QString() : QFileInfo(dir).absolutePath(), true);
    });
    QObject::connect(pathEdit, &QLineEdit::returnPressed, q, [this] { commitPathEdit(); });
    QObject::connect(bookmarkButton, &QToolButton::toggled, q, [this](bool on) { bookmarkFolder(dir, on); });

    // Sidebar | file list.
    splitter = new QSplitter;
    splitter->setChildrenCollapsible(false);
    auto* side = new QWidget;
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(4);
    sidebarFilter = new QLineEdit;
    sidebarFilter->setPlaceholderText("Filter bookmarks...");
    sidebarFilter->addAction(icon("search"), QLineEdit::LeadingPosition);
    sidebarFilter->setClearButtonEnabled(true);
    sidebar = new QTreeWidget;
    sidebar->setHeaderHidden(true);
    sidebar->setRootIsDecorated(false);
    sidebar->setIndentation(10);
    sidebar->setIconSize(QSize(16, 16));
    sidebar->setItemDelegate(new FdSidebarDelegate(sidebar));
    sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
    sidebar->setFrameShape(QFrame::NoFrame);
    sideLayout->addWidget(sidebarFilter);
    sideLayout->addWidget(sidebar, 1);
    splitter->addWidget(side);

    view = new QTreeView;
    view->setModel(proxy);
    view->setRootIsDecorated(false);
    view->setItemsExpandable(false);
    view->setUniformRowHeights(true);
    view->setAlternatingRowColors(true);
    view->setSortingEnabled(true);
    view->sortByColumn(0, Qt::AscendingOrder);
    view->setEditTriggers(QAbstractItemView::EditKeyPressed);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (fileOps::allowed(fileOps::kAllowMovePref))
    {
        view->setDragDropMode(QAbstractItemView::DragDrop);
        view->viewport()->installEventFilter(q);
    }
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setFrameShape(QFrame::NoFrame);
    view->installEventFilter(q);
    QHeaderView* header = view->header();
    header->setStretchLastSection(false);
    header->setSectionsMovable(true);
    header->moveSection(header->visualIndex(1), 3); // Name, Type, Date Modified, Size
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->resizeSection(1, 90);
    header->resizeSection(2, 140);
    header->resizeSection(3, 140);
    splitter->addWidget(view);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 800});
    root->addWidget(splitter, 1);

    // Filter and counts.
    auto* filterRow = new QHBoxLayout;
    filter = new QLineEdit;
    filter->addAction(icon("search"), QLineEdit::LeadingPosition);
    filter->setClearButtonEnabled(true);
    filter->setMaximumWidth(260);
    filter->installEventFilter(q);
    counts = new QLabel;
    counts->setStyleSheet(QString("color: %1;").arg(themeColor("dimText").name()));
    filterRow->addWidget(filter);
    filterRow->addStretch(1);
    filterRow->addWidget(counts);
    root->addLayout(filterRow);

    // Name, type and the buttons.
    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(mode == Mode::Directory ? "Folder" : "File name"));
    name = new QLineEdit;
    nameRow->addWidget(name, 1);
    types = new QComboBox;
    types->addItems(filters);
    types->setVisible(mode != Mode::Directory);
    types->setMinimumWidth(180);
    nameRow->addWidget(types);
    root->addLayout(nameRow);

    status = new QLabel;
    status->setStyleSheet(QString("color: %1;").arg(themeColor("statusError").name()));
    status->hide();
    root->addWidget(status);

    auto* bottom = new QHBoxLayout;
    auto* brand = new QLabel("Lila File Dialog");
    brand->setToolTip("Lila's own file dialog, the same on Windows, macOS and Linux");
    brand->setStyleSheet(QString("color: %1;").arg(themeColor("dimText").name()));
    auto* useSystem = new QPushButton("Use System Dialog...");
    useSystem->setFlat(true);
    useSystem->setAutoDefault(false);
    useSystem->setToolTip("Switch to your platform's file dialog (Preferences > Interface > File Dialog)");
    QObject::connect(useSystem, &QPushButton::clicked, q, [this] { offerNative(); });
    bottom->addWidget(brand);
    bottom->addWidget(useSystem);
    bottom->addStretch(1);
    auto* buttons = new QDialogButtonBox;
    ok = buttons->addButton(mode == Mode::SaveFile    ? "Save"
                            : mode == Mode::Directory ? "Select Folder"
                                                      : "Open",
                            QDialogButtonBox::AcceptRole);
    ok->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, q, [this] { q->tryAccept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, q, &QDialog::reject);
    bottom->addWidget(buttons);
    root->addLayout(bottom);

    // Behaviour.
    QObject::connect(view, &QTreeView::activated, q, [this](const QModelIndex& index) {
        const QModelIndex src = proxy->mapToSource(index);
        if (fs->isDir(src))
            show(fs->filePath(src), true);
        else
            q->tryAccept();
    });
    QObject::connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, q,
                     [this] { syncNameFromSelection(); });
    QObject::connect(view, &QTreeView::customContextMenuRequested, q, [this](const QPoint& pos) { showListMenu(pos); });
    QObject::connect(filter, &QLineEdit::textChanged, q, [this](const QString& text) {
        proxy->setText(text);
        queueCounts();
    });
    QObject::connect(types, &QComboBox::currentIndexChanged, q, [this] { applyTypeFilter(); });
    QObject::connect(name, &QLineEdit::textEdited, q, [this] { status->hide(); });
    QObject::connect(proxy, &QAbstractItemModel::rowsInserted, q, [this] { queueCounts(); });
    QObject::connect(proxy, &QAbstractItemModel::rowsRemoved, q, [this] { queueCounts(); });
    QObject::connect(proxy, &QAbstractItemModel::layoutChanged, q, [this] { queueCounts(); });
    QObject::connect(proxy, &QAbstractItemModel::modelReset, q, [this] { queueCounts(); });
    QObject::connect(fs, &QFileSystemModel::directoryLoaded, q, [this] { queueCounts(); });
    QObject::connect(fs, &QFileSystemModel::fileRenamed, q, [this] { queueCounts(); });

    auto shortcut = [&](const QKeySequence& keys, auto fn) {
        QObject::connect(new QShortcut(keys, q), &QShortcut::activated, q, fn);
    };
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Left), [this] { back->click(); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Right), [this] { forward->click(); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Up), [this] { up->click(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_L), [this] { editPath(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_F), [this] {
        filter->setFocus();
        filter->selectAll();
    });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] { newFolder(); });

    buildSidebar();
    QObject::connect(sidebarFilter, &QLineEdit::textChanged, q, [this](const QString& text) {
        for (int s = 0; s < sidebar->topLevelItemCount(); ++s)
        {
            QTreeWidgetItem* section = sidebar->topLevelItem(s);
            for (int i = 0; i < section->childCount(); ++i)
                section->child(i)->setHidden(!section->child(i)->text(0).contains(text, Qt::CaseInsensitive));
        }
    });
    QObject::connect(sidebar, &QTreeWidget::itemClicked, q, [this](QTreeWidgetItem* item) {
        if (FdKind(item->data(0, FdKindRole).toInt()) == FdKind::Header)
            item->setExpanded(!item->isExpanded());
        else
            show(item->data(0, FdPathRole).toString(), true);
    });
    QObject::connect(sidebar, &QTreeWidget::customContextMenuRequested, q, [this](const QPoint& pos) {
        QTreeWidgetItem* item = sidebar->itemAt(pos);
        if (!item)
            return;
        const auto kind = FdKind(item->data(0, FdKindRole).toInt());
        const QString path = item->data(0, FdPathRole).toString();
        if (kind == FdKind::Header)
            return;
        QMenu menu;
        QStringList bookmarks = fdPrefList(kFdBookmarksPref);
        const int at = bookmarks.indexOf(path);
        if (kind == FdKind::Bookmark)
        {
            auto move = [this, bookmarks, at](int to) mutable {
                bookmarks.swapItemsAt(at, to);
                fdSetPrefList(kFdBookmarksPref, bookmarks);
                buildSidebar();
            };
            menu.addAction("Move Up", [move, at]() mutable { move(at - 1); })->setEnabled(at > 0);
            menu.addAction("Move Down", [move, at]() mutable { move(at + 1); })->setEnabled(at + 1 < bookmarks.size());
            menu.addAction(icon("bookmark-minus"), "Remove Bookmark", [this, path] { bookmarkFolder(path, false); });
        }
        else if (at < 0)
            menu.addAction(icon("bookmark-plus"), "Bookmark", [this, path] { bookmarkFolder(path, true); });
        if (kind == FdKind::Recent)
            menu.addAction("Remove from Recent", [this, path] {
                QStringList recent = fdPrefList(kFdRecentPref);
                recent.removeAll(path);
                fdSetPrefList(kFdRecentPref, recent);
                buildSidebar();
            });
        menu.addSeparator();
        menu.addAction("Copy Path", [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
        menu.exec(sidebar->viewport()->mapToGlobal(pos));
    });
}

void FileDialog::Impl::buildSidebar()
{
    QSet<QString> collapsed;
    for (int s = 0; s < sidebar->topLevelItemCount(); ++s)
        if (!sidebar->topLevelItem(s)->isExpanded())
            collapsed << sidebar->topLevelItem(s)->text(0);
    sidebar->clear();

    QFont headerFont = sidebar->font();
    headerFont.setBold(true);
    auto section = [&](const QString& title) {
        auto* item = new QTreeWidgetItem(sidebar, {title});
        item->setData(0, FdKindRole, int(FdKind::Header));
        item->setFont(0, headerFont);
        item->setForeground(0, themeColor("dimText"));
        item->setFlags(Qt::ItemIsEnabled);
        return item;
    };
    auto entry = [&](QTreeWidgetItem* parent, FdKind kind, const QString& path, const QString& label, const char* iconName) {
        auto* item = new QTreeWidgetItem(parent, {label});
        item->setData(0, FdPathRole, path);
        item->setData(0, FdKindRole, int(kind));
        item->setIcon(0, icon(iconName));
        item->setToolTip(0, path.isEmpty() ? label : QDir::toNativeSeparators(path));
        return item;
    };

    if (Workspace::get().isOpen())
    {
        const QString ws = fdClean(QString::fromStdWString(Workspace::get().root().wstring()));
        entry(section("Project"), FdKind::Project, ws, QDir(ws).dirName(), "folder");
    }

    QTreeWidgetItem* quick = section("Bookmarks");
    const QStringList bookmarks = fdPrefList(kFdBookmarksPref);
    for (const QString& d : bookmarks)
        entry(quick, FdKind::Bookmark, d, fdDisplayName(d), "folder");
    if (bookmarks.isEmpty())
    {
        auto* hint = new QTreeWidgetItem(quick, {"Bookmark folders with the bookmark button"});
        hint->setFlags(Qt::NoItemFlags);
        hint->setData(0, FdKindRole, int(FdKind::Header));
    }

    QTreeWidgetItem* recent = section("Recent");
    for (const QString& d : fdPrefList(kFdRecentPref))
        if (!bookmarks.contains(d))
            entry(recent, FdKind::Recent, d, fdDisplayName(d), "history");

    QTreeWidgetItem* storage = section("Storage");
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes())
    {
        if (!v.isValid() || !v.isReady())
            continue;
#ifndef Q_OS_WIN
        // Skip the pseudo filesystems Linux and macOS mount everywhere.
        if (v.bytesTotal() <= 0 || v.rootPath().startsWith("/sys") || v.rootPath().startsWith("/proc") ||
            v.rootPath().startsWith("/dev") || v.rootPath().startsWith("/run") || v.rootPath().startsWith("/System/Volumes"))
            continue;
#endif
        const QString path = fdClean(v.rootPath());
        QTreeWidgetItem* item = entry(storage, FdKind::Drive, path, fdDisplayName(path), "server");
        item->setData(0, FdTotalRole, v.bytesTotal());
        item->setData(0, FdFreeRole, v.bytesAvailable());
        item->setToolTip(0, QString("%1\n%2 free of %3")
                                .arg(QDir::toNativeSeparators(path), QLocale().formattedDataSize(v.bytesAvailable()),
                                     QLocale().formattedDataSize(v.bytesTotal())));
    }

    QTreeWidgetItem* places = section("Places");
#ifdef Q_OS_WIN
    entry(places, FdKind::Place, QString(), "This PC", "device-desktop");
#endif
    const struct
    {
        QStandardPaths::StandardLocation loc;
        const char* label;
        const char* iconName;
    } standard[] = {
        {QStandardPaths::HomeLocation, nullptr, "home"},
        {QStandardPaths::DesktopLocation, "Desktop", "device-desktop"},
        {QStandardPaths::DownloadLocation, "Downloads", "download"},
        {QStandardPaths::DocumentsLocation, "Documents", "file-text"},
        {QStandardPaths::MusicLocation, "Music", "music"},
        {QStandardPaths::PicturesLocation, "Pictures", "photo"},
        {QStandardPaths::MoviesLocation, "Videos", "movie"},
    };
    for (const auto& s : standard)
    {
        const QString path = fdClean(QStandardPaths::writableLocation(s.loc));
        if (!path.isEmpty() && QFileInfo(path).isDir())
            entry(places, FdKind::Place, path, s.label ? QString(s.label) : QDir(path).dirName(), s.iconName);
    }

    for (int s = 0; s < sidebar->topLevelItemCount(); ++s)
        sidebar->topLevelItem(s)->setExpanded(!collapsed.contains(sidebar->topLevelItem(s)->text(0)));
    if (!sidebarFilter->text().isEmpty())
        Q_EMIT sidebarFilter->textChanged(sidebarFilter->text());
}

void FileDialog::Impl::rebuildCrumbs()
{
    QLayout* layout = crumbs->layout();
    while (QLayoutItem* item = layout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    QStringList chain;
    for (QString d = dir; !d.isEmpty();)
    {
        chain.prepend(d);
        if (QDir(d).isRoot())
            break;
        d = QFileInfo(d).absolutePath();
    }
#ifdef Q_OS_WIN
    chain.prepend(QString());
#endif
    for (int i = 0; i < chain.size(); ++i)
    {
        if (i > 0)
        {
            auto* sep = new QLabel;
            sep->setPixmap(icon("chevron-right").pixmap(12, 12));
            layout->addWidget(sep);
        }
        auto* b = new QToolButton;
        b->setText(fdDisplayName(chain[i]));
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        const QString target = chain[i];
        QObject::connect(b, &QToolButton::clicked, q, [this, target] { show(target, true); });
        layout->addWidget(b);
    }
    static_cast<QHBoxLayout*>(layout)->addStretch(1);
}

void FileDialog::Impl::show(const QString& raw, bool record)
{
    const QString d = fdClean(raw);
    status->hide();
    dir = d;
    if (record)
    {
        history = history.mid(0, historyPos + 1);
        if (history.isEmpty() || history.last() != d)
            history << d;
        historyPos = int(history.size()) - 1;
    }
    filter->clear();
    fs->setRootPath(d);
    proxy->setRoot(d);
    view->setRootIndex(proxy->mapFromSource(fs->index(d)));
    view->clearSelection();
    view->scrollToTop();
    back->setEnabled(historyPos > 0);
    forward->setEnabled(historyPos + 1 < history.size());
    up->setEnabled(!d.isEmpty());
    {
        const QSignalBlocker block(bookmarkButton);
        bookmarkButton->setChecked(fdPrefList(kFdBookmarksPref).contains(d));
        bookmarkButton->setEnabled(!d.isEmpty());
        bookmarkButton->setIcon(icon(bookmarkButton->isChecked() ? "bookmark-minus" : "bookmark"));
    }
    rebuildCrumbs();
    pathStack->setCurrentWidget(crumbs);
    for (QTreeWidgetItemIterator it(sidebar); *it; ++it)
        (*it)->setSelected(FdKind((*it)->data(0, FdKindRole).toInt()) != FdKind::Header &&
                           (*it)->data(0, FdPathRole).toString() == d);
    queueCounts();
}

void FileDialog::Impl::applyTypeFilter()
{
    if (mode == Mode::Directory || types->currentIndex() < 0)
        return;
    QStringList patterns = fdPatterns(types->currentText());
    if (patterns.contains("*") || patterns.contains("*.*"))
        patterns.clear();
    fs->setNameFilters(patterns);
    queueCounts();
}

void FileDialog::Impl::queueCounts()
{
    if (countsQueued)
        return;
    countsQueued = true;
    QTimer::singleShot(0, q, [this] {
        countsQueued = false;
        updateCounts();
    });
}

void FileDialog::Impl::updateCounts()
{
    folders = files = 0;
    const QModelIndex rootIndex = view->rootIndex();
    for (int r = 0, n = proxy->rowCount(rootIndex); r < n; ++r)
        (fs->isDir(proxy->mapToSource(proxy->index(r, 0, rootIndex))) ? folders : files)++;
    counts->setText(mode == Mode::Directory ? QString("%1 folders").arg(folders)
                                            : QString("%1 folders  %2 files").arg(folders).arg(files));
    filter->setPlaceholderText(QString("Filter %1 %2...").arg(folders + files).arg(folders + files == 1 ? "item" : "items"));
}

void FileDialog::Impl::syncNameFromSelection()
{
    QStringList names;
    for (const QModelIndex& index : view->selectionModel()->selectedRows())
    {
        const QModelIndex src = proxy->mapToSource(index);
        if (fs->isDir(src) == (mode == Mode::Directory))
            names << fs->fileName(src);
    }
    if (names.isEmpty())
        return;
    if (names.size() > 1 && mode != Mode::OpenFiles)
    {
        const QModelIndex current = proxy->mapToSource(view->currentIndex());
        if (!current.isValid() || !names.contains(fs->fileName(current)))
            return;
        names = {fs->fileName(current)};
    }
    if (names.size() == 1)
        name->setText(names[0]);
    else
    {
        for (QString& n : names)
            n = '"' + n + '"';
        name->setText(names.join(' '));
    }
    status->hide();
}

QStringList FileDialog::Impl::typedNames() const
{
    const QString text = name->text().trimmed();
    QStringList out;
    if (text.startsWith('"'))
    {
        static const QRegularExpression quoted(R"re("([^"]+)")re");
        for (auto it = quoted.globalMatch(text); it.hasNext();)
            out << it.next().captured(1);
    }
    else if (!text.isEmpty())
        out << text;
    return out;
}

void FileDialog::Impl::editPath()
{
    pathEdit->setText(QDir::toNativeSeparators(dir));
    pathStack->setCurrentWidget(pathEdit);
    pathEdit->setFocus();
    pathEdit->selectAll();
}

void FileDialog::Impl::commitPathEdit()
{
    QString text = pathEdit->text().trimmed();
    if (text.startsWith('~'))
        text = QDir::homePath() + text.mid(1);
    const QFileInfo fi(text);
    if (fi.isDir())
        show(fi.absoluteFilePath(), true);
    else if (fi.exists() && mode != Mode::Directory)
    {
        show(fi.absolutePath(), true);
        name->setText(fi.fileName());
    }
    else
    {
        status->setText("No folder at " + text);
        status->show();
        return;
    }
    view->setFocus();
}

void FileDialog::Impl::bookmarkFolder(const QString& d, bool on)
{
    if (d.isEmpty())
        return;
    QStringList bookmarks = fdPrefList(kFdBookmarksPref);
    bookmarks.removeAll(d);
    if (on)
        bookmarks << d;
    fdSetPrefList(kFdBookmarksPref, bookmarks);
    buildSidebar();
    if (d == dir)
    {
        const QSignalBlocker block(bookmarkButton);
        bookmarkButton->setChecked(on);
        bookmarkButton->setIcon(icon(on ? "bookmark-minus" : "bookmark"));
    }
}

void FileDialog::Impl::pushRecent(const QString& d)
{
    if (d.isEmpty())
        return;
    QStringList recent = fdPrefList(kFdRecentPref);
    recent.removeAll(d);
    recent.prepend(d);
    fdSetPrefList(kFdRecentPref, recent.mid(0, kFdRecentMax));
    PrefStore::get().set(kFdPrefs, kFdLastDirPref, d.toStdString());
}

void FileDialog::Impl::showListMenu(const QPoint& pos)
{
    const QModelIndex index = view->indexAt(pos);
    const QModelIndex src = proxy->mapToSource(index);
    const QString path = index.isValid() ? fs->filePath(src) : dir;
    QMenu menu;
    if (index.isValid() && fs->isDir(src))
    {
        menu.addAction("Open", [this, path] { show(path, true); });
        const bool bookmarked = fdPrefList(kFdBookmarksPref).contains(path);
        menu.addAction(icon(bookmarked ? "bookmark-minus" : "bookmark-plus"), bookmarked ? "Remove Bookmark" : "Bookmark",
                       [this, path, bookmarked] { bookmarkFolder(path, !bookmarked); });
    }
    const bool edit = fileOps::allowed(fileOps::kAllowEditPref), moves = fileOps::allowed(fileOps::kAllowMovePref);
    menu.addSeparator();
    if (index.isValid() && moves)
    {
        menu.addAction(icon("cut"), "Cut\tCtrl+X", [this] { copyToClipboard(true); });
        menu.addAction(icon("copy"), "Copy\tCtrl+C", [this] { copyToClipboard(false); });
    }
    const QString into = index.isValid() && fs->isDir(src) ? path : dir;
    if (moves && !into.isEmpty() && QApplication::clipboard()->mimeData()->hasUrls())
        menu.addAction(icon("clipboard"), "Paste\tCtrl+V", [this, into] { paste(into); });
    if (index.isValid() && edit)
        menu.addAction(icon("pencil"), "Rename\tF2", [this, index] { view->edit(index.siblingAtColumn(0)); });
    if (index.isValid() && fileOps::allowed(fileOps::kAllowDeletePref))
        menu.addAction(icon("trash"), "Delete\tDel", [this] { deleteSelection(); });
    menu.addSeparator();
    menu.addAction("Copy Path", [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    if (!dir.isEmpty())
    {
        if (edit)
            menu.addAction(icon("folder-plus"), "New Folder", [this] { newFolder(); });
        menu.addAction("Open in File Manager", [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    }
    menu.exec(view->viewport()->mapToGlobal(pos));
}

void FileDialog::Impl::fail(const QString& message)
{
    status->setText(message);
    status->setVisible(!message.isEmpty());
}

QStringList FileDialog::Impl::selectedPaths() const
{
    QStringList out;
    for (const QModelIndex& index : view->selectionModel()->selectedRows())
        out << fs->filePath(proxy->mapToSource(index));
    return out;
}

QString FileDialog::Impl::targetDir(const QPoint& viewportPos) const
{
    const QModelIndex src = proxy->mapToSource(view->indexAt(viewportPos));
    return src.isValid() && fs->isDir(src) ? fs->filePath(src) : dir;
}

// The Windows shell's flag for a cut, so a cut here pastes as a move in Explorer and other file managers too.
constexpr const char* kFdDropEffectMime = "application/x-qt-windows-mime;value=\"Preferred DropEffect\"";
constexpr const char* kFdCutMime = "application/x-lila-cut";

void FileDialog::Impl::copyToClipboard(bool cut)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty() || !fileOps::allowed(fileOps::kAllowMovePref))
        return;
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString& p : paths)
        urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls);
    const char effect[4] = {char(cut ? 2 : 5), 0, 0, 0};
    mime->setData(kFdDropEffectMime, QByteArray(effect, 4));
    if (cut)
        mime->setData(kFdCutMime, "1");
    QApplication::clipboard()->setMimeData(mime);
    fail({});
}

void FileDialog::Impl::paste(const QString& into)
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (into.isEmpty() || !mime || !mime->hasUrls() || !fileOps::allowed(fileOps::kAllowMovePref))
        return;
    QStringList paths;
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile())
            paths << fdClean(u.toLocalFile());
    const bool cut = mime->hasFormat(kFdCutMime) || (mime->hasFormat(kFdDropEffectMime) && !mime->data(kFdDropEffectMime).isEmpty() &&
                                                      (mime->data(kFdDropEffectMime)[0] & 2));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    if (cut)
    {
        fileOps::moveInto(paths, into, &error);
        QApplication::clipboard()->clear();
    }
    else
        fileOps::copyInto(paths, into, &error);
    QApplication::restoreOverrideCursor();
    fail(error);
}

void FileDialog::Impl::deleteSelection()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty() || !fileOps::allowed(fileOps::kAllowDeletePref))
        return;
#ifdef Q_OS_WIN
    const QString bin = "Recycle Bin";
#else
    const QString bin = "Trash";
#endif
    QStringList names;
    for (const QString& p : paths.mid(0, 8))
        names << "  " + QFileInfo(p).fileName() + (QFileInfo(p).isDir() ? "/" : "");
    if (paths.size() > 8)
        names << QString("  and %1 more").arg(paths.size() - 8);
    QString text = (paths.size() == 1 ? QString("Move this to the %1?") : QString("Move these %2 items to the %1?")).arg(bin).arg(paths.size()) +
                   "\n\n" + names.join('\n');
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int tracked = fileOps::trackedCount(paths);
    QApplication::restoreOverrideCursor();
    if (tracked > 0)
        text += QString("\n\n%1 tracked by source control will be marked for delete instead; revert to get them back.")
                    .arg(tracked == 1 ? QString("1 file") : QString("%1 files").arg(tracked));
    const QString scene = QString::fromStdWString(io::currentPath().wstring());
    for (const QString& p : paths)
        if (!scene.isEmpty() && (fdClean(scene) == p || fdClean(scene).startsWith(p + '/')))
            text += "\n\nThis includes the scene you have open.";
    QMessageBox box(QMessageBox::Warning, "Delete", text, QMessageBox::Cancel, q);
    QPushButton* yes = box.addButton("Delete", QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString error = fileOps::remove(paths);
    QApplication::restoreOverrideCursor();
    fail(error);
}

bool FileDialog::Impl::handleDrag(QEvent* event)
{
    auto* drop = static_cast<QDropEvent*>(event);
    const QMimeData* mime = drop->mimeData();
    if (!mime || !mime->hasUrls() || !fileOps::allowed(fileOps::kAllowMovePref))
    {
        drop->ignore();
        return true;
    }
    QStringList paths;
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile())
            paths << fdClean(u.toLocalFile());
    const QString into = targetDir(drop->position().toPoint());
    // Nothing to do dropping a file on its own folder, or a folder into itself.
    const bool pointless = into.isEmpty() || std::all_of(paths.begin(), paths.end(), [&](const QString& p) {
        return QFileInfo(p).absolutePath() == into || into == p || into.startsWith(p + '/');
    });
    if (paths.isEmpty() || pointless)
    {
        drop->ignore();
        return true;
    }
    // Explorer's rule: a move on the same drive, a copy across drives; Ctrl copies, Shift moves.
    const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
    const bool sameDrive = QStorageInfo(paths[0]).rootPath() == QStorageInfo(into).rootPath();
    const bool move = (mods & Qt::ShiftModifier) || (!(mods & Qt::ControlModifier) && sameDrive);
    drop->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
    drop->accept();
    if (event->type() != QEvent::Drop)
        return true;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    if (move)
        fileOps::moveInto(paths, into, &error);
    else
        fileOps::copyInto(paths, into, &error);
    QApplication::restoreOverrideCursor();
    fail(error);
    return true;
}

void FileDialog::Impl::newFolder()
{
    if (dir.isEmpty() || !fileOps::allowed(fileOps::kAllowEditPref))
        return;
    QString base = "New Folder", folderName = base;
    for (int i = 2; QFileInfo::exists(dir + '/' + folderName); ++i)
        folderName = QString("%1 (%2)").arg(base).arg(i);
    const QModelIndex created = fs->mkdir(fs->index(dir), folderName);
    if (!created.isValid())
    {
        status->setText("Couldn't create a folder in " + QDir::toNativeSeparators(dir));
        status->show();
        return;
    }
    filter->clear();
    const QModelIndex shown = proxy->mapFromSource(created);
    view->setCurrentIndex(shown);
    view->scrollTo(shown);
    view->edit(shown);
}

void FileDialog::Impl::offerNative()
{
    QMessageBox box(QMessageBox::Question, "Use System Dialog",
                    "Lila uses its own file dialog so it works the same on Windows, macOS and Linux, with bookmarks, "
                    "recent folders and type-to-filter. It takes the place of your platform's dialog, and of any "
                    "file manager that hooks it.\n\nUse the system dialog from now on? Preferences > Interface > File "
                    "Dialog switches back.",
                    QMessageBox::Cancel, q);
    QPushButton* yes = box.addButton("Use System Dialog", QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() != yes)
        return;
    PrefStore::get().set(kFdPrefs, fileDialog::kNativePref, true);
    switchToNative = true;
    q->reject();
}

void FileDialog::Impl::saveState()
{
    json state;
    state["geometry"] = q->saveGeometry().toBase64().toStdString();
    state["splitter"] = splitter->saveState().toBase64().toStdString();
    state["header"] = view->header()->saveState().toBase64().toStdString();
    PrefStore::get().set(kFdPrefs, kFdStatePref, state);
}

void FileDialog::Impl::restoreState()
{
    const json state = PrefStore::get().value(kFdPrefs, kFdStatePref);
    if (!state.is_object())
        return;
    auto bytes = [&](const char* key) {
        return QByteArray::fromBase64(QByteArray::fromStdString(state.value(key, std::string())));
    };
    q->restoreGeometry(bytes("geometry"));
    splitter->restoreState(bytes("splitter"));
    view->header()->restoreState(bytes("header"));
}

FileDialog::FileDialog(QWidget* parent, Mode mode, const QString& caption, const QString& start, const QString& filter)
    : QDialog(parent), m(std::make_unique<Impl>())
{
    m->q = this;
    m->mode = mode;
    m->filters = filter.split(";;", Qt::SkipEmptyParts);
    if (m->filters.isEmpty())
        m->filters << "All files (*)";
    m->build(caption);
    m->restoreState();
    m->applyTypeFilter();

    // `start` may be a folder, a file in one, or a bare file name for the last folder used.
    QString lastDir = QString::fromStdString(PrefStore::get().value<std::string>(kFdPrefs, kFdLastDirPref, ""));
    if (lastDir.isEmpty() || !QFileInfo(lastDir).isDir())
        lastDir = QDir::homePath();
    QString startDir = lastDir, startName;
    if (!start.isEmpty())
    {
        const QFileInfo fi(start);
        if (fi.isDir())
            startDir = fi.absoluteFilePath();
        else if (fi.isAbsolute())
        {
            if (QFileInfo(fi.absolutePath()).isDir())
                startDir = fi.absolutePath();
            startName = fi.fileName();
        }
        else
        {
            const QFileInfo inLast(lastDir + '/' + start);
            startDir = QFileInfo(inLast.absolutePath()).isDir() ? inLast.absolutePath() : lastDir;
            startName = inLast.fileName();
        }
    }
    m->show(startDir, true);
    if (!startName.isEmpty() && mode != Mode::Directory)
    {
        m->name->setText(startName);
        if (mode == Mode::SaveFile)
        {
            const int dot = startName.lastIndexOf('.');
            m->name->setSelection(0, dot > 0 ? dot : int(startName.size()));
        }
    }
    m->name->setFocus();
}

FileDialog::~FileDialog() = default;

bool FileDialog::navigate(const QString& dir)
{
    if (!dir.isEmpty() && !QFileInfo(dir).isDir())
        return false;
    m->show(dir.isEmpty() ? QString() : QFileInfo(dir).absoluteFilePath(), true);
    return true;
}

QString FileDialog::currentDir() const { return m->dir; }
void FileDialog::setFileName(const QString& name) { m->name->setText(name); }
QString FileDialog::fileName() const { return m->name->text(); }
void FileDialog::setNameFilter(const QString& text) { m->filter->setText(text); }
QString FileDialog::selectedFilter() const { return m->types->currentText(); }

void FileDialog::selectFilter(const QString& filter)
{
    const int i = m->types->findText(filter);
    if (i >= 0)
        m->types->setCurrentIndex(i);
}

int FileDialog::folderCount() const
{
    m->updateCounts();
    return m->folders;
}

int FileDialog::fileCount() const
{
    m->updateCounts();
    return m->files;
}

QStringList FileDialog::selectedFiles() const { return m->result; }

bool FileDialog::tryAccept()
{
    auto fail = [this](const QString& message) {
        m->status->setText(message);
        m->status->show();
        return false;
    };
    auto absolute = [this](QString n) {
        if (n.startsWith('~'))
            n = QDir::homePath() + n.mid(1);
        return fdClean(QFileInfo(n).isAbsolute() ? n : m->dir + '/' + n);
    };
    auto finish = [this](QStringList paths) {
        m->result = std::move(paths);
        m->pushRecent(m->mode == Mode::Directory ? m->result.value(0) : QFileInfo(m->result.value(0)).absolutePath());
        m->saveState();
        accept();
        return true;
    };

    const QStringList names = m->typedNames();
    if (m->mode == Mode::Directory)
    {
        if (names.isEmpty())
            return m->dir.isEmpty() ? fail("Choose a folder") : finish({m->dir});
        const QString path = absolute(names[0]);
        return QFileInfo(path).isDir() ? finish({path}) : fail("No folder named " + names[0]);
    }

    if (names.isEmpty())
    {
        // Enter on a selected folder opens it.
        const QModelIndex current = m->proxy->mapToSource(m->view->currentIndex());
        if (current.isValid() && m->fs->isDir(current) && m->view->hasFocus())
        {
            m->show(m->fs->filePath(current), true);
            return false;
        }
        return fail(m->mode == Mode::SaveFile ? "Type a file name" : "Choose a file");
    }

    if (names.size() == 1)
    {
        if (names[0].contains('*') || names[0].contains('?'))
        {
            m->fs->setNameFilters(names[0].split(' ', Qt::SkipEmptyParts));
            m->name->clear();
            return false;
        }
        const QString path = absolute(names[0]);
        if (QFileInfo(path).isDir())
        {
            m->show(path, true);
            m->name->clear();
            return false;
        }
    }

    if (m->mode == Mode::SaveFile)
    {
        QString path = absolute(names[0]);
        const QStringList patterns = fdPatterns(m->types->currentText());
        const bool matches = std::any_of(patterns.begin(), patterns.end(), [&](const QString& p) {
            return QDir::match(p, QFileInfo(path).fileName());
        });
        if (!matches && patterns[0].startsWith("*.") && patterns[0].indexOf('*', 1) < 0)
            path += patterns[0].mid(1);
        if (!QFileInfo(QFileInfo(path).absolutePath()).isDir())
            return fail("No folder " + QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
        if (QFileInfo(path).isDir())
            return fail(QFileInfo(path).fileName() + " is a folder");
        if (QFileInfo::exists(path) &&
            QMessageBox::question(this, windowTitle(), QFileInfo(path).fileName() + " already exists. Replace it?",
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return false;
        return finish({path});
    }

    QStringList paths;
    for (const QString& n : names)
    {
        const QString path = absolute(n);
        if (!QFileInfo(path).isFile())
            return fail("No file named " + n);
        paths << path;
    }
    if (m->mode == Mode::OpenFile)
        paths = paths.mid(0, 1);
    return finish(paths);
}

bool FileDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m->crumbs && event->type() == QEvent::MouseButtonPress)
    {
        m->editPath();
        return true;
    }
    if (watched == m->pathEdit && event->type() == QEvent::FocusOut && m->pathStack->currentWidget() == m->pathEdit)
    {
        QTimer::singleShot(0, this, [this] {
            if (!m->pathEdit->hasFocus())
                m->pathStack->setCurrentWidget(m->crumbs);
        });
    }
    if (m->view && watched == m->view->viewport() &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop))
        return m->handleDrag(event);
    if (event->type() != QEvent::KeyPress)
        return QDialog::eventFilter(watched, event);

    auto* key = static_cast<QKeyEvent*>(event);
    if (watched == m->pathEdit && key->key() == Qt::Key_Escape)
    {
        m->pathStack->setCurrentWidget(m->crumbs);
        m->view->setFocus();
        return true;
    }
    if (watched == m->filter)
    {
        if (key->key() == Qt::Key_Escape && !m->filter->text().isEmpty())
        {
            m->filter->clear();
            m->view->setFocus();
            return true;
        }
        if (key->key() == Qt::Key_Down)
        {
            m->view->setFocus();
            if (!m->view->currentIndex().isValid())
                m->view->setCurrentIndex(m->proxy->index(0, 0, m->view->rootIndex()));
            return true;
        }
    }
    if (watched == m->view)
    {
        if (key->key() == Qt::Key_Backspace)
        {
            m->up->click();
            return true;
        }
        if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut))
        {
            m->copyToClipboard(key->matches(QKeySequence::Cut));
            return true;
        }
        if (key->matches(QKeySequence::Paste))
        {
            m->paste(m->dir);
            return true;
        }
        if (key->matches(QKeySequence::Delete))
        {
            m->deleteSelection();
            return true;
        }
        // Typing in the list filters it.
        const QString text = key->text();
        if (!text.isEmpty() && text[0].isPrint() && !text[0].isSpace() &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        {
            m->filter->setFocus();
            m->filter->setText(m->filter->text() + text);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

namespace fileOps
{
namespace
{
/** "name (2).ext", "name (3).ext"... the first that doesn't exist in dir. */
QString freeName(const QString& dir, const QString& name)
{
    QString candidate = dir + '/' + name;
    if (!QFileInfo::exists(candidate))
        return candidate;
    const QFileInfo fi(name);
    const QString stem = fi.isDir() || fi.suffix().isEmpty() ? name : fi.completeBaseName();
    const QString ext = stem == name ? QString() : "." + fi.suffix();
    for (int i = 2;; ++i)
        if (candidate = QString("%1/%2 (%3)%4").arg(dir, stem).arg(i).arg(ext); !QFileInfo::exists(candidate))
            return candidate;
}

namespace fs = std::filesystem;

fs::path fsPath(const QString& p) { return fs::path(p.toStdWString()); }

/** The file a status record is for: providers report paths as they were given, narrow or UTF-8. */
const fs::path* statusFile(const std::vector<fs::path>& files, const std::string& reported)
{
    for (const fs::path& f : files)
    {
        if (paths::toUtf8(f) == reported || fs::path(paths::fromUtf8(reported)) == f)
            return &f;
        try
        {
            if (f.string() == reported)
                return &f;
        }
        catch (const std::exception&)
        {
        }
    }
    return nullptr;
}

/** The files under paths (folders expanded), each with the provider tracking it; untracked files are left out. */
std::vector<vcs::SourceControl::Group> trackedGroups(const QStringList& paths)
{
    std::vector<fs::path> files;
    for (const QString& p : paths)
    {
        if (QFileInfo(p).isDir())
            for (QDirIterator it(p, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();)
                files.push_back(fsPath(it.next()));
        else
            files.push_back(fsPath(p));
    }
    std::vector<vcs::SourceControl::Group> out;
    try
    {
        for (auto& [provider, group] : vcs::SourceControl::get().group(files))
        {
            if (!provider)
                continue;
            std::vector<fs::path> tracked;
            for (const vcs::FileStatus& s : provider->status(group))
                if (s.state != vcs::FileState::NotTracked && s.state != vcs::FileState::Unknown && s.state != vcs::FileState::Deleted)
                    if (const fs::path* f = statusFile(group, s.path))
                        tracked.push_back(*f);
            if (!tracked.empty())
                out.push_back({provider, std::move(tracked)});
        }
    }
    catch (const std::exception&)
    {
        // Source control unreachable: treat everything as untracked rather than refuse to work.
    }
    return out;
}

/** Moves what is left of from into to (after source control moved its tracked files), then removes empty folders. */
bool mergeInto(const QString& from, const QString& to)
{
    if (!QFileInfo(from).isDir())
        return !QFileInfo::exists(from) || QFile::rename(from, to);
    QDir().mkpath(to);
    bool ok = true;
    for (const QFileInfo& e : QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        ok &= mergeInto(e.absoluteFilePath(), to + '/' + e.fileName());
    return QDir().rmdir(from) && ok;
}

bool copyTree(const QString& from, const QString& to, QStringList& created)
{
    if (!QFileInfo(from).isDir())
    {
        if (!QFile::copy(from, to))
            return false;
        created << to;
        return true;
    }
    if (!QDir().mkpath(to))
        return false;
    bool ok = true;
    for (const QFileInfo& e : QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        ok &= copyTree(e.absoluteFilePath(), to + '/' + e.fileName(), created);
    return ok;
}

void addError(QString* error, const QString& message)
{
    if (error)
        *error += (error->isEmpty() ? "" : "\n") + message;
}
} // namespace

bool allowed(const char* pref) { return PrefStore::get().value<bool>("general", pref, true); }

QString move(const QString& rawFrom, const QString& rawTo)
{
    const QString from = fdClean(rawFrom), to = fdClean(rawTo);
    if (!QFileInfo::exists(from))
        return "No such file: " + QDir::toNativeSeparators(from);
    // Same name in another case: allowed, it's a rename on case-insensitive file systems.
    if (QFileInfo::exists(to) && from.compare(to, Qt::CaseInsensitive) != 0)
        return QFileInfo(to).fileName() + " already exists";
    if (to.startsWith(from + '/'))
        return "Can't move a folder into itself";
    auto groups = trackedGroups({from});
    if (!groups.empty())
    {
        try
        {
            groups[0].first->move(fsPath(from), fsPath(to));
            // Perforce moves only the tracked files of a folder; bring the rest along.
            if (QFileInfo::exists(from) && !mergeInto(from, to))
                return "Moved the tracked files; some others in " + QDir::toNativeSeparators(from) + " were left behind";
            return {};
        }
        catch (const std::exception& e)
        {
            return QString("Source control couldn't move %1: %2").arg(QFileInfo(from).fileName(), e.what());
        }
    }
    if (QDir().rename(from, to))
        return {};
    // A rename can't cross drives: copy, then send the original to the bin.
    QStringList created;
    if (QStorageInfo(from).rootPath() != QStorageInfo(QFileInfo(to).absolutePath()).rootPath() && copyTree(from, to, created) &&
        QFile::moveToTrash(from))
        return {};
    return "Couldn't move " + QFileInfo(from).fileName();
}

QStringList moveInto(const QStringList& paths, const QString& dir, QString* error)
{
    QStringList out;
    for (const QString& p : paths)
    {
        const QString to = fdClean(dir) + '/' + QFileInfo(p).fileName();
        if (fdClean(QFileInfo(p).absolutePath()) == fdClean(dir))
            continue;
        if (const QString e = move(p, to); !e.isEmpty())
            addError(error, e);
        else
            out << to;
    }
    return out;
}

QStringList copyInto(const QStringList& paths, const QString& dir, QString* error)
{
    QStringList out;
    for (const QString& p : paths)
    {
        if (fdClean(dir) == fdClean(p) || fdClean(dir).startsWith(fdClean(p) + '/'))
        {
            addError(error, "Can't copy a folder into itself");
            continue;
        }
        const QString to = freeName(fdClean(dir), QFileInfo(p).fileName());
        QStringList created;
        if (!copyTree(p, to, created))
            addError(error, "Couldn't copy " + QFileInfo(p).fileName());
        else
            out << to;
        for (const QString& c : created)
            vcs::markForAddIfNew(fsPath(c));
    }
    return out;
}

int trackedCount(const QStringList& paths)
{
    int n = 0;
    for (const auto& [provider, files] : trackedGroups(paths))
        n += int(files.size());
    return n;
}

QString remove(const QStringList& paths)
{
    QString error;
    for (auto& [provider, files] : trackedGroups(paths))
    {
        try
        {
            provider->remove(files);
        }
        catch (const std::exception& e)
        {
            addError(&error, QString("Source control couldn't delete: %1").arg(e.what()));
        }
    }
    for (const QString& p : paths)
        if (QFileInfo::exists(p) && !QFile::moveToTrash(p))
            addError(&error, "Couldn't move " + QFileInfo(p).fileName() + " to the bin; nothing was deleted");
    return error;
}
} // namespace fileOps

namespace fileDialog
{
namespace
{
bool useNative() { return PrefStore::get().value<bool>(kFdPrefs, kNativePref, false); }

QWidget* fdParent(QWidget* parent) { return parent ? parent : QApplication::activeWindow(); }

/** Runs Lila's dialog; false when the person switched to the system dialog from it, so the caller shows that one. */
bool runLila(FileDialog::Mode mode, QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
             QString* selectedFilter, QStringList& out)
{
    FileDialog dlg(fdParent(parent), mode, caption, dir, filter);
    if (selectedFilter && !selectedFilter->isEmpty())
        dlg.selectFilter(*selectedFilter);
    const bool accepted = dlg.exec() == QDialog::Accepted;
    if (!accepted && useNative())
        return false;
    if (accepted)
    {
        out = dlg.selectedFiles();
        if (selectedFilter)
            *selectedFilter = dlg.selectedFilter();
    }
    return true;
}
} // namespace

QString getOpenFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QString* selectedFilter)
{
    QStringList out;
    if (useNative() || !runLila(FileDialog::Mode::OpenFile, parent, caption, dir, filter, selectedFilter, out))
        return QFileDialog::getOpenFileName(parent, caption, dir, filter, selectedFilter);
    return out.value(0);
}

QStringList getOpenFileNames(QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
                             QString* selectedFilter)
{
    QStringList out;
    if (useNative() || !runLila(FileDialog::Mode::OpenFiles, parent, caption, dir, filter, selectedFilter, out))
        return QFileDialog::getOpenFileNames(parent, caption, dir, filter, selectedFilter);
    return out;
}

QString getSaveFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QString* selectedFilter)
{
    QStringList out;
    if (useNative() || !runLila(FileDialog::Mode::SaveFile, parent, caption, dir, filter, selectedFilter, out))
        return QFileDialog::getSaveFileName(parent, caption, dir, filter, selectedFilter);
    return out.value(0);
}

QString getExistingDirectory(QWidget* parent, const QString& caption, const QString& dir)
{
    QStringList out;
    if (useNative() || !runLila(FileDialog::Mode::Directory, parent, caption, dir, {}, nullptr, out))
        return QFileDialog::getExistingDirectory(parent, caption, dir);
    return out.value(0);
}
} // namespace fileDialog
} // namespace lila::ui
