#include "ui/editor/ScriptEditor.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "python/Python.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/FindBar.h"
#include "ui/editor/Interpreter.h"
#include "ui/editor/LspClient.h"
#include "ui/editor/OutputView.h"
#include "ui/editor/TextUtil.h"
#include "ui/editor/Tools.h"
#include "ui/editor/VimBinding.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QSaveFile>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
constexpr const char* kPrefs = "scriptEditor";
constexpr const char* kSceneProperty = "lilaSceneScript";
using storage::Location;

bool vimEnabled()
{
    return PrefStore::get().value<bool>(kPrefs, "/vim", false);
}

VimBinding* vimOf(CodeEditor* editor)
{
    return editor ? editor->findChild<VimBinding*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

bool samePath(const QString& a, const QString& b)
{
#ifdef _WIN32
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
}

QString readText(const QString& path, bool* ok)
{
    QFile f(path);
    *ok = f.open(QIODevice::ReadOnly);
    if (!*ok)
        return {};
    QString s = QString::fromUtf8(f.readAll());
    s.replace("\r\n", "\n");
    return s;
}
} // namespace

ScriptEditor::ScriptEditor(QWidget* parent) : QWidget(parent)
{
    mFont = QFont("JetBrains Mono");
    mFont.setStyleHint(QFont::Monospace);
    mFont.setPointSizeF(9.5);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QToolBar(this);
    bar->setIconSize(QSize(14, 14));
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto add = [&](const QString& text, const QString& tip, auto fn) {
        QAction* a = bar->addAction(text);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    add("New", "New tab (Ctrl+N)", [this] { newTab(); });
    auto addMenuButton = [&](const QString& text, const QString& tip, auto fn, void (ScriptEditor::*fill)(QMenu*)) {
        auto* button = new QToolButton(bar);
        button->setText(text);
        button->setToolTip(tip);
        button->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new QMenu(button);
        menu->setToolTipsVisible(true);
        connect(menu, &QMenu::aboutToShow, this, [this, menu, fill] {
            menu->clear();
            (this->*fill)(menu);
        });
        button->setMenu(menu);
        connect(button, &QToolButton::clicked, this, fn);
        bar->addWidget(button);
    };
    addMenuButton("Open", "Open file (Ctrl+O); the arrow lists scene, project and global scripts", [this] { openFileDialog(); },
                  &ScriptEditor::fillOpenMenu);
    addMenuButton("Save", "Save (Ctrl+S, Ctrl+Shift+S to save as); the arrow picks where", [this] { saveTab(mTabs->currentIndex()); },
                  &ScriptEditor::fillSaveMenu);
    bar->addSeparator();
    add("Run", "Run selection or tab (Ctrl+Enter, numpad Enter)", [this] { runCurrent(false); });
    add("Run Cell", "Run the # %% cell under the cursor (Shift+Enter)", [this] { runCurrent(true); });
    bar->addSeparator();
    add("Format", "Format with ruff (Ctrl+Alt+L)", [this] {
        if (CodeEditor* e = currentEditor())
            e->formatDocument();
    });
    add("Clear Output", "Clear the output pane", [this] { mOutput->clear(); });
    layout->addWidget(bar);

    mSplitter = new QSplitter(Qt::Vertical, this);
    mSplitter->setChildrenCollapsible(false);

    auto* top = new QWidget(mSplitter);
    auto* topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);
    mTabs = new QTabWidget(top);
    mTabs->setDocumentMode(true);
    mTabs->setMovable(true);
    mTabs->setUsesScrollButtons(true);
    auto* plus = new QToolButton(mTabs);
    plus->setText("+");
    plus->setAutoRaise(true);
    plus->setToolTip("New tab");
    connect(plus, &QToolButton::clicked, this, [this] { newTab(); });
    mTabs->setCornerWidget(plus, Qt::TopRightCorner);
    topLayout->addWidget(mTabs, 1);
    mFind = new FindBar(top);
    topLayout->addWidget(mFind);

    mOutput = new OutputView(mSplitter);
    mSplitter->addWidget(top);
    mSplitter->addWidget(mOutput);
    mSplitter->setStretchFactor(0, 3);
    mSplitter->setStretchFactor(1, 1);
    layout->addWidget(mSplitter, 1);

    auto* status = new QWidget(this);
    auto* statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(6, 2, 6, 2);
    mVimMode = new QLabel(status);
    mVimMode->setMinimumWidth(64);
    mVimMode->setStyleSheet("font-weight: 600;");
    mVimMode->hide();
    mStatus = new QLabel(status);
    mToolStatus = new QLabel(status);
    mToolStatus->setStyleSheet("color: #8c8c8c;");
    statusLayout->addWidget(mVimMode);
    statusLayout->addWidget(mStatus, 1);
    statusLayout->addWidget(mToolStatus);
    layout->addWidget(status);

    mLsp = new LspClient(this);
    connect(mLsp, &LspClient::ready, this, [this] {
        mLspRestarts = 0;
        setToolStatus("basedpyright ready");
    });
    connect(mLsp, &LspClient::stopped, this, [this](const QString& reason) {
        log::warn("script editor: {}", reason.toStdString());
        if (mLspRestarts++ < 3)
        {
            setToolStatus("Language server stopped, restarting...");
            QTimer::singleShot(2000, this, &ScriptEditor::startLanguageServer);
        }
        else
            setToolStatus("Language server unavailable");
    });

    connect(mTabs, &QTabWidget::tabCloseRequested, this, &ScriptEditor::closeTab);
    connect(mTabs, &QTabWidget::currentChanged, this, [this] {
        mFind->setEditor(currentEditor());
        updateStatus();
        updateVimStatus();
        scheduleSessionSave();
    });
    connect(mTabs->tabBar(), &QTabBar::tabMoved, this, &ScriptEditor::scheduleSessionSave);
    connect(mOutput, &OutputView::traceClicked, this, &ScriptEditor::jumpToTrace);

    mSessionTimer.setSingleShot(true);
    mSessionTimer.setInterval(1500);
    connect(&mSessionTimer, &QTimer::timeout, this, &ScriptEditor::saveSession);

    mScriptsListener = scene::scripts::addListener([this] { onSceneScriptsChanged(); });
    scene::scripts::setOpenHandler([this](const std::vector<scene::scripts::Script>& list) {
        // After the load has finished, not from inside it.
        QTimer::singleShot(0, this, [this, list] { runOpenedSceneScripts(list); });
    });
    mPrefsListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == kPrefs)
            applyVim();
    });

    restoreSession();
    if (mTabs->count() == 0)
        newTab();
    applyVim();
}

