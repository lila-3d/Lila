#include "ui/Credits.h"

#include "core/Log.h"
#include "core/Paths.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <climits>
#include <memory>

namespace lila::ui::credits
{
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
/** Kept in step with DEFAULT_ALLOWLIST in services/telemetry-worker/src/credits.js. */
constexpr std::array<std::string_view, 5> kPlatforms = {"github.com", "artstation.com", "linkedin.com", "youtube.com", "x.com"};

std::string fileUrl()
{
    if (const char* v = std::getenv("LILA_CREDITS_URL"); v && *v)
        return v;
    return kUrl;
}

QDir cacheDir()
{
    QDir dir(QString::fromStdWString((paths::userConfigDir() / "credits").wstring()));
    dir.mkpath(".");
    return dir;
}

QString cacheFile()
{
    return cacheDir().filePath("credits.json");
}

QNetworkAccessManager& nam()
{
    static QPointer<QNetworkAccessManager> nam;
    if (!nam)
        nam = new QNetworkAccessManager(qApp);
    return *nam;
}

QNetworkRequest request(const QString& url)
{
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(10000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    return req;
}

json readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return json();
    return json::parse(f.readAll().toStdString(), nullptr, false);
}

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool flag(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

bool tierFromId(const std::string& id, Tier& out)
{
    static const std::pair<const char*, Tier> kTiers[] = {
        {"supporter", Tier::Supporter}, {"patron", Tier::Patron}, {"legendary", Tier::Legendary},
        {"mythic", Tier::Mythic},       {"eternal", Tier::Eternal},
    };
    for (const auto& [name, tier] : kTiers)
        if (id == name)
        {
            out = tier;
            return true;
        }
    return false;
}

const json& array(const json& file, const char* key)
{
    static const json kEmpty = json::array();
    if (!file.is_object())
        return kEmpty;
    auto it = file.find(key);
    return it != file.end() && it->is_array() ? *it : kEmpty;
}

/** Where a sponsor's logo is cached, whether or not it has been downloaded yet. */
QString logoPath(const Sponsor& s)
{
    const QString url = QString::fromStdString(s.logo);
    const QString hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    const QString ext = QFileInfo(QUrl(url).path()).suffix();
    return cacheDir().filePath(ext.isEmpty() ? hash : hash + "." + ext);
}

std::string trimmed(std::string s)
{
    return QString::fromStdString(s).simplified().toStdString();
}
} // namespace

QString tierTitle(Tier t)
{
    switch (t)
    {
    case Tier::Supporter: return "Supporters";
    case Tier::Patron: return "Patrons";
    case Tier::Legendary: return "Legendary";
    case Tier::Mythic: return "Mythic";
    case Tier::Eternal: return "Eternal";
    }
    return {};
}

bool linkAllowed(std::string_view url, bool corporate)
{
    const QUrl u(QString::fromUtf8(url.data(), qsizetype(url.size())), QUrl::StrictMode);
    if (!u.isValid() || u.scheme() != "https" || !u.userInfo().isEmpty() || u.port() != -1 || u.host().isEmpty())
        return false;
    if (corporate)
        return true;
    QString host = u.host().toLower();
    if (host.startsWith("www."))
        host = host.mid(4);
    if (std::ranges::none_of(kPlatforms, [&](std::string_view p) { return host == QLatin1StringView(p.data(), qsizetype(p.size())); }))
        return false;
    QString path = u.path();
    while (path.endsWith('/'))
        path.chop(1);
    return !path.isEmpty();
}

std::string linkHost(std::string_view url)
{
    QString host = QUrl(QString::fromUtf8(url.data(), qsizetype(url.size()))).host().toLower();
    return host.startsWith("www.") ? host.mid(4).toStdString() : host.toStdString();
}

Credits parse(const json& file, bool withLinks, std::string_view url)
{
    Credits c;
    const QUrl base(QString::fromUtf8(url.data(), qsizetype(url.size())));
    auto link = [&](const json& j, bool corporate) {
        std::string u = withLinks ? str(j, "url") : std::string();
        return linkAllowed(u, corporate) ? u : std::string();
    };
    auto sponsors = [&](const char* key, std::vector<Sponsor>& out, bool links) {
        for (const json& j : array(file, key))
        {
            if (!j.is_object())
                continue;
            Sponsor s{trimmed(str(j, "name")), str(j, "logo"), links ? link(j, true) : std::string()};
            if (s.name.empty())
                continue;
            if (!s.logo.empty())
                s.logo = base.resolved(QUrl(QString::fromStdString(s.logo))).toString().toStdString();
            out.push_back(std::move(s));
        }
    };
    sponsors("corporate", c.corporate, true);
    sponsors("pastSponsors", c.pastSponsors, false);

    for (const json& j : array(file, "supporters"))
    {
        Supporter s;
        if (!j.is_object() || !tierFromId(str(j, "tier"), s.tier))
            continue;
        s.name = trimmed(str(j, "name"));
        if (s.name.empty())
            continue;
        auto n = j.find("number");
        s.number = n != j.end() && n->is_number_integer() ? n->get<int>() : 0;
        s.founder = flag(j, "founder");
        if (s.tier == Tier::Eternal)
            s.url = link(j, false);
        c.supporters.push_back(std::move(s));
    }
    std::stable_sort(c.supporters.begin(), c.supporters.end(), [](const Supporter& a, const Supporter& b) {
        if (a.tier != b.tier)
            return a.tier > b.tier;
        if (a.number != b.number)
            return (a.number ? a.number : INT_MAX) < (b.number ? b.number : INT_MAX);
        return QString::compare(QString::fromStdString(a.name), QString::fromStdString(b.name), Qt::CaseInsensitive) < 0;
    });

    for (const json& j : array(file, "contributors"))
    {
        if (!j.is_object())
            continue;
        Contributor ct{trimmed(str(j, "name")), trimmed(str(j, "role")), {}, link(j, false)};
        if (ct.name.empty())
            continue;
        for (const json& a : array(j, "areas"))
            if (a.is_string() && !a.get<std::string>().empty())
                ct.areas.push_back(a.get<std::string>());
        c.contributors.push_back(std::move(ct));
    }

    for (const json& j : array(file, "testers"))
    {
        if (!j.is_object())
            continue;
        Tester t{trimmed(str(j, "name")), flag(j, "founding")};
        if (!t.name.empty())
            c.testers.push_back(std::move(t));
    }
    std::stable_sort(c.testers.begin(), c.testers.end(), [](const Tester& a, const Tester& b) { return a.founding > b.founding; });
    return c;
}

std::vector<std::string> madePossibleBy(const Credits& c, int rotation)
{
    std::vector<std::string> out;
    for (const Supporter& s : c.supporters)
        if (s.tier >= Tier::Mythic)
            out.push_back(s.name);
    const std::vector<Supporter> legendary = c.tier(Tier::Legendary);
    if (!legendary.empty())
    {
        const size_t i = size_t(std::max(rotation, 0)) % legendary.size();
        out.push_back(legendary[i].name);
    }
    return out;
}

fs::path bundledFile()
{
    return paths::resourcesDir() / "credits" / "credits.json";
}

Credits current()
{
    const json live = readJson(cacheFile());
    if (live.is_object())
        return parse(live, true, fileUrl());
    return parse(readJson(QString::fromStdWString(bundledFile().wstring())), false, fileUrl());
}

QString logoFile(const Sponsor& s)
{
    if (s.logo.empty())
        return {};
    const QString path = logoPath(s);
    return QFileInfo::exists(path) ? path : QString();
}

void refresh(std::function<void(const Credits&)> done)
{
    QNetworkReply* reply = nam().get(request(QString::fromStdString(fileUrl())));
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            log::info("credits: {}", reply->errorString().toStdString());
            return;
        }
        const QByteArray body = reply->readAll();
        if (!json::parse(body.toStdString(), nullptr, false).is_object())
            return;
        if (QFile f(cacheFile()); f.open(QIODevice::WriteOnly))
            f.write(body);

