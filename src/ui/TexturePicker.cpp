#include "ui/TexturePicker.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/ProjectTextures.h"
#include "core/TextMatch.h"
#include "core/Workspace.h"
#include "scene/TextureFolders.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>

#include <unordered_map>
#include <unordered_set>

namespace lila::ui::texturePicker
{
namespace
{
namespace fs = std::filesystem;
namespace pt = projectTextures;
namespace tf = scene::textureFolders;
using json = nlohmann::json;

constexpr int kIcon = 32;

enum Kind
{
    kNone,
    kBrowse,
    kFile,
};
constexpr int kKindRole = Qt::UserRole;
constexpr int kRelRole = Qt::UserRole + 1;
constexpr int kThumbRole = Qt::UserRole + 2;

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

json runCommand(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return nullptr;
}

/** Icon-sized previews read on the thread pool, keyed by path and modification time; unreadable files stay blank. */
struct Thumbs
{
    std::unordered_map<std::string, QPixmap> done;
    std::unordered_set<std::string> pending;
    std::vector<std::function<void(const std::string&)>> waiting;

    static Thumbs& get()
    {
        static Thumbs t;
        return t;
    }

    static std::string key(const fs::path& file)
    {
        std::error_code ec;
        auto t = fs::last_write_time(file, ec);
        return paths::toUtf8(file) + "|" + std::to_string(ec ? 0 : t.time_since_epoch().count());
    }

    /** The cached pixmap, or null after queueing a read; then arrived() reports the key. */
    const QPixmap* find(const std::string& k, const fs::path& file)
    {
        if (auto it = done.find(k); it != done.end())
            return &it->second;
        if (pending.insert(k).second)
        {
            QString path = QString::fromStdU16String(file.u16string());
            QThreadPool::globalInstance()->start([k, path] {
                QImageReader reader(path);
                QImage img;
                if (reader.canRead())
                {
                    QSize size = reader.size();
                    if (size.isValid() && (size.width() > kIcon * 2 || size.height() > kIcon * 2))
                        reader.setScaledSize(size.scaled(kIcon * 2, kIcon * 2, Qt::KeepAspectRatio));
                    img = reader.read();
                }
                QMetaObject::invokeMethod(qApp, [k, img] { get().arrived(k, img); }, Qt::QueuedConnection);
            });
        }
        return nullptr;
    }

    void arrived(const std::string& k, const QImage& img)
    {
        pending.erase(k);
        if (done.size() > 4000)
            done.clear();
        QPixmap pm(kIcon, kIcon);
        pm.fill(Qt::transparent);
        if (!img.isNull())
        {
            QImage s = img.scaled(kIcon, kIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPainter p(&pm);
            p.drawImage((kIcon - s.width()) / 2, (kIcon - s.height()) / 2, s);
        }
        done[k] = pm;
        for (auto& fn : waiting)
            if (fn)
                fn(k);
    }
};

class Popup : public QFrame
{
public:
    Popup(QWidget* anchor, const std::string& current, std::function<void(const std::string&)> pick)
        : QFrame(anchor->window(), Qt::Popup), mAnchor(anchor), mPick(std::move(pick))
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        if (!current.empty())
        {
            std::error_code ec;
            mCurrent = textMatch::lower(paths::toUtf8(fs::weakly_canonical(paths::resolvePath(current), ec)));
        }

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);
        mEdit = new QLineEdit(this);
        mEdit->setPlaceholderText("Search project textures");
        mEdit->setClearButtonEnabled(true);
        mList = new QListWidget(this);
        mList->setIconSize({kIcon, kIcon});
        mList->setUniformItemSizes(true);
        mList->setFocusPolicy(Qt::NoFocus);
        mList->setTextElideMode(Qt::ElideMiddle);
        auto* foot = new QHBoxLayout;
        mStatus = new QLabel(this);
        mStatus->setEnabled(false);
        auto* refresh = new QToolButton(this);
        refresh->setIcon(icon("refresh"));
        refresh->setToolTip("Rescan the project folder");
        refresh->setAutoRaise(true);
        auto* folders = new QToolButton(this);
        folders->setText("Texture Folders...");
        folders->setIcon(icon("filter"));
        folders->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        folders->setToolTip("Which folders of the project are listed, for the workspace and this scene");
        folders->setAutoRaise(true);
        foot->addWidget(mStatus, 1);
        foot->addWidget(refresh);
        foot->addWidget(folders);
        lay->addWidget(mEdit);
        lay->addWidget(mList, 1);
        lay->addLayout(foot);