ScriptEditor::~ScriptEditor()
{
    scene::scripts::removeListener(mScriptsListener);
    scene::scripts::setOpenHandler(nullptr);
    PrefStore::get().removeOnChanged(mPrefsListener);
    saveSession();
    // Editors close their LSP documents on destruction; take them down while the client still exists.
    while (mTabs->count() > 0)
    {
        QWidget* w = mTabs->widget(0);
        mTabs->removeTab(0);
        delete w;
    }
}

void ScriptEditor::setIconProvider(std::function<QIcon(const QString&)> fn)
{
    mIconProvider = std::move(fn);
    for (int i = 0; i < mTabs->count(); ++i)
        updateTabTitle(editorAt(i));
}

void ScriptEditor::setEditorFont(const QFont& font)
{
    mFont = font;
    for (int i = 0; i < mTabs->count(); ++i)
        editorAt(i)->setEditorFont(font);
    mOutput->setEditorFont(font);
}

CodeEditor* ScriptEditor::currentEditor() const
{
    return qobject_cast<CodeEditor*>(mTabs->currentWidget());
}

CodeEditor* ScriptEditor::editorAt(int index) const
{
    return qobject_cast<CodeEditor*>(mTabs->widget(index));
}

QString ScriptEditor::nextUntitledName()
{
    for (int n = 1;; ++n)
    {
        QString name = QString("Untitled-%1").arg(n);
        bool used = false;
        for (int i = 0; i < mTabs->count() && !used; ++i)
            used = editorAt(i)->filePath().isEmpty() && editorAt(i)->untitledName() == name;
        if (!used)
            return name;
    }
}

CodeEditor* ScriptEditor::addEditor(const QString& path, const QString& text, bool modified)
{
    auto* editor = new CodeEditor(mTabs);
    editor->setEditorFont(mFont);
    if (path.isEmpty())
        editor->setUntitledName(nextUntitledName());
    editor->setPlainText(text);
    editor->document()->setModified(modified);
    editor->setFilePath(path);
    editor->setRuffExe(mRuffExe);
    editor->setLanguageClient(mLsp);
    wire(editor);
    if (vimEnabled())
        attachVim(editor);
    int index = mTabs->addTab(editor, editor->displayName());
    // Our own close button: the style's default is tinted, and the app stays grayscale.
    auto* close = new QToolButton(mTabs->tabBar());
    close->setText(QString(QChar(0x00D7)));
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setFixedSize(16, 16);
    close->setToolTip("Close (Ctrl+W)");
    close->setStyleSheet("QToolButton { border: none; color: #8a8a8a; padding: 0; }"
                         "QToolButton:hover { color: #f0f0f0; background: #5a5a5a; border-radius: 2px; }");
    connect(close, &QToolButton::clicked, this, [this, editor] { closeTab(mTabs->indexOf(editor)); });
    mTabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    mTabs->setTabToolTip(index, path.isEmpty() ? editor->untitledName() : QDir::toNativeSeparators(path));
    updateTabTitle(editor);
    mTabs->setCurrentIndex(index);
    editor->setFocus();
    scheduleSessionSave();
    return editor;
}

