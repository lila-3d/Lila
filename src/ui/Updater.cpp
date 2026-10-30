#include "ui/Updater.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Version.h"
#include "ui/Telemetry.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <cstdlib>
#include <functional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace lila::ui::updater
{
namespace
{
constexpr const char* kManifestUrl = "https://github.com/lila-3d/Lila/releases/latest/download/latest.json";
constexpr int kStartupDelayMs = 15 * 1000;

struct State
{
    QPointer<QNetworkAccessManager> nam;
    bool busy = false;
};

State& state()
{
    static State s;
    return s;
}

QNetworkAccessManager& nam()
{
    State& s = state();
    if (!s.nam)
        s.nam = new QNetworkAccessManager(qApp);
    return *s.nam;
}

std::string manifestUrl()
{
    if (const char* v = std::getenv("LILA_UPDATE_URL"); v && *v)
        return v;
    return kManifestUrl;
}

/** Steam updates itself and a source build is the developer's own; an explicit LILA_UPDATE_URL overrides the latter. */
bool supported()
{
    if (const char* v = std::getenv("LILA_NO_UPDATE_CHECK"); v && *v && std::string_view(v) != "0")
        return false;
    const std::string channel = telemetry::installChannel(paths::executableDir());
    if (channel == "steam")
        return false;
    return channel != "source" || std::getenv("LILA_UPDATE_URL");
}

QString updateDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("LilaUpdate");
}

QString sha256Of(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

/** Kept in prefs, so a crash before Lila closes doesn't lose the download. */
void readyToInstall(QWidget* window, const Release& r, const QString& file)
{
    PrefStore::get().set("general", kPendingPref, file.toStdString());
    log::info("Lila {} downloaded; it installs when Lila closes", r.version);
    if (window)
        QMessageBox::information(window, "Update", QString("Lila %1 is ready. It installs when you close Lila.").arg(QString::fromStdString(r.version)));
}

/** Another Lila still running would hold the files the installer replaces. */
bool otherInstancesRunning()
{
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = e.th32ProcessID != GetCurrentProcessId() && _wcsicmp(e.szExeFile, L"lila.exe") == 0;
    CloseHandle(snap);
    return found;
#else
    return false;
#endif
}

void fetchManifest(std::function<void(bool ok, const Release&)> done)
{
    QNetworkRequest req{QUrl(QString::fromStdString(manifestUrl()))};
    req.setTransferTimeout(10000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = nam().get(req);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            log::info("update check failed: {}", reply->errorString().toStdString());
            done(false, {});
            return;
        }
        Release r = parseManifest(nlohmann::json::parse(reply->readAll().toStdString(), nullptr, false));
        done(!r.version.empty(), r);
    });
}

/** Downloads the installer next to nothing else of the user's, and keeps it only when the hash matches. */
void download(QWidget* window, const Release& r)
{
    State& s = state();
    if (s.busy)
        return;
    const QString file = QDir(updateDir()).filePath(QString("Lila-%1-Setup.exe").arg(QString::fromStdString(r.version)));
    if (QFile existing(file);
        existing.open(QIODevice::ReadOnly) && sha256Of(existing.readAll()).compare(QString::fromStdString(r.sha256), Qt::CaseInsensitive) == 0)
    {
        readyToInstall(window, r, file);
        return;
    }
    s.busy = true;
    QNetworkRequest req{QUrl(QString::fromStdString(r.installer))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    QNetworkReply* reply = nam().get(req);
    QPointer<QWidget> win(window);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, r, win, file] {
        reply->deleteLater();
        state().busy = false;
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || sha256Of(data).compare(QString::fromStdString(r.sha256), Qt::CaseInsensitive) != 0)
        {
            log::warn("update download failed or didn't match its checksum");
            if (win)
                QMessageBox::warning(win, "Update", "The update couldn't be downloaded. You can get it from the release page instead.");
            return;
        }
        QDir().mkpath(updateDir());
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
            return;
        out.close();
        readyToInstall(win, r, file);
    });
}

