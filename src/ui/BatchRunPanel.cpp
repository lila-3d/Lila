#include "ui/BatchRunPanel.h"

#include "batch/BatchJob.h"
#include "batch/BatchRunner.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "ops/Command.h"
#include "scene/SceneScripts.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/TokenCompleter.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/ScriptEditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <array>
#include <fstream>
#include <sstream>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;
constexpr const char* kPrefs = "batchRun";

enum Column
{
    kIndex,
    kFile,
    kStatus,
    kTime,
    kOutput,
    kMessage,
    kFirstResult,
};

enum Source
{
    kFromSnippet,
    kFromFile,
    kFromEditor,
    kFromScene,
};

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

json run(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

QToolButton* toolButton(QWidget* parent, const char* iconName, const QString& tip, const QString& text = {})
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    if (!text.isEmpty())
    {
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    return b;
}

const char* statusIcon(batch::Status s)
{
    switch (s)
    {
    case batch::Status::Running:
        return "loader-2";
    case batch::Status::Done:
        return "circle-check";
    case batch::Status::Failed:
        return "circle-x";
    case batch::Status::Skipped:
        return "player-skip-forward";
    case batch::Status::Cancelled:
        return "ban";
    case batch::Status::Planned:
        return "list-check";
    default:
        return "clock";
    }
}

QStringList splitList(const QString& s) { return s.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts); }

json listJson(const QString& s)
{
    json out = json::array();
    for (const QString& part : splitList(s))
        if (!part.trimmed().isEmpty())
            out.push_back(ss(part.trimmed()));
    return out;
}

QString joinList(const json& j)
{
    if (j.is_string())
        return qs(j.get<std::string>());
    QStringList parts;
    if (j.is_array())
        for (const json& v : j)
            if (v.is_string())
                parts << qs(v.get<std::string>());
    return parts.join(", ");
}

ScriptEditor* scriptEditor()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* e = w->findChild<ScriptEditor*>())
            return e;
    return nullptr;
}