void ScriptEditor::wire(CodeEditor* editor)
{
    editor->hostKeyHandler = [this](QKeyEvent* e) { return hostKey(e); };
    connect(editor, &CodeEditor::runRequested, this, [this] { runCurrent(false); });
    connect(editor, &CodeEditor::runCellRequested, this, [this] { runCurrent(true); });
    connect(editor, &CodeEditor::findRequested, this, [this, editor](bool replace) {
        mFind->setEditor(editor);
        mFind->activate(replace);
    });
    connect(editor, &CodeEditor::showDocs, this, &ScriptEditor::showDocs);
    connect(editor, &CodeEditor::openLocation, this, [this](const QString& path, int line, int col) { openFile(path, line, col); });
    connect(editor, &CodeEditor::statusMessage, this, [this](const QString& text) {
        mStatus->setText(text);
        QTimer::singleShot(4000, this, &ScriptEditor::updateStatus);
    });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, &ScriptEditor::updateStatus);
    connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor] { updateTabTitle(editor); });
    connect(editor->document(), &QTextDocument::contentsChanged, this, &ScriptEditor::scheduleSessionSave);
}

void ScriptEditor::updateTabTitle(CodeEditor* editor)
{
    int index = mTabs->indexOf(editor);
    if (index < 0)
        return;
    mTabs->setTabText(index, editor->displayName() + (editor->document()->isModified() ? " *" : ""));

    const Location loc = locationOf(editor);
    const QString path = QDir::toNativeSeparators(editor->filePath());
    QString tip;
    switch (loc)
    {
    case Location::Unsaved: tip = editor->untitledName() + " (not saved)"; break;
    case Location::Scene:
    {
        const QString name = sceneName(editor);
        const scene::scripts::Script* s = scene::scripts::find(name.toStdString());
        tip = QString("Scene script '%1', stored in the scene file").arg(name);
        if (!s)
            tip += " (not in the open scene; saving adds it)";
        else if (s->runOnOpen)
            tip += ", runs when the scene opens";
        break;
    }
    case Location::Project: tip = "Project script: " + path; break;
    case Location::Global: tip = "Global script: " + path; break;
    case Location::File: tip = path; break;
    }
    mTabs->setTabToolTip(index, tip);
    if (mIconProvider)
        mTabs->setTabIcon(index, mIconProvider(storage::iconName(loc)));
}

void ScriptEditor::updateStatus()
{
    CodeEditor* e = currentEditor();
    if (!e)
    {
        mStatus->clear();
        return;
    }
    QTextCursor c = e->textCursor();
    QString s = QString("Ln %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1);
    if (c.hasSelection())
        s += QString("  (%1 selected)").arg(c.selectionEnd() - c.selectionStart());
    int errors = 0, warnings = 0;
    for (const Diagnostic& d : e->diagnostics())
    {
        if (d.severity == Severity::Error)
            ++errors;
        else if (d.severity == Severity::Warning)
            ++warnings;
    }
    if (errors || warnings)
        s += QString("    %1 errors, %2 warnings").arg(errors).arg(warnings);
    mStatus->setText(s);
}

void ScriptEditor::setToolStatus(const QString& text)
{
    mToolStatus->setText(text);
}

CodeEditor* ScriptEditor::newTab()
{
    return addEditor(QString(), QString(), false);
}

CodeEditor* ScriptEditor::openFile(const QString& path, int line, int col)
{
    LILA_ZONE();

    QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    CodeEditor* editor = nullptr;
    for (int i = 0; i < mTabs->count(); ++i)
        if (samePath(editorAt(i)->filePath(), clean))
        {
            editor = editorAt(i);
            mTabs->setCurrentIndex(i);
            break;
        }
    if (!editor)
    {
        bool ok = false;
        QString text = readText(clean, &ok);
        if (!ok)
        {
            log::error("script editor: cannot open {}", clean.toStdString());
            return nullptr;
        }
        // Replace a lone empty untitled tab instead of stacking another next to it.
        if (mTabs->count() == 1 && editorAt(0)->filePath().isEmpty() && editorAt(0)->document()->isEmpty())
        {
            QWidget* w = mTabs->widget(0);
            mTabs->removeTab(0);
            delete w;
        }
        editor = addEditor(clean, text, false);
        mLastDir = QFileInfo(clean).absolutePath();
    }
    if (line >= 0)
        editor->goToLine(line, col);
    editor->setFocus();
    return editor;
}

void ScriptEditor::openFileDialog()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Open Script", mLastDir, "Python (*.py *.pyi);;All files (*)");
    for (const QString& f : files)
        openFile(f);
}

bool ScriptEditor::saveTab(int index, bool saveAs)
{
    CodeEditor* editor = editorAt(index);
    if (!editor)
        return false;
    const Location loc = locationOf(editor);
    if (saveAs || loc == Location::Unsaved)
        return saveTabAs(index, loc);
    if (loc == Location::Scene)
    {
        const scene::scripts::Script* s = scene::scripts::find(sceneName(editor).toStdString());
        return saveToScene(editor, sceneName(editor), s && s->runOnOpen);
    }
    return saveToFile(editor, editor->filePath());
}

