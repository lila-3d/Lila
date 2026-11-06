// HTTP downloads for terrain sources, on Qt Network like the rest of Lila's downloads.

#include "Http.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace terrain
{
namespace httpdetail
{
/** Qt Network needs an application object; lilabatch and test runners may have none. */
void ensureApp()
{
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "lila";
    static char* argv[] = {name, nullptr};
    static std::unique_ptr<QCoreApplication> app = std::make_unique<QCoreApplication>(argc, argv);
}

/** Runs one GET, handing each chunk to sink; false from sink aborts. */
int get(const std::string& url, const std::function<bool(const QByteArray&)>& sink, std::string* why)
{
    ensureApp();
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(QString::fromStdString(url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromUtf8(httpUserAgent()));
    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QTimer idle;
    idle.setSingleShot(true);
    idle.setInterval(120000);
    bool timedOut = false, sinkFailed = false;
    QObject::connect(&idle, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        idle.start();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200 && !sink(reply->readAll()))
        {
            sinkFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    idle.start();
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200 && !sinkFailed && !timedOut)
        sink(reply->readAll());
    else if (status != 200 && why)
    {
        // Error bodies are short text (OpenTopography explains bad keys and areas there).
        const QByteArray body = reply->readAll().left(400);
        *why = timedOut ? std::string("no answer for 120 s") : sinkFailed ? std::string("can't write the download") : reply->errorString().toStdString();
        if (!body.isEmpty() && !timedOut)
            *why += ": " + body.toStdString();
    }
    reply->deleteLater();
    if (timedOut || sinkFailed)
        return 0;
    return status;
}
} // namespace httpdetail

const char* httpUserAgent() { return "Lila/1 (3D app terrain import; https://github.com/lila-3d/Lila)"; }

int httpGet(const std::string& url, const std::filesystem::path& dest, std::string* why)
{
    QFile out(QString::fromStdWString(dest.wstring()));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (why)
            *why = "can't write " + dest.string();
        return 0;
    }
    const int status = httpdetail::get(url, [&](const QByteArray& chunk) { return out.write(chunk) == chunk.size(); }, why);
    out.close();
    return status;
}

int httpGetText(const std::string& url, std::string* body, std::string* why)
{
    std::string text;
    const int status = httpdetail::get(url, [&](const QByteArray& chunk) {
        text.append(chunk.constData(), size_t(chunk.size()));
        return true;
    }, why);
    if (body)
        *body = std::move(text);
    return status;
}
} // namespace terrain