        mEdit->installEventFilter(this);
        connect(mEdit, &QLineEdit::textChanged, this, [this] { refill(); });
        connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { activate(it); });
        connect(mList->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { requestThumbs(); });
        connect(refresh, &QToolButton::clicked, this, [] { pt::Index::get().refresh(); });
        connect(folders, &QToolButton::clicked, this, [this] {
            QPointer<QWidget> parent = mAnchor ? mAnchor->window() : nullptr;
            close();
            showFoldersDialog(parent);
        });

        mListener = pt::Index::get().addListener([this] { refill(); });
        mThumbSlot = Thumbs::get().waiting.size();
        Thumbs::get().waiting.push_back([this](const std::string& k) { thumbArrived(k); });
        pt::Index::get().follow();
        refill();
        place();
    }

    ~Popup() override
    {
        pt::Index::get().removeListener(mListener);
        Thumbs::get().waiting[mThumbSlot] = nullptr;
    }

    void start()
    {
        show();
        mEdit->setFocus();
        requestThumbs();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != mEdit || e->type() != QEvent::KeyPress)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        int n = mList->count();
        int row = mList->currentRow();
        switch (k->key())
        {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
        {
            if (!n)
                return true;
            int step = k->key() == Qt::Key_Down ? 1 : k->key() == Qt::Key_Up ? -1 : k->key() == Qt::Key_PageDown ? 10 : -10;
            mList->setCurrentRow(std::clamp(row + step, 0, n - 1));
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter: activate(mList->currentItem()); return true;
        case Qt::Key_Escape: close(); return true;
        default: return false;
        }
    }

private:
    void place()
    {
        int w = std::max(mAnchor->width(), 380);
        resize(w, 420);
        QPoint at = mAnchor->mapToGlobal(QPoint(0, mAnchor->height()));
        if (QScreen* screen = mAnchor->screen())
        {
            QRect avail = screen->availableGeometry();
            if (at.y() + height() > avail.bottom())
                at.setY(mAnchor->mapToGlobal(QPoint(0, 0)).y() - height());
            at.setX(std::clamp(at.x(), avail.left(), std::max(avail.left(), avail.right() - width())));
        }
        move(at);
    }

    void refill()
    {
        QString keep;
        if (QListWidgetItem* it = mList->currentItem())
            keep = it->data(kRelRole).toString();

        pt::Index::Snapshot snap = pt::Index::get().snapshot();
        mRoot = snap.root;
        tf::Rules rules = tf::current();
        std::vector<std::string> files = rules.filter.apply(snap.files);
        std::string queryText = mEdit->text().trimmed().toStdString();
        textMatch::Query query(queryText);

        // Matches in the file name rank above ones that only match a folder.
        std::vector<const std::string*> byName, byFolder;
        for (const std::string& f : files)
        {
            if (!query.matches({f}))
                continue;
            size_t slash = f.rfind('/');
            std::string_view name = slash == std::string::npos ? std::string_view(f) : std::string_view(f).substr(slash + 1);
            (query.empty() || query.matches({name}) ? byName : byFolder).push_back(&f);
        }

        mList->clear();
        auto special = [&](Kind kind, const char* text, const char* iconName, const char* words) {
            if (!query.empty() && !query.matches({std::string_view(words)}))
                return;
            auto* it = new QListWidgetItem(icon(iconName), text, mList);
            it->setData(kKindRole, kind);
            it->setSizeHint({0, kIcon + 4});
        };
        special(kNone, "None", "ban", "none clear");
        special(kBrowse, "Browse...", "folder-open", "browse file");

        QListWidgetItem* select = nullptr;
        for (const auto* list : {&byName, &byFolder})
            for (const std::string* f : *list)
            {
                fs::path abs = snap.root / paths::fromUtf8(*f);
                size_t slash = f->rfind('/');
                QString name = qs(slash == std::string::npos ? *f : f->substr(slash + 1));
                QString folder = slash == std::string::npos ? QString() : qs(f->substr(0, slash));
                auto* it = new QListWidgetItem(folder.isEmpty() ? name : name + "\n" + folder, mList);
                it->setData(kKindRole, kFile);
                it->setData(kRelRole, qs(*f));
                it->setToolTip(QString::fromStdU16String(abs.u16string()));
                it->setSizeHint({0, kIcon + 6});
                if (!mCurrent.empty() && textMatch::lower(paths::toUtf8(abs.lexically_normal())) == mCurrent)
                {
                    QFont bold = it->font();
                    bold.setBold(true);
                    it->setFont(bold);
                    if (!select && keep.isEmpty())
                        select = it;
                }
                if (!keep.isEmpty() && it->data(kRelRole).toString() == keep)
                    select = it;
            }
        if (!select && !query.empty())
            for (int i = 0; i < mList->count() && !select; ++i)
                if (mList->item(i)->data(kKindRole).toInt() == kFile)
                    select = mList->item(i);
        if (!select && mList->count())
            select = mList->item(0);
        mList->setCurrentItem(select);
        if (select)
            mList->scrollToItem(select);

        size_t shown = byName.size() + byFolder.size();
        QString status;
        if (snap.root.empty() && !snap.scanning)
            status = "No project folder: open a workspace or save the scene";
        else if (snap.scanning && snap.files.empty())
            status = "Scanning...";
        else
        {
            status = QString("%1 of %2 textures").arg(shown).arg(snap.files.size());
            if (snap.truncated)
                status += " (folder too large, partial)";
            else if (snap.scanning)
                status += " (rescanning)";
        }
        mStatus->setText(status);
        mStatus->setToolTip(QString::fromStdU16String(snap.root.u16string()));
        requestThumbs();
    }