        const Credits c = current();
        auto pending = std::make_shared<int>(1);
        auto finish = [pending, c, done] {
            if (--*pending == 0 && done)
                done(c);
        };
        std::vector<Sponsor> logos = c.corporate;
        logos.insert(logos.end(), c.pastSponsors.begin(), c.pastSponsors.end());
        for (const Sponsor& s : logos)
        {
            if (s.logo.empty() || !logoFile(s).isEmpty())
                continue;
            ++*pending;
            QNetworkReply* image = nam().get(request(QString::fromStdString(s.logo)));
            QObject::connect(image, &QNetworkReply::finished, qApp, [image, path = logoPath(s), finish] {
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

void refreshIfStale(std::function<void(const Credits&)> done)
{
    const QFileInfo cached(cacheFile());
    if (cached.exists() && cached.lastModified().secsTo(QDateTime::currentDateTime()) < 3600)
        return;
    refresh(std::move(done));
}

void report(const std::string& name, const std::string& url, std::function<void(bool)> done)
{
    QNetworkRequest req = request(kReportUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const std::string body = json{{"name", name}, {"url", url}}.dump();
    QNetworkReply* reply = nam().post(req, QByteArray::fromStdString(body));
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, done = std::move(done)] {
        reply->deleteLater();
        if (done)
            done(reply->error() == QNetworkReply::NoError);
    });
}

fs::path licenceNotices()
{
    // Installed builds ship it beside lila.exe; a source build finds it at the repo root above resources.
    for (const fs::path& dir : {paths::executableDir(), paths::resourcesDir().parent_path()})
    {
        std::error_code ec;
        if (fs::path f = dir / "THIRD-PARTY-NOTICES.md"; fs::exists(f, ec))
            return f;
    }
    return {};
}
} // namespace lila::ui::credits
