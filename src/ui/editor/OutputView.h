#pragma once

#include "core/Export.h"

#include <QPlainTextEdit>
#include <QTextCharFormat>

class QTextDocument;

namespace lila::ui
{
/** Foreground for a log level (a LogLevel as int) in the palette look, tagged so recolorLog can redo it. */
LILA_UI_API QTextCharFormat logFormat(int level);
/** Recolours every run logFormat made, after the palette look changes. */
LILA_UI_API void recolorLog(QTextDocument* doc);

/** The script editor's history: the log as the script panel shows it, with traceback frames clickable. */
class LILA_UI_API OutputView : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit OutputView(QWidget* parent = nullptr);
    ~OutputView() override;

    void setEditorFont(const QFont& font);

signals:
    void appendRequested(int level, const QString& text);
    void traceClicked(const QString& file, int line);

protected:
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    /** Background, text and every line's level colour from the palette look. */
    void applyLook();
    void append(int level, const QString& text);

    int mSink = 0;
};
} // namespace lila::ui