    void requestThumbs()
    {
        if (!isVisible() || !mList->count())
            return;
        QRect view = mList->viewport()->rect();
        int first = mList->row(mList->itemAt(view.topLeft()));
        int last = mList->row(mList->itemAt(view.bottomLeft()));
        if (first < 0)
            first = 0;
        if (last < 0)
            last = mList->count() - 1;
        for (int i = first; i <= last && i < mList->count(); ++i)
        {
            QListWidgetItem* it = mList->item(i);
            if (it->data(kKindRole).toInt() != kFile || !it->data(kThumbRole).isNull())
                continue;
            fs::path abs = mRoot / paths::fromUtf8(it->data(kRelRole).toString().toStdString());
            std::string k = Thumbs::key(abs);
            it->setData(kThumbRole, qs(k));
            if (const QPixmap* pm = Thumbs::get().find(k, abs))
                it->setIcon(*pm);
        }
    }

    void thumbArrived(const std::string& k)
    {
        QString key = qs(k);
        for (int i = 0; i < mList->count(); ++i)
            if (QListWidgetItem* it = mList->item(i); it->data(kThumbRole).toString() == key)
                if (auto found = Thumbs::get().done.find(k); found != Thumbs::get().done.end())
                    it->setIcon(found->second);
    }

    void activate(QListWidgetItem* it)
    {
        if (!it)
            return;
        auto pick = mPick;
        QPointer<QWidget> anchor = mAnchor;
        fs::path root = mRoot;
        int kind = it->data(kKindRole).toInt();
        std::string chosen;
        if (kind == kFile)
            chosen = paths::toUtf8(root / paths::fromUtf8(it->data(kRelRole).toString().toStdString()));
        close();
        if (kind == kBrowse)
        {
            QString start = root.empty() ? QString() : QString::fromStdU16String(root.u16string());
            QString f = QFileDialog::getOpenFileName(anchor ? anchor->window() : nullptr, "Texture", start,
                                                     "Images (*.png *.tga *.jpg *.jpeg *.exr *.tif *.tiff *.psd *.hdr *.bmp *.dds);;All files (*)");
            if (f.isEmpty())
                return;
            chosen = paths::toUtf8(fs::path(f.toStdU16String()));
        }
        if (pick)
            pick(chosen);
    }

    QPointer<QWidget> mAnchor;
    std::function<void(const std::string&)> mPick;
    /** The slot's file, canonical and lowercased, to compare against listed paths without touching the disk. */
    std::string mCurrent;
    fs::path mRoot;
    QLineEdit* mEdit = nullptr;
    QListWidget* mList = nullptr;
    QLabel* mStatus = nullptr;
    int mListener = 0;
    size_t mThumbSlot = 0;
};

QString joinLines(const json& list)
{
    QStringList out;
    if (list.is_array())
        for (const json& v : list)
            if (v.is_string())
                out << qs(v.get<std::string>());
    return out.join('\n');
}

json splitLines(const QPlainTextEdit* edit)
{
    json out = json::array();
    for (const QString& line : edit->toPlainText().split('\n', Qt::SkipEmptyParts))
        if (QString t = line.trimmed(); !t.isEmpty())
            out.push_back(t.toStdString());
    return out;
}

class FoldersDialog : public QDialog
{
public:
    explicit FoldersDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle("Texture Folders");
        setAttribute(Qt::WA_DeleteOnClose);
        auto* lay = new QVBoxLayout(this);

        pt::Index::get().follow();
        fs::path root = pt::projectRoot();
        auto* where = new QLabel(root.empty() ? QString("No project folder: open a workspace or save the scene.")
                                              : QString("Project folder: %1").arg(QString::fromStdU16String(root.u16string())),
                                 this);
        where->setWordWrap(true);
        lay->addWidget(where);
        auto* help = new QLabel("One glob per line, relative to the project folder. * stays within a folder name, ** spans "
                                "folders, a pattern without / matches at any depth and a folder takes everything in it. "
                                "Examples: Textures/**   **/*_old.*   Saved. An empty include list takes everything; "
                                "excludes win.",
                                this);
        help->setWordWrap(true);
        help->setEnabled(false);
        lay->addWidget(help);

