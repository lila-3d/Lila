#include "ui/editor/Tools.h"

#include "core/Log.h"
#include "ui/editor/Interpreter.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace lila::ui
{
namespace
{
#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif
} // namespace

Tools& Tools::get()
{
    static Tools* t = new Tools;
    return *t;
}

QString Tools::dir() const
{
    QString over = qEnvironmentVariable("LILA_TOOLS_DIR");
    if (!over.isEmpty())
        return QDir::cleanPath(over);
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir::cleanPath(base + "/Lila/tools");
}

QString Tools::nodeExe() const
{
#ifdef _WIN32
    return dir() + "/nodejs_wheel/node.exe";
#else
    return dir() + "/nodejs_wheel/bin/node";
#endif
}

QString Tools::langserverScript() const
{
    return dir() + "/basedpyright/langserver.index.js";
}

QString Tools::ruffExe() const
{
    return dir() + "/bin/ruff" + kExe;
}

bool Tools::hasLangserver() const
{
    return QFileInfo::exists(nodeExe()) && QFileInfo::exists(langserverScript());
}

bool Tools::hasRuff() const
{
    return QFileInfo::exists(ruffExe());
}

void Tools::setState(State s, const QString& message)
{
    mState = s;
    mMessage = message;
    Q_EMIT stateChanged(s, message);
}

void Tools::ensure()
{
    if (mState == State::Installing || mState == State::Ready || mState == State::Failed)
        return;
    if (hasLangserver() && hasRuff())
    {
        setState(State::Ready, "Python tools ready");
        return;
    }
    install();
}

void Tools::install()
{
    QString python = interp::basePython();
    if (python.isEmpty())
    {
        setState(State::Failed, "No base Python found to install basedpyright and ruff");
        return;
    }
    // pip --target into a staging dir so a failed install never leaves a half-populated tools dir behind.
    QString staging = dir() + ".staging";
    QDir(staging).removeRecursively();
    QDir().mkpath(staging);

    setState(State::Installing, "Installing basedpyright and ruff...");
    log::info("script editor: installing basedpyright and ruff into {}", dir().toStdString());

    mPip = new QProcess(this);
    mPip->setProcessChannelMode(QProcess::MergedChannels);
    connect(mPip, &QProcess::finished, this, [this, staging](int code, QProcess::ExitStatus status) {
        QString output = QString::fromUtf8(mPip->readAll());
        mPip->deleteLater();
        mPip = nullptr;
        if (status != QProcess::NormalExit || code != 0)
        {
            QDir(staging).removeRecursively();
            log::warn("script editor: tool install failed\n{}", output.right(2000).toStdString());
            setState(State::Failed, "Python tools failed to install; editing works without them");
            return;
        }
        QDir(dir()).removeRecursively();
        if (!QDir().rename(staging, dir()))
        {
            setState(State::Failed, "Could not move installed tools into " + dir());
            return;
        }
        bool ok = hasLangserver() && hasRuff();
        setState(ok ? State::Ready : State::Failed, ok ? "Python tools ready" : "Python tools installed incompletely");
    });
    connect(mPip, &QProcess::errorOccurred, this, [this, python](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        setState(State::Failed, "Could not run " + python);
        mPip->deleteLater();
        mPip = nullptr;
    });
    mPip->start(python, {"-m", "pip", "install", "--disable-pip-version-check", "--no-input", "--target", staging,
                         "basedpyright", "ruff"});
}
} // namespace lila::ui