bool ScriptEditor::saveTabAs(int index, Location preset)
{
    CodeEditor* editor = editorAt(index);
    if (!editor)
        return false;
    const QString projectDir = storage::projectDir();
    if (preset == Location::Unsaved)
    {
        const QString last = QString::fromStdString(PrefStore::get().value<std::string>(kPrefs, "/saveLocation", "File"));
        preset = Location::File;
        for (Location loc : {Location::Scene, Location::Project, Location::File, Location::Global})
            if (storage::locationName(loc) == last)
                preset = loc;
    }
    if (preset == Location::Project && projectDir.isEmpty())
        preset = Location::File;

    QString name = sceneName(editor);
    if (name.isEmpty())
        name = editor->filePath().isEmpty() ? editor->untitledName() : QFileInfo(editor->filePath()).completeBaseName();
    const scene::scripts::Script* current = scene::scripts::find(sceneName(editor).toStdString());

    QDialog dialog(this);
    dialog.setWindowTitle("Save Script");
    auto* form = new QFormLayout(&dialog);
    auto* where = new QComboBox(&dialog);
    for (Location loc : {Location::Scene, Location::Project, Location::File, Location::Global})
        where->addItem(mIconProvider ? mIconProvider(storage::iconName(loc)) : QIcon(), storage::locationName(loc), int(loc));
    if (projectDir.isEmpty())
        if (auto* model = qobject_cast<QStandardItemModel*>(where->model()))
        {
            model->item(1)->setEnabled(false);
            model->item(1)->setToolTip("No workspace is open");
        }
    where->setCurrentIndex(where->findData(int(preset)));
    auto* nameEdit = new QLineEdit(name, &dialog);
    auto* runOnOpen = new QCheckBox("Run when the scene opens", &dialog);
    runOnOpen->setChecked(current && current->runOnOpen);
    auto* note = new QLabel(&dialog);
    note->setWordWrap(true);
    note->setStyleSheet("color: #8c8c8c;");
    form->addRow("Save to", where);
    form->addRow("Name", nameEdit);
    form->addRow(QString(), runOnOpen);
    form->addRow(QString(), note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto update = [&] {
        const Location loc = Location(where->currentData().toInt());
        nameEdit->setEnabled(loc != Location::File);
        runOnOpen->setVisible(loc == Location::Scene);
        switch (loc)
        {
        case Location::Scene:
            note->setText("Stored inside the scene file and loaded with it. It only runs when you run it, unless it is "
                          "set to run when the scene opens.");
            break;
        case Location::Project: note->setText("Saved in " + QDir::toNativeSeparators(projectDir)); break;
        case Location::Global: note->setText("Saved in " + QDir::toNativeSeparators(storage::globalDir()) + ", for every workspace."); break;
        default: note->setText("Choose the file next."); break;
        }
    };
    connect(where, &QComboBox::currentIndexChanged, &dialog, update);
    update();
    if (dialog.exec() != QDialog::Accepted)
        return false;

    const Location loc = Location(where->currentData().toInt());
    PrefStore::get().set(kPrefs, "/saveLocation", storage::locationName(loc).toStdString());
    const QString clean = storage::sanitizeName(nameEdit->text());
    if (loc != Location::File && clean.isEmpty())
    {
        QMessageBox::warning(this, "Save Script", "The script needs a name.");
        return false;
    }
    switch (loc)
    {
    case Location::Scene:
    {
        const scene::scripts::Script* other = scene::scripts::find(clean.toStdString());
        if (other && clean != sceneName(editor) &&
            QMessageBox::question(this, "Save Script", "The scene already has a script named '" + clean + "'. Replace it?") != QMessageBox::Yes)
            return false;
        bool run = runOnOpen->isChecked();
        if (run && !(other && other->runOnOpen) && !confirmRunOnOpen())
            run = false;
        return saveToScene(editor, clean, run);
    }
    case Location::Project:
    case Location::Global:
    {
        const QString dir = loc == Location::Project ? projectDir : storage::globalDir();
        QDir().mkpath(dir);
        const QString path = QDir(dir).filePath(clean + ".py");
        if (QFileInfo::exists(path) && !samePath(path, editor->filePath()) &&
            QMessageBox::question(this, "Save Script", QDir::toNativeSeparators(path) + " exists. Replace it?") != QMessageBox::Yes)
            return false;
        return saveToFile(editor, path);
    }
    default:
    {
        QString start = editor->filePath().isEmpty() ? QDir(mLastDir).filePath(name + ".py") : editor->filePath();
        QString path = QFileDialog::getSaveFileName(this, "Save Script", start, "Python (*.py);;All files (*)");
        if (path.isEmpty())
            return false;
        return saveToFile(editor, path);
    }
    }
}

bool ScriptEditor::saveToScene(CodeEditor* editor, const QString& name, bool runOnOpen)
{
    if (PrefStore::get().value<bool>(kPrefs, "/formatOnSave", false))
        editor->formatDocument();
    try
    {
        ops::execute("sceneScript", {{"args", {name.toStdString()}}, {"text", editor->toPlainText().toStdString()}, {"runOnOpen", runOnOpen}});
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "Save Script", QString::fromStdString(e.what()));
        return false;
    }
    setSceneName(editor, name);
    editor->setFilePath(QString());
    editor->setUntitledName(name);
    editor->document()->setModified(false);
    updateTabTitle(editor);
    scheduleSessionSave();
    return true;
}

