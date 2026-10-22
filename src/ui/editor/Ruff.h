#pragma once

#include "ui/editor/EditorTypes.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

class QProcess;

namespace lila::ui::ruff
{
struct FormatResult
{
    bool ok = false;
    QString text;
    QString error;
};

/** `ruff format -`. Blocks for at most a few seconds; ruff is fast enough that this is the simpler choice. */
FormatResult format(const QString& ruffExe, const QString& source, const QString& filename);

/** Parses `ruff check --output-format=json` against the checked text (for code point to UTF-16 columns). */
QList<Diagnostic> parseCheck(const QByteArray& output, const QStringList& lines);

/** Runs `ruff check` in the background; a new check supersedes one still running. */
class Checker : public QObject
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