std::string readText(const std::string& path)
{
    std::ifstream f(paths::fromUtf8(path), std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/** Files and folders dropped from a file browser are added as inputs. */
class DropList : public QListWidget
{
public:
    std::function<void(const QStringList&)> dropped;

    explicit DropList(QWidget* parent) : QListWidget(parent)
    {
        setAcceptDrops(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasUrls())
            e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (e->mimeData()->hasUrls())
            e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override
    {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        if (dropped && !paths.isEmpty())
            dropped(paths);
        e->acceptProposedAction();
    }
};
} // namespace

BatchRunPanel::BatchRunPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel("Preset", this));
    mPresets = new QComboBox(this);
    mPresets->setToolTip("Saved settings: script, files and options. Pick one to load it.");
    presetRow->addWidget(mPresets, 1);
    auto* savePresetButton = toolButton(this, "device-floppy", "Save these settings as a preset");
    auto* deletePresetButton = toolButton(this, "trash", "Delete the selected preset");
    presetRow->addWidget(savePresetButton);
    presetRow->addWidget(deletePresetButton);
    v->addLayout(presetRow);

    auto* split = new QSplitter(Qt::Vertical, this);
    mTabs = new QTabWidget(split);
    mTabs->addTab(buildScriptTab(), "Script");
    mTabs->addTab(buildFilesTab(), "Files");
    mTabs->addTab(buildSaveTab(), "Save");
    mTabs->addTab(buildOptionsTab(), "Options");
    split->addWidget(mTabs);

    auto* results = new QWidget(split);
    auto* rv = new QVBoxLayout(results);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(4);
    auto* actionsRow = new QHBoxLayout;
    mPreview = toolButton(results, "eye", "List the files, where each one saves and whether it can be written, without running anything", "Preview");
    mRun = toolButton(results, "player-play", "Run the script over every file, in background workers", "Run");
    mCancel = toolButton(results, "player-stop", "End the running workers; files not started are left cancelled", "Cancel");
    mRetry = toolButton(results, "refresh", "Run the failed and cancelled files again", "Retry Failed");
    mCsv = toolButton(results, "file-export", "Save the results table as CSV");
    mLogs = toolButton(results, "folder-open", "Open the run's folder: the job, each file's log and result");
    for (QToolButton* b : {mPreview, mRun, mCancel, mRetry})
        actionsRow->addWidget(b);
    actionsRow->addStretch(1);
    actionsRow->addWidget(mCsv);
    actionsRow->addWidget(mLogs);
    rv->addLayout(actionsRow);

    auto* progressRow = new QHBoxLayout;
    mProgress = new QProgressBar(results);
    mProgress->setTextVisible(false);
    mProgress->setMaximumHeight(8);
    mSummary = new QLabel(results);
    progressRow->addWidget(mProgress, 1);
    progressRow->addWidget(mSummary);
    rv->addLayout(progressRow);

    auto* resultSplit = new QSplitter(Qt::Vertical, results);
    mTable = new QTreeWidget(resultSplit);
    mTable->setRootIsDecorated(false);
    mTable->setUniformRowHeights(true);
    mTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mTable->setHeaderLabels({"#", "File", "Status", "Time", "Output", "Message"});
    mTable->setContextMenuPolicy(Qt::CustomContextMenu);
    mTable->header()->setStretchLastSection(true);
    mTable->setSortingEnabled(true);
    mLog = new QTextBrowser(resultSplit);
    mLog->setOpenLinks(false);
    mLog->setFont(monoFont());
    mLog->setPlaceholderText("Select a file to see its log. Traceback lines open the script at that line.");
    resultSplit->addWidget(mTable);
    resultSplit->addWidget(mLog);
    resultSplit->setStretchFactor(0, 3);
    resultSplit->setStretchFactor(1, 2);
    rv->addWidget(resultSplit, 1);
    split->addWidget(results);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    v->addWidget(split, 1);

    mPoll = new QTimer(this);
    mPoll->setInterval(150);
    connect(mPoll, &QTimer::timeout, this, &BatchRunPanel::refreshRows);
    mRemember = new QTimer(this);
    mRemember->setSingleShot(true);
    mRemember->setInterval(500);
    connect(mRemember, &QTimer::timeout, this, [this] { PrefStore::get().set(kPrefs, "/last", formState()); });

    connect(mPreview, &QToolButton::clicked, this, [this] { start(true); });
    connect(mRun, &QToolButton::clicked, this, [this] { start(false); });
    connect(mCancel, &QToolButton::clicked, this, &BatchRunPanel::cancel);
    connect(mRetry, &QToolButton::clicked, this, [this] { retry({}); });
    connect(mCsv, &QToolButton::clicked, this, &BatchRunPanel::exportCsv);
    connect(mLogs, &QToolButton::clicked, this, [this] {
        if (mRunShown)
            QDesktopServices::openUrl(QUrl::fromLocalFile(qs(paths::toUtf8(mRunShown->dir()))));
    });
    connect(mTable, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) { showLog(it); });
    connect(mTable, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        if (!mRunShown || !it)
            return;
        batch::FileResult r = mRunShown->result(size_t(it->data(kIndex, Qt::UserRole).toInt()));
        if (!r.errorFile.empty())
            openTrace(qs(r.errorFile), r.errorLine);
    });
    connect(mTable, &QTreeWidget::customContextMenuRequested, this, &BatchRunPanel::rowMenu);
    connect(mLog, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString target = url.toString();
        int sep = target.lastIndexOf('|');
        if (target.startsWith("trace:") && sep > 0)
            openTrace(target.mid(6, sep - 6), target.mid(sep + 1).toInt());
    });
    connect(mPresets, &QComboBox::activated, this, [this](int i) {
        if (i <= 0)
            return;
        json all = PrefStore::get().value(kPrefs, "/presets", json::object());
        std::string name = ss(mPresets->itemText(i));
        if (all.is_object() && all.contains(name))
            applyState(all[name]);
    });
    connect(savePresetButton, &QToolButton::clicked, this, &BatchRunPanel::savePreset);
    connect(deletePresetButton, &QToolButton::clicked, this, &BatchRunPanel::deletePreset);

    refreshPresets();
    json last = PrefStore::get().value(kPrefs, "/last", json());
    if (last.is_object())
        applyState(last);

    // Any edit to the form is remembered for next time.
    auto remember = [this] { rememberForm(); };
    for (QLineEdit* e : findChildren<QLineEdit*>())
        connect(e, &QLineEdit::textChanged, this, remember);
    for (QCheckBox* c : findChildren<QCheckBox*>())
        connect(c, &QCheckBox::toggled, this, remember);
    for (QComboBox* c : {mSource, mSceneScript, mSave, mFormat, mCheckout})
        connect(c, &QComboBox::currentIndexChanged, this, remember);
    for (QSpinBox* s : {mDepth, mWorkers})
        connect(s, &QSpinBox::valueChanged, this, remember);
    connect(mTimeout, &QDoubleSpinBox::valueChanged, this, remember);
    connect(mSnippet, &QPlainTextEdit::textChanged, this, remember);
    connect(mInputs->model(), &QAbstractItemModel::rowsInserted, this, remember);
    connect(mInputs->model(), &QAbstractItemModel::rowsRemoved, this, remember);

    batch::Run* newest = batch::findRun().get();
    if (newest)
        watch(newest->id());
    updateButtons();
}

BatchRunPanel::~BatchRunPanel() = default;

