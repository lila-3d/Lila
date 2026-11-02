#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <QFrame>

#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTabBar;
class QTableWidget;
class QToolButton;
class QWidget;

namespace lila::ui
{
/**
 * Every way to drive one plug, in place: its current source with Break and jump, Connect (search or pick the source),
 * Driven Key (driver plus a key table and its curve), Expression and Math (a rig math node wired in). It stays up while
 * the artist picks in the viewport, Outliner or Channel Box, and closes on commit or Esc. Child widgets carry object names
 * for tests.
 */
class LILA_UI_API DrivePopover : public QFrame
{
    Q_OBJECT

public:
    /** target is "node.attr" or "node.attrX". */
    explicit DrivePopover(std::string target, QWidget* parent = nullptr);
    ~DrivePopover() override;

    /** Plugs the source list offers for the search text: compatible with the target, other nodes only. */
    std::vector<std::string> candidates(const QString& text) const;
    /** connectAttr(source, target, force). */
    bool connectFrom(const std::string& source);
    /** driveChannel(target, driver, keys from the table). */
    bool applyDrivenKey();
    /** Adds the driver's and the target's values now as a key row. */
    void keyCurrent();
    /** Creates a math node of the type and wires its first matching output into the target. Returns the node's path. */
    std::string addMath(const std::string& nodeType);
    /** Sets the target from an expression (phase 6's expression command). */
    bool applyExpression();
    bool breakSource();

Q_SIGNALS:
    void committed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshSource();
    void refillSources();
    void refillMath();
    void fail(const QString& why);
    void drawCurve();

    std::string mTarget;
    QLabel* mSource = nullptr;
    QToolButton* mJump = nullptr;
    QToolButton* mBreak = nullptr;
    QTabBar* mTabs = nullptr;
    QStackedWidget* mPages = nullptr;
    QLineEdit* mSearch = nullptr;
    QToolButton* mPick = nullptr;
    QListWidget* mList = nullptr;
    QLineEdit* mDriver = nullptr;
    QTableWidget* mKeys = nullptr;
    QLabel* mCurve = nullptr;
    QLineEdit* mExpression = nullptr;
    QLabel* mExpressionNote = nullptr;
    QLineEdit* mMathSearch = nullptr;
    QListWidget* mMathList = nullptr;
    QLabel* mStatus = nullptr;
    int mGraphListener = 0;
    int mViewListener = 0;
};

namespace drivePopover
{
/** Opens one beside anchor; it deletes itself when closed. */
LILA_UI_API DrivePopover* show(const std::string& target, QWidget* anchor);
} // namespace drivePopover
} // namespace lila::ui
