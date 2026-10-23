#pragma once

#include "core/Export.h"

#include "ui/editor/EditorTypes.h"

#include <QFrame>

class QListWidget;
class QPlainTextEdit;

namespace lila::ui
{
/**
 * The completion list with a documentation pane beside it. It never takes focus: the editor keeps typing and
 * forwards navigation keys here.
 */
class LILA_UI_API CompletionPopup : public QFrame
{
    Q_OBJECT

public:
    explicit CompletionPopup(QWidget* owner);

    void setEditorFont(const QFont& font);
    void setEntries(QList<CompletionEntry> entries);
    QList<CompletionEntry>& entries() { return mEntries; }

    /** Filters and ranks by the typed prefix. Returns the number of visible rows. */
    int filter(const QString& typed);
    const QString& typed() const { return mTyped; }

    /** Index into entries() of the highlighted row, or -1. */
    int currentEntry() const;
    void moveSelection(int delta);

    void showBelow(const QRect& cursorRectGlobal);
    void refreshDocumentation();

signals:
    void activated();
    void currentEntryChanged(int entryIndex);

private:
    QListWidget* mList;
    QPlainTextEdit* mDoc;
    QList<CompletionEntry> mEntries;
    QString mTyped;
};
} // namespace lila::ui
