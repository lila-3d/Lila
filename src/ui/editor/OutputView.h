#pragma once

#include "core/Export.h"

#include <QPlainTextEdit>

namespace lila::ui
{
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
    void append(int level, const QString& text);

    int mSink = 0;
};
} // namespace lila::ui
