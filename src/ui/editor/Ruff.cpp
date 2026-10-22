#include "ui/editor/Ruff.h"

#include "core/Profile.h"
#include "ui/editor/TextUtil.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace lila::ui::ruff
{
using json = nlohmann::json;

namespace
{
QString workingDirFor(const QString& filename)
{
    QFileInfo fi(filename);
    return fi.absoluteDir().exists() ? fi.absolutePath() : QString();
}
} // namespace

FormatResult format(const QString& ruffExe, const QString& source, const QString& filename)
{
    LILA_ZONE();

    QProcess p;
    p.setWorkingDirectory(workingDirFor(filename));
    p.start(ruffExe, {"format", "--stdin-filename", filename, "-"});
    if (!p.waitForStarted(3000))
        return {false, {}, "ruff did not start"};
    p.write(source.toUtf8());
    p.closeWriteChannel();
    if (!p.waitForFinished(8000))
    {
        p.kill();
        return {false, {}, "ruff format timed out"};
    }
    if (p.exitCode() != 0)
        return {false, {}, QString::fromUtf8(p.readAllStandardError()).trimmed()};
    return {true, QString::fromUtf8(p.readAllStandardOutput()), {}};
}

QList<Diagnostic> parseCheck(const QByteArray& output, const QStringList& lines)
{
    QList<Diagnostic> out;
    json doc = json::parse(output.constBegin(), output.constEnd(), nullptr, false);
    if (!doc.is_array())
        return out;
    auto col = [&](int row, int cp) {
        if (row < 0 || row >= lines.size())
            return std::max(0, cp);
        return text::codepointToUtf16(lines[row], cp);
    };
    for (const json& d : doc)
    {
        // Syntax errors come without a code; pyright reports those already.
        if (!d.contains("code") || !d["code"].is_string() || d["code"] == "invalid-syntax")
            continue;
        Diagnostic diag;
        diag.line = d.value("/location/row"_json_pointer, 1) - 1;
        diag.col = col(diag.line, d.value("/location/column"_json_pointer, 1) - 1);
        diag.endLine = d.value("/end_location/row"_json_pointer, diag.line + 1) - 1;
        diag.endCol = col(diag.endLine, d.value("/end_location/column"_json_pointer, 1) - 1);
        diag.code = QString::fromStdString(d["code"].get<std::string>());
        diag.message = QString::fromStdString(d.value("message", std::string()));
        diag.source = "ruff";
        diag.severity = diag.code.startsWith("E9") ? Severity::Error : Severity::Warning;
        diag.unnecessary = diag.code == "F401" || diag.code == "F841";
        out.append(diag);
    }
    return out;
}

Checker::Checker(QObject* parent) : QObject(parent) {}

Checker::~Checker()
{
    if (mProcess)
    {
        mProcess->disconnect(this);
        mProcess->kill();
        mProcess->waitForFinished(500);
    }
}

void Checker::check(const QString& ruffExe, const QString& source, const QString& filename)
{
    LILA_ZONE();

    if (mProcess)
    {
        mProcess->disconnect(this);
        mProcess->kill();
        mProcess->waitForFinished(200);
        mProcess->deleteLater();
    }
    auto* p = new QProcess(this);
    mProcess = p;
    QStringList lines = source.split('\n');
    connect(p, &QProcess::finished, this, [this, p, lines](int, QProcess::ExitStatus) {
        if (p != mProcess)
            return;
        // ruff exits 1 when it found something, which is the normal case here.
        emit finished(parseCheck(p->readAllStandardOutput(), lines));
        mProcess = nullptr;
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && p == mProcess)
        {
            mProcess = nullptr;
            p->deleteLater();
        }
    });
    p->setWorkingDirectory(workingDirFor(filename));
    p->start(ruffExe, {"check", "--output-format=json", "--no-fix", "--quiet", "--stdin-filename", filename, "-"});
    p->write(source.toUtf8());
    p->closeWriteChannel();
}
} // namespace lila::ui::ruff
