#pragma once

#include "core/Export.h"

#include <QWidget>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QStackedWidget;
class QToolButton;

namespace lila::ui
{
inline constexpr const char* kNotesDock = "notes";

/**
 * Named notes (plain text) and todo lists saved with the scene, under the scene data key "notes". One page shows at a
 * time, picked from the list at the top, whose text renames it. Typing is one undo step per editing session.
 */
class LILA_UI_API NotesPanel : public QWidget
{
public:
    explicit NotesPanel(QWidget* parent = nullptr);
    ~NotesPanel() override;

private:
    void scheduleRefresh();
    void refresh();
    void showPage(int index);
    void addPage(const std::string& kind);
    void renameCurrent();
    void deleteCurrent();
    void commitText();
    void commitItems();
    void addItem();
    /** One item per non-empty line, list bullets stripped; false when there was none or the page is not a todo list. */
    bool addLines(const QString& text);
    /** The selected items to the clipboard, one per line, ticked ones as "[x] ". */
    void copyItems();
    void clearDone();
    /** Writes pages to the scene as one undo step; amend folds it into the previous step when that was this edit too. */
    void store(const std::string& label, bool amend = false);
    nlohmann::json* current();

    nlohmann::json mPages = nlohmann::json::array();
    int mCurrent = -1;
    QComboBox* mPicker = nullptr;
    QToolButton* mClearDone = nullptr;
    QStackedWidget* mStack = nullptr;
    QPlainTextEdit* mText = nullptr;
    QListWidget* mList = nullptr;
    QLineEdit* mNewItem = nullptr;
    std::vector<uint64_t> mSubs;
    uint64_t mTypingStep = 0; // the undo step the current typing session amends; 0 starts a new one
    bool mPending = false;
    bool mFilling = false;
};

/** "window.notes". */
LILA_UI_API void registerNotesPanel();
} // namespace lila::ui
