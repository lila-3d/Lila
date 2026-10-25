#pragma once

#include "core/Export.h"

#include "ui/editor/DocFormat.h"

#include <QTextDocument>
#include <QWidget>

namespace lila::ui
{
/**
 * A tooltip-like window drawing DocFormat HTML in the editor's theme: hover docs, signature help, definition peeks
 * and gutter diagnostics. It never takes focus; its owner decides when to hide it.
 */
class LILA_UI_API DocPopup : public QWidget
{
public:
    explicit DocPopup(QWidget* owner);

    /** Lays out `html` at most `maxWidth` wide, narrower when the content is. */
    void setHtml(const QString& html, const doc::Theme& theme, int maxWidth);

    /** Places the top left at `global`, flipped to end above `flipBottom` when it would leave the screen. */
    void showAt(const QPoint& global, int flipBottom);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QTextDocument mDoc;
    QColor mBackground, mBorder, mText;
};
} // namespace lila::ui
