#pragma once

#include <QWidget>

class QMenuBar;
class QToolButton;

namespace lila::ui
{
/**
 * The right part of the menu bar row: the layout tabs centred in the space the menus leave, then the folder name, the
 * mode (when more than one is installed), a chip naming a pinned menu set, the command search and Send to Unreal.
 */
class TopBar : public QWidget
{
public:
    TopBar(QMenuBar* bar, QWidget* layoutTabs, QWidget* folder, QWidget* modes, QWidget* parent);

    /** Sizes the bar to the room the menus leave; call when the menus shown change. */
    void relayout();
    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshPin();
    void refreshUnreal();

    QMenuBar* mBar;
    QToolButton* mPin = nullptr;
    QToolButton* mUnreal = nullptr;
    QToolButton* mSearch = nullptr;
    QWidget* mFolder = nullptr;
    QWidget* mModes = nullptr;
    int mFullWidth = 0;
    bool mConnected = false;
    int mWidth = 0;
};
} // namespace lila::ui