QWidget* BatchRunPanel::buildScriptTab()
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel("Run", page));
    mSource = new QComboBox(page);
    mSource->addItems({"Snippet", "Script file", "Script editor tab", "Scene script"});
    mSource->setToolTip("Where the script comes from. The script editor tab and scene script are read when the run starts.");
    row->addWidget(mSource, 1);
    v->addLayout(row);

    mSourcePages = new QStackedWidget(page);
    mSnippet = new QPlainTextEdit(mSourcePages);
    mSnippet->setFont(monoFont());
    mSnippet->setPlaceholderText("# Runs once per file, with that file open.\n"
                                 "# ctx.file, ctx.output, ctx.index, ctx.args; ctx.results['key'] = value adds a column.\n"
                                 "# ctx.save = False leaves the file unsaved; ctx.skip('why') skips it.\n"
                                 "ctx.results['nodes'] = len(ls())");
    mSnippet->setLineWrapMode(QPlainTextEdit::NoWrap);
    mSourcePages->addWidget(mSnippet);

    auto* filePage = new QWidget(mSourcePages);
    auto* fv = new QVBoxLayout(filePage);
    fv->setContentsMargins(0, 0, 0, 0);
    auto* fileRow = new QHBoxLayout;
    mScriptFile = new QLineEdit(filePage);
    mScriptFile->setPlaceholderText("path/to/script.py");
    auto* browse = toolButton(filePage, "folder-open", "Pick the script file");
    auto* edit = toolButton(filePage, "file-code", "Open it in the script editor");
    fileRow->addWidget(mScriptFile, 1);
    fileRow->addWidget(browse);
    fileRow->addWidget(edit);
    fv->addLayout(fileRow);
    fv->addStretch(1);
    mSourcePages->addWidget(filePage);
    connect(browse, &QToolButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, "Batch Script", mScriptFile->text(), "Python (*.py);;All files (*)");
        if (!f.isEmpty())
            mScriptFile->setText(QDir::fromNativeSeparators(f));
    });
    connect(edit, &QToolButton::clicked, this, [this] {
        if (!mScriptFile->text().isEmpty())
            openTrace(mScriptFile->text(), 0);
    });

    auto* editorPage = new QLabel("Runs the script editor's current tab as it is when the run starts, saved or not.", mSourcePages);
    editorPage->setWordWrap(true);
    editorPage->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    mSourcePages->addWidget(editorPage);

    auto* scenePage = new QWidget(mSourcePages);
    auto* sv = new QVBoxLayout(scenePage);
    sv->setContentsMargins(0, 0, 0, 0);
    mSceneScript = new QComboBox(scenePage);
    mSceneScript->setToolTip("A script saved in the open scene. Its text is taken when the run starts.");
    sv->addWidget(mSceneScript);
    sv->addStretch(1);
    mSourcePages->addWidget(scenePage);
    auto fillSceneScripts = [this] {
        const QString current = mSceneScript->currentText();
        mSceneScript->clear();
        for (const scene::scripts::Script& s : scene::scripts::all())
            mSceneScript->addItem(qs(s.name));
        if (int i = mSceneScript->findText(current); i >= 0)
            mSceneScript->setCurrentIndex(i);
    };
    fillSceneScripts();
    int listener = scene::scripts::addListener(fillSceneScripts);
    connect(this, &QObject::destroyed, [listener] { scene::scripts::removeListener(listener); });

    v->addWidget(mSourcePages, 1);
    connect(mSource, &QComboBox::currentIndexChanged, mSourcePages, &QStackedWidget::setCurrentIndex);
    return page;
}

QWidget* BatchRunPanel::buildFilesTab()
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);

    auto* buttons = new QHBoxLayout;
    auto* addFiles = toolButton(page, "file-plus", "Add scene files");
    auto* addFolder = toolButton(page, "folder-plus", "Add a folder; its scenes are found when the run starts");
    auto* remove = toolButton(page, "x", "Remove the selected entries");
    auto* clear = toolButton(page, "trash", "Remove every entry");
    mFileLists = toolButton(page, "playlist-add", "Saved file lists", "Lists");
    mFileLists->setPopupMode(QToolButton::InstantPopup);
    mFileLists->setMenu(new QMenu(mFileLists));
    connect(mFileLists->menu(), &QMenu::aboutToShow, this, &BatchRunPanel::fillFileListMenu);
    for (QToolButton* b : {addFiles, addFolder, remove, clear})
        buttons->addWidget(b);
    buttons->addStretch(1);
    buttons->addWidget(mFileLists);
    v->addLayout(buttons);

    auto* list = new DropList(page);
    list->dropped = [this](const QStringList& paths) { addInputs(paths); };
    list->setToolTip("Files, folders and globs (props/**/*.ma). Drop files or folders here.");
    mInputs = list;
    v->addWidget(mInputs, 1);
    mTypedInput = new QLineEdit(page);
    mTypedInput->setPlaceholderText("Type a path or glob and press Enter");
    v->addWidget(mTypedInput);
    connect(mTypedInput, &QLineEdit::returnPressed, this, [this] {
        if (!mTypedInput->text().trimmed().isEmpty())
            addInputs({mTypedInput->text().trimmed()});
        mTypedInput->clear();
    });
    auto startDir = [] { return Workspace::get().isOpen() ? qs(paths::toUtf8(Workspace::get().root())) : QString(); };
    connect(addFiles, &QToolButton::clicked, this, [this, startDir] {
        QStringList files = QFileDialog::getOpenFileNames(this, "Add Scenes", startDir(), "Scenes (*.lila *.lilaa *.ma *.mb *.fbx *.obj);;All files (*)");
        addInputs(files);
    });
    connect(addFolder, &QToolButton::clicked, this, [this, startDir] {
        QString dir = QFileDialog::getExistingDirectory(this, "Add Folder", startDir());
        if (!dir.isEmpty())
            addInputs({dir});
    });
    connect(remove, &QToolButton::clicked, this, [this] { qDeleteAll(mInputs->selectedItems()); });
    deletion::claim(mInputs, [this] { qDeleteAll(mInputs->selectedItems()); });
    connect(clear, &QToolButton::clicked, mInputs, &QListWidget::clear);

    auto* form = new QFormLayout;
    auto* depthRow = new QHBoxLayout;
    mRecursive = new QCheckBox("All subfolders", page);
    mDepth = new QSpinBox(page);
    mDepth->setRange(0, 64);
    mDepth->setToolTip("How many folder levels below each added folder to search; 0 for the folder itself");
    depthRow->addWidget(mRecursive);
    depthRow->addWidget(new QLabel("Depth", page));
    depthRow->addWidget(mDepth);
    depthRow->addStretch(1);
    connect(mRecursive, &QCheckBox::toggled, mDepth, &QWidget::setDisabled);
    form->addRow("Folders", depthRow);
    mExtensions = new QLineEdit(page);
    mExtensions->setPlaceholderText("every type Lila opens");
    mExtensions->setToolTip("File types folders are searched for, comma separated: lila, ma, fbx");
    completion::attachValues(mExtensions, [] {
        QStringList out;
        for (const std::string& e : batch::openableExtensions())
            out << QString::fromStdString(e).remove(0, e.starts_with('.') ? 1 : 0);
        return out;
    }, true);
    form->addRow("Types", mExtensions);
    mInclude = new QLineEdit(page);
    mInclude->setPlaceholderText("char_*, props/*");
    mInclude->setToolTip("Globs a file must match. With a slash they match the path below its folder.");
    form->addRow("Include", mInclude);
    mExclude = new QLineEdit(page);
    mExclude->setToolTip("Globs that leave a file out.");
    form->addRow("Exclude", mExclude);
    mExcludePrefix = new QLineEdit(page);
    mExcludePrefix->setToolTip("Leave out files whose name starts with this");
    form->addRow("Skip prefix", mExcludePrefix);
    mExcludeSuffix = new QLineEdit(page);
    mExcludeSuffix->setToolTip("Leave out files whose name, without its extension, ends with this");
    form->addRow("Skip suffix", mExcludeSuffix);
    mExcludeContains = new QLineEdit(page);
    mExcludeContains->setToolTip("Leave out files whose name contains this");
    form->addRow("Skip containing", mExcludeContains);
    v->addLayout(form);
    return page;
}

