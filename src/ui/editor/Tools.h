#pragma once

#include "core/Export.h"

#include <QObject>
#include <QString>

class QProcess;

namespace lila::ui
{
/**
 * basedpyright and ruff, pip-installed on demand into %LOCALAPPDATA%/Lila/tools (LILA_TOOLS_DIR overrides).
 * Installing runs pip in a child process, so the UI never blocks; without the tools the editor still edits and runs.
 */
class LILA_UI_API Tools : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Unknown,
        Installing,
        Ready,
        Failed,
    };

    static Tools& get();

    /** Checks the tools dir and starts an install if anything is missing. Safe to call repeatedly. */
    void ensure();

    State state() const { return mState; }
    QString message() const { return mMessage; }

    QString dir() const;
    QString nodeExe() const;
    QString langserverScript() const;
    QString ruffExe() const;
    bool hasLangserver() const;
    bool hasRuff() const;

signals:
    void stateChanged(lila::ui::Tools::State state, const QString& message);

private:
    Tools() = default;
    void setState(State s, const QString& message);
    void install();

    State mState = State::Unknown;
    QString mMessage;
    QProcess* mPip = nullptr;
};
} // namespace lila::ui