        json ws = tf::workspaceRules();
        json sc = tf::sceneRules();
        auto* wsBox = new QGroupBox(Workspace::get().isOpen() ? "Workspace (.lila/settings.json)" : "User settings (no workspace open)", this);
        auto* wsForm = new QFormLayout(wsBox);
        mWsInclude = editor(joinLines(ws.value("include", json::array())));
        mWsExclude = editor(joinLines(ws.value("exclude", json::array())));
        wsForm->addRow("Include", mWsInclude);
        wsForm->addRow("Exclude", mWsExclude);
        lay->addWidget(wsBox);

        auto* scBox = new QGroupBox("This scene (overrides the workspace, list by list)", this);
        auto* scForm = new QFormLayout(scBox);
        mScOverrideInclude = new QCheckBox("Override include", scBox);
        mScOverrideExclude = new QCheckBox("Override exclude", scBox);
        mScOverrideInclude->setChecked(sc.contains("include"));
        mScOverrideExclude->setChecked(sc.contains("exclude"));
        mScInclude = editor(joinLines(sc.value("include", json::array())));
        mScExclude = editor(joinLines(sc.value("exclude", json::array())));
        scForm->addRow(mScOverrideInclude);
        scForm->addRow("Include", mScInclude);
        scForm->addRow(mScOverrideExclude);
        scForm->addRow("Exclude", mScExclude);
        lay->addWidget(scBox);

        mPreview = new QLabel(this);
        lay->addWidget(mPreview);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        lay->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); accept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        for (QCheckBox* c : {mScOverrideInclude, mScOverrideExclude})
            connect(c, &QCheckBox::toggled, this, [this] { update(); });
        for (QPlainTextEdit* e : {mWsInclude, mWsExclude, mScInclude, mScExclude})
            connect(e, &QPlainTextEdit::textChanged, this, [this] { update(); });
        mListener = pt::Index::get().addListener([this] { update(); });
        update();
        resize(520, 560);
    }

    ~FoldersDialog() override { pt::Index::get().removeListener(mListener); }

private:
    QPlainTextEdit* editor(const QString& text)
    {
        auto* e = new QPlainTextEdit(text, this);
        e->setTabChangesFocus(true);
        e->setFixedHeight(fontMetrics().lineSpacing() * 5 + 12);
        return e;
    }

    json workspaceEdit() const { return {{"include", splitLines(mWsInclude)}, {"exclude", splitLines(mWsExclude)}}; }

    json sceneEdit() const
    {
        json s = json::object();
        if (mScOverrideInclude->isChecked())
            s["include"] = splitLines(mScInclude);
        if (mScOverrideExclude->isChecked())
            s["exclude"] = splitLines(mScExclude);
        return s;
    }

    void update()
    {
        mScInclude->setEnabled(mScOverrideInclude->isChecked());
        mScExclude->setEnabled(mScOverrideExclude->isChecked());
        pt::Index::Snapshot snap = pt::Index::get().snapshot();
        tf::Rules r = tf::resolve(workspaceEdit(), sceneEdit());
        QString text = QString("Lists %1 of %2 images").arg(r.filter.apply(snap.files).size()).arg(snap.files.size());
        if (snap.scanning)
            text += " (scanning)";
        mPreview->setText(text);
    }

    void apply()
    {
        json ws = workspaceEdit();
        json wsNow = tf::workspaceRules();
        if (ws["include"] != wsNow.value("include", json::array()) || ws["exclude"] != wsNow.value("exclude", json::array()))
            runCommand("textureFolders", ws);
        json sc = sceneEdit();
        if (sc != tf::sceneRules())
        {
            json call = sc;
            call["scene"] = true;
            call["clear"] = true;
            runCommand("textureFolders", call);
        }
    }

    QPlainTextEdit* mWsInclude = nullptr;
    QPlainTextEdit* mWsExclude = nullptr;
    QCheckBox* mScOverrideInclude = nullptr;
    QCheckBox* mScOverrideExclude = nullptr;
    QPlainTextEdit* mScInclude = nullptr;
    QPlainTextEdit* mScExclude = nullptr;
    QLabel* mPreview = nullptr;
    int mListener = 0;
};
} // namespace

void open(QWidget* anchor, const std::string& current, std::function<void(const std::string&)> pick)
{
    if (!anchor)
        return;
    (new Popup(anchor, current, std::move(pick)))->start();
}

void showFoldersDialog(QWidget* parent)
{
    auto* d = new FoldersDialog(parent);
    d->show();
}
} // namespace lila::ui::texturePicker