QWidget* BatchRunPanel::buildSaveTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(4, 4, 4, 4);
    mSave = new QComboBox(page);
    mSave->addItem("Don't save", "none");
    mSave->addItem("Overwrite", "overwrite");
    mSave->addItem("Save as", "saveAs");
    mSave->setToolTip("Overwrite saves in place; a scene Lila cannot write (.ma, .fbx) saves as .lila beside it.");
    form->addRow("After the script", mSave);
    auto* outRow = new QHBoxLayout;
    mOutputDir = new QLineEdit(page);
    mOutputDir->setPlaceholderText("beside each source");
    auto* browse = toolButton(page, "folder-open", "Pick the output folder");
    outRow->addWidget(mOutputDir, 1);
    outRow->addWidget(browse);
    form->addRow("Output folder", outRow);
    connect(browse, &QToolButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(this, "Output Folder", mOutputDir->text());
        if (!dir.isEmpty())
            mOutputDir->setText(QDir::fromNativeSeparators(dir));
    });
    mSuffix = new QLineEdit(page);
    mSuffix->setPlaceholderText("_fixed");
    form->addRow("Name suffix", mSuffix);
    mFormat = new QComboBox(page);
    mFormat->addItem("Same as source", "");
    mFormat->addItem("Lila binary (.lila)", "lila");
    mFormat->addItem("Lila text (.lilaa)", "lilaa");
    form->addRow("Format", mFormat);
    mKeepTree = new QCheckBox("Mirror subfolders under the output folder", page);
    mKeepTree->setChecked(true);
    form->addRow("", mKeepTree);
    mCheckout = new QComboBox(page);
    mCheckout->addItem("Check out read-only files", "auto");
    mCheckout->addItem("Leave source control alone", "never");
    mCheckout->setToolTip("Read-only files under source control are checked out before saving, following its autoCheckout pref.");
    form->addRow("Source control", mCheckout);
    mMakeWritable = new QCheckBox("Make other read-only files writable", page);
    form->addRow("", mMakeWritable);

    auto sync = [this] {
        const bool saving = mSave->currentData().toString() != "none";
        const bool saveAs = mSave->currentData().toString() == "saveAs";
        for (QWidget* w : std::initializer_list<QWidget*>{mOutputDir, mSuffix, mFormat, mKeepTree})
            w->setEnabled(saveAs);
        mCheckout->setEnabled(saving);
        mMakeWritable->setEnabled(saving);
    };
    connect(mSave, &QComboBox::currentIndexChanged, this, sync);
    sync();
    return page;
}

QWidget* BatchRunPanel::buildOptionsTab()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(4, 4, 4, 4);
    mWorkers = new QSpinBox(page);
    mWorkers->setRange(0, 64);
    mWorkers->setSpecialValueText("Auto");
    mWorkers->setToolTip("Files processed at once, each in its own lilabatch. Auto: half the cores, at most 8.");
    form->addRow("Workers", mWorkers);
    mStopOnError = new QCheckBox("Stop at the first failure", page);
    mStopOnError->setToolTip("Off: a failing file is reported and the rest carry on.");
    form->addRow("", mStopOnError);
    mTimeout = new QDoubleSpinBox(page);
    mTimeout->setRange(0, 86400);
    mTimeout->setDecimals(0);
    mTimeout->setSuffix(" s");
    mTimeout->setSpecialValueText("None");
    mTimeout->setToolTip("A file taking longer than this has its worker ended and fails.");
    form->addRow("Timeout per file", mTimeout);
    mArgs = new QLineEdit(page);
    mArgs->setPlaceholderText("{\"scale\": 0.01}");
    mArgs->setToolTip("A JSON object handed to the script as ctx.args.");
    form->addRow("Script args", mArgs);
    return page;
}