bool ScriptEditor::saveToFile(CodeEditor* editor, const QString& path)
{
    if (PrefStore::get().value<bool>(kPrefs, "/formatOnSave", false))
        editor->formatDocument();

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
    {
        QMessageBox::warning(this, "Save Script", "Cannot write " + QDir::toNativeSeparators(path));
        return false;
    }
    f.write(editor->toPlainText().toUtf8());
    if (!f.commit())
    {
        QMessageBox::warning(this, "Save Script", "Cannot write " + QDir::toNativeSeparators(path));
        return false;
    }
    setSceneName(editor, QString());
    editor->setFilePath(path);
    editor->document()->setModified(false);
    updateTabTitle(editor);
    if (storage::locationOfPath(path) == Location::File)
        mLastDir = QFileInfo(path).absolutePath();
    scheduleSessionSave();
    return true;
}

QString ScriptEditor::sceneName(CodeEditor* editor) const
{
    return editor ? editor->property(kSceneProperty).toString() : QString();
}

void ScriptEditor::setSceneName(CodeEditor* editor, const QString& name)
{
    editor->setProperty(kSceneProperty, name.isEmpty() ? QVariant() : QVariant(name));
}

Location ScriptEditor::locationOf(CodeEditor* editor) const
{
    if (!sceneName(editor).isEmpty())
        return Location::Scene;
    return storage::locationOfPath(editor->filePath());
}

