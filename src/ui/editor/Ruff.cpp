#include "ui/editor/Ruff.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "ui/editor/TextUtil.h"

#include <nlohmann/json.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
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

FormatResult finish(QProcess& p)
{
    FormatResult r;
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.ok = r.exitCode == 0;
    r.text = QString::fromUtf8(p.readAllStandardOutput());
    r.text.replace("\r\n", "\n");
    if (!r.ok)
        r.error = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return r;
}
} // namespace

QString namespaceBuiltinsConfig()
{
    static const QString path = QString::fromStdWString((paths::pythonDir() / "lila" / "_namespace" / "ruff_builtins.toml").wstring());
    static QDateTime cachedStamp;
    static QString cached;
    QFileInfo fi(path);
    QDateTime stamp = fi.exists() ? fi.lastModified() : QDateTime();
    if (stamp == cachedStamp)
        return cached;
    cachedStamp = stamp;
    cached.clear();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        cached = QString::fromUtf8(f.readAll()).trimmed();
    return cached;
}

FormatResult run(const QString& ruffExe, const QStringList& args, const QString& source, const QString& filename)
{
    LILA_ZONE();

    QProcess p;
    p.setWorkingDirectory(workingDirFor(filename));
    p.start(ruffExe, QStringList(args) << "--stdin-filename" << filename << "-");
    if (!p.waitForStarted(3000))
        return {false, {}, "ruff did not start"};
    p.write(source.toUtf8());
    p.closeWriteChannel();
    if (!p.waitForFinished(8000))
    {
        p.kill();
        return {false, {}, "ruff timed out"};
    }
    return finish(p);
}

FormatResult format(const QString& ruffExe, const QString& source, const QString& filename)
{
    return run(ruffExe, {"format"}, source, filename);
}

QStringList fixArgs(const QStringList& extra)
{
    QStringList args{"check", "--fix-only", "--exit-zero", "--quiet"};
    QString builtins = namespaceBuiltinsConfig();
    if (!builtins.isEmpty())
        args << "--config" << builtins;
    return args + extra;
}

QList<Diagnostic> checkNow(const QString& ruffExe, const QString& source, const QString& filename)
{
    QStringList args{"check", "--output-format=json", "--no-fix", "--quiet"};
    QString builtins = namespaceBuiltinsConfig();
    if (!builtins.isEmpty())
        args << "--config" << builtins;
    FormatResult r = run(ruffExe, args, source, filename);
    // ruff exits 1 when it found something.
    if (r.exitCode != 0 && r.exitCode != 1)
        return {};
    return parseCheck(r.text.toUtf8(), source.split('\n'));
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
        if (const json* fix = d.contains("fix") && d["fix"].is_object() ? &d["fix"] : nullptr;
            fix && fix->value("applicability", std::string()) != "display-only")
        {
            diag.fixTitle = QString::fromStdString(fix->value("message", std::string()));
            diag.fixUnsafe = fix->value("applicability", std::string()) == "unsafe";
            for (const json& e : fix->value("edits", json::array()))
            {
                TextEdit edit;
                edit.line = e.value("/location/row"_json_pointer, 1) - 1;
                edit.col = col(edit.line, e.value("/location/column"_json_pointer, 1) - 1);
                edit.endLine = e.value("/end_location/row"_json_pointer, 1) - 1;
                edit.endCol = col(edit.endLine, e.value("/end_location/column"_json_pointer, 1) - 1);
                edit.text = QString::fromStdString(e.value("content", std::string()));
                diag.fix.append(edit);
            }
            if (diag.fixTitle.isEmpty() && !diag.fix.isEmpty())
                diag.fixTitle = "Fix " + diag.code;
        }
        out.append(diag);
    }
    return out;
}

Runner::Runner(QObject* parent) : QObject(parent) {}

Runner::~Runner()
{
    cancel();
}

void Runner::cancel()
{
    if (!mProcess)
        return;
    mProcess->disconnect(this);
    mProcess->kill();
    mProcess->waitForFinished(200);
    mProcess->deleteLater();
    mProcess = nullptr;
}

void Runner::start(const QString& ruffExe, const QStringList& args, const QString& source, const QString& filename,
                   std::function<void(const FormatResult&)> done)
{
    LILA_ZONE();

    cancel();
    auto* p = new QProcess(this);
    mProcess = p;
    connect(p, &QProcess::finished, this, [this, p, done](int, QProcess::ExitStatus) {
        if (p != mProcess)
            return;
        mProcess = nullptr;
        p->deleteLater();
        done(finish(*p));
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && p == mProcess)
        {
            mProcess = nullptr;
            p->deleteLater();
        }
    });
    p->setWorkingDirectory(workingDirFor(filename));
    p->start(ruffExe, QStringList(args) << "--stdin-filename" << filename << "-");
    p->write(source.toUtf8());
    p->closeWriteChannel();
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
        Q_EMIT finished(parseCheck(p->readAllStandardOutput(), lines));
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
    QStringList args{"check", "--output-format=json", "--no-fix", "--quiet"};
    QString builtins = namespaceBuiltinsConfig();
    if (!builtins.isEmpty())
        args << "--config" << builtins;
    p->start(ruffExe, args << "--stdin-filename" << filename << "-");
    p->write(source.toUtf8());
    p->closeWriteChannel();
}
} // namespace lila::ui::ruff