json BatchRunPanel::call(QString* problem, bool withScript) const
{
    json c = json::object();
    json inputs = json::array();
    for (int i = 0; i < mInputs->count(); ++i)
        inputs.push_back(ss(mInputs->item(i)->text()));
    c["args"] = inputs;

    switch (withScript ? mSource->currentIndex() : -1)
    {
    case kFromSnippet:
        c["script"] = ss(mSnippet->toPlainText());
        break;
    case kFromFile:
        c["scriptFile"] = ss(mScriptFile->text().trimmed());
        break;
    case kFromEditor: {
        ScriptEditor* e = scriptEditor();
        CodeEditor* tab = e ? e->currentEditor() : nullptr;
        if (!tab)
        {
            *problem = "The script editor has no tab open.";
            return nullptr;
        }
        c["script"] = ss(tab->toPlainText());
        if (!tab->filePath().isEmpty())
            c["scriptFile"] = ss(QDir::fromNativeSeparators(tab->filePath()));
        break;
    }
    case kFromScene:
        if (mSceneScript->currentText().isEmpty())
        {
            *problem = "The scene has no scripts.";
            return nullptr;
        }
        c["sceneScript"] = ss(mSceneScript->currentText());
        break;
    }

    if (mRecursive->isChecked())
        c["recursive"] = true;
    else if (mDepth->value() > 0)
        c["depth"] = mDepth->value();
    if (json e = listJson(mExtensions->text()); !e.empty())
        c["extensions"] = e;
    if (json e = listJson(mInclude->text()); !e.empty())
        c["include"] = e;
    if (json e = listJson(mExclude->text()); !e.empty())
        c["exclude"] = e;
    for (auto [flag, field] : {std::pair{"excludePrefix", mExcludePrefix}, std::pair{"excludeSuffix", mExcludeSuffix},
                               std::pair{"excludeContains", mExcludeContains}})
        if (!field->text().isEmpty())
            c[flag] = ss(field->text());

    const std::string save = ss(mSave->currentData().toString());
    if (save != "none")
    {
        c["save"] = save;
        if (mCheckout->currentData().toString() != "auto")
            c["checkout"] = ss(mCheckout->currentData().toString());
        if (mMakeWritable->isChecked())
            c["makeWritable"] = true;
    }
    if (save == "saveAs")
    {
        if (!mOutputDir->text().trimmed().isEmpty())
            c["outputDir"] = ss(mOutputDir->text().trimmed());
        if (!mSuffix->text().isEmpty())
            c["suffix"] = ss(mSuffix->text());
        if (!mFormat->currentData().toString().isEmpty())
            c["format"] = ss(mFormat->currentData().toString());
        if (!mKeepTree->isChecked())
            c["keepTree"] = false;
    }
    if (mWorkers->value() > 0)
        c["workers"] = mWorkers->value();
    if (mStopOnError->isChecked())
        c["stopOnError"] = true;
    if (mTimeout->value() > 0)
        c["timeout"] = mTimeout->value();
    if (!mArgs->text().trimmed().isEmpty())
    {
        json a = json::parse(ss(mArgs->text()), nullptr, false);
        if ((a.is_discarded() || !a.is_object()) && withScript)
        {
            *problem = "Script args must be a JSON object, like {\"scale\": 0.01}.";
            return nullptr;
        }
        if (a.is_object())
            c["scriptArgs"] = a;
    }
    return c;
}

json BatchRunPanel::formState() const
{
    QString unused;
    const int source = mSource->currentIndex();
    json c = call(&unused, false);
    c["inputs"] = c.contains("args") ? c["args"] : json::array();
    c.erase("args");
    if (c.contains("scriptArgs"))
    {
        c["args"] = c["scriptArgs"];
        c.erase("scriptArgs");
    }
    if (c.contains("recursive"))
    {
        c["depth"] = -1;
        c.erase("recursive");
    }
    c["source"] = std::array{"snippet", "file", "editor", "scene"}[size_t(std::clamp(source, 0, 3))];
    // Kept so batchRun(preset=...) runs the same script; the editor tab's text is only read when a run starts.
    c["snippet"] = ss(mSnippet->toPlainText());
    if (source == kFromSnippet)
        c["script"] = c["snippet"];
    if (source == kFromFile)
        c["scriptFile"] = ss(mScriptFile->text().trimmed());
    else
        c["scriptFileField"] = ss(mScriptFile->text().trimmed());
    if (source == kFromScene)
        c["sceneScript"] = ss(mSceneScript->currentText());
    return c;
}

