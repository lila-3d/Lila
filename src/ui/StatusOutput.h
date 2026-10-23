#pragma once

#include "core/Export.h"

#include "ui/OutputTail.h"

#include <QWidget>

#include <functional>

namespace lila::ui
{
/**
 * Maya's command line result: the script output's last line in the status bar while the Script Output panel is out of
 * sight. Warnings and errors carry an icon and errors are bold. Hover for the last few lines; click to open the panel.
 */
class LILA_UI_API StatusOutput : public QWidget
{
    Q_OBJECT

public:
    /** outputShowing: whether the Script Output panel is on screen, so the line can step aside. */
    StatusOutput(std::function<void()> openOutput, std::function<bool()> outputShowing, QWidget* parent = nullptr);
    ~StatusOutput() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void appendRequested(int level, const QString& text);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void append(int level, const QString& text);
    void refresh();
    bool shown() const;

    OutputTail mTail;
    std::function<void()> mOpen;
    std::function<bool()> mOutputShowing;
    int mSink = 0;
};
} // namespace lila::ui