bool ScriptEditor::confirmRunOnOpen()
{
    return QMessageBox::warning(this, "Run When the Scene Opens",
                                "A script set to run on open is Python stored in the scene file. Whoever opens the scene is "
                                "asked whether to run it, and it can do anything Lila can, including changing files.\n\n"
                                "Set it to run when the scene opens?",
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

void ScriptEditor::setRunOnOpen(CodeEditor* editor, bool on)
{
    const QString name = sceneName(editor);
    if (name.isEmpty() || (on && !confirmRunOnOpen()))
        return;
    try
    {
        ops::execute("sceneScript", {{"args", {name.toStdString()}}, {"runOnOpen", on}});
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "Run When the Scene Opens", QString::fromStdString(e.what()));
    }
    updateTabTitle(editor);
}

void ScriptEditor::removeFromScene(CodeEditor* editor)
{
    const QString name = sceneName(editor);
    if (name.isEmpty())
        return;
    // The tab keeps the text as an unsaved buffer.
    setSceneName(editor, QString());
    editor->document()->setModified(true);
    try
    {
        ops::execute("sceneScript", {{"args", {name.toStdString()}}, {"remove", true}});
    }
    catch (const std::exception& e)
    {
        log::warn("script editor: {}", e.what());
    }
    updateTabTitle(editor);
    scheduleSessionSave();
}

CodeEditor* ScriptEditor::openSceneScript(const QString& name)
{
    for (int i = 0; i < mTabs->count(); ++i)
        if (sceneName(editorAt(i)) == name)
        {
            mTabs->setCurrentIndex(i);
            editorAt(i)->setFocus();
            return editorAt(i);
        }
    const scene::scripts::Script* s = scene::scripts::find(name.toStdString());
    if (!s)
        return nullptr;
    if (mTabs->count() == 1 && editorAt(0)->filePath().isEmpty() && sceneName(editorAt(0)).isEmpty() && editorAt(0)->document()->isEmpty())
    {
        QWidget* w = mTabs->widget(0);
        mTabs->removeTab(0);
        delete w;
    }
    CodeEditor* editor = addEditor(QString(), QString::fromStdString(s->text), false);
    setSceneName(editor, name);
    editor->setUntitledName(name);
    updateTabTitle(editor);
    return editor;
}

void ScriptEditor::onSceneScriptsChanged()
{
    bool removed = false;
    for (int i = mTabs->count() - 1; i >= 0; --i)
    {
        CodeEditor* e = editorAt(i);
        const QString name = sceneName(e);
        if (name.isEmpty())
            continue;
        const scene::scripts::Script* s = scene::scripts::find(name.toStdString());
        if (s && !e->document()->isModified())
        {
            const QString text = QString::fromStdString(s->text);
            if (text != e->toPlainText())
            {
                e->replaceAllText(text);
                e->document()->setModified(false);
            }
        }
        else if (!s && !e->document()->isModified())
        {
            // A clean tab of a script the scene no longer has: another scene opened, or the add was undone.
            if (mFind->isVisible())
                mFind->setEditor(nullptr);
            mTabs->removeTab(i);
            e->deleteLater();
            removed = true;
            continue;
        }
        updateTabTitle(e);
    }
    if (removed)
    {
        if (mTabs->count() == 0)
            newTab();
        mFind->setEditor(currentEditor());
        scheduleSessionSave();
    }
}

void ScriptEditor::runOpenedSceneScripts(const std::vector<scene::scripts::Script>& list)
{
    QStringList names;
    for (const auto& s : list)
        names << QString::fromStdString(s.name);
    if (PrefStore::get().value<std::string>(kPrefs, "/sceneScriptsOnOpen", "ask") == "never")
    {
        log::info("scene scripts set to run on open were not run: {}", names.join(", ").toStdString());
        return;
    }
    auto answer = QMessageBox::warning(window(), "Scene Scripts",
                                       QString("This scene has scripts set to run when it opens:\n\n    %1\n\nThey can do anything "
                                               "Lila can, including changing files. Run them only if you trust where the "
                                               "scene came from.")
                                           .arg(names.join("\n    ")),
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
    {
        log::info("scene scripts not run: {}", names.join(", ").toStdString());
        return;
    }
    if (!python::initialized())
    {
        log::error("Python is not available");
        return;
    }
    for (const auto& s : list)
    {
        const QString filename = "<scene:" + QString::fromStdString(s.name) + ">";
        interp::registerSource(filename, QString::fromStdString(s.text));
        python::run(s.text, filename.toStdString());
    }
}

void ScriptEditor::fillSaveMenu(QMenu* menu)
{
    CodeEditor* editor = currentEditor();
    if (!editor)
        return;
    menu->addAction("Save", this, [this] { saveTab(mTabs->currentIndex()); });
    menu->addAction("Save As...", this, [this] { saveTab(mTabs->currentIndex(), true); });
    menu->addSeparator();
    const bool haveProject = !storage::projectDir().isEmpty();
    for (Location loc : {Location::Scene, Location::Project, Location::File, Location::Global})
    {
        QAction* a = menu->addAction(mIconProvider ? mIconProvider(storage::iconName(loc)) : QIcon(),
                                     "Save to " + storage::locationName(loc) + "...", this,
                                     [this, loc] { saveTabAs(mTabs->currentIndex(), loc); });
        if (loc == Location::Project && !haveProject)
        {
            a->setEnabled(false);
            a->setToolTip("No workspace is open");
        }
    }
    menu->addSeparator();
    const scene::scripts::Script* s = scene::scripts::find(sceneName(editor).toStdString());
    QAction* run = menu->addAction("Run When the Scene Opens");
    run->setCheckable(true);
    run->setChecked(s && s->runOnOpen);
    run->setEnabled(s != nullptr);
    run->setToolTip("Offer to run this scene script whenever the scene is opened");
    connect(run, &QAction::toggled, this, [this, editor](bool on) { setRunOnOpen(editor, on); });
    QAction* remove = menu->addAction("Remove from Scene", this, [this, editor] { removeFromScene(editor); });
    remove->setEnabled(s != nullptr);
}

void ScriptEditor::fillOpenMenu(QMenu* menu)
{
    menu->addAction("Open File...", this, [this] { openFileDialog(); });
    menu->addSeparator();
    auto section = [&](Location loc, const QStringList& names, bool enabled, auto open) {
        QMenu* sub = menu->addMenu(mIconProvider ? mIconProvider(storage::iconName(loc)) : QIcon(), storage::locationName(loc) + " Scripts");
        sub->setEnabled(enabled);
        if (names.isEmpty())
            sub->addAction("None")->setEnabled(false);
        for (const QString& name : names)
            sub->addAction(name, this, [open, name] { open(name); });
    };
    QStringList sceneNames;
    for (const auto& s : scene::scripts::all())
        sceneNames << QString::fromStdString(s.name);
    section(Location::Scene, sceneNames, true, [this](const QString& name) { openSceneScript(name); });
    const QString project = storage::projectDir();
    section(Location::Project, storage::scriptsIn(project), !project.isEmpty(),
            [this, project](const QString& name) { openFile(QDir(project).filePath(name + ".py")); });
    const QString global = storage::globalDir();
    section(Location::Global, storage::scriptsIn(global), true,
            [this, global](const QString& name) { openFile(QDir(global).filePath(name + ".py")); });
}

void ScriptEditor::applyVim()
{
    const bool on = vimEnabled();
    for (int i = 0; i < mTabs->count(); ++i)
    {
        CodeEditor* e = editorAt(i);
        VimBinding* vim = vimOf(e);
        if (on && !vim)
            attachVim(e);
        else if (!on && vim)
            delete vim;
    }
    updateVimStatus();
}

void ScriptEditor::attachVim(CodeEditor* editor)
{
    auto* vim = new VimBinding(editor);
    connect(vim, &VimBinding::statusChanged, this, [this, editor] {
        if (editor == currentEditor())
            updateVimStatus();
    });
    connect(vim, &VimBinding::writeRequested, this, [this, editor] { saveTab(mTabs->indexOf(editor)); });
    // Queued: the tab must not be deleted while it is still handling the key.
    connect(vim, &VimBinding::closeRequested, this, [this, editor] { closeTab(mTabs->indexOf(editor)); }, Qt::QueuedConnection);
    connect(vim, &VimBinding::messageShown, this, [this](const QString& text) {
        mStatus->setText(text);
        QTimer::singleShot(4000, this, &ScriptEditor::updateStatus);
    });
}

void ScriptEditor::updateVimStatus()
{
    VimBinding* vim = vimOf(currentEditor());
    mVimMode->setVisible(vim != nullptr);
    if (vim)
        mVimMode->setText(vim->status());
}

bool ScriptEditor::closeTab(int index)
{
    CodeEditor* editor = editorAt(index);
    if (!editor)
        return false;
    bool worthKeeping = !editor->filePath().isEmpty() || !editor->document()->isEmpty();
    if (editor->document()->isModified() && worthKeeping)
    {
        mTabs->setCurrentIndex(index);
        auto answer = QMessageBox::question(this, "Close Tab", "Save changes to " + editor->displayName() + "?",
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save && !saveTab(index))
            return false;
    }
    if (mFind->isVisible())
        mFind->setEditor(nullptr);
    mTabs->removeTab(index);
    delete editor;
    if (mTabs->count() == 0)
        newTab();
    mFind->setEditor(currentEditor());
    scheduleSessionSave();
    return true;
}

bool ScriptEditor::hostKey(QKeyEvent* e)
{
    Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    int key = e->key();
    if (mods == Qt::ControlModifier)
    {
        switch (key)
        {
        case Qt::Key_S: saveTab(mTabs->currentIndex()); return true;
        case Qt::Key_N: newTab(); return true;
        case Qt::Key_O: openFileDialog(); return true;
        case Qt::Key_W:
        case Qt::Key_F4: closeTab(mTabs->currentIndex()); return true;
        case Qt::Key_Tab: mTabs->setCurrentIndex((mTabs->currentIndex() + 1) % mTabs->count()); return true;
        default: break;
        }
    }
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier))
    {
        if (key == Qt::Key_S)
        {
            saveTab(mTabs->currentIndex(), true);
            return true;
        }
        if (key == Qt::Key_Backtab || key == Qt::Key_Tab)
        {
            mTabs->setCurrentIndex((mTabs->currentIndex() + mTabs->count() - 1) % mTabs->count());
            return true;
        }
    }
    return false;
}