void BatchRunPanel::applyState(const json& s)
{
    mApplying = true;
    auto text = [&](const char* k) { return s.contains(k) && s[k].is_string() ? qs(s[k].get<std::string>()) : QString(); };
    std::string source = s.value("source", std::string());
    if (source.empty())
        source = s.contains("sceneScript") ? "scene" : s.contains("scriptFile") ? "file" : "snippet";
    mSnippet->setPlainText(s.contains("snippet") ? text("snippet") : text("script"));
    mScriptFile->setText(s.contains("scriptFile") ? text("scriptFile") : text("scriptFileField"));
    if (int i = mSceneScript->findText(text("sceneScript")); i >= 0)
        mSceneScript->setCurrentIndex(i);
    mSource->setCurrentIndex(source == "file" ? kFromFile : source == "editor" ? kFromEditor : source == "scene" ? kFromScene : kFromSnippet);

    mInputs->clear();
    if (s.contains("inputs") && s["inputs"].is_array())
        for (const json& v : s["inputs"])
            if (v.is_string())
                mInputs->addItem(qs(v.get<std::string>()));
    const int depth = s.value("depth", 0);
    mRecursive->setChecked(depth < 0);
    mDepth->setValue(std::max(0, depth));
    mExtensions->setText(joinList(s.value("extensions", json())));
    mInclude->setText(joinList(s.value("include", json())));
    mExclude->setText(joinList(s.value("exclude", json())));
    mExcludePrefix->setText(text("excludePrefix"));
    mExcludeSuffix->setText(text("excludeSuffix"));
    mExcludeContains->setText(text("excludeContains"));

    auto pick = [](QComboBox* box, const QString& data) {
        int i = box->findData(data);
        box->setCurrentIndex(std::max(0, i));
    };
    pick(mSave, s.contains("save") ? text("save") : QString("none"));
    mOutputDir->setText(text("outputDir"));
    mSuffix->setText(text("suffix"));
    pick(mFormat, text("format"));
    mKeepTree->setChecked(s.value("keepTree", true));
    pick(mCheckout, s.contains("checkout") ? text("checkout") : QString("auto"));
    mMakeWritable->setChecked(s.value("makeWritable", false));
    mWorkers->setValue(s.value("workers", 0));
    mStopOnError->setChecked(s.value("stopOnError", false));
    mTimeout->setValue(s.value("timeout", 0.0));
    mArgs->setText(s.contains("args") && s["args"].is_object() && !s["args"].empty() ? qs(s["args"].dump()) : QString());
    mApplying = false;
}

void BatchRunPanel::rememberForm()
{
    if (!mApplying)
        mRemember->start();
}

void BatchRunPanel::addInputs(const QStringList& paths)
{
    QSet<QString> have;
    for (int i = 0; i < mInputs->count(); ++i)
        have.insert(mInputs->item(i)->text());
    for (const QString& p : paths)
    {
        QString clean = QDir::fromNativeSeparators(p);
        if (!have.contains(clean))
        {
            mInputs->addItem(clean);
            have.insert(clean);
        }
    }
}

void BatchRunPanel::start(bool dryRun)
{
    QString problem;
    json c = call(&problem);
    if (c.is_null())
    {
        QMessageBox::warning(this, "Batch Script Execution", problem);
        return;
    }
    if (c["args"].empty())
    {
        mTabs->setCurrentIndex(1);
        QMessageBox::information(this, "Batch Script Execution", "Add the scene files, folders or globs to run over.");
        return;
    }
    if (dryRun)
        c["dryRun"] = true;
    PrefStore::get().set(kPrefs, "/last", formState());

    json started = run("batchRun", c);
    if (started.is_null())
    {
        QMessageBox::warning(this, "Batch Script Execution", "The run did not start; the Script Output has the reason.");
        return;
    }
    if (started.contains("warnings"))
        for (const json& w : started["warnings"])
            log::warn("batchRun: {}", w.get<std::string>());
    watch(started["id"].get<int>());
}

void BatchRunPanel::cancel()
{
    if (mRunShown && !mRunShown->isFinished())
        run("batchRunCancel", {{"args", {mRunShown->id()}}});
}

void BatchRunPanel::retry(const std::vector<size_t>& indices)
{
    if (!mRunShown)
        return;
    json c = {{"args", {mRunShown->id()}}};
    if (!indices.empty())
        c["indices"] = indices;
    json started = run("batchRunRetry", c);
    if (!started.is_null())
        watch(started["id"].get<int>());
}

void BatchRunPanel::exportCsv()
{
    if (!mRunShown)
        return;
    QString start = Workspace::get().isOpen() ? qs(paths::toUtf8(Workspace::get().root() / "batch_results.csv")) : QString("batch_results.csv");
    QString f = QFileDialog::getSaveFileName(this, "Export Results", start, "CSV (*.csv)");
    if (f.isEmpty())
        return;
    std::ofstream out(paths::fromUtf8(ss(f)), std::ios::binary);
    out << batch::toCsv(mRunShown->items(), mRunShown->results());
    if (!out)
        QMessageBox::warning(this, "Export Results", "Could not write " + f);
}

void BatchRunPanel::watch(int runId)
{
    mRunShown = batch::findRun(runId);
    mShownVersion = ~uint64_t(0);
    mResultKeys.clear();
    mTable->setSortingEnabled(false);
    mTable->clear();
    mTable->setColumnCount(kFirstResult);
    mTable->setHeaderLabels({"#", "File", "Status", "Time", "Output", "Message"});
    mLog->clear();
    if (mRunShown)
    {
        const auto& items = mRunShown->items();
        QList<QTreeWidgetItem*> rows;
        for (size_t i = 0; i < items.size(); ++i)
        {
            auto* row = new QTreeWidgetItem;
            row->setData(kIndex, Qt::DisplayRole, int(i + 1));
            row->setData(kIndex, Qt::UserRole, int(i));
            row->setText(kFile, qs(paths::toUtf8(items[i].path.filename())));
            row->setToolTip(kFile, qs(paths::toUtf8(items[i].path)));
            row->setText(kOutput, items[i].output.empty() ? QString() : qs(paths::toUtf8(items[i].output.filename())));
            row->setToolTip(kOutput, qs(paths::toUtf8(items[i].output)));
            rows << row;
        }
        mTable->addTopLevelItems(rows);
        mTable->resizeColumnToContents(kFile);
    }
    mTable->setSortingEnabled(true);
    refreshRows();
    if (mRunShown && !mRunShown->isFinished())
        mPoll->start();
}

