#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace lila::ui
{
/**
 * Editable flags of the last adjustable command (scene::lastOp). Editing a field re-runs the command with the new
 * value; each settled edit (a release, a confirmed entry) is one undo step, a drag coalesced into it. Shown in the viewport's in-view popup and
 * in the Modeling Toolkit's Tool Options section.
 */
class LILA_UI_API LastOpEditor : public QWidget
{
    Q_OBJECT

public:
    explicit LastOpEditor(QWidget* parent = nullptr);
    ~LastOpEditor() override;

    /** Rebuilds or refreshes the fields for the current op. */
    void refresh();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void rebuild();
    void setFlag(const std::string& name, const nlohmann::json& value);
    void applyPending();

    QLabel* mTitle = nullptr;
    QHBoxLayout* mTitleRow = nullptr;
    QToolButton* mDocs = nullptr; // the op's docs page, when its option box names one
    QWidget* mFields = nullptr;
    QVBoxLayout* mFieldsLayout = nullptr;
    QToolButton* mNonDestructive = nullptr;
    QLabel* mStatus = nullptr;
    QTimer* mApply = nullptr;
    QTimer* mEcho = nullptr;
    QTimer* mLiveCheck = nullptr;
    std::string mCommand;
    uint64_t mBuiltFor = 0;
    nlohmann::json mPending = nlohmann::json::object();
    /** Flags the edit in progress changes; editing others starts a new undo step. */
    std::vector<std::string> mDragFlags;
    /** Shows a flag's value in its field without applying it, by flag name. */
    std::map<std::string, std::function<void(const nlohmann::json&)>> mShow;
    bool mUpdating = false;
    bool mSelfRerun = false;
    int mLastOpListener = 0;
    int mGraphListener = 0;
    int mComponentListener = 0;
};

namespace adjustLastOp
{
/** Pref (general) that turns the in-view popup off; default on. */
inline constexpr const char* kEnabledPref = "/modeling/adjustLastOp";

/** Tracks the last op and pops the in-view editor after adjustable UI commands. Registers "adjustLastOp.*" actions. */
LILA_UI_API void install();
} // namespace adjustLastOp
} // namespace lila::ui
