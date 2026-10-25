#pragma once

#include "core/Export.h"

#include "ui/editor/EditorTypes.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <functional>

class QProcess;

namespace lila::ui::ruff
{
struct FormatResult
{
    bool ok = false; // exit code 0
    QString text;    // stdout, with CRLF made LF
    QString error;
    int exitCode = -1;
};

/** `ruff <args> --stdin-filename <filename> -` on `source`, blocking for at most a few seconds. */
LILA_UI_API FormatResult run(const QString& ruffExe, const QStringList& args, const QString& source, const QString& filename);

/** `ruff format -`. Blocks for at most a few seconds; ruff is fast enough that this is the simpler choice. */
LILA_UI_API FormatResult format(const QString& ruffExe, const QString& source, const QString& filename);

/** `ruff check` now, blocking, for code actions that need diagnostics matching the current text. */
LILA_UI_API QList<Diagnostic> checkNow(const QString& ruffExe, const QString& source, const QString& filename);

/** `ruff check --fix-only` arguments, plus the interactive namespace's builtins so its names are never "fixed". */
LILA_UI_API QStringList fixArgs(const QStringList& extra);

/**
 * The interactive namespace's bare names (xform, Node, ...) as a `builtins = [...]` override for `ruff check --config`,
 * read from the file the stub generator writes beside __builtins__.pyi. Empty before Python has written it.
 */
LILA_UI_API QString namespaceBuiltinsConfig();

/** Parses `ruff check --output-format=json` against the checked text (for code point to UTF-16 columns). */
LILA_UI_API QList<Diagnostic> parseCheck(const QByteArray& output, const QStringList& lines);

/** Runs one ruff job at a time in the background; starting another drops the one still running, whose callback never runs. */
class LILA_UI_API Runner : public QObject
{
    Q_OBJECT

public:
    explicit Runner(QObject* parent = nullptr);
    ~Runner() override;

    void start(const QString& ruffExe, const QStringList& args, const QString& source, const QString& filename,
               std::function<void(const FormatResult&)> done);
    void cancel();

private:
    QProcess* mProcess = nullptr;
};

/** Runs `ruff check` in the background; a new check supersedes one still running. */
class LILA_UI_API Checker : public QObject
{
    Q_OBJECT

public:
    explicit Checker(QObject* parent = nullptr);
    ~Checker() override;

    void check(const QString& ruffExe, const QString& source, const QString& filename);

signals:
    void finished(const QList<lila::ui::Diagnostic>& diagnostics);

private:
    QProcess* mProcess = nullptr;
};
} // namespace lila::ui::ruff
