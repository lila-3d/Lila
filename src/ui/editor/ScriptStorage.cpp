#include "ui/editor/ScriptStorage.h"

#include "core/PrefStore.h"
#include "core/Workspace.h"

#include <QDir>
#include <QFileInfo>

namespace lila::ui::storage
{
namespace
{
bool isUnder(const QString& path, const QString& dir)
{
    if (dir.isEmpty())
        return false;
    QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath()) + '/';
#ifdef _WIN32
    return p.startsWith(d, Qt::CaseInsensitive);
#else
    return p.startsWith(d);
#endif
}
} // namespace

QString locationName(Location loc)
{
    switch (loc)
    {
    case Location::Unsaved: return "Unsaved";
    case Location::Scene: return "Scene";
    case Location::Project: return "Project";
    case Location::File: return "File";
    case Location::Global: return "Global";
    }
    return {};
}

QString iconName(Location loc)
{
    switch (loc)
    {
    case Location::Unsaved: return "file-plus";
    case Location::Scene: return "cube";
    case Location::Project: return "folder-code";
    case Location::File: return "file-code";
    case Location::Global: return "world";
    }
    return {};
}

QString projectDir()
{
    Workspace& ws = Workspace::get();
    if (!ws.isOpen())
        return {};
    return QDir::cleanPath(QString::fromStdWString((ws.stateDir(false) / "scripts").wstring()));
}

QString globalDir()
{
    return QDir::cleanPath(QString::fromStdWString((PrefStore::get().userDir() / "scripts").wstring()));
}

Location locationOfPath(const QString& path, const QString& project, const QString& global)
{
    if (path.isEmpty())
        return Location::Unsaved;
    if (isUnder(path, project))
        return Location::Project;
    if (isUnder(path, global))
        return Location::Global;
    return Location::File;
}

Location locationOfPath(const QString& path)
{
    return locationOfPath(path, projectDir(), globalDir());
}

QString sanitizeName(const QString& name)
{
    QString s = name.trimmed();
    if (s.endsWith(".py", Qt::CaseInsensitive))
        s.chop(3);
    for (QChar& c : s)
        if (QStringLiteral("/\\:*?\"<>|").contains(c) || c.unicode() < 32)
            c = '_';
    while (s.startsWith('.'))
        s.remove(0, 1);
    return s;
}

QStringList scriptsIn(const QString& dir)
{
    if (dir.isEmpty())
        return {};
    QStringList names;
    for (const QFileInfo& f : QDir(dir).entryInfoList({"*.py"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        names << f.completeBaseName();
    return names;
}
} // namespace lila::ui::storage