void BatchRunPanel::refreshRows()
{
    if (!mRunShown)
    {
        mPoll->stop();
        mProgress->setRange(0, 1);
        mProgress->setValue(0);
        mSummary->clear();
        updateButtons();
        return;
    }
    const uint64_t version = mRunShown->version();
    const bool finishedRun = mRunShown->isFinished();
    if (version != mShownVersion)
    {
        mShownVersion = version;
        std::vector<batch::FileResult> rs = mRunShown->results();
        for (const batch::FileResult& r : rs)
            for (auto it = r.results.begin(); it != r.results.end(); ++it)
                if (std::find(mResultKeys.begin(), mResultKeys.end(), it.key()) == mResultKeys.end())
                {
                    mResultKeys.push_back(it.key());
                    mTable->setColumnCount(kFirstResult + int(mResultKeys.size()));
                    mTable->headerItem()->setText(kFirstResult + int(mResultKeys.size()) - 1, qs(it.key()));
                }
        mTable->setSortingEnabled(false);
        for (int row = 0; row < mTable->topLevelItemCount(); ++row)
        {
            QTreeWidgetItem* it = mTable->topLevelItem(row);
            const size_t i = size_t(it->data(kIndex, Qt::UserRole).toInt());
            if (i >= rs.size())
                continue;
            const batch::FileResult& r = rs[i];
            it->setIcon(kStatus, icon(statusIcon(r.status)));
            it->setText(kStatus, batch::statusName(r.status));
            it->setData(kTime, Qt::DisplayRole, r.seconds > 0 ? QString::number(r.seconds, 'f', 1) : QString());
            QString message = qs(r.message);
            if (!r.errorFile.empty())
                message += QString("  (%1:%2)").arg(QFileInfo(qs(r.errorFile)).fileName()).arg(r.errorLine);
            it->setText(kMessage, message);
            it->setToolTip(kMessage, r.traceback.empty() ? qs(r.message) : qs(r.traceback));
            for (size_t k = 0; k < mResultKeys.size(); ++k)
            {
                auto v = r.results.find(mResultKeys[k]);
                QString cell = v == r.results.end() ? QString() : v->is_string() ? qs(v->get<std::string>()) : qs(v->dump());
                it->setText(kFirstResult + int(k), cell);
            }
        }
        mTable->setSortingEnabled(true);

        json s = mRunShown->summary(false);
        const json& counts = s["counts"];
        const int total = s["total"].get<int>();
        const int doneCount = total - counts["pending"].get<int>() - counts["running"].get<int>();
        mProgress->setRange(0, std::max(1, total));
        mProgress->setValue(doneCount);
        QStringList parts;
        for (const char* k : {"running", "done", "failed", "skipped", "cancelled", "planned"})
            if (counts[k].get<int>() > 0)
                parts << QString("%1 %2").arg(counts[k].get<int>()).arg(k);
        mSummary->setText(QString("%1/%2  %3").arg(doneCount).arg(total).arg(parts.join(", ")));
        if (QTreeWidgetItem* cur = mTable->currentItem())
            showLog(cur);
    }
    if (finishedRun)
        mPoll->stop();
    updateButtons();
}

void BatchRunPanel::showLog(QTreeWidgetItem* item)
{
    if (!item || !mRunShown)
    {
        mLog->clear();
        return;
    }
    const size_t i = size_t(item->data(kIndex, Qt::UserRole).toInt());
    batch::FileResult r = mRunShown->result(i);
    QString text = r.log.empty() ? QString() : qs(readText(r.log));
    if (text.isEmpty())
        text = r.status == batch::Status::Planned || r.status == batch::Status::Pending ? qs(r.message) : qs(r.message + "\n" + r.traceback);

    static const QRegularExpression trace(R"re(File "([^"]+)", line (\d+))re");
    QString html = "<pre style=\"margin:0\">";
    for (const QString& line : text.split('\n'))
    {
        QRegularExpressionMatch m = trace.match(line);
        QString escaped = line.toHtmlEscaped();
        if (m.hasMatch() && !m.captured(1).startsWith('<'))
            escaped = QString("<a href=\"trace:%1|%2\">%3</a>").arg(m.captured(1).toHtmlEscaped(), m.captured(2), escaped);
        html += escaped + "\n";
    }
    html += "</pre>";
    const int scroll = mLog->verticalScrollBar() ? mLog->verticalScrollBar()->value() : 0;
    const bool atEnd = mLog->verticalScrollBar() && scroll >= mLog->verticalScrollBar()->maximum() - 4;
    mLog->setHtml(html);
    if (mLog->verticalScrollBar())
        mLog->verticalScrollBar()->setValue(atEnd && r.status == batch::Status::Running ? mLog->verticalScrollBar()->maximum() : scroll);
}

void BatchRunPanel::openTrace(const QString& file, int line)
{
    if (QFileInfo::exists(file))
    {
        panels::show(kScriptEditorDock);
        if (ScriptEditor* e = scriptEditor())
            e->openFile(file, line > 0 ? line - 1 : -1);
        return;
    }
    // A snippet has no file: put the cursor on the line in the snippet itself.
    if (mSource->currentIndex() == kFromSnippet && line > 0)
    {
        mTabs->setCurrentIndex(0);
        QTextCursor c(mSnippet->document()->findBlockByNumber(line - 1));
        mSnippet->setTextCursor(c);
        mSnippet->setFocus();
    }
}

void BatchRunPanel::rowMenu(const QPoint& at)
{
    QTreeWidgetItem* item = mTable->itemAt(at);
    if (!item || !mRunShown)
        return;
    const size_t i = size_t(item->data(kIndex, Qt::UserRole).toInt());
    const batch::Item file = mRunShown->items()[i];
    const batch::FileResult r = mRunShown->result(i);
    std::vector<size_t> selected;
    for (QTreeWidgetItem* s : mTable->selectedItems())
        selected.push_back(size_t(s->data(kIndex, Qt::UserRole).toInt()));

    QMenu menu(this);
    QAction* openError = menu.addAction(icon("file-code"), "Open Script at Error", this, [this, r] { openTrace(qs(r.errorFile), r.errorLine); });
    openError->setEnabled(!r.errorFile.empty());
    QAction* openLog = menu.addAction("Open Log", this, [r] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(r.log))); });
    openLog->setEnabled(!r.log.empty());
    menu.addAction(icon("folder-open"), "Show Source Folder", this,
                   [file] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(paths::toUtf8(file.path.parent_path())))); });
    QAction* showOut = menu.addAction("Show Output Folder", this,
                                      [file] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(paths::toUtf8(file.output.parent_path())))); });
    showOut->setEnabled(!file.output.empty());
    menu.addAction("Copy Path", this, [file] { QApplication::clipboard()->setText(qs(paths::toUtf8(file.path))); });
    menu.addSeparator();
    QAction* retrySelected = menu.addAction(icon("refresh"), "Retry Selected", this, [this, selected] { retry(selected); });
    retrySelected->setEnabled(mRunShown->isFinished());
    menu.exec(mTable->viewport()->mapToGlobal(at));
}