void ScriptEditor::runCurrent(bool cell)
{
    LILA_ZONE();

    CodeEditor* editor = currentEditor();
    if (!editor)
        return;
    int firstLine = 0;
    QString code = text::dedent(editor->codeToRun(cell, &firstLine));
    if (code.trimmed().isEmpty())
        return;
    if (!python::initialized())
    {
        log::error("Python is not available");
        return;
    }
    QString filename = editor->runFilename();
    interp::registerSource(filename, editor->toPlainText());

    QString echo = code;
    while (echo.endsWith('\n'))
        echo.chop(1);
    log::write(LogLevel::Echo, echo.toStdString() + "\n");
    // Leading newlines keep traceback line numbers equal to the tab's own.
    python::run((QString(firstLine, '\n') + code).toStdString(), filename.toStdString());
}

void ScriptEditor::jumpToTrace(const QString& file, int line)
{
    for (int i = 0; i < mTabs->count(); ++i)
    {
        CodeEditor* e = editorAt(i);
        if (e->runFilename() == file || (!e->filePath().isEmpty() && samePath(e->filePath(), file)))
        {
            mTabs->setCurrentIndex(i);
            e->goToLine(line - 1);
            return;
        }
    }
    if (QFileInfo(file).isFile())
        openFile(file, line - 1);
}

void ScriptEditor::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (mStarted)
        return;
    mStarted = true;
    Tools& tools = Tools::get();
    connect(&tools, &Tools::stateChanged, this, [this](Tools::State s, const QString& message) {
        setToolStatus(message);
        if (s == Tools::State::Ready)
            onToolsReady();
    });
    tools.ensure();
    setToolStatus(tools.message());
    if (tools.state() == Tools::State::Ready)
        onToolsReady();
}

void ScriptEditor::onToolsReady()
{
    Tools& tools = Tools::get();
    if (tools.hasRuff())
    {
        mRuffExe = tools.ruffExe();
        for (int i = 0; i < mTabs->count(); ++i)
            editorAt(i)->setRuffExe(mRuffExe);
    }
    if (tools.hasLangserver() && !mLsp->isReady())
        startLanguageServer();
}

