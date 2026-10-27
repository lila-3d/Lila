#pragma once

#include "core/Export.h"

#include <QPlainTextEdit>

#include <string>

class QTimer;

namespace lila::ui
{
/**
 * A text box for a string attr of several lines. Enter starts a new line; the text commits when typing pauses, on Ctrl+Enter
 * and when the box loses focus, so each pause is one undo step.
 */
class LILA_UI_API MultilineEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit MultilineEdit(QWidget* parent = nullptr);

    /** Shows a value without committing it; ignored while the box is being typed in. */
    void showText(const std::string& text);
    QSize sizeHint() const override;

Q_SIGNALS:
    void committed(const QString& text);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    void commit();

    QTimer* mPause = nullptr;
    QString mCommitted;
};
} // namespace lila::ui
