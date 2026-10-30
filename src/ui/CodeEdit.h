#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>

class QPlainTextEdit;
class QPushButton;

namespace lila::ui
{
/**
 * A monospace box for source code: Wrangle snippets, Slang, expressions. Edits are sent on Apply or Ctrl+Enter, never per
 * keystroke or pause, because each one recompiles. Escape restores the applied text.
 */
class LILA_UI_API CodeEdit : public QWidget
{
    Q_OBJECT

public:
    explicit CodeEdit(QWidget* parent = nullptr, int lines = 6);

    /** Shows a value without applying it; ignored while there are unapplied edits. */
    void showText(const std::string& text);
    std::string text() const;
    void setReadOnly(bool readOnly);

Q_SIGNALS:
    void applied(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void apply();

    QPlainTextEdit* mEdit = nullptr;
    QPushButton* mApply = nullptr;
    QString mApplied;
};
} // namespace lila::ui