void ScriptEditor::startLanguageServer()
{
    LILA_ZONE();

    Tools& tools = Tools::get();
    if (!tools.hasLangserver())
        return;
    QString pythonDir = QString::fromStdWString(paths::pythonDir().wstring());
    nlohmann::json analysis = {
        {"extraPaths", {QDir::cleanPath(pythonDir).toStdString()}},
        {"typeCheckingMode", "standard"},
        {"diagnosticMode", "openFilesOnly"},
        {"autoImportCompletions", true},
        {"useLibraryCodeForTypes", true},
    };
    nlohmann::json settings = {
        {"basedpyright", {{"analysis", analysis}}},
        {"python", {{"pythonPath", interp::basePython().toStdString()}, {"analysis", analysis}}},
    };
    setToolStatus("Starting basedpyright...");
    mLsp->start(tools.nodeExe(), {tools.langserverScript(), "--stdio"}, settings);
}

void ScriptEditor::scheduleSessionSave()
{
    if (!mRestoring)
        mSessionTimer.start();
}

void ScriptEditor::saveSession()
{
    LILA_ZONE();

    mSessionTimer.stop();
    nlohmann::json tabs = nlohmann::json::array();
    int current = 0;
    for (int i = 0; i < mTabs->count(); ++i)
    {
        CodeEditor* e = editorAt(i);
        const QString sceneScript = sceneName(e);
        if (e->filePath().isEmpty() && sceneScript.isEmpty() && e->document()->isEmpty())
            continue;
        bool modified = e->document()->isModified();
        QTextCursor c = e->textCursor();
        nlohmann::json t = {
            {"path", e->filePath().toStdString()},
            {"name", e->untitledName().toStdString()},
            {"modified", modified},
            {"line", c.blockNumber()},
            {"col", c.positionInBlock()},
        };
        // Saved files reload from disk; anything else keeps its buffer here so nothing typed is lost.
        if (e->filePath().isEmpty() || modified)
            t["text"] = e->toPlainText().toStdString();
        if (!sceneScript.isEmpty())
            t["scene"] = sceneScript.toStdString();
        if (i == mTabs->currentIndex())
            current = int(tabs.size());
        tabs.push_back(std::move(t));
    }
    PrefStore& prefs = PrefStore::get();
    prefs.set(kPrefs, "/session", {{"tabs", tabs}, {"current", current}, {"lastDir", mLastDir.toStdString()}});
    QList<int> sizes = mSplitter->sizes();
    if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0)
        prefs.set(kPrefs, "/splitter", {sizes[0], sizes[1]});
}

void ScriptEditor::restoreSession()
{
    LILA_ZONE();

    mRestoring = true;
    PrefStore& prefs = PrefStore::get();
    nlohmann::json session = prefs.value(kPrefs, "/session", nlohmann::json::object());
    mLastDir = QString::fromStdString(session.value("lastDir", std::string()));
    if (session.contains("tabs") && session["tabs"].is_array())
        for (const nlohmann::json& t : session["tabs"])
        {
            if (!t.is_object())
                continue;
            QString path = QString::fromStdString(t.value("path", std::string()));
            bool modified = t.value("modified", false);
            QString text;
            if (t.contains("text") && t["text"].is_string())
                text = QString::fromStdString(t["text"].get<std::string>());
            else
            {
                bool ok = false;
                text = readText(path, &ok);
                if (!ok)
                    continue;
            }
            const QString sceneScript = QString::fromStdString(t.value("scene", std::string()));
            const scene::scripts::Script* stored = sceneScript.isEmpty() ? nullptr : scene::scripts::find(sceneScript.toStdString());
            if (stored && !modified)
                text = QString::fromStdString(stored->text);
            CodeEditor* e = addEditor(path, text, modified);
            if (!sceneScript.isEmpty())
            {
                setSceneName(e, sceneScript);
                e->setUntitledName(sceneScript);
                updateTabTitle(e);
            }
            else if (path.isEmpty())
            {
                QString name = QString::fromStdString(t.value("name", std::string()));
                if (!name.isEmpty())
                    e->setUntitledName(name);
                updateTabTitle(e);
            }
            e->goToLine(t.value("line", 0), t.value("col", 0));
        }
    int current = session.value("current", 0);
    if (current >= 0 && current < mTabs->count())
        mTabs->setCurrentIndex(current);
    nlohmann::json sizes = prefs.value(kPrefs, "/splitter", nlohmann::json());
    if (sizes.is_array() && sizes.size() == 2 && sizes[0].is_number() && sizes[1].is_number())
        mSplitter->setSizes({sizes[0].get<int>(), sizes[1].get<int>()});
    else
        mSplitter->setSizes({300, 120});
    mRestoring = false;
}
} // namespace lila::ui
