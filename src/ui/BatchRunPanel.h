#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTextBrowser;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::batch
{
class Run;
}

namespace lila::ui
{
inline constexpr const char* kBatchRunDock = "batchRun";

/**
 * Batch Script Execution: a script (snippet, file, script editor tab or scene script) run over many scene files by the
 * batchRun command, in lilabatch workers, so the UI never waits. Files, folders and globs are added by button, typed or
 * dropped; the table shows each file's progress and result columns, the log pane its captured output with traceback
 * lines that open the script at the failing line.
 */
class LILA_UI_API BatchRunPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BatchRunPanel(QWidget* parent = nullptr);
    ~BatchRunPanel() override;

private:
    QWidget* buildScriptTab();
    QWidget* buildFilesTab();
    QWidget* buildSaveTab();
    QWidget* buildOptionsTab();

    /**
     * The form as a batchRun call ({"args": inputs, flag: value}); null with a message when the script is missing.
     * withScript false leaves the script out, for saving the form.
     */
    nlohmann::json call(QString* problem, bool withScript = true) const;
    /** The form as a preset: the call's flags plus "inputs" and the script source. */
    nlohmann::json formState() const;
    void applyState(const nlohmann::json& state);
    void rememberForm();

    void start(bool dryRun);
    void cancel();
    void retry(const std::vector<size_t>& indices);
    void exportCsv();
    void watch(int runId);
    void refreshRows();
    void showLog(QTreeWidgetItem* item);
    void openTrace(const QString& file, int line);
    void rowMenu(const QPoint& at);
    void updateButtons();

    void addInputs(const QStringList& paths);
    void refreshPresets();
    void savePreset();
    void deletePreset();
    void fillFileListMenu();

    QComboBox* mPresets = nullptr;
    QTabWidget* mTabs = nullptr;

    QComboBox* mSource = nullptr;
    QStackedWidget* mSourcePages = nullptr;
    QPlainTextEdit* mSnippet = nullptr;
    QLineEdit* mScriptFile = nullptr;
    QComboBox* mSceneScript = nullptr;

    QListWidget* mInputs = nullptr;
    QLineEdit* mTypedInput = nullptr;
    QToolButton* mFileLists = nullptr;
    QCheckBox* mRecursive = nullptr;
    QSpinBox* mDepth = nullptr;
    QLineEdit* mExtensions = nullptr;
    QLineEdit* mInclude = nullptr;
    QLineEdit* mExclude = nullptr;
    QLineEdit* mExcludePrefix = nullptr;
    QLineEdit* mExcludeSuffix = nullptr;
    QLineEdit* mExcludeContains = nullptr;

    QComboBox* mSave = nullptr;
    QLineEdit* mOutputDir = nullptr;
    QLineEdit* mSuffix = nullptr;
    QComboBox* mFormat = nullptr;
    QCheckBox* mKeepTree = nullptr;
    QComboBox* mCheckout = nullptr;
    QCheckBox* mMakeWritable = nullptr;

    QSpinBox* mWorkers = nullptr;
    QCheckBox* mStopOnError = nullptr;
    QDoubleSpinBox* mTimeout = nullptr;
    QLineEdit* mArgs = nullptr;

    QToolButton* mPreview = nullptr;
    QToolButton* mRun = nullptr;
    QToolButton* mCancel = nullptr;
    QToolButton* mRetry = nullptr;
    QToolButton* mCsv = nullptr;
    QToolButton* mLogs = nullptr;
    QProgressBar* mProgress = nullptr;
    QLabel* mSummary = nullptr;
    QTreeWidget* mTable = nullptr;
    QTextBrowser* mLog = nullptr;

    QTimer* mPoll = nullptr;
    QTimer* mRemember = nullptr;
    std::shared_ptr<batch::Run> mRunShown;
    uint64_t mShownVersion = ~uint64_t(0);
    std::vector<std::string> mResultKeys;
    bool mApplying = false;
};

/** The panel and its window.batchRun toggle. */
LILA_UI_API void registerBatchRunPanel();
} // namespace lila::ui
