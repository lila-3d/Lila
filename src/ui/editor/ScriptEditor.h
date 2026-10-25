#pragma once

#include "core/Export.h"

#include "scene/SceneScripts.h"
#include "ui/editor/ScriptStorage.h"

#include <QIcon>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <vector>

class QAction;
class QKeyEvent;
class QLabel;
class QMenu;
class QToolBar;
class QSplitter;
class QTabWidget;

namespace lila::ui
{
class CodeEditor;
class FindBar;
class LspClient;
class OutputView;

/**
 * The Python script editor panel: tabs of CodeEditor over an output pane, run with Ctrl+Enter (or numpad Enter),
 * Shift+Enter for the caret's line and Ctrl+Shift+Enter per `# %%` cell. Open tabs, unsaved buffers included, come
 * back from the "scriptEditor" prefs.
 * Language tools are fetched on first show and the editor works without them meanwhile.
 *
 * Each tab saves to a storage::Location: inside the scene (the sceneScript command), the workspace's .lila/scripts,
 * any file, or the user prefs' scripts folder. Vim keybindings come from the "scriptEditor" /vim pref.
 */
class LILA_UI_API ScriptEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptEditor(QWidget* parent = nullptr);
    ~ScriptEditor() override;

    void setEditorFont(const QFont& font);
    /** Tab icons by Tabler name; the editor library can't reach the app's icon set itself. */
    void setIconProvider(std::function<QIcon(const QString&)> fn);

    CodeEditor* newTab();
    /** Focuses the file's tab if it is open already. 0-based line, -1 to leave the cursor alone. */
    CodeEditor* openFile(const QString& path, int line = -1, int col = 0);
    void openFileDialog();
    bool saveTab(int index, bool saveAs = false);
    /** Asks where to save, preselecting `preset`. */
    bool saveTabAs(int index, storage::Location preset);
    /** Focuses the scene script's tab, opening one if needed. Null when the scene has no such script. */
    CodeEditor* openSceneScript(const QString& name);
    /** Where a tab saves to. */
    storage::Location locationOf(CodeEditor* editor) const;
    /** Prompts when the tab has unsaved changes. Returns false if the user cancelled. */
    bool closeTab(int index);

    /** Runs the selection or the whole tab, or the cell under the cursor. */
    void runCurrent(bool cell);
    /** Runs the caret's line, or the selection when there is one. */
    void runLine();

    /**
     * Routes the run keys through the app's rebindable hotkeys: `fn` names the action a key is bound to ("script.run",
     * "script.runLine", "script.runCell"), or returns empty. Without it the editor's default keys apply.
     */
    void setRunKeyLookup(std::function<QString(QKeyEvent*)> fn);

    CodeEditor* currentEditor() const;
    CodeEditor* editorAt(int index) const;

    /** Writes the session now rather than on the next debounce. */
    void saveSession();

signals:
    /** F1 / Ctrl+Q on a symbol, fully qualified when the interpreter can resolve it. */
    void showDocs(const QString& symbol);
    /** The toolbar's API Docs button: the scripting section of the docs. */
    void apiDocsRequested();
    /** The toolbar's API Reference button: the live reference panel. */
    void apiReferenceRequested();
    /** The toolbar's Batch Run button: the batch script execution window. */
    void batchRunRequested();

protected:
    void showEvent(QShowEvent* e) override;

private:
    CodeEditor* addEditor(const QString& path, const QString& text, bool modified);
    void wire(CodeEditor* editor);
    void updateTabTitle(CodeEditor* editor);
    void updateStatus();
    void setToolStatus(const QString& text);
    void onToolsReady();
    void startLanguageServer();
    void jumpToTrace(const QString& file, int line);
    bool hostKey(QKeyEvent* e);
    QString nextUntitledName();
    void restoreSession();
    void scheduleSessionSave();

    QString sceneName(CodeEditor* editor) const;
    void setSceneName(CodeEditor* editor, const QString& name);
    bool saveToScene(CodeEditor* editor, const QString& name, bool runOnOpen);
    bool saveToFile(CodeEditor* editor, const QString& path);
    bool confirmRunOnOpen();
    void setRunOnOpen(CodeEditor* editor, bool on);
    void removeFromScene(CodeEditor* editor);
    void onSceneScriptsChanged();
    void runOpenedSceneScripts(const std::vector<scene::scripts::Script>& list);
    void fillSaveMenu(QMenu* menu);
    void fillOpenMenu(QMenu* menu);
    void fillFormatMenu(QMenu* menu);
    void run(CodeEditor* editor, QString code, int firstLine);
    void applyVim();
    void attachVim(CodeEditor* editor);
    void updateVimStatus();

    /** A toolbar toggle bound to a bool pref, kept in step when the pref changes elsewhere. */
    struct PrefToggle
    {
        QAction* action;
        const char* key;
        bool fallback;
    };

    QToolBar* mBar;
    std::vector<PrefToggle> mPrefToggles;
    std::function<QString(QKeyEvent*)> mRunKeyLookup;
    QTabWidget* mTabs;
    FindBar* mFind;
    OutputView* mOutput;
    QSplitter* mSplitter;
    QLabel* mStatus;
    QLabel* mToolStatus;
    QLabel* mVimMode;
    LspClient* mLsp;
    QFont mFont;
    QTimer mSessionTimer;
    QString mRuffExe;
    QString mLastDir;
    std::function<QIcon(const QString&)> mIconProvider;
    int mScriptsListener = 0;
    int mPrefsListener = 0;
    bool mStarted = false;
    bool mRestoring = false;
    int mLspRestarts = 0;
};
} // namespace lila::ui
