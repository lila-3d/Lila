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

#include <algorithm>
#include <memory>

namespace terrain
{
namespace httpdetail
{
/** The application object made here when the host had none. */
QCoreApplication* ownedApp = nullptr;

/** Qt Network needs an application object; lilabatch and test runners may have none. */
void ensureApp()
{
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "lila";
    static char* argv[] = {name, nullptr};
    ownedApp = new QCoreApplication(argc, argv);
}

/** Runs one GET, handing each chunk of a 200 or 206 answer to sink; false from sink aborts. range is a Range header value. */
int get(const std::string& url, const std::function<bool(const QByteArray&, int status)>& sink, std::string* why, const std::string& range = {})
{
    ensureApp();
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(QString::fromStdString(url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromUtf8(httpUserAgent()));
    if (!range.empty())
        req.setRawHeader("Range", QByteArray::fromStdString(range));
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
        if ((status == 200 || status == 206) && !sink(reply->readAll(), status))
        {
            sinkFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    idle.start();
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = status == 200 || status == 206;
    if (ok && !sinkFailed && !timedOut)
        sink(reply->readAll(), status);
    else if (!ok && why)
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
    const int status = httpdetail::get(url, [&](const QByteArray& chunk, int) { return out.write(chunk) == chunk.size(); }, why);
    out.close();
    return status;
}

int httpGetText(const std::string& url, std::string* body, std::string* why)
{
    std::string text;
    const int status = httpdetail::get(url, [&](const QByteArray& chunk, int) {
        text.append(chunk.constData(), size_t(chunk.size()));
        return true;
    }, why);
    if (body)
        *body = std::move(text);
    return status;
}

int httpGetRange(const std::string& url, uint64_t offset, uint64_t count, std::vector<uint8_t>* out, std::string* why)
{
    out->clear();
    if (count == 0)
        return 206;
    uint64_t at = 0;
    const int status = httpdetail::get(url, [&](const QByteArray& chunk, int st) {
        // A server that ignores Range sends the whole file from the start.
        const uint64_t from = st == 200 ? offset : 0;
        const uint64_t begin = std::max(at, from), end = std::min(at + uint64_t(chunk.size()), from + count);
        if (end > begin)
            out->insert(out->end(), chunk.constData() + (begin - at), chunk.constData() + (end - at));
        at += uint64_t(chunk.size());
        return out->size() < count;
    }, why, "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + count - 1));
    if (status == 0 && out->size() >= count)
        return 206;
    return status;
}

void httpShutdown()
{
    delete httpdetail::ownedApp;
    httpdetail::ownedApp = nullptr;
}
} // namespace terrain