void offer(QWidget* window, const Release& r)
{
    QMessageBox box(window);
    box.setWindowTitle("Update");
    box.setIcon(QMessageBox::Information);
    box.setText(QString("Lila %1 is available. You have %2.").arg(QString::fromStdString(r.version), version::number()));
    if (!r.notes.empty())
        box.setInformativeText(QString::fromStdString(r.notes));
    QPushButton* install = r.installer.empty() || r.sha256.empty() ? nullptr : box.addButton("Install When Lila Closes", QMessageBox::AcceptRole);
    QPushButton* page = r.page.empty() ? nullptr : box.addButton("Open Release Page", QMessageBox::ActionRole);
    QPushButton* skip = box.addButton("Skip This Version", QMessageBox::RejectRole);
    QPushButton* later = box.addButton("Later", QMessageBox::RejectRole);
    box.setDefaultButton(install ? install : page);
    box.setEscapeButton(later);
    box.exec();
    if (box.clickedButton() == install)
        download(window, r);
    else if (page && box.clickedButton() == page)
        QDesktopServices::openUrl(QUrl(QString::fromStdString(r.page)));
    else if (box.clickedButton() == skip)
        PrefStore::get().set("general", kSkipPref, r.version);
}
} // namespace

int compareVersions(std::string_view a, std::string_view b)
{
    auto next = [](std::string_view& s) {
        int n = 0;
        while (!s.empty() && s.front() >= '0' && s.front() <= '9')
        {
            n = n * 10 + (s.front() - '0');
            s.remove_prefix(1);
        }
        if (!s.empty())
            s.remove_prefix(1);
        return n;
    };
    while (!a.empty() || !b.empty())
    {
        int x = next(a), y = next(b);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

Release parseManifest(const nlohmann::json& j)
{
    Release r;
    if (!j.is_object())
        return r;
    auto str = [&](const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); };
    r.version = str("version");
    r.notes = str("notes");
    r.page = str("page");
    r.installer = str("installer");
    r.sha256 = str("sha256");
    return r;
}

void startAutoCheck(QWidget* window)
{
    // Running the version that was pending means it went in.
    const QString pending = QString::fromStdString(PrefStore::get().value<std::string>("general", kPendingPref, ""));
    if (!pending.isEmpty() && QFileInfo(pending).fileName() == QString("Lila-%1-Setup.exe").arg(version::number()))
    {
        PrefStore::get().remove("general", kPendingPref);
        QFile::remove(pending);
    }
    if (!supported() || !PrefStore::get().value<bool>("general", kCheckPref, true))
        return;
    QPointer<QWidget> win(window);
    QTimer::singleShot(kStartupDelayMs, qApp, [win] {
        fetchManifest([win](bool ok, const Release& r) {
            if (!ok || !win || compareVersions(r.version, version::number()) <= 0)
                return;
            if (PrefStore::get().value<std::string>("general", kSkipPref, "") == r.version)
                return;
            offer(win, r);
        });
    });
}

void checkNow(QWidget* window)
{
    if (!supported())
    {
        QMessageBox::information(window, "Check for Updates",
                                 telemetry::installChannel(paths::executableDir()) == "steam" ? "Steam keeps Lila up to date."
                                                                                                : "This is a source build: pull and rebuild to update.");
        return;
    }
    QPointer<QWidget> win(window);
    fetchManifest([win](bool ok, const Release& r) {
        if (!win)
            return;
        if (!ok)
            QMessageBox::warning(win, "Check for Updates", "Couldn't reach the update server. Check your connection and try again.");
        else if (compareVersions(r.version, version::number()) <= 0)
            QMessageBox::information(win, "Check for Updates", QString("Lila %1 is the latest version.").arg(version::number()));
        else
            offer(win, r);
    });
}

void installPendingOnExit()
{
    const QString file = QString::fromStdString(PrefStore::get().value<std::string>("general", kPendingPref, ""));
    if (file.isEmpty())
        return;
    if (!QFile::exists(file))
    {
        PrefStore::get().remove("general", kPendingPref);
        return;
    }
    if (otherInstancesRunning())
        return;
    PrefStore::get().flush();
    QProcess::startDetached(file, {"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER"});
}

bool deferToRunningInstall()
{
#ifdef _WIN32
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"LilaSetup");
    if (!m)
        return false;
    CloseHandle(m);
    // The installer starts the new Lila when it finishes if this flag exists.
    QDir().mkpath(updateDir());
    QFile flag(QDir(updateDir()).filePath("relaunch"));
    (void)flag.open(QIODevice::WriteOnly);
    return true;
#else
    return false;
#endif
}
} // namespace lila::ui::updater