void BatchRunPanel::updateButtons()
{
    const bool running = mRunShown && !mRunShown->isFinished();
    mRun->setEnabled(!running);
    mPreview->setEnabled(!running);
    mCancel->setEnabled(running);
    mRetry->setEnabled(mRunShown && !running && !mRunShown->spec().dryRun && !mRunShown->unfinishedOrFailed().empty());
    mCsv->setEnabled(mRunShown && !running);
    mLogs->setEnabled(mRunShown != nullptr);
}

void BatchRunPanel::refreshPresets()
{
    const QString current = mPresets->currentText();
    mPresets->clear();
    mPresets->addItem("(none)");
    json all = PrefStore::get().value(kPrefs, "/presets", json::object());
    if (all.is_object())
        for (auto it = all.begin(); it != all.end(); ++it)
            mPresets->addItem(qs(it.key()));
    if (int i = mPresets->findText(current); i >= 0)
        mPresets->setCurrentIndex(i);
}

void BatchRunPanel::savePreset()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "Save Preset", "Preset name", QLineEdit::Normal,
                                         mPresets->currentIndex() > 0 ? mPresets->currentText() : QString(), &ok)
                       .trimmed();
    if (!ok || name.isEmpty())
        return;
    run("batchRunPreset", {{"args", {ss(name)}}, {"spec", formState()}});
    refreshPresets();
    mPresets->setCurrentIndex(std::max(0, mPresets->findText(name)));
}

void BatchRunPanel::deletePreset()
{
    if (mPresets->currentIndex() <= 0)
        return;
    const QString name = mPresets->currentText();
    if (QMessageBox::question(this, "Delete Preset", "Delete the preset '" + name + "'?") != QMessageBox::Yes)
        return;
    run("batchRunPreset", {{"args", {ss(name)}}, {"delete", true}});
    refreshPresets();
}

void BatchRunPanel::fillFileListMenu()
{
    QMenu* m = mFileLists->menu();
    m->clear();
    m->addAction(icon("device-floppy"), "Save List As...", this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, "Save File List", "List name", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        json files = json::array();
        for (int i = 0; i < mInputs->count(); ++i)
            files.push_back(ss(mInputs->item(i)->text()));
        run("batchRunFileList", {{"args", {ss(name)}}, {"files", files}});
    });
    json lists = PrefStore::get().value(kPrefs, "/fileLists", json::object());
    if (!lists.is_object() || lists.empty())
        return;
    m->addSeparator();
    QMenu* remove = nullptr;
    for (auto it = lists.begin(); it != lists.end(); ++it)
    {
        const std::string name = it.key();
        const json files = it.value();
        QAction* load = m->addAction(qs(name), this, [this, files] {
            mInputs->clear();
            QStringList paths;
            for (const json& f : files)
                if (f.is_string())
                    paths << qs(f.get<std::string>());
            addInputs(paths);
        });
        load->setToolTip(QString("Replace the entries with this list (%1)").arg(files.size()));
        if (!remove)
        {
            m->addSeparator();
            remove = new QMenu("Delete List", m);
        }
        remove->addAction(qs(name), this, [name] { run("batchRunFileList", {{"args", {name}}, {"delete", true}}); });
    }
    if (remove)
        m->addMenu(remove);
}

void registerBatchRunPanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    batch::setInteractive(true);
    panels::add({.id = kBatchRunDock,
                 .title = "Batch Script Execution",
                 .create = [](QWidget* p) -> QWidget* { return new BatchRunPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "files",
                 .open = false});
    actions::add({.id = "window.batchRun",
                  .label = "Batch Script Execution...",
                  .category = "File",
                  .run = [] { panels::toggle(kBatchRunDock); },
                  .doc = "Run a Python script over many scene files in background workers: pick files, folders or globs, "
                         "save in place or to a folder, and collect each file's results into a table.",
                  .python = "lila.ui.run_action(\"window.batchRun\")"});
}
} // namespace lila::ui
