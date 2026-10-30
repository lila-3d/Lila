#include "ui/WelcomeNews.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Version.h"
#include "ui/Updater.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace lila::ui::news
{
using json = nlohmann::json;

namespace
{
std::string feedUrl()
{
    if (const char* v = std::getenv("LILA_NEWS_URL"); v && *v)
        return v;
    return kFeedUrl;
}

bool enabled()
{
    return PrefStore::get().value<bool>("general", kEnabledPref, true);
}

QDir cacheDir()
{
    QDir dir(QString::fromStdWString((paths::userConfigDir() / "news").wstring()));
    dir.mkpath(".");
    return dir;
}

QNetworkAccessManager& nam()
{
    static QPointer<QNetworkAccessManager> nam;
    if (!nam)
        nam = new QNetworkAccessManager(qApp);
    return *nam;
}

QNetworkReply* get(const QString& url)
{
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(10000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    return nam().get(req);
}

QString cachePath(const Item& item)
{
    const QString url = QString::fromStdString(item.image);
    const QString hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    const QString ext = QFileInfo(QUrl(url).path()).suffix();
    return cacheDir().filePath(ext.isEmpty() ? hash : hash + "." + ext);
}

std::vector<Item> readCache()
{
    QFile f(cacheDir().filePath("news.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parse(json::parse(f.readAll().toStdString(), nullptr, false), feedUrl(), version::number());
}
} // namespace

std::vector<Item> parse(const json& feed, std::string_view url, std::string_view version, size_t limit)
{
    std::vector<Item> out;
    const json& items = feed.is_object() ? feed.value("items", json::array()) : json::array();
    if (!items.is_array())
        return out;
    const QUrl base(QString::fromUtf8(url.data(), qsizetype(url.size())));
    for (const json& j : items)
    {
        if (!j.is_object() || !j.value("title", json()).is_string())
            continue;
        const std::string minVersion = j.value("minVersion", ""), maxVersion = j.value("maxVersion", "");
        if ((!minVersion.empty() && updater::compareVersions(version, minVersion) < 0) ||
            (!maxVersion.empty() && updater::compareVersions(version, maxVersion) > 0))
            continue;
        Item item;
        item.id = j.value("id", "");
        item.type = j.value("type", "news");
        item.date = j.value("date", "");
        item.title = j.value("title", "");
        item.body = j.value("body", "");
        item.author = j.value("author", "");
        item.url = j.value("url", "");
        if (const std::string image = j.value("image", ""); !image.empty())
            item.image = base.resolved(QUrl(QString::fromStdString(image))).toString().toStdString();
        out.push_back(std::move(item));
    }
    // ISO dates sort as text; stable so same-day items keep the feed's order.
    std::stable_sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.date > b.date; });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

std::vector<Item> cached()
{
    return enabled() ? readCache() : std::vector<Item>{};
}

QString imageFile(const Item& item)
{
    if (item.image.empty())
        return {};
    QString path = cachePath(item);
    return QFileInfo::exists(path) ? path : QString();
}

void refresh(std::function<void(const std::vector<Item>&)> done)
{
    if (!enabled())
        return;
    QNetworkReply* reply = get(QString::fromStdString(feedUrl()));
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            log::info("news: {}", reply->errorString().toStdString());
            return;
        }
        const QByteArray body = reply->readAll();
        if (json::parse(body.toStdString(), nullptr, false).is_discarded())
            return;
        if (QFile f(cacheDir().filePath("news.json")); f.open(QIODevice::WriteOnly))
            f.write(body);

        std::vector<Item> items = readCache();
        auto pending = std::make_shared<int>(1);
        auto finish = [pending, items, done] {
            if (--*pending == 0 && done)
                done(items);
        };
        for (const Item& item : items)
        {
            if (item.image.empty() || !imageFile(item).isEmpty())
                continue;
            ++*pending;
            QNetworkReply* image = get(QString::fromStdString(item.image));
            QObject::connect(image, &QNetworkReply::finished, qApp, [image, path = cachePath(item), finish] {
                image->deleteLater();
                if (image->error() == QNetworkReply::NoError)
                    if (QFile f(path); f.open(QIODevice::WriteOnly))
                        f.write(image->readAll());
                finish();
            });
        }
        finish();
    });
}
} // namespace lila::ui::news
