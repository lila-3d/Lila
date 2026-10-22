#pragma once

#include <QWidget>

class QPlainTextEdit;
class QLineEdit;

namespace lila::ui
{
/** Log/output view plus a one-line Python prompt. The full editor (Monaco) replaces the prompt later. */
class ScriptPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptPanel(QWidget* parent = nullptr);
    ~ScriptPanel() override;

signals:
    void appendRequested(int level, const QString& text);

private:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void append(int level, const QString& text);
    void runInput();

    QPlainTextEdit* mOutput;
    QLineEdit* mInput;
    int mSink = 0;
    QStringList mHistory;
    int mHistoryPos = 0;
};
} // namespace lila::ui
