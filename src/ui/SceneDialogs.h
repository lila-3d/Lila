#pragma once

#include "core/Export.h"

#include "io/Recovery.h"

#include <QDialog>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <vector>

class QCheckBox;
class QPushButton;
class QTreeWidget;

namespace lila::ui
{
LILA_UI_API void openCrashReport(const std::filesystem::path& dir);
LILA_UI_API void openCrashFolder(const std::filesystem::path& dir);
/** Puts report.txt on the clipboard. */
LILA_UI_API void copyCrashReport(const std::filesystem::path& dir);

/**
 * After a crash: what crashed (with the report a click away) when crashReport is set, and the scenes with newer
 * autosaves or emergency saves, with what to do with the selected one.
 */
class LILA_UI_API RecoveryDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Choice
    {
        Discard,
        Recover,
        OpenSaved,
    };

    RecoveryDialog(const std::vector<io::recovery::Candidate>& candidates, const std::filesystem::path& crashReport, QWidget* parent);

    Choice choice() const { return mChoice; }
    /** The candidate the choice applies to; only when there are candidates. */
    const io::recovery::Candidate& selected() const;

private:
    void finish(Choice c);

    std::vector<io::recovery::Candidate> mCandidates;
    QTreeWidget* mList = nullptr;
    QPushButton* mOpenSaved = nullptr;
    Choice mChoice = Choice::Discard;
};

/**
 * The frameless recent-scenes picker shown when Lila starts without a scene. Enter or double-click opens, Esc or a
 * click outside dismisses.
 */
class LILA_UI_API StartupDialog : public QDialog
{
    Q_OBJECT

public:
    struct Hooks
    {
        std::function<void(const std::filesystem::path&)> open; // empty path = browse
    };

    StartupDialog(Hooks hooks, QWidget* parent);

    /** Centered on the screen parent is on. */
    void present();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void openSelected();

    Hooks mHooks;
    QTreeWidget* mList = nullptr;
};

/** File > Versions: everything Lila and source control kept of the current scene. */
class LILA_UI_API VersionsDialog : public QDialog
{
    Q_OBJECT

public:
    struct Hooks
    {
        /** Unsaved-changes prompt; false = cancelled. */
        std::function<bool()> confirmDiscard;
        std::function<void()> opened;
    };

    VersionsDialog(const std::filesystem::path& scene, Hooks hooks, QWidget* parent);

private:
    void populate();
    void addRevisions(const nlohmann::json& revisions);
    void act(bool restore);
    void load(const std::filesystem::path& file, bool restore, const QString& label);

    std::filesystem::path mScene;
    Hooks mHooks;
    QTreeWidget* mList = nullptr;
    QPushButton* mOpen = nullptr;
    QPushButton* mRestore = nullptr;
};
} // namespace lila::ui
